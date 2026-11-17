#include "dllmain.h"

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <thread>

#include "scs_sdk/scssdk_telemetry.h"
#include "rendering.h"
#include "ImGui/imgui.h"

#include "globals.h"
#include "bmem.h"
#include "scs_logging.h"
#include "prism/prism.h"
#include "game_profiles.h"

#include "launch_arguments_manager.h"

#include "direct_input_manager.h"
#include <unordered_map>

#include "nlohmann/json.hpp"
using json = nlohmann::json;

#include <DirectXMath.h>
using namespace DirectX;


#pragma comment(lib, "../Lib/minhook.x64.lib")

using namespace scs_logging;

RenderManager* renderer;

std::vector<prism::game_trailer_actor_u*> locked_trailers;
std::vector<prism::game_trailer_actor_u*> temp_locked_trailers;

uintptr_t game_ctrl_ptr;
uint32_t game_actor_offset;

// 当前匹配到的游戏版本配置（见 game_profiles.h）
const gamecfg::GameProfile* active_profile = nullptr;
// 挂车对象相对 game_actor 的偏移，运行时由 active_profile 决定（旧代码硬编码 0xC0/0xC8）
uint32_t trailer_actor_offset = 0xC0;
uint32_t trailer_actor_offset_render = 0xC8;

// 挂车对象内部字段偏移，运行时由 active_profile 填充。
// 用这些偏移做动态访问，使同一份 DLL 能支持布局不同的多个游戏版本。
uint32_t off_steering = 0x640;
uint32_t off_steering_data = 0x648;
uint32_t off_hook_joint = 0xF68;
uint32_t off_slave = 0x1010;

// ---- 按运行时偏移访问挂车字段（替代编译期结构体的 -> 访问）----
template <typename T>
inline T& field_at(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + offset);
}
inline float& f_steering(void* a)        { return field_at<float>(a, off_steering); }
inline prism::steering_data_t*& f_steerdata(void* a) {
    return field_at<prism::steering_data_t*>(a, off_steering_data);
}
inline prism::physics_joint_physx_t*& f_hookjoint(void* a) {
    return field_at<prism::physics_joint_physx_t*>(a, off_hook_joint);
}
inline prism::game_trailer_actor_u*& f_slave(void* a) {
    return field_at<prism::game_trailer_actor_u*>(a, off_slave);
}

// 挂车指针有效性校验：防止错误的字段偏移导致野指针解引用而崩溃游戏。
// 只做轻量检查：非空、未对齐到明显非法值、前若干字节可读。
inline bool is_valid_trailer(const void* p) {
    if (p == nullptr) return false;
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v < 0x10000) return false;                 // 明显非法的小地址
    if (IsBadReadPtr(p, sizeof(void*) * 4)) return false; // 内存不可读
    return true;
}

// 挂车链最大遍历数，防止偏移错误时沿野链无限循环
constexpr int MAX_TRAILERS = 16;

bool is_fatal_error = false;
std::string error = "";

bool running_truckersmp = false;
bool shown_truckersmp_warning = false;

void SetupImGuiStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha = 1.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.WindowRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 3.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 3.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(4.0f, 3.0f);
    style.FrameRounding = 3.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 2.0f);
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 5.599999904632568f;
    style.ScrollbarRounding = 18.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.TabBorderSize = 0.0f;
//    style.TabMinWidthForCloseButton = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6000000238418579f, 0.6000000238418579f, 0.6000000238418579f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.125490203499794f, 0.125490203499794f, 0.125490203499794f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.125490203499794f, 0.125490203499794f, 0.125490203499794f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.125490203499794f, 0.125490203499794f, 0.125490203499794f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.125490203499794f, 0.125490203499794f, 0.125490203499794f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.125490203499794f, 0.125490203499794f, 0.125490203499794f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.3019607961177826f, 0.3019607961177826f, 0.3019607961177826f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.3490196168422699f, 0.3490196168422699f, 0.3490196168422699f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 0.7960784435272217f, 0.6078431606292725f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.0f, 0.7960784435272217f, 0.6078431606292725f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 0.5922746658325195f, 0.452229231595993f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.3019607961177826f, 0.3019607961177826f, 0.3019607961177826f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.3019607961177826f, 0.3019607961177826f, 0.3019607961177826f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.3019607961177826f, 0.3019607961177826f, 0.3019607961177826f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.250980406999588f, 0.250980406999588f, 0.250980406999588f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.168627455830574f, 0.168627455830574f, 0.168627455830574f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.2156862765550613f, 0.2156862765550613f, 0.2156862765550613f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.0f, 0.4705882370471954f, 0.843137264251709f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.0f, 0.3294117748737335f, 0.6000000238418579f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.0f, 0.4705882370471954f, 0.843137264251709f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.0f, 0.3294117748737335f, 0.6000000238418579f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.1882352977991104f, 0.1882352977991104f, 0.2000000029802322f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.3098039329051971f, 0.3098039329051971f, 0.3490196168422699f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.2274509817361832f, 0.2274509817361832f, 0.2470588237047195f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.05999999865889549f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.4705882370471954f, 0.843137264251709f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.8999999761581421f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.2588235437870026f, 0.5882353186607361f, 0.9764705896377563f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.699999988079071f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.800000011920929f, 0.800000011920929f, 0.800000011920929f, 0.2000000029802322f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.800000011920929f, 0.800000011920929f, 0.800000011920929f, 0.3499999940395355f);
}
bool isTrailerLocked(prism::game_trailer_actor_u* trailer)
{
    for (prism::game_trailer_actor_u* locked_trailer : locked_trailers) {
        if (locked_trailer == trailer)
            return true;
    }

    return false;
}

