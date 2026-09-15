# WinEase 环境与依赖补齐方案

> 适用环境：Windows 11 x64 · MSVC 19.51（VS 18 / v180 工具集）· CMake 4.4.2 ·
> Qt 6.8.4（**源码编译并 install 到 `D:\Qt`**）· Windows SDK 10.0.26100.0
>
> 本文回答两个问题：**缺失的 Qt 模块怎么补**、**需要哪些额外开发库**。

---

## 0. 结论速览

| 需求 | 采用方案 | 额外安装成本 | 是否引入第三方库 |
|---|---|---|---|
| PDF 预览/渲染 | **Windows SDK C++/WinRT `Windows.Data.Pdf`** | **0** | 否 |
| 视频预览 | `IShellItemImageFactory` 取首帧缩略图 + 交给系统播放器 | **0** | 否 |
| 视频内嵌播放 | **QtMultimedia（已由用户完成补装）** | 已完成 | 否（Qt 官方模块） |
| OCR 文字识别 | **Windows SDK `Windows.Media.Ocr`** | **0** | 否 |
| 媒体控制面板/播放信息 | **Windows SDK GSMTC（WinRT）** | **0** | 否 |
| 屏幕录制 | **Windows SDK `Windows.Graphics.Capture` + Media Foundation** | **0** | 否 |
| CPU / 主板温度 | **PawnIO 驱动 + 动态加载 PawnIOLib** | 一次装驱动（见 §4） | 是（用户指定，动态加载不静态链接） |
| NVMe / SATA 温度 | Windows 原生 `IOCTL_STORAGE_QUERY_PROPERTY` | **0** | 否 |
| GPU 温度 | 需厂商 SDK（NVML / ADL / IGCL） | 高 | 是 → 见 §5，**默认不做** |
| PDF 预览（备选） | 补装 QtPdf | **很高**（见 §2.2） | 否 |

**核心结论：除温度（PawnIO，你已指定）与可选的视频内嵌播放外，其余缺口全部用 Windows SDK 原生 API 解决，零额外安装。**

---

## 1. 已核实的环境事实（本机实测，非推断）

| 事实 | 验证方式 | 结论 |
|---|---|---|
| `D:\Qt` 是源码编译后的安装目录（扁平结构，无 `6.8.4/msvc2022_64` 层级） | 目录列表 | 官方在线安装器的预编译包**无法**与之混用 |
| `D:\Qt\bin\qt-configure-module.bat` **存在** | 文件检索 | ✅ "给已编译 Qt 追加模块"的官方工作流可用 |
| `Qt6Pdf`、`Qt6Multimedia` 均未安装 | `D:\Qt\lib\cmake` 检索返回 0 条 | 需要补装或改用替代方案 |
| Windows SDK **自带 C++/WinRT 头**：`...\10.0.26100.0\cppwinrt\winrt\windows.data.pdf.h`、`windows.media.ocr.h` | 文件检索命中 | ✅ **无需 NuGet / 无需额外 SDK** |
| Windows SDK **自带** `...\Lib\10.0.26100.0\um\x64\WindowsApp.lib` | 文件检索命中 | ✅ 链接该库即可调用全部 WinRT API |

> 这一点是本方案成立的基石：**C++/WinRT 属于 Windows SDK 的一部分，不是第三方依赖**。

---

## 2. 方案 A：补装 Qt 模块（保留但默认不采用）

公共前置：
1. 需要与已安装 Qt **完全同版本**的对应模块源码（6.8.4），源码树不能复用旧版本。
2. 保证 `D:\Qt\bin` 在 `PATH` 中，且用 **VS 2026 开发者命令提示符**（与编译 Qt 时同一工具集）执行。
3. 通用命令形态：
   ```bat
   cd <模块源码>/build
   D:\Qt\bin\qt-configure-module.bat .. -- <额外 -D 参数>
   cmake --build . --parallel
   cmake --install .
   ```

### 2.1 QtMultimedia（✅ 已由用户完成补装，2026-09-13）

> 现状：`D:\Qt\lib\cmake` 下已存在 `Qt6Multimedia` 与 `Qt6MultimediaWidgets`，
> 工程已通过 `find_package(Qt6 QUIET COMPONENTS Multimedia MultimediaWidgets)` 自动探测，
> 配置输出 `QtMultimedia : ON`，并定义 `WINEASE_HAS_MULTIMEDIA=1` 供代码分支使用。
>
> 注意：请求组件时必须**同时写 `MultimediaWidgets`**，否则 `Qt6::MultimediaWidgets`
> 这个 CMake 目标不会被创建，链接会报 "target was not found"。
>
> 以下为原始补装步骤，保留备查（重装 Qt 时可能需要）。

补装成本：中

```bat
:: 源码：qtmultimedia（6.8.4 分支），需先 git submodule update --init --recursive
D:\Qt\bin\qt-configure-module.bat .. -- -nomake examples -no-pch
cmake --build . --parallel
cmake --install .
```

**需要的额外开发库**：
- **无**（推荐路径）：Qt 自带 FFmpeg 预编译方案，源码位于 `qtmultimedia/src/3rdparty/ffmpeg`，随 QtMultimedia 模块一起构建与安装（FFmpeg 7.1.3，LGPLv2.1+，不含 GPL 组件）。
- 若坚持使用系统 FFmpeg，则需自行准备 **LGPL 兼容构建**的 FFmpeg 7.x 开发包（headers + import lib），并显式指定系统 FFmpeg 路径。**不推荐**，徒增部署复杂度。
- Windows 上还可以尝试关闭 FFmpeg 仅保留 **WMF（Media Foundation）后端**，好处是零外部二进制；但 WMF 后端功能覆盖较弱（尤其音频捕获与格式支持），**是否可用需在动手时实测确认**，不要预设。

**风险**：Qt 6.8.4 发布早于 MSVC 19.51，需实测是否有新的编译告警被当作错误；QtMultimedia 与我们的 `/W4` 无关（它是独立构建的模块，不受本工程编译选项影响）。

### 2.2 QtPdf（补装成本：很高——不建议）

由 Qt 官方 Wiki《QtPDF Build Instructions》确认：

```bat
git clone https://code.qt.io/qt/qtwebengine.git
cd qtwebengine && git checkout 6.8
git submodule update --init --recursive       :: ← 约 7.1 GB

mkdir build && cd build
D:\Qt\bin\qt-configure-module.bat ../qtwebengine ^
    -- -DQT_FEATURE_qtwebengine_build=OFF ^
       -DQT_SHOW_EXTRA_IDE_SOURCES=OFF
cmake --build . --parallel
cmake --install .
```

