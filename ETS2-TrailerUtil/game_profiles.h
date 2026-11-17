#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <scs_sdk/scssdk.h>
#include <scs_sdk/scssdk_telemetry.h>

// ============================================================================
// game_profiles.h
// ----------------------------------------------------------------------------
// 游戏版本"内存布局配置层"。
//
// 背景：
//   本插件的核心功能依赖对游戏进程的逆向：通过 AOB 特征码扫描定位游戏内部
//   函数/全局变量，再用偏移读取挂车对象。SCS 每次升级游戏（1.60 -> 1.61 ->
//   1.62 ...）都可能改动这些机器码字节和结构体布局，导致特征码失配、插件失效。
//
//   过去每次适配都要在 dllmain.cpp 里到处改散落的字符串和偏移（见 1.60 提交）。
//   这里把所有"版本相关常量"集中到一张 profile 表里：
//     - 适配新版本 = 在 profiles() 里新增一条记录，不用再翻遍代码；
//     - 同一份 DLL 可携带多个版本的配置，运行时按 game_id + 特征码自动匹配。
//
// 如何为新版本添加 profile：
//   1. 用 x64dbg/Cheat Engine 打开新版游戏 exe，定位下列 4 个目标：
//        - game_ctrl 指针指令      （含 RIP 相对寻址的 mov）
//        - game_ctrl->game_actor   偏移读取指令
//        - prism::g_set_vehicle_steering 函数入口
//        - prism::g_advance_steering     函数入口（核心 Hook 点）
//   2. 各取一段唯一的字节序列作为 AOB 特征码，变化的字节写成 "??"。
//      工具：项目 tools/scan_aob.py 可在 exe 中验证特征码是否唯一匹配。
//   3. 从目标函数反汇编中读出各结构体字段偏移（见下方字段注释）。
//   4. 新增一个 GameProfile，填好，并放到列表【最前面】（自上而下优先匹配）。
//
// 字段偏移验证等级（1.62）—— 用 1.60 与 1.62 两版 exe 交叉对比得出：
//   [确认] sig_* 特征码：均在新版 exe 中唯一命中（ETS2 1.62.3.3 / ATS 1.62.2.2）
//   [确认] steering 0x640->0x6B8, steering_data 0x648->0x6C0 (+0x78)：
//          对比两版 g_advance_steering 的读写指令（铁证）
//   [确认] hook_joint 0xF68->0xFB0 (+0x48)：
//          对比两版"读关节指针->虚表 call [+0xE0]"的调用模式（铁证）
//   [确认] trailer_actor_offset_render=0xC8：从游戏挂车链表遍历代码反推
//   [保守] slave=0x1010：两版 exe 在该区域的访问模式完全相同，判定未变，
//          沿用原仓库 1.60 定义（未实测）
//   [已移除] suspension_height：1.62 偏移静态/运行时均无法可靠确定，且属次要功能，
//          已从菜单中移除悬挂高度调节（应需求）。
//
// 重要：结构体不是简单整体平移！不同字段增量不同（steering +0x78, joint +0x48），
//       所以字段访问已全部改为运行时从本配置读取（见 dllmain.cpp 的 f_* 辅助函数），
//       同一份 DLL 可同时支持 1.60 / 1.62 两种布局。
// ============================================================================

namespace gamecfg
{
    struct GameProfile
    {
        // ---- 标识 ----
        const char* label;      // 版本标签，仅用于日志
        const char* game_id;    // SCS_GAME_ID_EUT2 / SCS_GAME_ID_ATS；nullptr=通用

        // ---- AOB 特征码 ----
        const char* sig_game_ctrl_ptr;        // 定位 game_ctrl 全局指针
        const char* sig_game_actor_offset;    // 定位 game_ctrl->game_actor 偏移
        const char* sig_set_vehicle_steering; // 定位 g_set_vehicle_steering
        const char* sig_advance_steering;     // 定位 g_advance_steering（核心 Hook）

        // ---- 解析参数 ----
        int game_ctrl_ptr_field;     // RIP 位移字段相对 sig 首地址的偏移
        int game_ctrl_ptr_instr_len; // 该指令长度（= 字段偏移 + 4）
        int game_actor_offset_field; // game_actor 偏移字段相对 sig 首地址的偏移