bool isTrailerTempLocked(prism::game_trailer_actor_u* trailer)
{
    for (prism::game_trailer_actor_u* temp_locked_trailer : temp_locked_trailers) {
        if (temp_locked_trailer == trailer)
            return true;
    }

    return false;
}


bool isTrailerSteerable(prism::game_trailer_actor_u* trailer)
{
    return f_steerdata(trailer) != nullptr;
}

void render_error(std::string message) {
    ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), message.c_str());
}

void render_fatal(std::string message) {
    render_error("A fatal error has occured within this plugin!");
    ImGui::TextWrapped("Please make sure your game is fully loaded!");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Get Help!"))
    {
        ShellExecuteA(NULL, "open", "https://github.com/suagrsu/TrailerUtils", NULL, NULL, SW_SHOWNORMAL);
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

#ifdef _DEBUG
    if (ImGui::CollapsingHeader("Developer Information"))
    {
        render_error("Error: " + message);
        ImGui::Separator();

        uint8_t* game_ctrl = *(uint8_t**)game_ctrl_ptr;

        ImGui::TextWrapped("game_ctrl_ptr: %p", game_ctrl_ptr);
        ImGui::TextWrapped("game_ctrl: %p", game_ctrl);

        if (game_ctrl) {
            uint8_t* game_actor = *(uint8_t**)(game_ctrl + game_actor_offset);
            ImGui::TextWrapped("game_actor: %p", game_actor);

            if (game_actor) {
                uint8_t* trailer_actor = *(uint8_t**)(game_actor + trailer_actor_offset);
                ImGui::TextWrapped("trailer_actor: %p", trailer_actor);


            }
        }

        ImGui::TextWrapped("game_actor_offset: %d", game_actor_offset);
    }
#endif

    ImGui::EndTabItem();
}

BOOL CALLBACK EnumAxesCallback(const DIDEVICEOBJECTINSTANCE* pdidoi, VOID* pContext)
{
    auto* axisList = reinterpret_cast<std::vector<DIDEVICEOBJECTINSTANCE>*>(pContext);

    if (pdidoi->dwType & DIDFT_AXIS)
    {
        axisList->push_back(*pdidoi);
    }

    return DIENUM_CONTINUE;
}

struct input_device
{
    std::string name;
    LPDIRECTINPUTDEVICE8 di8_device;
    std::unordered_map<std::string, float> joystates;
    DIJOYSTATE2 joystate;
    bool is_keyboard = false; // 虚拟键盘设备：用 GetAsyncKeyState 而非 DirectInput
};
std::vector<input_device> input_devices;

int axis_device_index = -1; // the index in the array ^ of the device thats being used for steering
int last_axis_device_index = -1;

std::string steering_axis_name = "None";
std::string last_steering_axis_name = "None";

int steer_button_device_index = -1;
int last_steer_button_device_index = -1;

std::string steer_left_button_name = "None";
std::string last_steer_left_button_name = "None";
std::string steer_right_button_name = "None";
std::string last_steer_right_button_name = "None";

// 切换手动/自动控制(Lock the steering position)的按键
std::string toggle_lock_button_name = "None";
std::string last_toggle_lock_button_name = "None";
bool toggle_lock_key_was_down = false; // 边沿触发：防止按住键时反复切换

int current_controlled_trailer = 0; // the trailer you are currently controlling
int last_current_controlled_trailer = 0;

bool use_axis_steering = false;
bool last_use_axis_steering = false;
bool use_button_steering = false;
bool last_use_button_steering = false;

// 键盘虚拟键码 -> 友好名称（用于菜单显示）
static std::string vk_name(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
        case VK_SPACE:    return "Space";
        case VK_SHIFT:    return "Shift";
        case VK_LSHIFT:   return "L-Shift";
        case VK_RSHIFT:   return "R-Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_LCONTROL: return "L-Ctrl";
        case VK_RCONTROL: return "R-Ctrl";
        case VK_MENU:     return "Alt";
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_RETURN:   return "Enter";
        case VK_TAB:      return "Tab";
        case VK_BACK:     return "Backspace";
        case VK_ESCAPE:   return "Esc";
        default:          return "VK" + std::to_string(vk);
    }
}

// 键名字符串 -> 显示名（键盘用 "K<vk>" 前缀存储，手柄用纯数字索引）
static std::string key_display_name(const std::string& key)
{
    if (key.size() > 1 && key[0] == 'K' && isdigit((unsigned char)key[1]))
        return vk_name(std::atoi(key.c_str() + 1));
    return key;
}

std::string get_next_button_input(int device_index)
{
    input_device& device = input_devices[device_index];

    if (device.is_keyboard)
    {
        // 扫描常用虚拟键码（排除鼠标键），返回 "K<vk>" 形式
        for (int vk = 0x08; vk <= 0xFE; ++vk)
        {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (GetAsyncKeyState(vk) & 0x8000)
                return "K" + std::to_string(vk);
        }
        return "";
    }

    for (int i = 0; i < 128; ++i)
        if (device.joystate.rgbButtons[i] & 0x80)
            return std::to_string(i);

    return "";
}

std::string get_button_name_xbox(std::string index)
{
    int i = -1;
    try {
        i = std::stoi(index);
    }
    catch (...) {
        return index;
    }

    switch (i)
    {
        case 0: return "A";
        case 1: return "B";
        case 2: return "X";
        case 3: return "Y";
        case 4: return "Left Shoulder";
        case 5: return "Right Shoulder";
        case 6: return "Back";
        case 7: return "Start";
        case 8: return "Left Stick";
        case 9: return "Right Stick";
        default: return index + " (Unknown Key Name)";
    }
}

std::string get_button_name_ps(std::string index)
{
    int i = -1;
    try {
        i = std::stoi(index);
    }
    catch (...) {
        return index;
    }

    switch (i)
    {
        case 0: return "X";
        case 1: return "O";
        case 2: return "Sqare";
        case 3: return "Y";
        case 4: return "Left Shoulder";
        case 5: return "Right Shoulder";
        case 6: return "Select";
        case 7: return "Start";
        case 8: return "Left Stick";
        case 9: return "Right Stick";
        default: return index + " (Unknown Key Name)";
    }
}