**需要的额外开发库/工具**：
| 依赖 | 说明 |
|---|---|
| Python 3 | configure 必需 |
| `html5lib` | `pip install html5lib`（或用系统包管理器） |
| `gn` + `ninja` | Chromium 构建工具链，构建过程会自动获取 |
| PDFium | Chromium 的一部分，**会被真实编译**（`-DQT_FEATURE_qtwebengine_build=OFF` 只关掉 WebEngineCore，不关 PDFium） |
| 精确匹配的 Windows SDK | Chromium 对 SDK 版本敏感 |

**为什么建议不采用**：
1. **7.1 GB 源码 + 小时级构建**，只为渲染 PDF，性价比极低；
2. QtPdf **没有独立仓库**，只能连同 qtwebengine 一起检出；
3. Chromium 构建链对编译器版本敏感，MSVC 19.51 属未验证组合，可能需要额外装一套 VS 2022 构建工具，而**用不同工具集编译的 DLL 与现有 Qt 混链存在 ABI 风险**（MSVC v14x 系列理论二进制兼容，但不能想当然）；
4. 每次 Qt 升级都要重复这套流程。

**结论：用 §3 的 WinRT 方案替代。**

---

## 3. 方案 B：Windows SDK 原生（推荐，默认采用）

### 3.1 能力映射

| 功能 | WinRT / Win32 API | 备注 |
|---|---|---|
| PDF 渲染 | `winrt::Windows::Data::Pdf::PdfDocument` → `RenderPageToStream` → `QImage` | 支持按需渲染单页，内存友好 |
| OCR | `winrt::Windows::Media::Ocr::OcrEngine` | 需安装对应语言包，中英文需检查 `AvailableRecognizerLanguages` |
| 媒体控制 | `winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager` | Win10 1809+；提供播放/暂停/切歌/进度/曲目信息 |
| 录屏 | `winrt::Windows::Graphics::Capture::GraphicsCaptureItem` + Media Foundation `IMFSinkWriter` | 最复杂的一条，放最后 |
| 视频缩略图 | `IShellItemImageFactory::GetImage` | 零依赖拿首帧缩略图 |

### 3.2 CMake 集成要点（统一封装到 P0 的 `WinEaseWin32`）

```cmake
# WinRT 需要 windowsapp.lib；C++/WinRT 头由 Windows SDK 提供，无需额外包
target_link_libraries(WinEaseWin32 PUBLIC windowsapp.lib)
target_compile_definitions(WinEaseWin32 PUBLIC
    WINRT_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
```

**平台层的 Qt 模块约束（别轻易破）**：`WinEaseWin32` 只依赖 `Qt6::Core` + `Qt6::Gui`。
它是静态库，会被链接进主程序**与每一个插件**，多引一个 Qt 模块就等于每个插件都多带一份
DLL 依赖。因此需要"看起来像 Qt 的事"时优先自己写：例如 `NetUtils` 的 IPv4/IPv6 地址格式化
没有用 `QHostAddress`（那是 QtNetwork），而是手写了 RFC 5952 的 `::` 压缩。
新增平台模块时若确实需要新模块，请在根 `CMakeLists` 里 `find_package` 并说明理由。

### 3.3 三个必须提前定好的工程约定（否则一定踩坑）

1. **`signals` / `slots` 宏与 WinRT 头冲突**
   `winrt/*.h` 中会使用 `signals` 等标识符。解决办法二选一：
   - 在包含任何 `winrt/` 头之前 `#undef signals` / `#undef slots`；或
   - 全工程禁用 Qt 关键字（`QT_NO_KEYWORDS`），统一使用 `Q_SIGNALS` / `Q_SLOTS` / `Q_EMIT`。
   **推荐后者**，一次到位、不再有隐藏顺序依赖；代价是所有既有代码要替换宏（工程刚起步，现在改最便宜）。

2. **COM / WinRT 初始化**
   WinRT 要求线程以正确的套间模型初始化。约定：在 `main()` 里显式
   `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`，退出前 `CoUninitialize()`；
   若某功能内部起线程调用 WinRT，该线程需自行初始化（MTA 或 STA）。

3. **异常边界**
   WinRT 以 **C++ 异常（`winrt::hresult_error`）** 报错，必须转换为我们自己的错误类型，
   绝不允许异常穿透到 Qt 事件循环或插件边界，否则会直接终止进程。
   统一在 `WinEaseWin32` 里提供 `Win32Error::toMessage(hresult)`（中文文案）。

4. **判断"某个 WinRT 运行时类是否可用"不要用 `ApiInformation`**
    `Windows.Foundation.Metadata.ApiInformation::IsTypePresent` 面向有"包标识"
    (package identity) 的应用。在**未打包的桌面程序**里调用它会命中 C++/WinRT 内部断言
    （`winrt/base.h:2948 assert(value)`）**直接崩溃**，而不是优雅返回错误。
    → 工程已提供 `WinRtSupport::probeWinRtClass()`，内部用 `RoGetActivationFactory` 探测，
    并请求 **`IID_IUnknown`**（静态类的工厂实现的是自己的静态接口而非 `IActivationFactory`，
    请求后者会对可用类型误报 `E_NOINTERFACE`）。

5. **DPI 感知必须显式声明**（P0-1 实现中踩到的真实坑）
   非 GUI 的辅助进程（例如 `WinEaseHelper.exe`、命令行工具、自检程序）**必须**在
   创建任何窗口之前显式声明 DPI 感知：

   ```cpp
   ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   ```

   否则 `GetWindowRect` 返回被 Windows 虚拟化的像素，而
   `DWMWA_EXTENDED_FRAME_BOUNDS` 始终返回物理像素，两者的坐标系会相差一个缩放倍数
   （实测：150% 缩放下视觉边界比窗口矩形还大，758 vs 520）。
   `WinEase.exe` 由 Qt 自动设置，无需额外处理；**新写的每个可执行文件都要检查这一点**。

---

## 4. PawnIO（温度监控，你已指定）

### 4.1 PawnIO 是什么

> 脚本化通用内核驱动，提供 MSR、I/O 端口、PCI 配置、SMBus、ACPI EC 等硬件访问能力，
> 已获微软签名（attestation），可在开启 Secure Boot 的 Windows 11 上安装。

**组成**（均为独立发行物，不随我们的程序分发）：

| 组件 | 作用 |
|---|---|
| `PawnIO.sys` | 内核驱动 |
| `PawnIO.Setup` | 安装器（管理员运行一次） |
| `PawnIOLib.dll` / `PawnIOLib.h` | 用户态客户端库（我们的代码调用它） |
| `*.bin` 模块 | Pawn 脚本编译产物，按需加载（Intel MSR、AMD SMN、ACPI EC…） |

**用户态调用链**（deepwiki 索引自 `PawnIOLib.cpp` / `PawnIOLib.h` 确认）：

