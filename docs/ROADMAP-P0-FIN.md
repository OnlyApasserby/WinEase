## P0 基础设施

- [x] **P0-1 `WinEaseWin32` 共享静态库** — 难度 L · 无提权（批次 A + B 均已交付）
  - 位置：`src/win32/`，静态库，供主程序与所有插件链接
  - 设计约束：**只放可复用平台原语、不放功能逻辑；纯函数式、无全局状态**
    （本库会被静态链接进主程序与每个插件 DLL，任何全局状态都会各持一份）
  - CMake：链接 `windowsapp.lib`（批次 B 用）；定义 `_WIN32_WINNT=0x0A00`
  - 说明：**避免 20 个插件各写一遍 COM / Win32 样板代码**
  - **批次 A —— 已完成（2026-09-13）**
    - [x] `Win32Error`：常见错误码内置中文表 + `FormatMessageW` 回退 + `HRESULT_FROM_WIN32` 还原
    - [x] `ComApartment`：STA/MTA RAII 守卫，正确处理 `S_FALSE`（Qt 已提前 OleInitialize）
      与 `RPC_E_CHANGED_MODE`；提供只读套间状态探测
    - [x] `WindowUtils`：窗口信息/几何/Z 序/扩展样式/透明度/显示器/DPI 与坐标换算
      - 含 `visualWindowRect()`（剔除 Win10/11 不可见投影边框，分屏对齐必需）
      - 含 `OpacitySnapshot` / `ExStyleSnapshot` 快照与还原（满足"停用后不留副作用"）
    - [x] `RegistryUtils`：`RegistryKey` RAII + QVariant 双向转换 + 64/32 位视图 +
      `broadcastSettingChange()` / `refreshDesktop()`
    - [x] `ProcessUtils`：进程枚举/路径/名称/父进程/提权判定/结束进程
    - [x] `ScreenCapture`：区域/显示器/窗口/虚拟桌面抓取 + 单点取色 + 取色预览块
      （`CAPTUREBLT` 抓分层窗口；`PrintWindow(PW_RENDERFULLCONTENT)` 抓被遮挡窗口）
    - [x] 自检程序 `tests/win32_smoke`（**68 项断言全部通过**，退出码 0）
  - **批次 B —— 已完成（2026-09-13）**
    - [x] `ShellUtils`：`IShellItemImageFactory` 缩略图、`IShellItemImageFactory` 回退用
      系统图像列表取高清图标（`SHIL_JUMBO`）、`IFileOperation` 回收站/永久删除、
      资源管理器定位、已知文件夹、文件类型描述与 Content Type
    - [x] `SystemInfo`：内存（`GlobalMemoryStatusEx`）、CPU（PDH 每核心 + 总体）、
      网络吞吐（PDH，见下方说明）、GPU 利用率（PDH `\GPU Engine(*)\Utilization`）、
      **物理磁盘温度**（`IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceTemperatureProperty`，
      零额外驱动）、系统运行时间、电池状态
    - [x] `CoreAudio`：`IAudioEndpointVolume` 主音量 / 静音 / 步进调节 / 切换静音，
      输出与输入双向；pimpl 隐藏 COM 类型
    - [x] `DisplayControl`：DDC/CI 亮度与对比度（能力探测 + 失败降级）、
      gamma 快照与还原、色温→通道增益（Tanner Helland 近似并归一化）、
      **基于 baseline 计算避免多次调节叠加失真**
    - [x] `WinRtSupport`：`ensureWinRtReady`（COM 套间前置检查）、
      `probeWinRtClass`（运行时类可用性探测）、`runWinRt`（**异常边界**：hresult_error /
      std::exception / 未知异常全部转中文错误，禁止穿透）、`describeWinRtFailure`
    - [x] 自检扩展：新增 5 组共 35 项断言（**Release 下 103/103 通过**）
  - **从批次 B 移出的项**
    - [ ] `PawnIoClient`：PawnIO 动态加载封装 → **移到 P3-07 前置**。
      原因：`pawnio_execute` 的精确函数签名与模块 `.bin` 名尚未确认（见 P3-07 预研子项），
      在签名未确认前写出的封装只会是猜测，不如等到预研时一次做对
    - [x] `WinRtSupport` 的字符串转换辅助 → **已移除**。
      原因：自检发现环境中 `QString::toStdWString()` 行为异常，隔离测试证明问题不在本层代码；
      根因已定位为"Debug 构建 + release-only Qt"的 STL 不兼容（见踩坑记录 #9）。
      Release 下该互操作正常，但为保持 API 可信度，暂不提供该辅助，待 P2-02 首次真实消费
      WinRT 字符串时按真实数据校验后再补

