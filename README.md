<<<<<<< HEAD
# WinEase

WinEase 是一个面向 Windows 11 的易用性增强工具集。项目采用 C++20、Qt 6.8 Widgets 和 CMake 构建，将系统能力封装在平台层，将每项用户功能拆分为可独立加载的 Qt 插件。

> 当前版本：`0.1.0`  
> 当前状态：P0、P1、P2 已完成；P3 正在推进，具体进度见 [`docs/ROADMAP.md`](docs/ROADMAP.md)。

## 特性概览

- 窗口增强：置顶、置底、透明度、快速分屏、虚拟桌面。
- 文件工具：快速预览、批量重命名、重复文件查找、文件解锁、格式转换、敏感文件粉碎、右键菜单扩展。
- 输入与剪贴板：剪贴板历史、隐私过滤、滚轮增强。
- 显示与媒体：屏幕取色、标尺、焦点高亮、放大镜、亮度/色温、麦克风静音、音量混合器、媒体控制面板。
- 系统与开发工具：网页搜索、Hosts 编辑、环境变量管理、端口占用查看、文本格式化、编码转换。
- 系统维护：定时任务、电源面板、摄像头/麦克风使用提醒、USB 设备管控、硬件监控悬浮窗。
- 固定深色主题：界面强制使用黑底白字，不随 Windows 浅色/深色模式切换。

功能以插件形式加载，插件输出到 `build/bin/plugins`，主程序启动时自动扫描该目录。

## 架构

```text
src/win32/  WinEaseWin32  Windows/WinRT 平台能力层（静态库）
src/sdk/    WinEaseSdk     插件契约与宿主服务接口（静态库）
src/app/    WinEase         主程序（Qt Widgets）
src/helper/ WinEaseHelper   提权助手（独立进程）
plugins/*   独立功能插件 DLL
tests/*     运行时自检程序
```

依赖方向为 `WinEaseWin32 -> WinEaseSdk -> WinEase -> plugins`。主程序保持普通权限；需要管理员权限的操作通过 `WinEaseHelper.exe` 按需触发 UAC。插件不得直接依赖主程序单例，应通过 `PluginServices` 获取配置、日志、快捷键、钩子、悬浮层和提权服务。

项目优先使用 Windows SDK 自带的 Win32/C++/WinRT 能力，避免引入不必要的第三方库：

- PDF、OCR、媒体会话、录屏等能力使用 Windows SDK API。
- 硬件温度监控使用 PawnIO 动态加载方案。
- QtMultimedia 为可选模块；未安装时，相关功能使用回退路径，项目仍可构建。

## 环境要求

- Windows 11 x64
- Visual Studio 2026 / MSVC v180 工具集
- CMake 3.28 或更高版本（项目参考环境为 CMake 4.4.2）
- Qt 6.8（项目参考环境为 Qt 6.8.4）
  - 必需：`Core`、`Gui`、`Widgets`、`Svg`、`Network`
  - 可选：`Multimedia`、`MultimediaWidgets`
- Windows SDK 10.0.26100.0 或兼容版本

项目当前按 MSVC 构建，非 MSVC 工具链会在 CMake 配置阶段被拒绝。更完整的依赖说明和 Qt 模块策略见 [`docs/ENV-SETUP.md`](docs/ENV-SETUP.md)。

## 构建

请在 Visual Studio 开发者 PowerShell 或已配置 MSVC 环境的终端中执行。下面示例假设 Qt 安装在 `D:\Qt`：

```powershell
cmake -S . -B build `
  -G "Visual Studio 18 2026" `
  -DCMAKE_PREFIX_PATH="D:\Qt"

cmake --build build --config RelWithDebInfo --parallel
```

也可以使用 `Release` 配置：

```powershell
cmake --build build --config Release --parallel
```

### 重要的构建约定

1. 推荐使用 `RelWithDebInfo` 或 `Release`，不要使用 `Debug`。本项目环境中的 Qt 为 release-only 安装，Debug 与 Qt 混用可能导致 STL/CRT ABI 不兼容和运行时内存损坏。
2. 构建产物统一位于 `build/bin/`，插件位于 `build/bin/plugins/`。
3. 运行主程序或自检程序时会锁定部分 EXE/DLL；重新构建前请先退出程序。
4. 如不需要构建自检程序，可在配置时关闭：