```
pawnio_open()                    // NtOpenFile 打开驱动设备，GENERIC_READ | GENERIC_WRITE
      ↓
pawnio_load(<signed .bin>)       // IOCTL_PIO_LOAD_BINARY，驱动先校验签名再建 VM
      ↓
pawnio_execute(<fn>, <args...>)  // IOCTL_PIO_EXECUTE_FN，32 字节函数名 + ULONG64 参数数组
      ↓
pawnio_close()                   // NtClose
```

返回码有三种风格（`PAWNIOAPI`→HRESULT / `PAWNIOWINAPI`→BOOL / `PAWNIONTIAPI`→NTSTATUS），
统一按 HRESULT 版本处理即可。

### 4.2 集成策略（重要）

| 决策 | 理由 |
|---|---|
| **运行期 `LoadLibraryW(L"PawnIOLib.dll")` 动态加载，不做静态链接** | 未安装 PawnIO 的机器上功能自动降级为"不可用"，程序不受影响；也不违反"不引入第三方库" |
| **不随包分发驱动** | 驱动需用户以管理员身份安装（签名校验），由用户在安装向导中触发 |
| **所有 PawnIO 调用走 `WinEaseHelper.exe`** | 打开驱动设备需要管理员权限；主程序保持普通权限 |
| **打开失败 / 模块缺失 / CPU 不支持 → 卡片显示原因，不弹错** | 温度是"锦上添花"能力，绝不能因此影响主程序 |
| **数值必须标注"仅供粗略参考"** | DTS 读数在空载时可能明显偏离真实温度，且不同 CPU 家族补偿算法不同 |

### 4.3 温度读取路径（参考 LibreHardwareMonitor 的 PawnIO 模块划分）

| 目标 | PawnIO 模块 | 读取方式 |
|---|---|---|
| Intel CPU 每核温度 | `IntelMsr` | MSR `0x19C` IA32_THERM_STATUS（bit22:16 数字读数，bit15 有效性） |
| Intel CPU 封装温度 | `IntelMsr` | MSR `0x1B1` IA32_PACKAGE_THERM_STATUS（bit22:16） |
| Intel TjMax 基准 | `IntelMsr` | MSR `0x1A2` IA32_TEMPERATURE_TARGET（bit23:16） |
| AMD Zen 温度 | `AmdFamily17` | SMN 寄存器 `F17H_M01H_THM_TCON_CUR_TMP`（bit31:21 为温度；bit19 置位时需 +49°C）；每 CCD 见 `F17H_M70H_CCD#_TEMP` |
| AMD K10 及更早 | 对应家族模块 | 不同寄存器，按家族分支 |
| 主板 / EC 传感器（可选） | ACPI EC（LPC I/O）类模块 | 需读 EC 寄存器映射，**实现成本高，放 P3 后期** |
| NVMe / SATA 硬盘温度 | **不需要 PawnIO** | `IOCTL_STORAGE_QUERY_PROPERTY` → `StorageDeviceTemperatureProperty` |

> **换算公式**：`温度 = TjMax − 数字读数`（Intel）；AMD 走 SMN 值 + 架构相关偏移。

### 4.4 实现前必须确认的未决项

写代码前需要读源码/文档确认（不要凭猜）：
1. `PawnIOLib.h` 中 `pawnio_execute` 的**精确函数签名**与参数打包方式；
2. PawnIO 的**许可证条款**（尤其是否允许我们的程序仅通过 DLL 调用、不二次分发驱动）；
3. 各 `.bin` 模块的**准确文件名与导出函数名**（Pawn 脚本的 public function）；
4. 驱动设备 ACL 是否要求管理员（**按需要走 WinEaseHelper**）；
5. 与反作弊 / 其他硬件监控工具（HWiNFO、FanControl）共用驱动时的冲突表现。

以上确认工作作为 **P3-07 技术预研 spike**（`tests/p3_spike`，见 §6 的运行命令）的产物，
先出结论再动手。

### 4.5 预研实测结论（2026-09-14）

| 事实 | 实测结果 |
|---|---|
| PawnIO 驱动是否已安装 | ✅ **已安装**：服务 `PawnIO` 已注册、设备 `\\.\PawnIO` 存在（普通权限打开返回错误码 5 = 需要管理员） |
| 用户态 `PawnIOLib.dll` | ❌ **缺失**（`LoadLibraryW` 失败）→ CPU 温度暂时做不了 |
| CPU 家族 | `GenuineIntel` family 6 → 走 `IntelMsr` 模块（MSR `0x19C`/`0x1B1`/`0x1A2`） |
| 磁盘温度 | ✅ **已经可读**（原生 `IOCTL_STORAGE_QUERY_PROPERTY` → `PhysicalDrive0 39°C`，零依赖零提权） |