- [x] **P0-2 `HookService` 全局钩子服务** — 难度 L · 无提权 ✅ 已完成（2026-09-13）
  - 位置：抽象接口 `src/sdk/HookService.h`；实现 `src/app/core/HookServiceImpl.*`
    （比原计划多拆出一个 SDK 接口层，理由见下）；经 `PluginServices::hookService()` 暴露
  - 已完成内容
    - [x] 事件模型：`HookEvent`（键盘 / 鼠标按键 / 鼠标移动 / 滚轮），
      携带虚拟键码、扫描码、扩展键、系统键、**注入标记**、修饰键快照、时间戳、屏幕坐标；
      为让插件不必包含 `windows.h`，Win32 原始字段被平铺出来
    - [x] `HookListener` 订阅基类：**析构自动退订**，可选覆盖 `onServiceStopped` / `onDegraded`
    - [x] 两种投递模式（本项最重要的设计决策）
      - `Queued`（默认）：投递到订阅者线程，**输入永不被订阅者拖慢**，代价是不能拦截
      - `Direct`：在钩子线程同步调用，可返回 true 吞掉事件，可配合注入实现"拦截并改写"
    - [x] **拦截并改写**：`Direct` 吞掉原事件 + `sendKey` / `sendMouseButton` /
      `sendMouseWheel` / `sendMouseMove` 补发新事件（补发事件带 `isInjected`，
      插件据此避免自我回灌成死循环）
    - [x] 修饰键：每个事件携带 `modifiers` 快照；`modifiersMatch()` 做**恰好匹配**
      （避免 Ctrl+Shift+滚轮 被误判为 Ctrl+滚轮）
    - [x] 看门狗：Direct 回调超 `callbackBudgetMs()`（默认 50ms）连续 3 次即
      **永久降级为排队投递**，并通过信号 + `onDegraded` 双通道告知
    - [x] 钩子被摘除后自动重装：读取 `HKCU\Control Panel\Desktop\LowLevelHooksTimeout`
      作为硬阈值；回调超过它即判定"Windows 已静默摘除钩子"并发起重装（带在途去重）
    - [x] 高频保护：鼠标移动按订阅者限制积压（256 条），超出丢弃并计数，
      避免订阅者卡住时无限吃内存
    - [x] 统计：各类事件计数、拦截数、丢弃数、最大回调耗时、降级数、重装数
  - **关键实现决策**
    1. **钩子装在专用线程而不是主线程**
       低层钩子回调运行在"安装它的线程"上。装在主线程时，任何 UI 卡顿都会连带拖慢
       全局输入；放到专用线程后，主线程的 UI 与输入路径彻底解耦。
    2. **抽出 SDK 接口层**（原计划只写 `core/HookService.*`）
       插件必须能订阅事件，但**绝不能**自己装钩子（重复回调 + 互相拖慢）。
       把抽象接口与事件结构放在 SDK，实现留在主程序，
       与 `PluginServices` 的分层保持一致；文件名叫 `HookServiceImpl` 以避免与
       SDK 的 `HookService.h` 混淆。
    3. **排队投递用 lambda 形式的 `QueuedConnection`，而不是自定义 `QEvent`**
       见踩坑记录 #15。
  - 验收证据（`tests/hook_smoke`，**31/31 通过，exit 0**）
    - [x] 订阅者收到全局按键/滚轮/鼠标移动事件；`isInjected` 标记、虚拟键码解析正确
    - [x] 排队投递确实投递到**订阅者线程**（比对线程 id），而非钩子线程
    - [x] **"慢订阅者不阻塞输入"**：订阅者每次睡眠 150ms，注入 5 组按键总耗时 **1ms**
      （若为同步投递至少 750ms）—— 这条直接对应验收标准
    - [x] 看门狗：80ms 回调连续 3 次超预算后自动降级（实测记录 87ms），
      降级后仍以排队方式收到事件，服务继续运行
    - [x] 订阅者析构自动退订；显式退订；`stop()` 幂等
    - [x] 未发生误判重装（`reinstallCount == 0`）
    - [x] 主程序集成：日志确认 `全局输入钩子已安装 (回调预算 50 ms)`，界面正常
  - 安全设计（测试自身）
    测试会注入合成输入，因此**第一步就安装一个吞掉全部注入事件的拦截器**，
    并且只注入"即使泄漏也无害"的组合（F24 / 滚轮 ±120 净零 / 鼠标抖动 1px）。
    实测拦截 28 次，桌面未受影响。
  - ⚠ **环境依赖（2026-09-13 实测发现）**：`isInjected 全部为真` 这条断言要求
    "本机在测试期间除注入事件外没有任何真实输入"。本机存在持续产生**真实鼠标移动**
    的输入源（在一次只注入 2 次移动的采集里收到 34 次移动、统计口径下累计 164 次移动），
    因此该条会稳定失败（exit 1），**与本工程代码无关**（该目标不链接主题/主程序代码）。
    在真正空闲的机器上该断言成立。若要让它可移植，应把断言收窄为
    "注入事件全部被标记为 injected"（不再要求总回调数等于注入数）。