```powershell
cmake -S . -B build `
  -G "Visual Studio 18 2026" `
  -DCMAKE_PREFIX_PATH="D:\Qt" `
  -DWINEASE_BUILD_SMOKE_TESTS=OFF
```

## 运行

```powershell
.\build\bin\WinEase.exe
```

`WinEaseHelper.exe` 通常由主程序按需启动，不建议手动直接运行。

## 运行时自检

项目没有使用单元测试框架；测试目录中的目标是独立的运行时自检程序。退出码为 `0` 表示该自检通过：

```powershell
.\build\bin\win32_smoke.exe
.\build\bin\hook_smoke.exe
.\build\bin\overlay_smoke.exe
.\build\bin\theme_smoke.exe
.\build\bin\elevation_smoke.exe
.\build\bin\plugin_smoke.exe
.\build\bin\ui_smoke.exe
.\build\bin\feature_smoke.exe
.\build\bin\p3_spike.exe
```

可选参数示例：

```powershell
.\build\bin\theme_smoke.exe --with-os-light-mode
.\build\bin\elevation_smoke.exe --with-uac
.\build\bin\elevation_smoke.exe --quit-helper
```

`feature_smoke.exe` 会真实调用部分系统能力，可能短暂修改鼠标、前台窗口、主题、壁纸、剪贴板、音量、麦克风、亮度以及文件状态。运行前请阅读 [`CODEBUDDY.md`](CODEBUDDY.md) 中的自检注意事项，并保存未完成的工作。

## 插件开发

新增插件时建议从 [`plugins/template`](plugins/template) 复制目录，然后：

1. 修改插件类名、`id()`、名称、描述和图标元信息。
2. 继承 `WinEase::IFeaturePlugin`，添加 Qt 插件元数据。
3. 通过 `PluginServices` 使用宿主能力，不直接访问主程序实现。
4. 在 [`plugins/CMakeLists.txt`](plugins/CMakeLists.txt) 中登记 `add_subdirectory(...)`。
5. 使用对应的 `we_<插件目录名>` target 构建。

例如：

```powershell
cmake --build build `
  --config RelWithDebInfo `
  --target we_dev_text_format
```

插件契约和生命周期定义在 [`src/sdk/IFeaturePlugin.h`](src/sdk/IFeaturePlugin.h)，插件构建辅助逻辑见 [`cmake/WinEasePlugin.cmake`](cmake/WinEasePlugin.cmake)。

## 配置与数据

- 配置文件：`%APPDATA%\WinEase\config.ini`
- 插件快捷键格式：`pluginId::action`
- 插件运行状态由主程序统一保存和恢复。
- 主程序普通运行；涉及系统级写入、进程操作或电源操作的功能按设计经提权助手执行。

## 文档

- [`docs/ENV-SETUP.md`](docs/ENV-SETUP.md)：开发环境、Qt 模块、WinRT 和 PawnIO 说明。
- [`docs/ROADMAP.md`](docs/ROADMAP.md)：总体路线图、进度、架构决策和验收标准。
- [`docs/ROADMAP-P0-FIN.md`](docs/ROADMAP-P0-FIN.md)：P0 基础设施。
- [`docs/ROADMAP-P1-FIN.md`](docs/ROADMAP-P1-FIN.md)：P1 低风险功能。
- [`docs/ROADMAP-P2-FIN.md`](docs/ROADMAP-P2-FIN.md)：P2 中等复杂度功能。
- [`docs/ROADMAP-P3.md`](docs/ROADMAP-P3.md)：P3 高风险功能和预研结论。
- [`docs/traps.md`](docs/traps.md)：工程踩坑和已验证的规避方案。
- [`CODEBUDDY.md`](CODEBUDDY.md)：项目开发约定、架构细节和常用命令。

## 目录说明

```text
.
├─ cmake/       CMake 构建辅助模块
├─ docs/        环境、路线图和工程记录
├─ plugins/     功能插件及公共代码
├─ src/         平台层、SDK、主程序、提权助手和资源
├─ tests/       运行时自检程序
└─ refrences/   外部参考项目（不属于 WinEase 核心构建）
```
=======
# WinEase
>>>>>>> 4a7ce1b416e726e2ebd475e59bd5f4f12e54bb39