→ **实施顺序**：先做"磁盘温度 + CPU/内存/网速/GPU 利用率"（全部零依赖、本机可验），
CPU/主板温度作为第二个里程碑（前置是把 `PawnIOLib.dll` 放到 `build\bin\` 或 PATH）。
§4.4 的四条未决项**仍未定**（需要 PawnIO 发行物才能确认，不能凭猜写代码）。

---

## 5. GPU 温度：改由 C++/CLI 桥接解决（2026-09-15 结论已翻转）

原结论（"Windows 没有公开统一的 GPU 温度 API，只能用厂商 SDK，默认不做"）**只对了一半**：
Windows 确实没有统一 API，但**不需要我们自己去调厂商 SDK** ——
`LibreHardwareMonitorLib` 已经把 NVIDIA / AMD / Intel 三条路都封装好了，而且走的是
**厂商的用户态接口**，普通权限即可读到（本机实测 `GPU Core = 42.7°C`）。

**现行方案**：整个温度类采集（CPU / 主板 / GPU / 风扇）统一走
**C++/CLI 混合模式程序集 → LibreHardwareMonitorLib**，见 §5.1。

> 厂商 SDK 对照表（**留档**：说明"为什么是库在做这件事，而不是我们"）：

| 厂商 | 库内部走什么 | 我们是否需要处理 |
|---|---|---|
| NVIDIA | NVML / NVAPI | 否（由 LibreHardwareMonitorLib 封装） |
| AMD | ADL | 否（同上） |
| Intel | 核显走驱动接口；CPU 走 MSR（需内核驱动） | 否（同上；MSR 那条**需要管理员**，读不到就如实报原因） |

---

## 5.1 C++/CLI 桥接：依赖与构建约定（P3-07 改版）

**为什么允许在本工程里出现 `/clr`**：这是全工程唯一的例外，理由只有一个 ——
温度类读数在原生 C++ 侧没有等价物，而 `refrences/LiteMonitor` 用
`LibreHardwareMonitorLib` 已经把这件事做全了。任何**新功能**都不许顺手往 `src/bridge/` 里塞。

| 依赖 | 版本/位置 | 缺了会怎样 |
|---|---|---|
| .NET 8 SDK（`dotnet` 在 PATH） | 构建期 | CMake 打印"未找到 dotnet，跳过"，插件照常构建，运行时如实报"桥接组件不可用" |
| MSVC 的 C++/CLI 支持（`/clr:netcore`）+ `ijwhost.lib` | 构建期（VS 组件 / `Microsoft.NETCore.App.Host.win-x64` 包） | 同上（跳过桥接目标） |
| .NET 8 **运行时**（`Microsoft.NETCore.App 8.x`） | **运行期** | 桥接 DLL 里 `ijwhost.dll` 起不来 → 面板写"桥接组件 WinEaseLiteMonitorBridge.dll 未加载（错误码 …）" |
| `LibreHardwareMonitorLib 0.9.6` + 8 个传递依赖 | 构建期由 NuGet 还原，产物**平铺到 `build/bin`** | 构建期就失败（`dotnet publish` 报错），不会悄悄少文件 |

**部署形态**（`build/bin` 里与主程序同目录，CoreCLR 按"应用基目录"探测）：

```
WinEaseLiteMonitorBridge.dll          ← 桥接程序集（/clr:netcore，我们自己构建）
WinEaseLiteMonitorBridge.runtimeconfig.json  ← 告诉 ijwhost 加载 net8.0 共享框架（CMake 生成）
ijwhost.dll                           ← 从 Microsoft.NETCore.App.Host 包里取最新版本复制过来
LibreHardwareMonitorLib.dll + 8 个依赖 ← dotnet publish 的产物
```

**权限前提（写进帮助页，不藏）**：LibreHardwareMonitor 读 CPU 的 MSR / 主板 SuperIO 要经内核驱动
→ **多数机器需要以管理员身份运行 WinEase**；GPU 温度不需要。读不到时面板写
"无读数（需要管理员权限）"，**绝不显示 0°C**。

**构建期踩过的三个坑**（详见 `traps.md` #81~#83）：CMake 写 `CLRSupport=NetCore` 会让
VS 的 MSBuild 去 Import `Microsoft.NET.Sdk` 而解析不了；VC 工程默认 `TargetFrameworkVersion=v4.0`
会触发 `MSB3644`；`/AI`、`/FU` 若不紧贴路径会被 CMake 拆错位。

---

## 5.2 构建/自检"看起来卡死"时：用带超时的脚本

`feature_smoke` 通过 `WINEASE_PLUGIN_TARGETS` 依赖**全部插件**，因此它实际上是一次全量构建，
耗时较长；再叠加默认并行度把机器压满，很容易被误判成"卡死"。仓库里备了两个带超时的脚本
（**纯 ASCII 写成** —— Windows PowerShell 5.1 按 ANSI 读取 `.ps1`，中文注释会让它直接解析失败）：

```powershell
# 全量构建 feature_smoke：限并行 4、20 分钟超时、日志落 build\logs\
powershell -ExecutionPolicy Bypass -NoProfile -File build\build_feature_smoke.ps1
powershell -ExecutionPolicy Bypass -NoProfile -File build\build_feature_smoke.ps1 -TimeoutMinutes 30 -Parallel 2

# 跑任意自检：12 分钟超时，输出落盘并自动筛出 [失败] 行
powershell -ExecutionPolicy Bypass -NoProfile -File build\run_smoke.ps1 -Exe feature_smoke -TimeoutMinutes 12
powershell -ExecutionPolicy Bypass -NoProfile -File build\run_smoke.ps1 -Exe win32_smoke
```

两个脚本都在超时后**杀掉进程并打印日志尾部**（卡在哪一步一眼可见），并如实回传退出码。

---

## 6. 构建目录与构建配置约定

| 用途 | 目录 | 配置 |
|---|---|---|
| 日常开发构建 | **`build/`** | **`RelWithDebInfo`**（含调试信息） |
| 发布构建（后续） | `build-release/` | `Release` |

```powershell
# 配置
cmake -S . -B build -G "Visual Studio 18 2026" -DCMAKE_PREFIX_PATH="D:/Qt"

# 构建（含调试信息，可断点调试）
cmake --build build --config RelWithDebInfo --parallel

# 运行
.\build\bin\WinEase.exe
```

**自检程序**（退出码 `0` = 全部通过）：

```powershell
.\build\bin\win32_smoke.exe      # 平台能力层（103 项）
.\build\bin\hook_smoke.exe       # 全局钩子（31 项，要求注入事件之外没有真实输入）
.\build\bin\overlay_smoke.exe    # 悬浮层（34 项）
.\build\bin\theme_smoke.exe      # 深色主题强制固定（37 项）
.\build\bin\theme_smoke.exe --with-os-light-mode
                                 # 追加"系统浅色"场景（41 项）：会临时改写并恢复
                                 # HKCU\...\Themes\Personalize\AppsUseLightTheme
.\build\bin\elevation_smoke.exe                # 提权助手验收（helper 已在运行时全量，否则只做对照）
.\build\bin\elevation_smoke.exe --with-uac     # 允许弹 UAC 拉起 helper（全量 29 项）
.\build\bin\elevation_smoke.exe --quit-helper  # 运维：请求常驻 helper 优雅退出（重编译前必须先退出）
.\build\bin\elevation_smoke.exe --restore-hosts <备份文件>   # 运维：字节级还原 hosts
.\build\bin\plugin_smoke.exe     # 插件异常边界与崩溃隔离（37 项，把真实 PluginGuard/PluginManager 编入目标）
.\build\bin\ui_smoke.exe         # 主界面呈现（18 项：卡片介绍固定两行 + 完整文本进 Tooltip +
                                 #   描述区高度固定；帮助「关于插件」栏目）
.\build\bin\p3_spike.exe         # P3 技术预研探针（27 项，**只读**：不改系统状态、不真切设备）
                                 #   ⚠ 退出码语义特殊：exit 0 = 探针跑完了，
                                 #      "某项能力是否可用"看 [说明] 行（"未公开 API 已失效"也是有价值的结论）
.\build\bin\feature_smoke.exe    # P1/P2/P3 功能插件端到端（加载 build/bin/plugins 下真实插件 DLL 驱动；
                                 #   窗口组 + 工具组 + 悬浮层组 + 系统组 + 文件组 + 输入组
                                 #   + 显示与媒体组 + 系统与媒体组 + 安全与隐私组 + 开发运维组
                                 #   + P3 组（P3-01 虚拟桌面 42 + P3-02 文件锁定 38 + P3-14 USB 22）
                                 #   + P3-07 硬件监控悬浮窗 59 + P3-09 音量混合器 76
                                 #   + P3-11 媒体控制面板 89
                                 #   = 共 1169 项）
                                 #   ⚠ 总数会随环境浮动：P3 组（USB 管控）会按**插着的可移除设备**展开
                                 #     （本机 2026-09-15：插着移动硬盘时 1169 项、拔掉时 1158 项）；
                                 #     总数对不上不一定是回归 —— 看 `[失败]` 行数为 0 才是。