std::string get_button_name_logi(std::string index)
{
    int i = -1;
    try {
        i = std::stoi(index);
    }
    catch (...) {
        return index;
    }

    switch (i)
    {
        case 0:  return "Cross";
        case 1:  return "Square";
        case 2:  return "Circle";
        case 3:  return "Triangle";
        case 4:  return "L1 / Paddle Left";
        case 5:  return "R1 / Paddle Right";
        case 6:  return "L2";
        case 7:  return "R2";
        case 8:  return "Share";
        case 9:  return "Options";
        case 10: return "R3";
        case 11: return "L3";
        case 19: return "+";
        case 20: return "-";
        case 21: return "Knob Right";
        case 22: return "Knob Left";
        case 23: return "Reset Car / Enter";
        case 24: return "PS Button (Home)";
        default: return index + " (Unknown Key Name)";
    }
}

std::string get_button_name(int device_index, std::string key)
{
    input_device& device = input_devices[device_index];

    // 键盘设备：直接显示友好键名
    if (device.is_keyboard)
        return key_display_name(key);

    std::string name = device.name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });

    if (name.find("xbox") != std::string::npos)
    {
        return get_button_name_xbox(key);
    }

    if (name.find("playstation") != std::string::npos)
    {
        return get_button_name_ps(key);
    }

    if (name.find("g29") != std::string::npos)
    {
        return get_button_name_logi(key);
    }

    return key + (key != "None" ? " (Unknown Key Name)" : "");
}