- [x] **P0-3 `OverlayKit` 悬浮窗基类** — 难度 M · 无提权 ✅ 已完成（2026-09-13）
  - 位置：基类 `src/sdk/OverlayWindow.h/.cpp`（主程序与插件共同链接，插件直接继承）；
    生命周期宿主 `src/sdk/OverlayHost.*`（**P1-C 起从 `src/app/core/` 迁入 SDK**：
    插件 DLL 调用不了主程序的私有符号，详见 P1-C 的前置修正）；经 `PluginServices::overlayHost()` 暴露
  - 已完成内容
    - [x] 窗口行为：无边框 + 置顶 + 工具窗口（不进任务栏、不进 Alt+Tab）+ 半透明背景 + 显示不激活
    - [x] **点击穿透**（默认开）：窗口标志（`Qt::WindowTransparentForInput`）与
      直接改 `WS_EX_TRANSPARENT|WS_EX_LAYERED` 双路径；**运行时切换不重建原生窗口**（无闪烁）
    - [x] **不抢焦点**（默认开）：`WS_EX_NOACTIVATE`，实测显示悬浮层不改变前台窗口
    - [x] 放置：`coverMonitor()` / `coverMonitorAt()` / `coverAllScreens()` /
      `setOverlayGeometry()`，参数一律为**物理像素**，按**目标显示器**的缩放系数换算
    - [x] DPI：绘制契约用**逻辑坐标**，painter 与后备位图自动携带 `devicePixelRatio`
      → 绘制表面按原生分辨率分配（这是"不模糊"的真正机制）
    - [x] 后备位图（`backingLayer()`）：轨迹类增量绘制，避免整屏重绘
    - [x] 脏矩形更新：`requestOverlayUpdate(logicalRect)`
    - [x] 生命周期：`closeOverlay()` 先隐藏 → 走一次事件循环 → `deleteLater()`；
      窗口带标记属性，宿主与自检可枚举检测残留
    - [x] 宿主按 `ownerId`（插件 id）批量回收；插件被停用时**主程序兜底回收**
    - [x] 内容控件通道 `setContentWidget()`（可嵌 `QQuickWidget`，基类不依赖 Qt6::Quick）
  - **关键实现决策**
    1. **用 QWidget 而不是 QML —— 对原始设想的一处有意偏离**，五条理由：
       ① 插件契约本就是 QWidget 体系（`createSettingsWidget(QWidget*)`），C++ 基类可直接继承，
       而 5 个使用方的绘制内容都只是简单二维矢量图形，用 QML 收益为零、风险很大；
       ② 引入 Qt6::Quick 会让主程序同时存在两种渲染栈，且**每个插件 DLL 各有一个 QML 引擎**
       ——与本工程"绝不允许多份全局状态"的核心原则直接冲突；
       ③ DPI 处理必须集中在一处：`WinEaseWin32` 的"物理像素 + 屏幕坐标"契约与 QWidget 的
       `devicePixelRatio` 天然对齐，QQuickWindow 有自己的 DPR 规则，混用正是"高 DPI 模糊"的最大风险源；
       ④ 性能可达：3 个使用方是低频静态绘制、1 个瓶颈在抓屏、1 个可用后备位图增量绘制；
       ⑤ 保留逃生通道：需要 QML 时用 `setContentWidget(QQuickWidget*)` 即可。
       （已确认 Qt6Quick / Qt6Qml / Qt6QuickControls2 / qml 目录均已随 D:\Qt 编译安装，是"选不用"而非"没有"）
    2. **基类放 SDK 而不是主程序**：插件必须能继承它；放在主程序里插件就无法使用
    3. **跨 DPI 必须"每显示器一个悬浮层"**：一个原生窗口只能有一个 `devicePixelRatio`。
       若用单个悬浮层横跨两台不同缩放的显示器，只有窗口实际所在的那台是原生清晰的，
       另一台上会被 DWM 整体缩放而模糊——这是 Windows 的限制，Qt 无法绕过。
       因此宿主提供 `createOverlaysForAllMonitors()` 一次性为每台显示器各建一个
  - 验收证据（`tests/overlay_smoke`，**34/34 通过，exit 0**；本机单屏 2560×1600 @150%）
    - [x] **"全屏透明覆盖层不影响下层窗口点击"**：`WindowFromPoint` 在显示器中心点
      → 创建前与穿透开启时为同一个窗口（`0x110992`）；
      **反向对照**：临时关闭穿透后命中悬浮层自身 → 证明探针有效、不是假通过；
      重新开启穿透后再次命中 `0x110992`
    - [x] **"跨 DPI 不模糊"**：目标 `(0,0 2560x1600)` → 实际 `(0,0 2561x1601)`（≤1px 且**完全包含目标**，无缝隙）；
      逻辑尺寸 `1707x1067`；绘制 DPR `1.50` == 显示器缩放；后备位图 `2561x1601 @ DPR 1.5`
    - [x] 不抢焦点：显示前后前台窗口不变，且悬浮层自身不会成为前台窗口
    - [x] 销毁不留残影：`EnumWindows` 按标记属性统计，关闭后**残留 0 个**；`closeAll()` 幂等
    - [x] 宿主归属：按 ownerId 批量回收（回收 2 个、剩余 1 个）
    - [x] 主程序集成：接入 `AppContext` 后启动与界面正常
  - 未自动覆盖：**跨异构 DPI 双屏**（本机只有单屏 150%）。每显示器的换算已逐台验证，
    真正跨屏需双屏异构 DPI 手工复核