        // ---- 结构体偏移 ----
        // game_actor -> 挂车对象。原代码两条路径用不同值（控制 0xC0 / 渲染 0xC8）；
        // 1.62 实测 0xC0 指向无效对象，故两条都填 0xC8，与 hook 收到的对象一致，
        // 否则按键锁定的对象与 hook 处理的对象不同，转向不生效。1.60 维持原值。
        uint32_t trailer_actor_offset;        // 控制/转向路径
        uint32_t trailer_actor_offset_render; // 渲染挂车列表路径

        // 挂车对象内的字段偏移（相对 game_trailer_actor_u 起始地址）。
        // 字段访问在运行时按这些值进行（dllmain.cpp 的 f_* 辅助函数），
        // 因此改这里即可生效，无需改动编译期结构体。
        uint32_t off_trailer_steering;       // float steering
        uint32_t off_trailer_steering_data;  // steering_data_t* steering_data
        uint32_t off_trailer_hook_joint;     // physics_joint_physx_t* hook_joint
        uint32_t off_trailer_slave;          // game_trailer_actor_u* slave
    };

    inline const std::vector<GameProfile>& profiles()
    {
        static const std::vector<GameProfile> list = {
            // ---- ETS2 1.62.3.3 / ATS 1.62.2.2（当前版本，2026-11 验证）----
            // 注：前三条特征码沿用 1.60 起就有效的版本；第 4 条 g_advance_steering
            //     在 1.62 改动，已重新提取，两游戏均唯一命中。
            {
                "ETS2/ATS 1.62",
                nullptr, // 对 eut2 和 ats 都适用（两游戏此版本布局一致）
                "48 8B 0D ?? ?? ?? ?? 0F 57 C0 48 8B D0",
                "4C 8B 81 ?? ?? ?? ?? 48 8B DA 4C 8B F1",
                "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 8B 41 ?? 48 8B D9 0F 29 74",
                "48 8B C4 55 53 56 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 80 B9 A9 0F 00 00 00 48 8B F1",
                3,    // game_ctrl_ptr_field
                7,    // game_ctrl_ptr_instr_len
                3,    // game_actor_offset_field
                0xC8, // trailer_actor_offset        [改为与hook/渲染一致: 实测0xC0指向错误对象]
                0xC8, // trailer_actor_offset_render  [确认]
                0x6B8, // off_trailer_steering       [确认 +0x78]
                0x6C0, // off_trailer_steering_data  [确认 +0x78]
                0xFB0, // off_trailer_hook_joint     [确认 +0x48]
                0x1058 // off_trailer_slave          [推断: hook 0xFB0 + 0xA8]
            },

            // ---- ETS2/ATS 1.60（旧版本 fallback，来自 "1.60 support" 提交）----
            {
                "ETS2/ATS 1.60",
                nullptr,
                "48 8B 0D ?? ?? ?? ?? 0F 57 C0 48 8B D0",
                "4C 8B 81 ?? ?? ?? ?? 48 8B DA 4C 8B F1",
                "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ?? 8B 41 ?? 48 8B D9 0F 29 74",
                "40 55 53 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 91",
                3, 7, 3,
                0xC0, 0xC8,
                0x640, // off_trailer_steering
                0x648, // off_trailer_steering_data
                0xF68, // off_trailer_hook_joint
                0x1010 // off_trailer_slave
            },

            // ---- 在此之上新增新版本 profile，例如：----
            // {
            //     "ETS2 1.63",
            //     SCS_GAME_ID_EUT2,
            //     "<新特征码>", "<新特征码>", "<新特征码>", "<新特征码>",
            //     3, 7, 3,
            //     0x??, 0x??,   // trailer_actor_offset, trailer_actor_offset_render
            //     0x???, 0x???, 0x???, 0x???  // steering, steering_data, hook_joint, slave
            // },
        };
        return list;
    }

    inline bool matches_game(const GameProfile& p, const char* game_id)
    {
        if (p.game_id == nullptr) return true;
        if (game_id == nullptr) return false;
        return std::strcmp(p.game_id, game_id) == 0;
    }
}
