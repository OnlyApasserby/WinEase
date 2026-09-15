# WinEase

> Windows 上缺的那些易用性小功能，一次补齐。常驻托盘的一体化面板，功能按插件加载，随用随开。

![平台](https://img.shields.io/badge/platform-Windows%2011%20x64-0078D4)
![语言](https://img.shields.io/badge/C%2B%2B-20-00599C)
![界面](https://img.shields.io/badge/Qt-6.8%20Widgets-41CD52)
![许可证](https://img.shields.io/badge/license-MIT-green)

---

## 目录

- [项目简介](#项目简介)
- [功能特性](#功能特性)
- [安装与使用](#安装与使用)
  - [方式一：安装包（普通用户）](#方式一安装包普通用户)
  - [方式二：从源码构建](#方式二从源码构建)
- [配置说明](#配置说明)
- [使用示例](#使用示例)
- [架构与目录结构](#架构与目录结构)
- [开发指南](#开发指南)
- [打包与发布](#打包与发布)
- [贡献指南](#贡献指南)
- [许可证](#许可证)
- [文档索引](#文档索引)

---

## 项目简介

WinEase 是一个面向 **Windows 11** 的易用性增强工具集：把系统里那些"本该有、却要装一堆小工具"的能力
收进一个常驻托盘的 Qt 应用，**每个功能都是可独立加载的插件**，互不影响、可单独开关。

**要解决的问题**：Windows 上大量高频小需求（窗口置顶、批量重命名、剪贴板历史、音量混合器、
屏幕取色、Hosts 编辑、文件解锁……）散落在形形色色的第三方工具里，每个都要单独安装、单独更新，
还常常互相打架。WinEase 把它们统一到一个界面、一套配置、一份日志里，并且**每个功能都能一键关掉**。

**适用场景**：

- 桌面与办公：窗口整理、剪贴板历史、屏幕标尺、批量文件处理；
- 开发与运维：JSON/XML 格式化、编码转换、端口占用查看、Hosts 与环境变量管理；
- 内容与多媒体：音量混合器、媒体控制面板、麦克风一键静音、亮度色温；
- 想在电脑与手机之间快速倒文件：内置**局域网传输**，手机免装 App，用浏览器直接收发。

**当前状态**：P0（基础设施）、P1（低风险功能）、P2（中等复杂度功能）已完成；P3 已按需求裁剪
（未完成项已从路线图中移除，详见 [`docs/ROADMAP-P3.md`](docs/ROADMAP-P3.md)），并新增批量移动文件、
局域网文件传输、单文件安装程序三项能力。

**几条决定了项目"长什么样"的取舍**：

- **优先使用 Windows 自带的 Win32 / C++/WinRT 能力**，能不引第三方库就不引；
- **最小权限**：主程序以普通权限运行，需要管理员的动作一律交给 `WinEaseHelper.exe` 按需触发 UAC；
- **读不到就如实说**：任何指标拿不到读数时，界面写明原因（缺组件 / 权限不够 / 硬件没有），
  绝不用 `0` 或空白冒充读数；
- **写系统状态的功能必须可还原**：停用即回到启用前，退出则保持现状。

## 功能特性

共 **37 个功能插件**（另有一个插件模板随构建产出）。每项一句话说明其作用。

### 窗口管理

| 功能 | 说明 |
|---|---|
| 窗口置顶 / 置底 | 让目标窗口始终在最前，或压到最底层（可批量操作） |
| 窗口透明度 | 调整任意窗口透明度，便于对照参考资料 |
| 窗口快速分屏 | 半屏 / 四分之一屏 / 九宫格等布局，一键贴合 |
| 虚拟桌面增强 | **只读**列出各虚拟桌面及其窗口一览（不含搬移：公开 API 只允许搬本进程窗口） |

### 文件增强

| 功能 | 说明 |
|---|---|
| 批量重命名 | 规则化批量改名，先预览再落盘，可直接撤销 |
| 批量移动文件 | 用**正则表达式**挑文件批量移动，先出计划、可预演、可撤销 |
| 批量格式转换 | 图片格式转换与文本字符集转换，进度可取消 |
| 快速文件预览 | 空格键式快速预览文件内容 |
| 重复文件查找 | 多级流水线（大小 → 头部 → 抽样 → 全量）找出重复文件 |
| 文件解锁 | 查出谁占用了文件，结束占用进程或将其移入回收站 |
| 敏感文件粉碎 | 覆写后删除，并回读校验 |
| 右键菜单扩展 | 向资源管理器右键菜单注入 WinEase 动作 |
| 局域网文件传输 | 局域网内电脑 ↔ 电脑、电脑 ↔ 手机**双向**传文件（手机免装 App） |

### 输入与效率

| 功能 | 说明 |
|---|---|
| 剪贴板历史 | 带来源标记的历史记录；标记为"不记录"的内容一字不落盘 |
| 滚轮增强 | 例如在任务栏上滚动调节音量（只吞自己该吞的事件） |
| 网页快速搜索 | 选中文本后用指定搜索引擎直接搜索 |
| 快速启动（搜索） | 从主界面搜索并启动已安装的程序或文件 |

### 显示与媒体

| 功能 | 说明 |
|---|---|
| 屏幕取色器 | 像素级取色并复制色值 |
| 屏幕标尺 | 在屏幕上量尺寸（坐标按物理像素对齐） |
| 焦点高亮 | 高亮当前输入焦点，便于录屏演示 |
| 放大镜增强 | 基于系统 Magnification API 的原生放大镜，CPU 占用低 |
| 亮度与色温护眼 | DDC/CI + WMI 调亮度、gamma 调色温，停用即还原 |
| 麦克风一键静音 | 全局快捷键闭麦，托盘徽标常驻显示当前状态 |
| 音量混合器 | 按应用调音量与静音（一个应用的多份会话合并为一行） |
| 媒体控制面板 | 显示当前曲目、进度与封面，并精确控制播放与媒体键 |

### 系统监控与维护

| 功能 | 说明 |
|---|---|
| 硬件监控悬浮窗 | 桌面小面板：CPU / 内存 / 网速 / GPU 利用率、磁盘温度，以及 CPU / 主板 / GPU 温度与风扇转速 |
| 电源快速面板 | 快速关机 / 重启 / 睡眠，倒计时可取消 |
| 定时任务 | 定时提醒 / 静音 / 电源动作；休眠唤醒后补触发 |
| USB 设备管控 | 识别可安全移除的设备并逐个弹出（含二次确认与 veto 提示） |
| 摄像头与麦克风提醒 | 设备被占用时提醒，同一应用只提醒一次 |

### 开发运维

| 功能 | 说明 |
|---|---|
| JSON / XML 格式化 | 文本格式美化与压缩 |
| 编码转换 | 文本字符集与换行符转换 |
| 端口占用查看 | 列出监听端口及其占用进程 |
| Hosts 快速编辑 | 写入前自动备份，支持字节级还原 |
| 环境变量管理 | 用户级直写；系统级走提权助手 |

### 界面与主题

| 功能 | 说明 |
|---|---|
| 强制深色主题 | 界面固定黑底白字，不随系统浅色 / 深色模式切换 |
| 壁纸自动切换 | 按计划轮换壁纸（`IDesktopWallpaper`） |

> 逐项进度、验收标准与已确认决策见 [`docs/ROADMAP.md`](docs/ROADMAP.md)。

## 安装与使用

### 方式一：安装包（普通用户）

获取 **`WinEase-<版本>-x64-Setup.exe`**（单文件、离线可装），双击即可。

```text
默认安装位置：D:\WinEase                                  ← 首选
              本机没有可用的 D 盘时**自动回退**：C:\Program Files\WinEase
安装范围    ：装到 D:\WinEase 时沿用当前用户，不需要管理员权限；
              回退到 C:\Program Files 时**必须以管理员身份运行安装程序**
附带内容    ：主程序 + 38 个插件 + Qt 运行库 + VC++ 运行库
卸载方式    ：开始菜单，或「设置 → 应用 → 已安装的应用」中点击卸载
```

**默认安装目录及其回退规则**（安装器 `--default-dir` 可以只读地打印出本机的实际取值）：

1. **首选 `D:\WinEase`** —— 数据盘根目录默认允许普通用户建目录，所以这条路径**不需要管理员权限**。
2. **本机没有可用的 D 盘时，自动回退到 `C:\Program Files\WinEase`**
   （严格说是 `%ProgramFiles%\WinEase`：系统盘被改成别的字母时它会跟着走，不会指到一个不存在的路径）。
   "没有可用的 D 盘"包含三种情况：盘符根本不存在、盘符对应的是未知/未挂载设备、D 盘是光驱
   —— 光驱的盘符是"存在"的，但往里装东西必然失败，因此同样按不可用处理（判据是卷的类型与
   根目录可访问性，而不是盘符字母）。
3. **回退路径属于系统目录，只有管理员能写**：这种情况下请右键安装程序 →
   「以管理员身份运行」；否则安装器会如实报「权限不足（错误码 5）」并给出两种解决办法，
   绝不会留下一个装了一半的目录。
4. 想装到别处：`WinEase-<版本>-x64-Setup.exe --dir "E:\Tools\WinEase"`（给了 `--dir` 就完全
   按它走，上面这套默认规则不生效）。

安装完成后从开始菜单启动 `WinEase`，程序会常驻系统托盘。

**两点提示（都是 Windows 的正常行为，不是故障）**：

1. 安装包**未做代码签名**，SmartScreen 会对未知发布者告警 —— 核对来源无误后选「更多信息 → 仍要运行」。
2. 使用「局域网文件传输」时，Windows 防火墙会询问是否允许联网（TCP 8720 / UDP 27182）。
   仅在**自家局域网**上勾选「专用网络」。

> 需要 CPU / 主板温度这类读数时，LibreHardwareMonitor 要经内核驱动读 MSR / SuperIO，
> 通常需要**以管理员身份运行** WinEase；不满足时面板会写「无读数（需要管理员权限）」而不是 0°C。
> GPU 温度不需要管理员权限。

### 方式二：从源码构建

#### 环境要求

| 项目 | 要求 | 说明 |
|---|---|---|
| 操作系统 | Windows 11 x64 | 目标平台 |
| 编译器 | Visual Studio 2026（MSVC v180 工具集） | **仅支持 MSVC**；非 MSVC 工具链会在 CMake 配置阶段被拒绝 |
| CMake | 3.28 或更高（参考环境 4.4.2） | |
| Qt | 6.8（参考环境 6.8.4） | 必需模块：`Core`、`Gui`、`Widgets`、`Svg`、`Network`；可选：`Multimedia`、`MultimediaWidgets` |
| Windows SDK | 10.0.26100.0 或兼容版本 | 提供 C++/WinRT 头文件 |
| .NET SDK | 8.0（**可选**） | 仅用于构建「硬件监控」的 C++/CLI 桥接层；缺失时该目标自动跳过，插件照常构建 |

#### 构建步骤

在 Visual Studio 开发者 PowerShell（或已配置 MSVC 环境的终端）中执行；示例假设 Qt 安装在 `D:\Qt`：

```powershell
# 1) 配置（只需一次，之后可直接构建）
cmake -S . -B build -G "Visual Studio 18 2026" -DCMAKE_PREFIX_PATH="D:\Qt"

# 2) 构建（推荐 RelWithDebInfo）
cmake --build build --config RelWithDebInfo --parallel
```

> ⚠ **不要用 `Debug` 构建**：本项目的 Qt 是 release-only 安装，Debug 与 Qt 混用会因 STL 布局与
> CRT 堆不兼容造成隐蔽内存损坏。请使用 `RelWithDebInfo` 或 `Release`。

只构建单个插件（target 名为 `we_<插件目录名>`）：

```powershell
cmake --build build --config RelWithDebInfo --target we_dev_text_format
```

只构建主程序与插件、跳过自检程序：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -DCMAKE_PREFIX_PATH="D:\Qt" -DWINEASE_BUILD_SMOKE_TESTS=OFF
```

#### 运行

```powershell
.\build\bin\WinEase.exe
```

构建产物统一落在 `build/bin/`，插件在 `build/bin/plugins/`（主程序启动时自动扫描该目录）。
`WinEaseHelper.exe` 由主程序按需拉起，不需要手动运行。

> ⚠ 运行中的 WinEase 与自检程序会锁定 EXE / DLL；重新编译前请先退出。

#### 运行自检（项目没有单元测试框架）

`tests/` 下是**独立的运行时自检程序**，退出码 `0` 表示通过：

```powershell
.\build\bin\win32_smoke.exe       # 平台能力层
.\build\bin\hook_smoke.exe        # 全局钩子服务
.\build\bin\overlay_smoke.exe     # 悬浮层
.\build\bin\theme_smoke.exe       # 深色主题强制固定
.\build\bin\elevation_smoke.exe   # 提权助手（助手未运行时只做对照）
.\build\bin\plugin_smoke.exe      # 插件契约与崩溃隔离
.\build\bin\ui_smoke.exe          # 主界面呈现
.\build\bin\feature_smoke.exe     # 功能插件端到端（加载真实插件 DLL）
.\build\bin\p3_spike.exe          # P3 技术预研探针（只读）
.\build\bin\installer_smoke.exe   # 打包链路（需先构建 winease_installer）
```

部分自检支持附加参数：

```powershell
.\build\bin\theme_smoke.exe --with-os-light-mode      # 追加"系统浅色"场景
.\build\bin\elevation_smoke.exe --with-uac            # 允许弹 UAC，跑全量
.\build\bin\elevation_smoke.exe --quit-helper         # 请常驻助手优雅退出
```

> ⚠ **`feature_smoke` 会短暂干扰真实桌面**（移动真实光标、切换主题与壁纸、写入剪贴板、调整音量、
> 变更文件状态等，绝大多数带守卫还原）。运行前请保存工作，并且不要同时操作鼠标或切换前台窗口。

> `p3_spike.exe` 的退出码语义与其它自检不同：它只判「预研探针能否跑通」，
> 某项能力是否可用要看其后紧跟的 `[说明]` 行。

#### 构建较慢或疑似卡住时

`feature_smoke` 通过全局属性依赖**全部插件**，因此它实际上是一次全量构建。仓库提供了带超时的脚本
（脚本为纯 ASCII —— Windows PowerShell 5.1 按 ANSI 读取 `.ps1`，中文注释会让脚本解析失败）：

```powershell
powershell -ExecutionPolicy Bypass -NoProfile -File build\build_target.ps1 -Target feature_smoke
powershell -ExecutionPolicy Bypass -NoProfile -File build\build_feature_smoke.ps1 -TimeoutMinutes 30
powershell -ExecutionPolicy Bypass -NoProfile -File build\run_smoke.ps1 -Exe feature_smoke
```

超时会杀掉进程并打印日志尾部；日志落在 `build\logs\`。

## 配置说明

- **配置文件**：`%APPDATA%\WinEase\config.ini`
- **结构**：全局设置位于根段；每个插件的设置位于 `[Plugins/<pluginId>]` 段（`pluginId` 形如 `file.batch_move`）
- **快捷键**：以 `pluginId::action` 形式的 id 由主程序统一注册与分发，插件不自行挂钩子
- **状态持久化**：插件的启用状态与运行状态由主程序统一保存与恢复
- **修改方式**：界面上修改即可自动写回；直接编辑 `config.ini` 也可，但**需要重启 WinEase** 生效

`config.ini` 示例（片段）：

```ini
[Plugins/monitor.hardware_hud]
showCpu=true          ; 显示 CPU 利用率
showMemory=true       ; 显示内存占用
showNetwork=true      ; 显示网络上下行速率
showGpu=true          ; 显示 GPU 利用率
showTemperature=true  ; 显示磁盘温度 + CPU 温度（一个开关管两行）
showGpuTemp=true      ; 显示 GPU 温度（桥接层，普通权限可读）
showBoardTemp=true    ; 显示主板温度（多数机器需要管理员权限）
showFan=true          ; 显示风扇转速
intervalMs=1000       ; 刷新间隔，500 ~ 10000 毫秒
interactive=false     ; false = 点击穿透（默认）；true = 面板可拖拽
anchorX=1680          ; 面板左上角的物理像素坐标（拖动后自动记住）
anchorY=24

[Plugins/file.batch_move]
sourceDirectory=D:\下载
targetDirectory=D:\归档
pattern=^2024.*\.pdf$
nameTemplate=          ; 改名模板：{name} 原名、{ext} 扩展名（含点）、{n} 序号
caseSensitive=false    ; 正则是否区分大小写
recursive=false        ; 是否递归子目录
keepStructure=false    ; 递归时是否保留目录结构
matchFullPath=false    ; 正则匹配完整路径，还是仅匹配文件名
conflictPolicy=skip    ; 目标重名策略：skip（跳过）/ rename（自动改名）/ overwrite（覆盖）

[Plugins/net.lan_transfer]
shareDirectory=D:\互传
httpPort=8720          ; 手机浏览器访问的端口
deviceName=WinEase@我的电脑
```

> 界面信息层级：插件卡片只展示「图标 + 名称 + 两行描述 + 开关」；完整说明、版本与标识在卡片的
> 帮助入口「关于插件」里。

## 使用示例

### 1. 把下载目录里 2024 年的 PDF 归档到另一个目录

1. 打开 WinEase → 左侧「文件增强」→「批量移动文件」；
2. 源目录填 `D:\下载`，目标目录填 `D:\归档`；
3. 正则填 `^2024.*\.pdf$`（默认只匹配文件名、不区分大小写）；
4. 点「生成计划」：计划表逐项写明**会搬到哪 / 为什么不动**，此时磁盘上一个字节都没动；
5. 想先确认真实行为，点「预演」——只报告会移动多少条，不动盘；
6. 点「执行移动」；若结果不符合预期，点「撤销上次移动」即搬回原处。

### 2. 手机与电脑互传文件（免装 App）

1. 打开 WinEase →「局域网文件传输」→ 选择一个**分享目录**（例如 `D:\互传`）；
2. 点「启用服务」，面板显示访问地址，形如 `http://192.168.1.23:8720`，可点「复制地址」；
3. 手机连接同一个 Wi-Fi，用浏览器打开该地址：
   - 页面列出分享目录里的文件，点文件名即可下载到手机（电脑 → 手机）；
   - 点页面上的「上传」按钮选择文件，即可把手机里的文件存到电脑（手机 → 电脑）；
4. 用完后点「停止服务」——端口立刻关闭，局域网里不再有这台机器的服务。

> 同一局域网内另一台装了 WinEase 的电脑会出现在「发现的设备」列表里，选中对端 + 选择文件即可直发。
> 该功能为明文 HTTP 且无鉴权，请勿在公共 Wi-Fi 上启用。
>
> **面板日志里出现 `192.168.x.x:5xxxx：没有这个地址（GET /favicon.ico …）` 不用管**：那是
> **手机浏览器自己**在页面加载完之后补发的站点图标请求（页面没声明图标时，浏览器就会去根路径找
> `/favicon.ico`，华为自带浏览器、夸克、Chrome、Safari 都会发，与用户操作无关），前缀里的
> `192.168.x.x:5xxxx` 是**手机的地址和临时端口**，不是你要访问的地址。它不影响页面显示与传文件。
> 现已两头收口：页面自己声明了内联图标（浏览器不再去请求），服务端对 `/favicon.ico` 与 iOS 的
> `/apple-touch-icon*.png` 直接回 `204`（不算错误、不再写日志），而真正写错的地址仍然如实回 404。

### 3. 查看某个端口被谁占用

WinEase →「开发运维」→「端口占用查看」，按端口排序后即可看到监听地址与占用进程（零提权，只读）。

### 4. 写一个新插件（骨架）

从 [`plugins/template`](plugins/template) 复制目录，然后：

```cpp
// my_feature_plugin.h
#include "sdk/IFeaturePlugin.h"

class MyFeaturePlugin : public WinEase::IFeaturePlugin   // 不要再继承 QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "my_feature_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    QString id() const override { return QStringLiteral("dev.my_feature"); }
    QString name() const override { return QStringLiteral("我的功能"); }
    // initialize() / shutdown() / createSettingsWidget() 等按需实现
};
```

接着在插件目录的 `CMakeLists.txt` 中调用 `winease_add_plugin`，并在
[`plugins/CMakeLists.txt`](plugins/CMakeLists.txt) 登记 `add_subdirectory(...)`：

```powershell
cmake --build build --config RelWithDebInfo --target we_my_feature
```

### 5. 打包一个安装程序

```powershell
cmake --build build --config RelWithDebInfo --target winease_installer
.\build\bin\installer_smoke.exe
```

## 架构与目录结构

### 分层与依赖方向

```text
src/win32/       WinEaseWin32              平台原语层（静态库）
src/sdk/         WinEaseSdk                插件契约层（静态库）
src/app/         WinEase                   主程序（Qt Widgets）
src/helper/      WinEaseHelper             提权助手（独立进程，按需 UAC）
src/bridge/      WinEaseLiteMonitorBridge  C++/CLI 桥接层（工程中唯一允许 /clr 的目录）
plugins/*        独立功能插件 DLL
plugins/common/  插件与自检**共用**的纯函数引擎
tests/*          运行时自检程序
tools/sfx/       自解压安装器的 SFX 壳（winease-setup.exe）
```

依赖方向为 `WinEaseWin32 → WinEaseSdk → WinEase → plugins`。关键约定：

- **主程序不导出符号**，因此插件要用的一切契约都必须住在 `src/sdk/`；插件**不得**直接依赖主程序
  单例，配置、日志、通知、快捷键、钩子、悬浮层、提权服务全部从 `PluginServices` 获取；
- `WinEaseWin32` 与 `WinEaseSdk` 会被静态链接进主程序和**每一个**插件，因此两者**禁止全局状态**；
- 全局低级钩子（`WH_KEYBOARD_LL` / `WH_MOUSE_LL`）、悬浮层与提权通道都是**主程序中的唯一单例**，
  插件只订阅 —— 否则多个插件各自挂钩子会互相抢事件；
- 坐标一律使用**物理像素**，与 Qt 逻辑像素的换算显式提供。

### 仓库目录

```text
.
├─ cmake/          CMake 辅助模块（插件注册、打包编排）
├─ docs/           环境、路线图、踩坑与打包文档
├─ packaging/      随安装包分发的说明文件
├─ plugins/        功能插件 + common（共享引擎）+ template（模板）
├─ scripts/        打包脚本
├─ src/            平台层 / SDK / 主程序 / 提权助手 / 桥接层 / 资源
├─ tests/          运行时自检程序（10 个独立可执行文件）
├─ tools/          构建期工具（sfx：自解压安装器）
└─ refrences/      外部参考项目（不参与核心构建）
```

## 开发指南

### 新增插件要改四处

1. 复制 `plugins/template/` 为 `plugins/<新目录>`；
2. 修改类名、元信息与 `<name>.json`（`name` / `id` / `version` / `category` / `author` /
   `requiresAdmin` / `description`）；
3. 在插件目录的 `CMakeLists.txt` 中调用
   `winease_add_plugin(we_<目录名> CLASS_NAME ... SOURCES ...)`；
4. 在 `plugins/CMakeLists.txt` 中登记 `add_subdirectory(<新目录>)`。

### 工程约定（提交前请自查）

- **语言与编码**：C++20；源文件使用 UTF-8；界面、注释与日志一律中文。
- **Qt 关键字**：全局启用 `QT_NO_KEYWORDS`，一律写 `Q_SIGNALS` / `Q_SLOTS` / `Q_EMIT`。
- **平台层只放无状态纯函数**；有状态的采样器以对象形式提供，状态由插件成员持有。
- **共享引擎放 `plugins/common/`**，并让**插件与自检编译同一份源码** —— 否则自检验的是另一套实现。
- **危险操作先出计划**：文件类改动先在引擎中计算 `Plan` 并支持 dry-run，落盘只经一个出口，且可撤销。
- **回调里不要同步读自身状态**（在 `onEnable()` 里读 `isEnabled()` 得到的是旧值），需要时排到事件循环下一轮。
- **程序化回填控件要用 `QSignalBlocker`**，否则"改控件 → 信号 → 回调又改控件"会栈溢出。
- **构建脚本（`.ps1`）必须纯 ASCII**：PowerShell 5.1 按 ANSI 读取脚本，中文注释会让它解析失败。
- **改完代码必须回填文档**：`docs/ROADMAP.md`（进度与验收）、`docs/traps.md`（踩坑）、
  `docs/ENV-SETUP.md`（环境与构建约定）——三份文档是工程的一部分，不是附件。

### 自检纪律（新增用例时）

- **真实状态优先于返回值**：断言要落到"磁盘上真有这个文件""窗口样式真的变了"，而不是"函数返回 true"。
- **前置条件必须断言出来**，不成立就如实报失败，绝不静默跳过。
- **等待状态用"连续稳定"**（超时内连续多次采样均为目标），不要判断"某一次采样等于"。
- **危险或不可撤销的操作在系统调用前用记录型桩截住**；文件类操作走引擎级 dry-run，
  而不是给界面加开关。
- **自检不得污染真实桌面**：写剪贴板走守卫、改音量走守卫、改配置前把 `APPDATA` 重定向到临时目录。

## 打包与发布

一条命令产出"用户拿到的那一个 exe"（单文件、离线可装）：

```powershell
cmake --build build --config RelWithDebInfo --target winease_installer
# -> build\dist\WinEase-0.1.0-x64-Setup.exe            （单文件离线安装程序）
# -> build\dist\WinEase-0.1.0-x64-Setup.manifest.txt   （逐文件 SHA-256 审计副本）

.\build\bin\installer_smoke.exe                        # 打包链路自检
```

打包会**自动收集**：主程序 + 提权助手 + 全部插件 + C++/CLI 桥接及其托管依赖 +
Qt 运行库与插件（`windeployqt`）+ VC++ 运行库（应用本地部署）；生成清单后压缩为 CAB，
再追加到静态链接的 SFX 壳上，因此安装器自身不依赖任何运行库。

安装器支持：`--default-dir`（只读打印本机解析出的默认安装目录，见
[默认安装目录及其回退规则](#方式一安装包普通用户)）、`--verify`（解到临时目录逐文件校验 SHA-256）、
`--selftest`（验证桥接运行时可用）、`--uninstall`（按清单删除文件，并回收注册项与快捷方式）。

> 完整设计、依赖收集策略与**实测边界**（例如 .NET 默认不随包、安装器如何检测并如实说明影响面）
> 见 [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md)。

## 贡献指南

欢迎提交 Issue 与 Pull Request。为了减少来回沟通，请按下述方式参与。

### 提交 Issue

- **Bug 反馈**请包含：现象、期望行为、复现步骤、**环境信息**（Windows 版本、Qt 版本、
  是否以管理员身份运行、是否安装 .NET 8 运行时），以及相关日志或自检输出。
- **功能建议**请说明使用场景与期望的交互方式；若涉及写系统状态，请一并说明"停用时如何还原"。
- **能在自检里复现的**，请附上自检名称与失败行（例如 `feature_smoke` 的输出片段），定位会快很多。

### 提交 Pull Request

1. Fork 本仓库，从 `main` 切出分支，命名建议 `feat/<简述>`、`fix/<简述>`、`docs/<简述>`。
2. 小步提交，提交信息说明"改了什么、为什么"。
3. **提交前请确保**：
   - 构建通过（`RelWithDebInfo`）；
   - 相关自检退出码为 `0`（至少运行与改动相关的自检，必要时跑全量）；
   - 新增行为配有自检用例，且断言落在**真实状态**上；
   - 文档已按需回填（`docs/ROADMAP.md` / `docs/traps.md` / `docs/ENV-SETUP.md`）。
4. PR 描述请包含：动机、改动要点、**验证证据**（自检输出 / 截图 / 日志）、文档与自检的更新情况。
5. 评审意见请在原分支追加提交，不要强推覆盖历史。

### 代码风格与红线

- 保持与现有代码一致的风格（中文注释、`Q_SIGNALS` 系列宏、无状态平台层）。
- **不要为一个功能引入新的第三方依赖**；确有必要时，请在 PR 中说明为什么 Windows 自带能力不够。
- **不允许静默失败**：读不到数据、写不进去、权限不够，都要在界面与日志中如实说明。
- 不要引入未经论证的"全局状态 / 单例"：平台层与 SDK 会被静态链接进每个插件，各持一份必然出错。

## 许可证

本项目采用 **MIT License**，详见 [`LICENSE`](LICENSE)（Copyright © 2026 OnlyApasserby）。

分发或二次开发时请注意第三方组件的各自许可证：

| 组件 | 用途 | 许可证 |
|---|---|---|
| Qt 6 | 界面与基础库 | LGPLv3（或商业许可） |
| LibreHardwareMonitorLib | 硬件传感器读数（经 C++/CLI 桥接调用） | MPL-2.0 |
| .NET 8 运行时 | 桥接层的运行时依赖（目标机安装） | MIT |

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | 总体路线图、进度、架构决策与验收标准 |
| [`docs/ROADMAP-P0-FIN.md`](docs/ROADMAP-P0-FIN.md) | P0 基础设施 |
| [`docs/ROADMAP-P1-FIN.md`](docs/ROADMAP-P1-FIN.md) | P1 低风险功能 |
| [`docs/ROADMAP-P2-FIN.md`](docs/ROADMAP-P2-FIN.md) | P2 中等复杂度功能 |
| [`docs/ROADMAP-P3.md`](docs/ROADMAP-P3.md) | P3 高风险功能、预研结论与裁剪记录 |
| [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md) | 打包与发布：包结构、依赖收集、实测边界 |
| [`docs/ENV-SETUP.md`](docs/ENV-SETUP.md) | 开发环境、Qt 模块、WinRT 约束与构建约定 |
| [`docs/traps.md`](docs/traps.md) | 工程踩坑记录与已验证的规避方案 |
| `CODEBUDDY.md`（仓库根目录，本地开发文档，未纳入版本库） | 面向 AI 助手与协作者的完整项目约定与常用命令 |