```

调试自检输出时用 **`$env:WINEASE_SMOKE_ASCII=1`**：标记会从 `[通过]/[失败]/[信息]` 变成
`[PASS]/[FAIL]/[INFO]`，方便在 PowerShell 里 `Select-String -Pattern "^\[FAIL\]"` 直接捞失败行
（中文标记在"命令参数 → 管道 → 文件"的往返里会被编码来回糟蹋，出现过"明明有失败却 grep 不到"）。
最稳的收集方式：`.\build\bin\feature_smoke.exe 2>&1 | Out-File build\run.txt -Encoding utf8`。

⚠ **跑自检前先确认构建真的编译过目标文件**：把构建输出重定向到日志文件时，
要确认日志文件确实产生了、且里面能看到相关 `.cpp` 被编译（例：`PluginManager.cpp`）。
本轮曾因"源码已修好但二进制是旧的"而误判成 2 项失败（详见 [`traps.md`](./traps.md) 踩坑 #31）。
最省事的写法是一条命令串起来：

```powershell
cmake --build build --config RelWithDebInfo --parallel; .\build\bin\plugin_smoke.exe; "exit=$LASTEXITCODE"
```

`feature_smoke` 与其它自检的**运行方式差异**（首次跑之前务必知道，详见 [`traps.md`](./traps.md) 踩坑 #33）：
- 它加载的是 `build/bin/plugins/` 下**真实的插件 DLL**（不是把插件源码编进目标），
  所以要测的插件必须先构建；单独 `--target feature_smoke` 时依赖由
  `WINEASE_PLUGIN_TARGETS` 全局属性自动挂上，新增插件批次无需再改测试的 CMake。
- 它会**短暂干扰真实桌面**：移动真实光标（"跟随鼠标"模式必须真移）、让一个测试窗口
  置顶/半透明/置底、显示一块置顶纯色"色卡"、并可能抢一次前台；结束时还原光标与剪贴板、关掉窗口。
  跑之前**别在它上面敲字**，跑完桌面会自己恢复。
- ⚠ **跑的时候不要动鼠标、不要切前台窗口**：窗口组的前置条件是"光标下就是探针窗口"，
  外部移动鼠标会让 `SetCursorPos` 的结果被抢回去（实测同一份二进制两次运行，
  失败项 21 → 2 全部集中在"前置：光标…"）。用例对此会做**约 1 秒的连续稳定确认**
  （连续 3 次采样都保持目标，采样间隔 30ms，每次采样之间处理 Qt 事件）；
  若仍有失败项且**全部**是"前置：光标…"，那就是被外部操作打断，直接重跑即可。
- ⚠ **前台切换类前置**（`P1-04 前置：子进程成功把辅助窗口切到前台`）：
  Windows 的**前台锁定**会在用户正在操作窗口时拒绝任何跨进程的 `SetForegroundWindow`。
  用例现在的做法是：子进程**分阶段**切前台（直接试 → 立即检查 → 等事件循环 →
  `AttachThreadInput` 抢权再试 → 交给连续稳定确认），失败时再整轮重试一次（2 × 1500ms）；
  仍失败就**如实报"前置不成立"**（不无限重试、不静默跳过）。
  后果提示：这条失败时"z 序维持"那条会退化成**假通过**（没有前台事件 → 插件无需维持 → 断言自然成立），
  所以必须当真：报失败就重跑，**不要在"只有这条失败"的情况下宣称窗口组全绿**。
  实测同一份二进制：无人操作时应当 **866/866 exit 0**；有人在动窗口时可能只剩这 1 条失败。
- 目标窗口跑在本 exe 的 `--probe-window` 子进程里（平台层会排除本进程窗口，
  同进程的测试窗口测不到真实场景）；子进程还有一个"辅助窗口"，用来制造
  来自别的进程的前台切换与"另一个可操作窗口"的对照条件。
- 工具组用例**只写剪贴板**（有 `ClipboardGuard` 兜底还原），并且默认把插件的
  `useSelection`/`pasteBack` 关掉 —— 自检不向你的前台窗口注入 Ctrl+C / Ctrl+V。
- 悬浮层组用例**会短暂显示全屏透明的置顶层**（标尺 / 聚光灯），并读**合成后的屏幕像素**做断言；
  结束时按 ownerId 全部回收，桌面像素恢复原样。跑的时候屏幕上会闪一层变暗/变亮，属正常现象。
  像素断言一律用"**同一个像素相对自己**"的 A/B 比较（开灯前先断言这块像素稳定），
  **不假设能看到某块测试窗口的颜色** —— 真实桌面上别的窗口随时可能盖在它上面（曾被控制台盖住 → 整片假失败）。
- ⚠ **系统组用例会改真实系统状态**（P1-13 主题 / P1-14 壁纸 / P1-05 右键菜单注册表）：
  它先把原值读出来，结束时逐项还原并回读校验；中途失败也会走还原路径。
  跑的时候**别同时改主题/壁纸**；如果它中途被杀掉，检查一下
  `HKCU\Software\Classes\*\shell\` 下有没有残留的 `WinEase.*` 项（正常情况一个都不该剩）。
  壁纸用例用的是临时目录里的 3 张纯色 BMP，不会碰你自己的图片。
- ⚠ **文件组用例会在磁盘上真造文件、真改名、真覆写、真删**（P2-01/02/03/12）：
  全部fixture都在临时目录 `%TEMP%/winease_feature_smoke_<pid>/` 里（用完即删），
  覆写用 `zero1`（1 遍 0x00）以免长时间写盘；**预览/粉碎面板会被真的打开再关掉**（短暂闪窗，属正常）。
  唯一会"流出临时目录"的副作用：**P2-03 会点一次「移到回收站」，把 2 个 64 KB 的自检临时文件
  真实地移进你的回收站**（用例里已明说；这是"没被硬删"唯一的硬证据 —— `SHQueryRecycleBinW` 条目数差分）。
  它**无法程序化还原**（回收站里的项目要你手动清），跑完发现回收站里多了两个
  `winease_dup_*.bin` 就删掉即可。别的东西一个字节都不会动。
- ⚠ **输入组用例会碰真实剪贴板与真实系统音量**（P2-04/05），是本套自检里最"贴手"的一组：
  - 这条命令会**在自检进程内装一对真实的全局低层钩子**（`WH_KEYBOARD_LL` + `WH_MOUSE_LL`，
    用的是主程序那份 `app/core/HookServiceImpl.cpp`，不再用桩）——因为 P2-05 要证明的正是
    "任务栏上的那个滚轮事件被吞掉了 + 系统音量真的变了"，用桩只能测出手写的返回值。
  - P2-04 会**清空并改写你的系统剪贴板**（`EmptyClipboard` + `SetClipboardData`，含密码管理器
    那种"不记录"标记格式）：用例开头用 `ClipboardGuard` 存下你原来的剪贴板，结束时还原。
    中途失败/被杀则可能留下自检内容——跑之前先把要用的东西**粘到别处**，比事后猜安全。
  - P2-05 会把**真实光标移到任务栏上**、发**合成的滚轮/ Ctrl 按键**、并**真的改系统音量**
    （一格 7%）：音量由 `AudioVolumeGuard` 保存/还原，光标停在任务栏上（不是原处）。
  - 这组的前提是"任务栏可见且在上面"（自动隐藏任务栏、任务栏被移到副屏时会**如实报前置不成立**，
    不会静默跳过）；无音频输出设备时同样如实报前置不成立。
- ⚠ **显示与媒体组用例会动真实光标、真改麦克风静音、真改屏幕亮度与色温，并在屏幕上放一个置顶放大镜窗口**（P2-08/09/10）：
  - P2-10 会**真的翻转系统麦克风的静音状态**（`IAudioEndpointVolume::SetMute`）：开头用 `MicMuteGuard`
    记下你原来的状态，结束时写回并**回读确认**。中途被杀可能留下"被静音/被打开"的麦克风 ——
    按一下耳机线麦键或去系统"声音设置"里看一眼即可。托盘徽标走的是宿主服务接口（自检里是桩，
    **不会动你的托盘图标**）。
  - P2-08 会**真的改屏幕亮度（±25%）与 gamma 色温**：亮度开头记下原值、收尾写回并读回确认
    （中途被杀请自己把亮度调回去）；色温由插件在**停用时逐元素还原**到启用前的 ramp。
    用例还会**逐档试写色温**来探测"本机 gamma 能写到多暖"（本机实测只能到 3500K，再暖会被
    Windows 静默拒绝，详见 [`traps.md`](./traps.md) 踩坑 #53）—— 探测期间屏幕色温会连续小幅跳几档，属正常。
  - P2-09 会**把真实光标挪到屏幕中间 / 左上 1/4 / 右下角**（"跟随鼠标"只有真移鼠标才验得了），
    并在屏幕上显示一个**置顶、不进任务栏、不抢焦点**的原生放大镜窗口（默认 240px），
    之后还要跑 40 帧 ×2 的 CPU 对照（约 2.4 秒）。跑的时候屏幕上出现"一块跟着光标跳的放大画面"、
    光标自己跳，都属正常；结束时窗口被销毁、光标还原到原处。
  - 倍率/遮罩这些值是用例**驱动插件设置面板控件**写进配置的（用户真实路径），但配置目录已被
    重定向到临时沙盒（见本节开头"APPDATA 重定向"），**不会污染你的 `%APPDATA%/WinEase/config.ini`**。
  - 这组也解释了为什么"CPU 对照"能跑：它先测"关掉放大镜、同样时长"的空转基线，再用
    `QueryProcessCycleTime` 比 CPU 周期（`GetProcessTimes` 的 15.6ms 分辨率测不出这个量级，
    详见 [`traps.md`](./traps.md) 踩坑 #52）。
- ⚠ **系统与媒体组用例（P2-06/P2-07）动的是"后果不可撤销"的东西，但自检本身是安全的**：
  - **电源请求全部打在记录型提权桩上**（`RecordingElevationService`）：确认 → 倒计时 →
    参数组装 → 提交请求这条链路真实跑，但"系统真的关机"这一步永远不会发生。
    所以你**不会**在跑自检时被关机或重启。
  - 「锁定」按钮**一次都不点**（点了会真的锁屏，你得重新登录）：它的"不需要提权"这条决策
    用纯函数断言，不靠点击。
  - 用例结尾会调一次 `AbortSystemShutdown`（`hasPendingShutdown`）确认**本机此刻没有挂起的关机**——
    在没有任何挂起时它只返回"没有挂起"，是安全的。**但如果跑自检时你恰好发起过关机倒计时，
    它会把那一次取消掉**（这是接口本身的语义：Windows 没有"只查不取消"的 API）。
  - P2-07 的"定时静音"任务会**真的把麦克风静音**（这是验收原文"定时静音"的内容）：
    用例开头记下原状态，结束写回并**回读确认**；中途被杀请看一眼系统声音设置。
  - 这组会等**几次真实的倒计时**（倒计时秒数由配置压到 3 秒），整组约多花 10 秒。
- ⚠ **安全与隐私组用例（P2-11）会在注册表里临时造一条假的"摄像头正在被使用"记录**：
  - 位置：`HKCU\Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\webcam\NonPackaged\C:#WinEaseSmoke#camera_smoke.exe`
    —— 它与系统「设置 → 隐私和安全性 → 摄像头」页面**同源**，所以**那期间打开那个页面会看到这个假条目**。
  - 夹具只写 `LastUsedTimeStart` / `LastUsedTimeStop` 两个时间戳，**不写 `Value`**
    （不碰任何权限语义：它不会给任何程序授权）。
  - 用例结束（以及析构兜底）会删掉它，并**读两遍确认列表里没有这条记录**。
    如果你在跑完之后仍然看到它（异常中断的情形），手动删掉那个键即可。
  - 这组还会把麦克风监视关掉（只验证摄像头那半段）——那只是测试的配置，
    **不会改你系统里的任何隐私开关**。