void render_keybinds()
{
    // Initalize the devices list
    std::vector<const char*> device_items; // No static here, rebuild every frame
    device_items.push_back("None");
    for (input_device& device : input_devices)
        device_items.push_back(device.name.c_str());

    // Device selections from saved values
    int selected_device_axis = (axis_device_index == -1) ? 0 : axis_device_index + 1;
    int selected_device_button = (steer_button_device_index == -1) ? 0 : steer_button_device_index + 1;

    // ==========================
    // Steering Axis
    // ==========================
    ImGui::TextWrapped("Steering Axis:");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Checkbox("Use Axis Steering?", &use_axis_steering);

    if (use_axis_steering)
    {
        use_button_steering = false;

        // Device combo
        if (ImGui::BeginCombo("Steering Device", device_items[selected_device_axis]))
        {
            for (int n = 0; n < (int)device_items.size(); n++)
            {
                bool is_selected = (selected_device_axis == n);
                if (ImGui::Selectable(device_items[n], is_selected))
                    selected_device_axis = n;
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Select the input device to use for steering.");
        axis_device_index = selected_device_axis - 1;

        if (axis_device_index != -1)
        {
            std::vector<const char*> axis_items;
            axis_items.push_back("None");
            for (const auto& [name, value] : input_devices[axis_device_index].joystates)
                axis_items.push_back(name.c_str());

            ImGui::TextWrapped("Using the table below, select which key you want to use for steering");
            ImGui::TextWrapped("The steering value should be: center = 0, full left = -1, full right = 1");

            if (ImGui::BeginTable("JoystickAxes", 2, ImGuiTableFlags_Borders))
            {
                for (const auto& [name, value] : input_devices[axis_device_index].joystates)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s", name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%f", value);
                }
                ImGui::EndTable();
            }

            // Find saved steering axis index
            int selected_axis = 0;
            for (size_t i = 1; i < axis_items.size(); i++)
            {
                if (steering_axis_name == axis_items[i])
                {
                    selected_axis = (int)i;
                    break;
                }
            }

            if (ImGui::BeginCombo("Inputs", axis_items[selected_axis]))
            {
                for (int n = 0; n < (int)axis_items.size(); n++)
                {
                    bool is_selected = (selected_axis == n);
                    if (ImGui::Selectable(axis_items[n], is_selected))
                        selected_axis = n;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            steering_axis_name = axis_items[selected_axis];

            if (steering_axis_name != "None")
            {
                ImGui::SliderFloat("Steering Preview",
                    &input_devices[axis_device_index].joystates[steering_axis_name],
                    -1.f, 1.f);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The steering value should be: center = 0, full left = -1, full right = 1");
            }
        }
    }

    // ==========================
    // Button Steering
    // ==========================
    ImGui::Spacing(); ImGui::Spacing();
    ImGui::TextWrapped("Button Steering");
    ImGui::Separator();
    ImGui::Checkbox("Use Button Steering?", &use_button_steering);

    if (use_button_steering)
    {
        use_axis_steering = false;

        if (ImGui::BeginCombo("Steering Device", device_items[selected_device_button]))
        {
            for (int n = 0; n < (int)device_items.size(); n++)
            {
                bool is_selected = (selected_device_button == n);
                if (ImGui::Selectable(device_items[n], is_selected))
                    selected_device_button = n;
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Select the input device to use for steering.");
        steer_button_device_index = selected_device_button - 1;

        if (steer_button_device_index != -1)
        {
            static bool want_left_button_input = false;
            static bool want_right_button_input = false;
            static bool want_toggle_lock_input = false;

            std::string steer_left_text = steer_left_button_name;
            std::string steer_right_text = steer_right_button_name;
            std::string toggle_lock_text = toggle_lock_button_name;

            if (want_left_button_input)
            {
                want_right_button_input = false;
                want_toggle_lock_input = false;
                steer_left_text = "Press a key...";
                std::string new_button = get_next_button_input(steer_button_device_index);
                if (!new_button.empty())
                {
                    steer_left_button_name = new_button;
                    if (steer_left_button_name == steer_right_button_name)
                        steer_right_button_name = "None";
                    if (steer_left_button_name == toggle_lock_button_name)
                        toggle_lock_button_name = "None";
                    want_left_button_input = false;
                }
            }

            if (want_right_button_input)
            {
                want_left_button_input = false;
                want_toggle_lock_input = false;
                steer_right_text = "Press a key...";
                std::string new_button = get_next_button_input(steer_button_device_index);
                if (!new_button.empty())
                {
                    steer_right_button_name = new_button;
                    if (steer_right_button_name == steer_left_button_name)
                        steer_left_button_name = "None";
                    if (steer_right_button_name == toggle_lock_button_name)
                        toggle_lock_button_name = "None";
                    want_right_button_input = false;
                }
            }

            if (want_toggle_lock_input)
            {
                want_left_button_input = false;
                want_right_button_input = false;
                toggle_lock_text = "Press a key...";
                std::string new_button = get_next_button_input(steer_button_device_index);
                if (!new_button.empty())
                {
                    toggle_lock_button_name = new_button;
                    if (toggle_lock_button_name == steer_left_button_name)
                        steer_left_button_name = "None";
                    if (toggle_lock_button_name == steer_right_button_name)
                        steer_right_button_name = "None";
                    want_toggle_lock_input = false;
                }
            }

            if (ImGui::Button(get_button_name(steer_button_device_index, steer_left_text).c_str()))
                want_left_button_input = !want_left_button_input;

            ImGui::SameLine(); ImGui::TextWrapped("Steer Left");

            if (ImGui::Button((get_button_name(steer_button_device_index, steer_right_text) + "##1").c_str()))
                want_right_button_input = !want_right_button_input;

            ImGui::SameLine(); ImGui::TextWrapped("Steer Right");

            ImGui::Spacing();

            if (ImGui::Button((get_button_name(steer_button_device_index, toggle_lock_text) + "##togglelock").c_str()))
                want_toggle_lock_input = !want_toggle_lock_input;

            ImGui::SameLine(); ImGui::TextWrapped("Toggle Manual/Auto (Lock steering)");
        }
    }

    ImGui::EndTabItem();

    // check if the settings changed
    if (   axis_device_index != last_axis_device_index
        || steering_axis_name != last_steering_axis_name
        || steer_button_device_index != last_steer_button_device_index
        || steer_left_button_name != last_steer_left_button_name
        || steer_right_button_name != last_steer_right_button_name
        || toggle_lock_button_name != last_toggle_lock_button_name
        || current_controlled_trailer != last_current_controlled_trailer
        || use_axis_steering != last_use_axis_steering
        || use_button_steering != last_use_button_steering
    )
    {
        json settings = {
            {"version", 1},
            {"axis_device_index", axis_device_index},
            {"steering_axis_name", steering_axis_name},
            {"steer_button_device_index", steer_button_device_index},
            {"steer_left_button_name", steer_left_button_name},
            {"steer_right_button_name", steer_right_button_name},
            {"toggle_lock_button_name", toggle_lock_button_name},
            {"current_controlled_trailer", current_controlled_trailer},
            {"use_axis_steering", use_axis_steering},
            {"use_button_steering", use_button_steering}
        };

        std::ofstream file("plugins/ETS2-TrailerUtil.json");
        file << settings.dump(4);
        file.close();

        last_axis_device_index = axis_device_index;
        last_steering_axis_name = steering_axis_name;
        last_steer_button_device_index = steer_button_device_index;
        last_steer_left_button_name = steer_left_button_name;
        last_steer_right_button_name = steer_right_button_name;
        last_toggle_lock_button_name = toggle_lock_button_name;
        last_current_controlled_trailer = current_controlled_trailer;
        last_use_axis_steering = use_axis_steering;
        last_use_button_steering = use_button_steering;

        scs_log(0, "Settings saved");
    }
}

void render_trailers()

{
    ImGui::TextWrapped("Vehicle Trailers:");
    ImGui::Separator();
    ImGui::Spacing();

    uint8_t* game_ctrl = *(uint8_t**)game_ctrl_ptr;

    if (game_ctrl)
    {
        uint8_t* game_actor = *(uint8_t**)(game_ctrl + game_actor_offset);
        if (game_actor)
        {
            prism::game_trailer_actor_u* trailer_actor = *(prism::game_trailer_actor_u**)(game_actor + trailer_actor_offset_render);

            if (trailer_actor)
            {
                ImGui::Spacing();

                int trailerIndex = 0;
                while (is_valid_trailer(trailer_actor) && trailerIndex < MAX_TRAILERS)
                {
                    bool steerable = isTrailerSteerable(trailer_actor);

                    std::stringstream ss;
                    ss << "Trailer (" << trailerIndex << ")" << (!steerable ? " | Not Steerable" : "");


                    if (ImGui::CollapsingHeader(ss.str().c_str()))
                    {
                        ImGui::BeginDisabled(!steerable);

                        bool locked = isTrailerLocked(trailer_actor);
                        bool tempLocked = isTrailerTempLocked(trailer_actor);

                        ImGui::TextWrapped("Steering: ");

                        if (ImGui::Checkbox(("##Locked" + std::to_string(trailerIndex)).c_str(), &locked)) {
                            if (locked)
                            {
                                scs_log(0, "Trailer '%p' was added to the locked list", trailer_actor);
                                locked_trailers.push_back(trailer_actor);
                            }
                            else
                            {
                                scs_log(0, "Trailer '%p' was removed from the locked list", trailer_actor);
                                locked_trailers.erase(
                                    std::remove(locked_trailers.begin(), locked_trailers.end(), trailer_actor),
                                    locked_trailers.end()
                                );
                            }
                        }

                        if (ImGui::IsItemHovered())
                        {
                            if (locked)
                                ImGui::SetTooltip("Unlock the steering position");
                            else
                                ImGui::SetTooltip("Lock the steering position");
                        }

                        ImGui::SameLine();
                        ImGui::PushItemWidth(-1);

                        ImGui::SliderFloat(("##Steering" + std::to_string(trailerIndex)).c_str(), &f_steering(trailer_actor), -1.f, 1.f);

                        ImGui::PopItemWidth();
                        if (ImGui::IsItemActive())
                        {
                            if (!locked && !tempLocked)
                            {
                                scs_log(0, "Trailer '%p' was added to the temp locked list", trailer_actor);
                                temp_locked_trailers.push_back(trailer_actor);
                            }
                        }
                        else
                        {
                            if (tempLocked)
                            {
                                scs_log(0, "Trailer '%p' was removed from the temp locked list", trailer_actor);
                                temp_locked_trailers.erase(
                                    std::remove(temp_locked_trailers.begin(), temp_locked_trailers.end(), trailer_actor),
                                    temp_locked_trailers.end()
                                );
                            }
                        }

                        ImGui::EndDisabled();

                        ImGui::Spacing();

                        prism::physics_joint_physx_t* hook_joint = f_hookjoint(trailer_actor);
                        // 校验 joint 及其内部 physx_joint 指针可读，避免野指针虚调用导致崩溃
                        if (is_valid_trailer(hook_joint) && is_valid_trailer(hook_joint->physx_joint))
                        {
                            std::string joint = "unknown";
                            std::string btn_text = "Unsupported";
                            switch (hook_joint->physx_joint->GetMotion(prism::PxD6Axis::Enum::eTWIST))
                            {
                            case prism::PxD6Motion::Enum::eFREE:
                                joint = "free";
                                btn_text = "Lock";
                                break;
                            case prism::PxD6Motion::Enum::eLIMITED:
                                joint = "limited";
                                break;
                            case prism::PxD6Motion::Enum::eLOCKED:
                                joint = "locked";
                                btn_text = "Unlock";
                                break;
                            }

                            ImGui::Text("Joint Type: %s", joint.c_str());

                            if (ImGui::Button((btn_text + "##" + std::to_string(trailerIndex)).c_str()))
                            {
                                if (btn_text == "Lock")
                                {
                                    hook_joint->physx_joint->SetMotion(prism::PxD6Axis::Enum::eTWIST, prism::PxD6Motion::Enum::eLOCKED);
                                }
                                else
                                {
                                    hook_joint->physx_joint->SetMotion(prism::PxD6Axis::Enum::eTWIST, prism::PxD6Motion::Enum::eFREE);
                                }
                            }
                        }


                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::Spacing();
                    }



                    trailer_actor = f_slave(trailer_actor);
                    trailerIndex++;
                }
            }
            else {
                ImGui::TextWrapped("You do not have a active trailer...");
                locked_trailers.clear();
            }
        }
        else return render_fatal("game_actor is missing");
    }
    else return render_fatal("game_ctrl is missing");



    ImGui::EndTabItem();
}

void render_about()
{
    ImGui::TextWrapped("About:");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("Version: 1.1.0");
    ImGui::TextWrapped("Matched profile: %s", active_profile ? active_profile->label : "none");
    ImGui::TextWrapped("Branch: SugarSu");
    ImGui::TextWrapped("Original Author: Baldy09");

    ImGui::Spacing(); ImGui::Spacing();

    ImGui::TextWrapped("Description:");
    ImGui::TextWrapped("ETS2-TrailerUtil is a plugin designed to enhance trailer control "
        "and steering customization within Euro Truck Simulator 2. It "
        "provides both axis and button steering options, real-time trailer "
        "steering adjustments.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Credits"))
    {
        ImGui::TextWrapped("Programming: Baldy09");
        ImGui::TextWrapped("Testing: FemmyRybo, VaselineeOnToast, OnyxSabertooth, superstar1607");
    }

    if (ImGui::CollapsingHeader("License"))
    {
        ImGui::TextWrapped("This plugin is distributed for personal and non-commercial use. "
            "No warranties are provided. Use at your own risk. "
            "Reuploads of the compiled DLL file are strictly prohibited. "
            "Any unauthorized distribution will result in the file being taken down.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Get Plugin Support"))
    {
        ShellExecuteA(NULL, "open", "https://github.com/suagrsu/TrailerUtils", NULL, NULL, SW_SHOWNORMAL);
    }

    ImGui::EndTabItem();    

    if (ImGui::Button("Support Original Author"))
    {
        ShellExecuteA(NULL, "open", "https://www.paypal.com/paypalme/BaldyMods", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
}



bool styled = false;
void dllmain::render_tick()
{
    if (!styled)
    {
        SetupImGuiStyle();
        styled = true;
    }

    ImGui::Begin("ETS2 Trailer Utils");

    if (running_truckersmp && !shown_truckersmp_warning)
    {
        ImGui::TextWrapped("TruckersMP Warning");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("You are currently running TruckersMP. This plugin is not supported on multiplayer servers (however it does work) and may result in a ban.");
        ImGui::TextWrapped("\nIf you do get banned whilst using this plugin, please try contact the Game Moderation Management and discuss your ban there");


        ImGui::TextWrapped("\n\nBy clicking 'I Understand', you acknowledge that you are using this plugin at your own risk.");
        if (ImGui::Button("I Understand"))
        {
            shown_truckersmp_warning = true;
        }
    }


    if (!is_fatal_error && (!running_truckersmp || shown_truckersmp_warning)) {

        if (ImGui::BeginTabBar("MyTabBar"))
        {
            if (ImGui::BeginTabItem("Trailers"))
            {
                render_trailers();
            }

            if (ImGui::BeginTabItem("Keybinds"))
            {
                render_keybinds();
            }

            if (ImGui::BeginTabItem("About"))
            {
                render_about();
            }

            ImGui::EndTabBar();
        }
    }
    else
        render_error(error);

    ImGui::End();
}



void dllmain::main_tick()
{
    // Update devices
    for (input_device& device : input_devices)
    {
        if (device.is_keyboard) continue; // 键盘用 GetAsyncKeyState 实时读取，无需 Poll

        DIJOYSTATE2 joystate = {};

        HRESULT hr = device.di8_device->Poll();
        if (FAILED(hr))
        {
            hr = device.di8_device->Acquire();
            while (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                hr = device.di8_device->Acquire();
            }

            if (FAILED(hr))
            {
                ImGui::TextWrapped("Failed to acquire device: 0x%08X", hr);
                continue;
            }
        }

        hr = device.di8_device->GetDeviceState(sizeof(DIJOYSTATE2), &joystate);
        if (FAILED(hr))
        {
            ImGui::TextWrapped("Failed to get device state: 0x%08X", hr);
            continue;
        }

        device.joystates = {
            { "lX",  ((float)joystate.lX - 32767.5f) / 32767.5f },
            { "lY",  ((float)joystate.lY - 32767.5f) / 32767.5f },
            { "lZ",  ((float)joystate.lZ - 32767.5f) / 32767.5f },
            { "lRx",  ((float)joystate.lRx - 32767.5f) / 32767.5f },
            { "lRy",  ((float)joystate.lRy - 32767.5f) / 32767.5f },
            { "lRz",  ((float)joystate.lRz - 32767.5f) / 32767.5f },
        };

        device.joystate = joystate;

        /*
            // Print joystick axes values every frame
            ImGui::TextWrapped("lX: %ld", joystate.lX);
            ImGui::TextWrapped("lY: %ld", joystate.lY);
            ImGui::TextWrapped("lZ: %ld", joystate.lZ);
            ImGui::TextWrapped("lRx: %ld", joystate.lRx);
            ImGui::TextWrapped("lRy: %ld", joystate.lRy);
            ImGui::TextWrapped("lRz: %ld", joystate.lRz);

            ImGui::TextWrapped("Slider 0: %ld", joystate.rglSlider[0]);
            ImGui::TextWrapped("Slider 1: %ld", joystate.rglSlider[1]);

            for (int i = 0; i < 4; ++i)
                ImGui::TextWrapped("POV %d: %lu", i, joystate.rgdwPOV[i]);

            for (int i = 0; i < 128; ++i)
                if (joystate.rgbButtons[i] & 0x80)
                    ImGui::TextWrapped("Button %d pressed", i);
        */
    }


    // Handle keybinds
    if (use_button_steering || use_axis_steering)
    {
        uint8_t* game_ctrl = *(uint8_t**)game_ctrl_ptr;

        if (game_ctrl)
        {
            uint8_t* game_actor = *(uint8_t**)(game_ctrl + game_actor_offset);

            if (game_actor)
            {
                prism::game_trailer_actor_u* trailer_actor = *(prism::game_trailer_actor_u**)(game_actor + trailer_actor_offset);
                if (trailer_actor)
                {
                    int trailerIndex = 0;
                    while (is_valid_trailer(trailer_actor) && trailerIndex < MAX_TRAILERS)
                    {
                        if (trailerIndex == current_controlled_trailer)
                        {
                            if (use_button_steering && steer_button_device_index != -1)
                            {
                                std::string pressed_key = get_next_button_input(steer_button_device_index);

                                // Toggle Manual/Auto（边沿触发：只在按下的那一帧切换一次）
                                bool toggle_down = (toggle_lock_button_name != "None"
                                                    && pressed_key == toggle_lock_button_name);
                                if (toggle_down && !toggle_lock_key_was_down)
                                {
                                    if (isTrailerLocked(trailer_actor))
                                    {
                                        locked_trailers.erase(
                                            std::remove(locked_trailers.begin(), locked_trailers.end(), trailer_actor),
                                            locked_trailers.end());
                                        scs_log(0, "Trailer '%p' steering set to AUTO (unlocked)", trailer_actor);
                                    }
                                    else
                                    {
                                        locked_trailers.push_back(trailer_actor);
                                        scs_log(0, "Trailer '%p' steering set to MANUAL (locked)", trailer_actor);
                                    }
                                }
                                toggle_lock_key_was_down = toggle_down;

                                if (pressed_key != "")
                                {
                                    if (pressed_key == steer_left_button_name)
                                    {
                                        if (!isTrailerLocked(trailer_actor))
                                        {
                                            scs_log(0, "Trailer '%p' was added to the locked list, steer left was pressed", trailer_actor);
                                            locked_trailers.push_back(trailer_actor);
                                        }

                                        if (f_steering(trailer_actor) - 0.01f < -1.f)
                                            f_steering(trailer_actor) = -1.f;
                                        else
                                            f_steering(trailer_actor) -= 0.01f;
                                    }

                                    if (pressed_key == steer_right_button_name)
                                    {
                                        if (!isTrailerLocked(trailer_actor))
                                        {
                                            scs_log(0, "Trailer '%p' was added to the locked list, steer right was pressed", trailer_actor);
                                            locked_trailers.push_back(trailer_actor);
                                        }

                                        if (f_steering(trailer_actor) + 0.01f > 1.f)
                                            f_steering(trailer_actor) = 1.f;
                                        else
                                            f_steering(trailer_actor) += 0.01f;
                                    }
                                }
                            }

                            if (use_axis_steering && axis_device_index != -1 && steering_axis_name != "None")
                            {
                                if (!isTrailerLocked(trailer_actor))
                                {
                                    scs_log(0, "Trailer '%p' was added to the locked list, axis steering in use", trailer_actor);
                                    locked_trailers.push_back(trailer_actor);
                                }

                                input_device device = input_devices[axis_device_index];

                                float steering_position = device.joystates[steering_axis_name];

                                if (steering_position <= 1.f && steering_position >= -1.f)
                                {
                                    f_steering(trailer_actor) = steering_position;
                                }
                                else
                                {
                                    //dxManager::ShowNotification("Axis steering input is invalid, it has been reset.");
                                    steering_axis_name = "None";
                                    use_axis_steering = false;
                                }
                            }

                            break;
                        }

                        trailer_actor = f_slave(trailer_actor);
                        trailerIndex++;
                    }
                }
            }
        }
    }
}




prism::g_advance_steering_t g_advance_steering_original;
void g_advance_steering_hook(prism::game_trailer_actor_u* vehicle_actor)
{
    if (isTrailerLocked(vehicle_actor) || isTrailerTempLocked(vehicle_actor))
    {
        prism::g_set_vehicle_steering(f_steerdata(vehicle_actor), f_steering(vehicle_actor));
        return;
    }

    g_advance_steering_original(vehicle_actor);
}



std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

// 尝试用单个 profile 解析游戏内存布局。全部特征码命中才返回 true。
// 成功时会填充 game_ctrl_ptr / game_actor_offset / prism::g_set_vehicle_steering
// 以及挂车相关偏移，并设置 active_profile。
static bool try_resolve_profile(const gamecfg::GameProfile& p, uintptr_t& out_advance_steering)
{
    // 1) game_ctrl 指针
    uintptr_t instr = bmem::patternScan(p.sig_game_ctrl_ptr);
    if (!bmem::isAddressValid(instr)) {
        scs_log(0, "  [%s] miss: game_ctrl pointer signature not found", p.label);
        return false;
    }
    uintptr_t ctrl_ptr = bmem::relativeToAbsolute(instr, p.game_ctrl_ptr_field, p.game_ctrl_ptr_instr_len);
    if (!bmem::isAddressValid(ctrl_ptr)) {
        scs_log(0, "  [%s] miss: game_ctrl pointer resolved out of module", p.label);
        return false;
    }

    // 2) game_ctrl->game_actor 偏移（原代码缺少有效性检查，这里补上）
    uintptr_t actor_off_instr = bmem::patternScan(p.sig_game_actor_offset);
    if (!bmem::isAddressValid(actor_off_instr)) {
        scs_log(0, "  [%s] miss: game_actor offset signature not found", p.label);
        return false;
    }
    uint32_t actor_offset = *reinterpret_cast<uint32_t*>(actor_off_instr + p.game_actor_offset_field);

    // 3) g_set_vehicle_steering
    uintptr_t set_steering = bmem::patternScan(p.sig_set_vehicle_steering);
    if (!bmem::isAddressValid(set_steering)) {
        scs_log(0, "  [%s] miss: g_set_vehicle_steering signature not found", p.label);
        return false;
    }

    // 4) g_advance_steering（核心 Hook 点）
    uintptr_t advance_steering = bmem::patternScan(p.sig_advance_steering);
    if (!bmem::isAddressValid(advance_steering)) {
        scs_log(0, "  [%s] miss: g_advance_steering signature not found", p.label);
        return false;
    }

    // 全部命中：提交解析结果
    game_ctrl_ptr = ctrl_ptr;
    game_actor_offset = actor_offset;
    trailer_actor_offset = p.trailer_actor_offset;
    trailer_actor_offset_render = p.trailer_actor_offset_render;
    off_steering = p.off_trailer_steering;
    off_steering_data = p.off_trailer_steering_data;
    off_hook_joint = p.off_trailer_hook_joint;
    off_slave = p.off_trailer_slave;
    prism::g_set_vehicle_steering = (prism::g_set_vehicle_steering_t)set_steering;
    out_advance_steering = advance_steering;
    active_profile = &p;

    scs_log(0, "Matched game profile: %s", p.label);
    scs_log(0, "  game_ctrl pointer:            %p", game_ctrl_ptr);
    scs_log(0, "  game_ctrl->game_actor offset: %d", game_actor_offset);
    scs_log(0, "  g_set_vehicle_steering:       %p", set_steering);
    scs_log(0, "  g_advance_steering:           %p", advance_steering);
    return true;
}

// 按 game_id 过滤并逐一尝试所有 profile，命中第一个即为当前游戏版本布局。
static bool resolve_game_layout(const char* game_id, uintptr_t& out_advance_steering)
{
    int tried = 0;
    for (const auto& p : gamecfg::profiles()) {
        if (!gamecfg::matches_game(p, game_id)) continue;
        ++tried;
        scs_log(0, "Trying game profile: %s (game_id=%s)",
            p.label, game_id ? game_id : "unknown");
        if (try_resolve_profile(p, out_advance_steering)) return true;
    }
    scs_log(2, "No compatible game profile matched (tried %d). "
        "The game version may be unsupported; add a new profile in game_profiles.h.", tried);
    return false;
}

#pragma comment( linker, "/export:scs_telemetry_init=scs_telemetry_init" )
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
	scs_logging::init(params, "ETS2-TrailerUtil");

	scs_log(0, "ETS2 Trailer Utils | By: Baldy09 - Insanux");
	scs_log(0, "Plugin Loading...");

    HMODULE TruckersMP = GetModuleHandleA("core_ets2mp.dll");
	if (TruckersMP)
	{
		scs_log(1, "TruckersMP detected! This plugin is not supported on multiplayer servers and may result in a ban.");
		running_truckersmp = true;
	}



    //MH_Initialize();

    // Hook DX11
    scs_log(0, "Hooking rendering API...");

    kiero::RenderType::Enum render_type = kiero::RenderType::Auto;

    LaunchArgsManager* arguments = new LaunchArgsManager();

    if (arguments->has_arg("-rdevice")) {
        std::string device = to_lower(arguments->get_arg("-rdevice"));

        if (device == "dx11")
        {
            scs_log(0, "Render device override: D3D11");
            render_type = kiero::RenderType::D3D11;
        }
        else if (device == "dx12")
        {
            scs_log(0, "Render device override: D3D12");
            render_type = kiero::RenderType::D3D12;
        }
        else if (device == "gl")
        {
            scs_log(0, "Render device override: OpenGL");
            render_type = kiero::RenderType::OpenGL;
        }
        else if (device == "vk")
        {
            scs_log(0, "Render device override: Vulkan");
            render_type = kiero::RenderType::Vulkan;
        }
        else
        {
            scs_log(0, "Unknown render device override: '%s'", device.c_str());
            render_type = kiero::RenderType::Vulkan;
        }
    }

    renderer = new RenderManager(render_type);

    switch (renderer->GetRenderDevice())
    {
    case kiero::RenderType::D3D11:
        scs_log(0, "Rendering API: D3D11");
        break;
    case kiero::RenderType::D3D12:
        scs_log(0, "Rendering API: D3D12");
        break;
    case kiero::RenderType::OpenGL:
        scs_log(0, "Rendering API: OpenGL");
        break;
    case kiero::RenderType::Vulkan:
        scs_log(0, "Rendering API: Vulkan");
        break;
    }

    scs_log(0, "Hooked!");

    // 解析游戏内存布局：按 game_id 自动匹配已知版本 profile（见 game_profiles.h）。
    // 以后适配新游戏版本，只需在 game_profiles.h 增加一条 profile，无需改动此处。
    const char* game_id = nullptr;
    if (params) {
        const auto* vparams = reinterpret_cast<const scs_telemetry_init_params_v101_t*>(params);
        game_id = vparams->common.game_id;
        scs_log(0, "Game: %s (api version %u.%u)",
            game_id ? game_id : "unknown",
            SCS_GET_MAJOR_VERSION(version), SCS_GET_MINOR_VERSION(version));
    }

    uintptr_t advance_steering_address = 0;
    if (!resolve_game_layout(game_id, advance_steering_address))
    {
        is_fatal_error = true;
        error = "No compatible game version profile matched. Game version may be unsupported.";
        return SCS_RESULT_ok;
    }

    bmem::MH_Success(MH_CreateHook((LPVOID)advance_steering_address, g_advance_steering_hook, (LPVOID*)&g_advance_steering_original));
    bmem::MH_Success(MH_EnableHook((LPVOID)advance_steering_address));

    dinput_manager::InitDirectInput();
    for (auto& device : dinput_manager::GetInputDevices())
    {
        // 正确地将 UTF-16 设备名转为 UTF-8（逐字符拷贝会截断宽字符并触发 C4244）
        std::wstring wstr(device.info.tszInstanceName);
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string str(len ? (size_t)len : 0, '\0');
        if (len) {
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                &str[0], len, nullptr, nullptr);
        }

        input_device inputdevice;
        inputdevice.name = str;
        inputdevice.di8_device = device.device;

        scs_log(0, "Added input device '%s'", str.c_str());

        input_devices.emplace_back(inputdevice);
    }

    // 加入一个虚拟键盘设备（用 GetAsyncKeyState 检测，不依赖 DirectInput 枚举）
    {
        input_device keyboard;
        keyboard.name = "Keyboard";
        keyboard.di8_device = nullptr;
        keyboard.is_keyboard = true;
        input_devices.emplace_back(keyboard);
        scs_log(0, "Added input device 'Keyboard'");
    }

    scs_log(0, "Reading settings...");

    try {
        std::ifstream file("plugins/ETS2-TrailerUtil.json");

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string file_content = ss.str();

        json settings = json::parse(file_content);

        axis_device_index               = settings["axis_device_index"];
        steering_axis_name              = settings["steering_axis_name"];
        steer_button_device_index       = settings["steer_button_device_index"];
        steer_left_button_name          = settings["steer_left_button_name"];
        steer_right_button_name         = settings["steer_right_button_name"];
        toggle_lock_button_name         = settings.value("toggle_lock_button_name", "None"); // 向后兼容旧配置
        current_controlled_trailer      = settings["current_controlled_trailer"];
        use_axis_steering               = settings["use_axis_steering"];
        use_button_steering             = settings["use_button_steering"];

        last_axis_device_index          = axis_device_index;
        last_steering_axis_name         = steering_axis_name;
        last_steer_button_device_index  = steer_button_device_index;
        last_steer_left_button_name     = steer_left_button_name;
        last_steer_right_button_name    = steer_right_button_name;
        last_toggle_lock_button_name    = toggle_lock_button_name;
        last_current_controlled_trailer = current_controlled_trailer;
        last_use_axis_steering          = use_axis_steering;
        last_use_button_steering        = use_button_steering;

        // Verify some values:
        if (axis_device_index < -1 || axis_device_index >= (int)input_devices.size()) {
			scs_log(1, "Warning: 'axis_device_index' is out of range, resetting to -1");
            axis_device_index = -1;
        }

		if (steer_button_device_index < -1 || steer_button_device_index >= (int)input_devices.size()) {
			scs_log(1, "Warning: 'steer_button_device_index' is out of range, resetting to -1");
			steer_button_device_index = -1;
		}

		file.close();
    }
    catch (...)
    {
        scs_log(2, "Failed to read settings!");
    }
    scs_log(0, "Done");

    scs_log(0, "Plugin Loaded");

    return SCS_RESULT_ok;
}

#pragma comment( linker, "/export:scs_telemetry_shutdown=scs_telemetry_shutdown" )
SCSAPI_VOID scs_telemetry_shutdown()
{
    dinput_manager::ShutdownDirectInput();

    delete renderer;

    scs_log(0, "Plugin Unloaded");
}


BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    global::hModule = hinstDLL;

    return TRUE;
}