- [x] **P0-4 `WinEaseHelper.exe` 提权助手** — 难度 L · ▲提权 ✅ 已完成（2026-09-13）
  - 位置：`src/helper/`（独立 exe，requireAdministrator 清单 → UAC 弹一次并常驻）；
    主程序侧客户端 `src/app/core/ElevationClient.*`；SDK 契约 `src/sdk/ElevationService.h`，
    经 `PluginServices::elevationService()` 暴露给插件
  - 通信：命名管道 `WinEase.Helper.v1`，JSON 行协议（请求 `{"id","op","args"}`，
    应答 `{"id","ok","error","data"}`）；helper 端为 **Win32 原生管道服务**
    （CreateNamedPipeW + 显式安全描述符，见踩坑 #24），客户端仍是 `QLocalSocket`
  - 安全设计（全部 fail-closed）
    - **调用方校验**：`GetNamedPipeClientProcessId` 取管道对端真实 PID（内核元数据，
      客户端无法谎报）→ 反查对端 exe → 必须与 helper **同目录** →
      有 Authenticode 签名则必须验证有效（`WinVerifyTrust`，无签名放行并记日志）
    - **白名单**：`ping` / `writeHosts`（append/replace，写前自动备份）/
      `setEnvVar`（machine|user，写后广播 WM_SETTINGCHANGE）/
      `killProcess`（关键系统进程黑名单：System/Registry/smss/csrss/wininit/winlogon/
      services/lsass/memcompression；禁止结束 helper 自身与调用方）/
      `powerAction`（lock/logoff/sleep/hibernate/reboot/shutdown）/
      `quit`；白名单外一律拒绝
    - **参数硬上限**：hosts 单条 4096 字符 / 整体 1 MiB / 环境变量名 255、值 32 KiB；
      请求总量 2 MiB；客户端读超时 30s 断开（防卡死服务线程）
    - 替换 hosts 用 **base64 字节级通道**（JSON 文本会剥掉 BOM，见踩坑 #26）
  - 验收证据（`tests/elevation_smoke`，**29/29 通过，exit 0**；测的是真实 ElevationClient）
    - [x] 普通权限直写 HKLM 被拒（对照项，证明提权通道必要）→ 经助手完成
      **hosts 追加/字节级恢复**与 **HKLM 环境变量写入/删除**（ROADMAP 验收标准 1、2）
    - [x] 伪造调用方（TEMP 目录副本）被拒绝且原因是安全校验（验收标准 3）
    - [x] 白名单外操作、非法参数、关键系统进程（pid=4）全部被拒
    - [x] `ping.elevated == true` 证明 requireAdministrator 清单真实生效
    - [x] UAC 拉起 → 30s 内就绪 → 常驻；`quit` 白名单操作可优雅退出
    - [x] 运维工具：`--restore-hosts <备份>` 可按备份字节级还原 hosts
  - 已知限制：电源操作 reboot/shutdown 由 InitiateSystemShutdownExW 执行（默认 5s 倒计时），
    自动化验收只验证参数校验路径，真实关机需人工触发