- ⚠ **开发运维组用例（P2-13）会读你的真实 hosts 文件，但绝不写它**：
  - **读**是真的（`C:\Windows\System32\drivers\etc\hosts`，普通权限即可），
    所以"面板里显示的就是磁盘上的内容"这条断言是真的；
  - **写**全部打在记录型提权桩上：`writeHosts` 请求只被记录下来，不落盘。
    所以你跑完自检后 hosts 一定没有变化，也不会多出备份文件。
  - 自检期间**不会**碰提权助手（不需要它），因此也**不会弹 UAC**。
  - 想验"真落盘 + 备份还原"就得让助手在运行：`.\build\bin\elevation_smoke.exe --with-uac`
    会拉起常驻的 helper（弹一次 UAC），之后那部分代码路径才会被真实执行（详见 ROADMAP 的未覆盖项说明）。
- ⚠ **开发运维组里的 P2-14 会真的写一个"自检专用"用户环境变量，但绝不动你的 PATH**：
  - 它往 `HKCU\Environment` 写一个叫 `WINEASE_SMOKE_ENVVAR`（值形如 `winease-<时间戳>`）的变量，
    读回来确认、然后**删掉**，收尾还会再断言一次"没留下任何东西"。
    这期间会广播 `WM_SETTINGCHANGE`（正常行为，没有任何程序会因此出问题）。
  - **你的 `Path` 变量一个字节都不会变**：自检只读 PATH、只对编辑区做去重，
    并且专门有一条断言盯着"点「去重」之后用户 PATH 没变"。
  - 系统级变量（HKLM）的写入打在提权桩上，**不会真写**，也**不会弹 UAC**。
