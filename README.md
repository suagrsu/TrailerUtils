<h1 align="center">
  SCS-TrailerUtils
</h1>

<p align="center">
  <a href="#中文">中文</a> · <a href="#english">English</a>
</p>

<p align="center">
  <img src="https://i.imgur.com/aWMkaI7.png" alt="Plugin menu screenshot / 插件菜单截图">
</p>

---

<a id="中文"></a>

## 中文

一款用于 ETS2 / ATS 的挂车转向控制插件——可通过菜单或外部输入设备自定义控制挂车转向。

### 简介

ETS2-TrailerUtil 是《欧洲卡车模拟 2》(ETS2) 和《美国卡车模拟》(ATS) 的插件，用于增强挂车控制与转向自定义。它让你可以**实时控制每一节挂车的转向**——不再依赖游戏默认的挂车跟随行为，而是通过屏幕菜单、键盘、手柄按键或方向盘轴来操作。

**主要功能：**
- **逐挂车转向控制**：支持多节挂车，可为每节挂车单独设置转向
- **手动 / 自动切换**：用快捷键把挂车锁定在当前转向角度（Lock the steering position），或释放回游戏自动控制
- **多种输入方式**：按键转向（键盘 / 手柄）与轴转向（方向盘）
- **设置持久化**：配置自动保存到游戏目录的 `ETS2-TrailerUtil.json`

### 支持的版本

| 游戏版本 | 状态 |
|---|---|
| ETS2 / ATS 1.62 | ✅ 支持（已实测） |

插件启动时会扫描游戏的 AOB 特征码，自动识别游戏版本并套用对应的内存偏移。适配原理、偏移表和后续版本适配方法见 [适配说明.md](适配说明.md)。

### 编译

**依赖：**
- **Visual Studio 2022**：安装"使用 C++ 的桌面开发"工作负载（MSVC + Windows SDK）
- **Vulkan SDK 1.4.x**：安装器会自动设置 `VULKAN_SDK` 环境变量。项目使用 `$(VULKAN_SDK)\Include` 和 `$(VULKAN_SDK)\Lib`，任意已安装版本均可
- **GLEW 2.3.0**（Windows 预编译版）：解压到 `C:\OpenGL_SDK\glew\`，使其下直接包含 `include\` 和 `lib\Release\x64\glew32s.lib`

**步骤：**
1. 用 Visual Studio 打开 `ETS2-TrailerUtil.sln`
2. 选择 **Release | x64**（或 Debug | x64），生成解决方案
3. 生成后事件会把 DLL 自动复制到：
   `D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\Plugins\`
   （游戏装在别处请修改项目属性中的"生成后事件"路径）
4. 启动游戏，打开插件菜单。游戏日志 `game.log.txt` 中会打印匹配到的 profile 和渲染 API，可用于确认适配生效：
   ```
   Rendering API: D3D11
   Matched game profile: ETS2/ATS 1.62
   ```

> 源文件使用 UTF-8 with BOM，项目已启用 `/utf-8`；新增含中文注释的源文件请保持 BOM，避免 MSVC 代码页报错。

### 使用方法

1. 在游戏中打开插件菜单，进入 **Keybinds** 页
2. 在 **Button Steering** 下启用它，选择输入设备（用键盘按键就选 **Keyboard**），然后绑定 **Steer Left**、**Steer Right**，以及可选的 **Toggle Manual/Auto (Lock steering)**
3. 在 **Trailers** 页选择当前控制的挂车；按切换键即可在"手动锁定"和"自动控制"之间切换

设置会自动保存到 `plugins\ETS2-TrailerUtil.json`。

> ⚠️ 本插件不支持 TruckersMP（联机服务器），使用可能导致封号，风险自负。

### 项目结构

```
ETS2-TrailerUtil/      插件源码
  game_profiles.h      版本配置层（特征码 + 内存偏移）
  dllmain.cpp          主逻辑、按键、菜单、字段运行时访问
  rendering.cpp        渲染 API 检测与 Hook
  prism.h / *.h        游戏结构体定义、输入/内存工具