- [x] **P0-5 插件契约扩展 + 异常边界** — 难度 M · 无提权 ✅ 已完成（契约 3/4 随 P0-2/P0-3/P0-4 落地，异常边界本轮补齐）
  - [x] `PluginServices::hookService()`（随 P0-2）
  - [x] `PluginServices::overlayHost()`（随 P0-3）
  - [x] `PluginServices::elevationService()`（即 `executeElevated()`，随 P0-4；
        同步接口 `ElevationService::execute(op, args)`，白名单语义见 P0-4）
  - [x] 插件加载/调用加 **SEH + C++ 异常边界**：一个插件崩溃不能拖垮主程序，
        崩溃插件自动标记为 `Failed` 并在卡片上显示原因
  - 交付物（2026-09-13）
    - **`src/app/core/PluginGuard.h/.cpp`（新增）**——两级边界：外层
      `catch(const std::exception&)` / `catch(...)`，内层 `__try` / `__except`。
      `Guard::invoke(fn, &report)` 一次调用同时被两者保护，返回 `CrashReport{kind, code, description, detail}`；
      `describeSehCode()` 把 SEH 码翻成中文（访问违例 / 除零 / 特权指令 / 栈溢出 / 堆损坏 / `/GS` 越界…）。
      C++ 异常返回 `what()` 原文，SEH 返回中文原因 + 十六进制码 —— 两者都**可直接展示给用户**
    - **`PluginManager` 接入边界 + 隔离机制**：所有触达插件的路径（元信息探针、`initializePlugin`、
      `setEnabled`、`shutdownPlugin`、`createSettingsWidget`、`dispatchHotkey`）一律经 `Guard::invoke()`
    - **元信息探针 + 缓存（`PluginMeta`）**：加载期守卫调用全部 getter 并缓存。
      插件崩溃后**不再调用它的任何方法**——错误上报、卡片展示、搜索、托盘菜单全部走缓存读
      （`idOf/nameOf/categoryOf/pluginFailureReason`），这是"隔离"能真正成立的关键
    - **隔离语义**：崩溃插件对外表现为"未启用"（`isPluginEnabled == false`）、
      进入 `failedPluginIds()`、配置里强制停用（下启动不再拉起）、
      后续 启用/快捷键分发/设置面板 请求一律被拒；`pluginCrashed(pluginId, reason)` 通知界面
    - **隔离后不释放**：不调用 `shutdown()`、`delete` 静态插件、不 unload 插件 DLL
      （在已损坏对象上跑析构属于二次崩溃，见踩坑 #30）；代价是该 DLL 常驻到进程退出
    - **界面接入**：`FeatureCard::markFailed()`（卡片状态区转红并显示中文原因，此后 `refresh()` 只更新状态区、
      不读插件；`m_searchText` 保留以便崩溃功能仍可被搜到）、`MainWindow` 监听 `pluginCrashed`、
      `SystemTrayManager`/`HotkeySettingsDialog`/统计栏/悬浮层兜底回收路径全部改为缓存读
  - 验收证据（`tests/plugin_smoke`，**36/36 通过，exit 0**；把真实 `PluginGuard` + `PluginManager` 编进测试目标）
    - [x] 边界原语自证：C++ 异常识别为 `CppException` 且取到 `what()`；访问违例识别为 SEH `0xC0000005`；正常调用返回 true 且无报告
    - [x] `initialize()` 抛异常 / 触发访问违例 → 注册失败、**进程存活**，且 `loadErrors()` 里给出中文原因（验收标准"原因可展示"）
    - [x] `onEnable()` 崩溃 → 返回失败、标记已失败、`pluginCrashed` 已发出、失败原因指出崩在 `onEnable`
    - [x] 隔离硬约束：再启用 / 快捷键分发 / 设置面板 三个入口全部被拒，且**插件调用计数不再增长**（证明"不再被调用"）
    - [x] `createSettingsWidget()` 崩溃 → 返回 nullptr 且被隔离（主程序存活）
    - [x] 其余功能完全不受影响：正常插件仍可启用、快捷键分发成功、`setAllEnabled(false)` 正常批量停用
    - [x] 卸载阶段：已隔离插件**不被调用 `shutdown()`**，正常插件 `shutdown()` 执行 1 次，进程正常退出
  - 已知限制：元信息在探针之后不再重复校验（插件若在元信息 getter 里随机崩溃，仍可能崩在界面刷新路径上）；
    SEH 恢复后出错帧内的 C++ 对象不析构，因此崩溃插件会泄漏少量资源到进程退出（这是隔离的必然代价）