- ⚠ **P3 组用例（P3-14 USB 管控）会枚举你插着的可移除存储设备，但绝不弹出它们**：
  - 枚举是**只读**的（SetupAPI + `IOCTL_STORAGE_QUERY_PROPERTY`），不会改动任何设备状态；
  - **自检一次都不会执行"安全弹出"**：你接的移动硬盘上可能有正在写入的数据。
    用例只验"判据对不对""界面与平台层一致不一致""没勾选确认时按钮是不是禁用"；
  - 想验真正的弹出，请在插件面板里手测（那里有二次确认，失败时会告诉你是哪个应用/服务在占用）；
  - ⚠ 顺带一条实测结论：**USB 移动硬盘读不到温度**（`ERROR_IO_DEVICE`，USB-SATA 桥不转发
    温度 IOCTL），内置 NVMe 可以 —— 所以 P3-07 的磁盘温度在移动硬盘上会如实报"不可用"。
- ⚠ **P3-09 音量混合器组会"自己造两个播放器"—— 在默认输出设备上短暂产生两份静音音频流**：
  - 验收原文是"对 **3 个同时播放的应用**分别调节互不影响"，而机器上通常一个能控的应用都没有
    （"系统声音"那一份永远存在，但它测不出"按应用调音量要覆盖该应用**全部**会话"这条路径）。
    所以自检用**本 exe 自己的子进程**（`--audio-session`，最小 WASAPI 渲染客户端，灌静音数据）
    起了**两份**真实会话：两个不同进程、同一个进程名。
  - **听不见声音**：子进程写的是静音数据；**不会改系统主音量**（这一点本身就是一条断言——
    "调会话音量不能动端点音量"，用例前后各读一次主音量做 A/B）；
    也**不会碰你正在播放的其它应用**（它只对 `feature_smoke.exe` 这一行动手，
    动完还把两份探针会话的音量写回原值）。
  - 副作用边界：① 输出设备上会短暂多出两个音频流（任务栏音量合成器里可能闪一下
    "feature_smoke.exe"）；② 探针进程随用例结束而退出，会话随之消失；
    ③ 若用例中途被杀，最多留下一个到点自行退出的子进程（探针有**自愈超时**）与
    "多出的一行会话"，不会残留任何系统设置。
  - ⚠ 无音频输出设备时，这组的前置条件会**如实报失败**并提前返回（不静默跳过）。
- ⚠ **P3-11 媒体控制面板组会"自己造一个播放器"—— 系统媒体列表里短暂多一条"WinEase 自检曲目"**：
  - 验收原文是"控制 Chrome / Spotify / 系统播放器均可识别"，而机器上通常一个播放器都没开着
    （拿"没有会话"当被测对象只能测到一半：只能验"没会话时面板怎么写"）。
    所以自检用**本 exe 自己的子进程**（`--media-session`）扮演 SMTC **发布方**：
    走桌面互操作接口 `ISystemMediaTransportControlsInterop::GetForWindow` 注册一份
    **货真价实的媒体会话**（自设 AUMID `WinEase.FeatureSmoke.MediaProbe`），带曲目/艺术家/专辑、
    3:00 的进度、一张纯色封面，并接管"播放/暂停"按钮事件与"跳转位置"请求
    （收到的事件逐条落盘，这就是"请求确实送到了播放器"的硬证据）。
  - **听不见声音、不放任何东西**：探针只是个"媒体会话"，没有音频流；
    它**不改系统音量、不改主题、不碰你的文件**，也不动你正在播放的其它播放器。
  - **媒体键会打到探针身上（这是有意为之）**：探针会话是"当前会话"，所以这一组发的
    `VK_MEDIA_*` 由系统路由给它（否则就会去暂停你正在放的音乐）。用例结束后探针进程退出、
    会话随之消失。
  - 副作用边界：① 任务栏的媒体浮出控件里可能闪一下"WinEase 自检曲目"；
    ② 桌面上会短暂出现一个**不抢焦点**的小窗口（互操作接口要求"给一个窗口"，位置在屏幕右下角）；
    ③ 探针进程随用例结束而退出（另带 2 分钟硬生命周期兜底），不留任何系统设置。
  - ⚠ 若系统里另有设备/播放器正在播**媒体**（浏览器放音频也算），"探针是当前会话"这条前置可能不成立
    → 这一组的媒体键断言会**如实报失败**（不会静默跳过），此时请先停掉那个播放器再跑
    （同窗口组的"别动鼠标"是同一类前置纪律）。
  - ⚠ 第三方播放器的**真实**会话无法自动验（机器上未必装着，也不该让自检去点开用户的播放器）：
    想人工复验，随便放点东西 → 打开本功能面板 → 列表里应当出现该播放器 → 点「播放/暂停」。

`theme_smoke` 的浅色场景会**另起一个子进程**（`--child-light-probe`）从零启动，
因为 `QWindowsTheme` 只在启动时读一次色彩方案，本进程内改注册表无法把自己变成浅色系统
（详见 [`traps.md`](./traps.md) 踩坑 #23）。

`elevation_smoke` 会把真实的 `ElevationClient` 编入测试目标并连接常驻的
`WinEaseHelper.exe`（管理员权限）。**重新编译 `WinEaseHelper` 目标前必须先
`--quit-helper`**，否则提权进程占用 exe 会报 LNK1168；且普通权限无法结束提权进程
（只能经它自己的 `quit` 白名单操作或一次 UAC 提权 taskkill）。

### ⚠ 为什么不能用 `Debug`

`D:\Qt` 是 **release-only** 安装（只有 `Qt6Core.dll`，没有 `Qt6Cored.dll`）。
MSVC 下 **Debug 与 Release 的 STL 类型布局与 CRT 堆不兼容**，
因此用 Debug 编译的程序去调用 Release 版 Qt 中"返回/接收 STL 类型"的接口
（`std::wstring` / `std::string` / `std::vector` / `std::function` 等）会**内存损坏**。

**实测症状（本项目真实踩到）**：

```
QString s = QStringLiteral("WinEase 中文测试");   // 12 个 UTF-16 单元
std::wstring w = s.toStdWString();               // ← 实测 size() 为 15（应为 12）
QString back = QString::fromWCharArray(w.c_str(), 15);  // ← 结果为空串
// 进程退出时：访问违例（exit 0xC0000005）
```