适配说明.md             适配原理与后续版本适配指南
```

### 致谢与许可

原插件作者 [Baldy09](https://github.com/Baldywaldy09)。测试：FemmyRybo、VaselineeOnToast、OnyxSabertooth、superstar1607。本分支新增了 1.62 适配、键盘输入和手动/自动切换功能。

仅供个人非商业使用，不提供任何担保，风险自负。严禁重新上传编译后的 DLL。

---

<a id="english"></a>

## English

An ETS2 / ATS plugin that lets you custom-steer trailers via the in-game menu or external input devices.

### About

ETS2-TrailerUtil is a plugin for **Euro Truck Simulator 2** and **American Truck Simulator** that enhances trailer control and steering customization. It lets you **steer each trailer in real time** — via the on-screen menu, keyboard, gamepad buttons, or a steering-wheel axis — instead of relying on the game's default trailer behavior.

**Features:**
- **Per-trailer steering control**: support multiple trailers, each steered independently
- **Manual / Auto toggle**: lock a trailer at its current steering angle ("Lock the steering position") or release it back to the game's automatic steering via a hotkey
- **Multiple input methods**: button steering (keyboard / gamepad) and axis steering (steering wheel)
- **Persistent settings**: configuration is saved to `ETS2-TrailerUtil.json`

### Supported versions

| Game version | Status |
|---|---|
| ETS2 / ATS 1.62 | ✅ Supported (tested) |

At startup the plugin scans for known AOB signatures, detects the game version, and applies the matching memory offsets automatically. See [适配说明.md](适配说明.md) for the adaptation details, offset tables, and how to support future versions.

### Build

**Requirements:**
- **Visual Studio 2022** with the **Desktop development with C++** workload (MSVC + Windows SDK)
- **Vulkan SDK 1.4.x** — the installer sets the `VULKAN_SDK` environment variable. The project uses `$(VULKAN_SDK)\Include` and `$(VULKAN_SDK)\Lib`, so any installed version works
- **GLEW 2.3.0** (Windows binaries) extracted to `C:\OpenGL_SDK\glew\`, with `include\` and `lib\Release\x64\glew32s.lib` directly under it

**Steps:**
1. Open `ETS2-TrailerUtil.sln` in Visual Studio
2. Select **Release | x64** (or Debug | x64) and build
3. The post-build event copies the DLL to:
   `D:\Steam\steamapps\common\Euro Truck Simulator 2\bin\win_x64\Plugins\`
   (edit the Post-Build Event path in project properties if your game is installed elsewhere)
4. Launch the game and open the plugin menu. `game.log.txt` prints the matched profile and rendering API to confirm the adaptation:
   ```
   Rendering API: D3D11
   Matched game profile: ETS2/ATS 1.62
   ```

> Source files use UTF-8 with BOM and the project is compiled with `/utf-8`; keep new files with Chinese comments in UTF-8/BOM to avoid MSVC codepage errors.

### Usage

1. Open the plugin menu in-game and go to the **Keybinds** tab
2. Under **Button Steering**, enable it, select the input device (choose **Keyboard** for keyboard keys), then bind **Steer Left**, **Steer Right**, and optionally **Toggle Manual/Auto (Lock steering)**
3. In the **Trailers** tab, select the currently controlled trailer; press the toggle hotkey to switch it between manual (locked) and automatic steering

Settings are saved automatically to `plugins\ETS2-TrailerUtil.json`.

> ⚠️ This plugin is not supported on TruckersMP (multiplayer) and may result in a ban. Use at your own risk.

### Project structure

```
ETS2-TrailerUtil/      plugin source
  game_profiles.h      version config layer (signatures + offsets)
  dllmain.cpp          main logic, keybinds, menu, runtime field access
  rendering.cpp        rendering API detection & hook
  prism.h / *.h        game struct definitions, input/memory utilities
适配说明.md             adaptation notes & future-version guide
```

### Credits & license

Original plugin by [Baldy09](https://github.com/Baldywaldy09). Testing: FemmyRybo, VaselineeOnToast, OnyxSabertooth, superstar1607. This fork adds 1.62 support, keyboard input, and the manual/auto toggle.

For personal and non-commercial use only. No warranties are provided; use at your own risk. Reuploading the compiled DLL is strictly prohibited.