- [x] **P0-6 工程约定落地：`QT_NO_KEYWORDS` + COM/WinRT 初始化** — 难度 M · 无提权 ✅ 已完成
  - 原因：`winrt/*.h` 会使用 `signals` 等标识符，与 Qt 关键字宏冲突；WinRT 还要求线程套间模型正确
  - 已完成动作（2026-09-13）
    - 根 CMakeLists 全局启用 `QT_NO_KEYWORDS`（含原因说明，避免后人误删）
    - 全工程 11 个头文件 + 10 个实现文件的 `signals/slots/emit` 统一替换为
      `Q_SIGNALS/Q_SLOTS/Q_EMIT`（纯机械替换，全量编译验证通过）
    - `ComApartment` 提供 RAII 套间守卫；`WinRtSupport::ensureWinRtReady()` 做调用前置检查
  - **验证结论**：`WinRtSupport` 已真实引入 `winrt/base.h` 并调用 `RoGetActivationFactory`，
    编译与运行均正常 → 证明 `QT_NO_KEYWORDS` 确实消除了宏冲突（这是本项最关键的验收点）
  - 说明：**不调用 `winrt::init_apartment()`**。Qt 的 Windows 平台插件已经建立套间，
    再次 init 会因套间模型冲突抛 `RPC_E_CHANGED_MODE`；C++/WinRT 只要求"线程已初始化 COM"，
    该点由 `ComApartment` 保证

---