这种问题**极其隐蔽**：返回值看起来可能"碰巧正确"，直到退出或下一次内存分配才崩溃。

已验证结论：

| 配置 | 跨模块 STL 互操作 | 自检结果 |
|---|---|---|
| `Debug` | ⚠ 异常（12→15→空串） | 退出时访问违例 |
| `RelWithDebInfo` / `Release` | ✅ 正常 | **103 / 103 通过，exit 0** |

**工程已做的防护**：
1. 根 `CMakeLists.txt` 在配置阶段检测 `Qt6Cored.dll` 是否存在，缺失时打印明确警告并给出正确命令；
2. `win32_smoke` 自检中有「跨模块 STL 互操作」诊断项，配置不匹配时以**警告**形式指出并说明原因。

### 如果确实需要 Debug 级调试体验

只有两条路：
1. 用 `RelWithDebInfo`（推荐）——有完整调试信息，可断点、可看变量，仅少了 `_DEBUG` 断言；
2. 另外编译/安装一套 **Debug 版 Qt**（会产生 `Qt6Cored.dll` 等），再用 `Debug` 配置构建。

---

## 7. 一次性环境检查清单

- [x] 确认 **QtMultimedia / QtMultimediaWidgets** 已安装（2026-09-13 完成，工程已自动探测）
- [x] 确认 Windows SDK 为 **10.0.26100.0**（本机已满足）
- [x] 确认 `D:\Qt\bin` 在 `PATH`（供 `qt-configure-module.bat` 与运行期 DLL 查找）
- [x] 安装 **PawnIO.Setup**（2026-09-14 预研实测：驱动与服务都已就位）
- [ ] 将 **`PawnIOLib.dll`** 放到 `build\bin\` 或系统 `PATH`（未放时温度功能自动降级）
      ← **目前就卡在这一条**：驱动在、用户态库缺，所以 CPU 温度暂时不可用
- [ ] （不推荐）补装 **QtPdf** —— 方案 A 已论证成本过高
- [ ] 确认温度监控所需的 CPU 家族已被 PawnIO 模块覆盖（Intel MSR / AMD Zen）

---

## 8. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-09-15 | **新增打包与发布**（另见 `docs/DISTRIBUTION.md`）：`cmake --build build --target winease_installer` 出一条命令产出**单文件离线安装程序**（21.3 MB，内含 Qt + VC++ 运行库 + 38 个插件 + 桥接）。相关外部工具：`windeployqt`（随 Qt）、`makecab`（Windows 自带）、`dotnet`（仅在开启 `.NET` 随包附带的实验开关时需要）。新增自检 `installer_smoke.exe`（打包链路 17 项，含**逐 PE 依赖审计**）。⚠ 两条硬约束记进 §5.2 那条纪律的家族：构建/打包脚本 `scripts\\stage_dist.ps1` **必须纯 ASCII**（PowerShell 5.1 按 ANSI 读 `.ps1`，中文注释会让脚本解析失败 → 踩坑 #91）；`.NET` 运行库默认**不**随包（自包含 IJW 在原生宿主里 fail-fast 0xC0000409 → 踩坑 #88），安装器改为主动检测并如实说明影响面 |
| 2026-09-15 | **§5 结论翻转 + 新增 §5.1 / §5.2（用户要求"P3-07 改用 C++/CLI 桥接"）**：GPU 温度不是"做不了"，而是**不必自己调厂商 SDK** —— 统一走 C++/CLI 桥接 `LibreHardwareMonitorLib`（与 `refrences/LiteMonitor` 同一个包 0.9.6），CPU / 主板 / GPU / 风扇四类读数一次解决，且 GPU 走用户态接口、**普通权限即可**。§5 保留厂商 SDK 对照表作为"为什么是库在做这件事"的留档。新增 **§5.1**（四个依赖谁缺了会怎样、`build/bin` 的部署形态、管理员权限前提、构建期三个坑）与 **§5.2**（带超时的构建/自检脚本 `build\build_feature_smoke.ps1` / `build\run_smoke.ps1`，以及"`.ps1` 必须 ASCII"这条硬约束）。PawnIO 方案随之下线（原 §4 保留为"为什么没走这条路"的记录） |
| 2026-09-13 | 初版。确认 Windows SDK 自带 C++/WinRT，将 PDF/OCR/媒体/录屏定为零安装方案；PawnIO 定为温度监控方案；论证 QtPdf 补装成本过高 |
| 2026-09-13 | 用户完成 QtMultimedia 补装，工程自动探测生效（`QtMultimedia : ON`）；评估 MinGW 迁移后决定保留 MSVC；补充 §3.3 第 4 条"DIP 感知必须显式声明" |
| 2026-09-15 | **P3-07 第一批落地后对 §4.5 的补正**：磁盘温度走 **`\\.\PhysicalDriveN` + SDK 枚举 `StorageDeviceTemperatureProperty`**（本机实测 40~46°C 可读，与 §4.5 的 39°C 一致）。⚠ 增补一条实现侧的硬约束：**属性 ID 不要用数字字面量** —— 曾手抄成 `22`，而本机 SDK 的 `StorageDeviceTemperatureProperty` 是 **52**，同一个 IOCTL 因此返回 `ERROR_INVALID_FUNCTION`，把"本来就可读"误判成"驱动不支持"（踩坑 #72）。另：型号改由 `StorageDeviceProperty` 的厂商/产品串拼出（不再依赖 SetupDi） |
| 2026-09-14 | **新增 §4.5 预研实测结论**：P3 技术预研（`tests/p3_spike`）实测出四条与本文件相关的事实 —— PawnIO 驱动**已安装**（服务+设备）、用户态 `PawnIOLib.dll` **缺失**（CPU 温度卡在这一条）、CPU 为 `GenuineIntel` family 6（走 `IntelMsr`）、**磁盘温度原生 IOCTL 已可读**（`PhysicalDrive0 39°C`）。§4.4 末尾的"P3-03 技术预研 spike"编号笔误改成 **P3-07**；§7 清单勾掉"安装 PawnIO.Setup"；§6 自检命令补上 `p3_spike.exe`（并注明其退出码语义与其它自检不同） |
| 2026-09-13 | **§6 改写**：`D:\Qt` 为 release-only 安装，实测 Debug 构建会导致跨模块 STL 互操作内存损坏（12→15→空串 + 退出违例），构建配置改为 **`RelWithDebInfo`**；§3.3 补充 WinRT 使用的两个实际约束（`ApiInformation` 在未打包程序中崩溃、`RoGetActivationFactory` 应用 `IID_IUnknown` 探测） |
