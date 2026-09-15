## P1 低风险快赢（14 项）

> 目标：无提权、无未公开 API，快速跑通"插件真的能用"的闭环，验证 UI 手感与快捷键体系

### P1 执行计划（2026-09-13 制定）

**分批推进**（每批一个可验收的闭环，批次内可并行）：

| 批次 | 任务 | 共同点 | 自检目标 |
|---|---|---|---|
| **P1-A ✅ 已交付** | P1-01 置顶 / P1-02 分屏 / P1-03 透明度 / P1-04 置底 | 都要先回答"操作哪个窗口"→ 共用 `WindowTarget` 原语与目标策略 | `feature_smoke`（窗口组）**72 项 / exit 0** |
| **P1-B ✅ 已交付** | P1-06 取色器 / P1-10 JSON 格式化 / P1-11 编码转换 / P1-12 端口占用 | 都是"工具面板 + 数据转换"，不碰系统状态 | `feature_smoke`（工具组）**累计 145 项 / exit 0** |
| **P1-C ✅ 已交付** | P1-07 屏幕标尺 / P1-08 焦点高亮 | 都基于 OverlayKit 悬浮层 | `feature_smoke`（悬浮层组）**累计 195 项 / exit 0** |
| **P1-D ✅ 已交付（P1 波次收尾）** | P1-05 右键菜单 / P1-09 网页搜索 / P1-13 主题切换 / P1-14 壁纸切换 | 都要写系统状态（注册表 / 桌面），**必须可完整还原** | `feature_smoke`（系统组）**累计 255 项 / exit 0** |

**新增基础设施（P1-A 前置）**
- `src/win32/WindowTarget.h/.cpp`：目标窗口解析原语（跟随鼠标 / 锁定窗口）+ 光标物理坐标。
  四个窗口插件必须行为一致，因此**不做四份实现**
- `plugins/common/WindowFeatureState.h`：窗口类插件共用的目标策略状态（模式 + 锁定窗口 + 配置读写 +
  设置面板里的"目标窗口"区块）。**头文件内不含 Q_OBJECT**（避免跨插件 moc 依赖）

**P1-B 新增基础设施（工具组前置）**
- `src/win32/InputUtils.h/.cpp`：合成按键（`sendKeyChord` / Ctrl+C / Ctrl+V）。
  取选中文本、就地替换都靠它；**UIPI 拦截时返回 false**，调用方据此回退到剪贴板。
- `src/win32/NetUtils.h/.cpp`：`GetExtendedTcpTable`/`GetExtendedUdpTable` → `EndpointInfo`/`PortOccupant`
  （端口 → PID → 进程名/完整路径）。枚举是只读操作，**普通权限即可**；只有"结束进程"才走提权助手。
- `plugins/common/TextTools.h/.cpp`：JSON/XML 格式化·压缩·转义·JSONPath 查询 + Base64/URL/哈希。
  **纯函数**，被 dev.text_format / dev.encoder 与自检**共用同一份源码**（这样逐字节断言才有意义）。
  两条实现要点：① JSON 走**保序词法扫描**而不是 `QJsonDocument` 往返（`QJsonObject` 内部按 key 排序，
  `toJson()` 会把用户的键顺序改掉）；② 错误行列号按**字符**计数、不用 UTF-8 字节偏移。
- `plugins/common/ColorFormats.h`：HEX/RGB/HSL/CMYK 与颜色文本解析（CMYK 取整规则固定在一处）。
- `plugins/common/ClipboardTools.h/.cpp`：`acquireText(useSelection)` 取"选中文本→回落剪贴板"、
  `setText()`（写入 + 读回校验 + 重试）、`pasteBack()`。三个插件共用，保证取词失败时的行为完全一致。

**P1-C 新增基础设施 / 前置修正（悬浮层组前置）**
- `plugins/common/OverlayGeometry.h`：物理屏幕坐标 ↔ 悬浮层逻辑坐标的唯一换算处
  （悬浮层的契约是"**物理像素进来、逻辑坐标画出去**"，两个插件各推一遍必然有一个错）
- ★ **`OverlayHost` 从 `src/app/core/` 移入 `src/sdk/`** —— P1-C 是第一个真正使用 OverlayKit 的
  插件批次，一上来就撞上结构性问题：插件 DLL 要调用宿主的方法，而**主程序的符号对 DLL 不可见**
  （MSVC 下 .exe 默认不导出符号），插件各编一份 `OverlayHost.cpp` 又等于"每个插件一份实现"。
  → OverlayHost 是 OverlayKit 生命周期契约的一部分，放进 SDK 随 `WinEaseSdk` 链接，
  实现只保留一份（它自身无全局状态，实例仍由主程序持有）
- ★ `OverlayHost::adoptOverlay(ownerId, overlay)`：**接管插件自建的 OverlayWindow 子类**。
  原先只有 `createOverlay()`（宿主代建，只能是基类实例），而 `OverlayWindow` 头文件里给的
  典型用法又是"继承它并重写 `paintOverlay()`"——两条路互斥，插件的子类实例没有任何
  交给宿主托管的通道，于是"插件崩溃后残留一层点不掉的置顶窗口"这个 P0-3 想防的问题防不住。
  `adoptOverlay()` 补上这条通道（重复接管同一实例只改归属，不重复登记）

**P1-D 新增基础设施 / 关键约定（系统组前置）**
- `src/win32/DesktopWallpaper.h/.cpp`：壁纸读写。**IDesktopWallpaper**（公开 COM，支持逐显示器读写、
  返回显示方式）优先，`SystemParametersInfo(SPI_SETDESKWALLPAPER)` 兜底（无 COM 时）。
  用 `initguid.h` 让 CLSID/IID 定义在本编译单元里，避免额外链接 uuid.lib。
- ★ **"可完整还原"的落点定在"停用功能"，不是"退出程序"**（P1-D 的一个明确决策）：
    · 停用功能（用户明确关掉）→ **恢复成启用前的样子**（这是本组的验收指标）
    · 退出 WinEase → **保持现状**（主题/壁纸是用户看得见的外观，不该因为退出而跳变；
      P1-14 的验收原文就写着"重启后保持"）
    · "无残留"针对的是**指向已不存在程序的引用**（如 P1-05 写的右键菜单命令），
      主题 DWORD / 壁纸路径只是一个合法的系统取值，不属于残留
- ★ **注册表菜单项的命令行引号（实测结论）**：explorer 传 `%1` 时**只有路径含空格才加引号**，
  所以命令行里自己写引号会让含空格路径被复制成带引号的字符串。
  正确写法：`cmd.exe /c for %I in (%1) do @echo %~I|clip`（`%~I` 去引号，两种路径都干净）
- `src/win32/WindowUtils::windowRectForVisualRect()`：`visualWindowRect()` 的**逆运算**。
  分屏/居中把"希望窗口看起来占的区域"换成窗口矩形时必须过它（否则每调一次窗口就缩一圈，
  见踩坑 #32）

**自检策略（`tests/feature_smoke`）**
- 与 `plugin_smoke` 同一思路，但**加载 build/bin/plugins 下真实的插件 DLL**（`QPluginLoader`），
  而不是把插件源码编进测试：验的是"用户真正装上的那个插件"
- 驱动方式用 `IFeaturePlugin::dispatchHotkey("<id>::<action>")`（公开入口），
  再回到 Win32 侧**读真实窗口状态**做断言（是否置顶 / 实际矩形 / alpha / z 序），
  避免"测试自己实现一遍逻辑"这种假通过
- 目标窗口跑在**本 exe 的 `--probe-window` 子进程**里：平台层 `isManageableWindow()` 会排除
  本进程窗口（WinEase 自己的界面/悬浮层不该被当成操作目标），同进程的测试窗口既测不到真实场景、
  也会让正确的产品行为变成"测不下去"
- 目标窗口有**两个**：主窗口 + 一个固定位置的"辅助窗口"。后者只用来制造**来自别的进程**的前台切换
  —— `WINEVENT_SKIPOWNPROCESS` 会把装钩子进程自己产生的事件过滤掉，"置底维持"用同进程窗口根本测不出
  （见踩坑 #33）
- 用例**会移动真实光标**（"跟随鼠标"模式下这是唯一诚实的驱动方式），结束后还原；
  且**每个动作之前**都必须重新把光标放回目标窗口并断言这个前置条件（见踩坑 #33）
- 每组用例结束必须**还原系统状态**并断言已还原（置顶取消、透明度回 100%、扩展样式无残留）

### 窗口与桌面

- [x] **P1-01 窗口置顶** — `window.always_on_top` · S · 无提权
  - 关键 API：`GetForegroundWindow` / `SetWindowPos(HWND_TOPMOST)` / `GWL_EXSTYLE | WS_EX_TOPMOST`
  - 交互：两种模式——「跟随鼠标下的窗口」与「锁定当前窗口」；快捷键 + 托盘都可触发
  - 验收：置顶后点击其它窗口不改变层级；插件停用后所有被置顶窗口恢复原状
  - 产出：`plugins/window_always_on_top/`。主动作切换置顶/取消；辅助快捷键 `Ctrl+Shift+Alt+P` 锁定|解锁
    目标窗口、`Ctrl+Shift+Alt+U` 取消全部置顶。每条置顶都记录 `{原置顶状态, 进程 id, 类名}`
    （还原前校验窗口没被复用），因此"本来就置顶的窗口"取消时不会被误降级；停用即全部还原。
    自检 23 项（含"锁定后把光标移开再动作仍作用于锁定窗口"的反向对照）

- [x] **P1-02 窗口快速分屏** — `window.snap_layout` · S · 无提权
  - 关键 API：`MonitorFromWindow` / `GetMonitorInfo(rcWork)` / `SetWindowPos`
  - 布局：左半、右半、上/下半、四象限、居中、1/3 分栏；按「窗口所在显示器」的工作区计算（多屏正确）
  - 验收：4K + 1080p 双屏下吸附位置均正确（含高 DPI 缩放）
  - 产出：`plugins/window_snap_layout/`。共 **11 个区域**（上半/下半、右侧第三列也补齐了）；
    每个区域一个快捷键（`Ctrl+Alt+方向/数字`、左右三分之一用 `Ctrl+Shift+Alt+方向`），
    设置面板给的是 3×3 网格按钮。全部经 `windowRectForVisualRect()` 换算，
    断言"左右两半精确铺满工作区宽（不重叠不留缝）"——奇数宽度下露缝正是这里的经典缺陷。
    停用时**有意不动窗口**（移动窗口是用户随手可再改的操作，不该被"停用"回滚）

- [x] **P1-03 窗口透明度调节** — `window.opacity` · M · 无提权
  - 关键 API：`SetWindowCompositionAttribute`（Win10+ 对普通窗口生效）；`SetLayeredWindowAttributes` 兜底
  - 交互：快捷键步进增减 + 设置面板滑块；需排除桌面、任务栏、输入法候选窗
  - 验收：对记事本/浏览器生效，对桌面右键菜单不误伤；停用后透明度归 100%
  - 产出：`plugins/window_opacity/`。**实现偏差（有意）**：只走**有文档**的
    `SetLayeredWindowAttributes(LWA_ALPHA)`，不用未公开的 `SetWindowCompositionAttribute`
    —— P1 批次的前提就是"无未公开 API"；代价是整窗统一 alpha、无法只模糊背景。
    步进幅度由配置 `[Plugins/window.opacity] stepPercent` 控制（默认 10，自检用 25 验证配置真被读到）；
    主动作变淡、`Ctrl+Alt+Plus` 变清、`Ctrl+Alt+0` 恢复。还原时**连 `WS_EX_LAYERED` 一起摘掉**
    （原本不是分层窗口的窗口要回到"普通窗口"，而不是留一个 alpha=255 的分层窗口）

- [x] **P1-04 窗口置底** — `window.bottom` · M · 无提权
  - 关键 API：`SetWindowPos(HWND_BOTTOM)` + `SetWinEventHook` 维持层级
  - 说明：「像动态壁纸」需压制该窗口抢焦点，并注意与任务栏 z-order 的关系
  - 验收：置底窗口被其它窗口正常遮挡，且点击不激活到最前
  - 产出：`plugins/window_bottom/`。`SetWindowPos(HWND_BOTTOM)` + 加 `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`
    （点击不激活到最前），再挂 `EVENT_SYSTEM_FOREGROUND` 钩子：只要别的窗口成为前台就把**所有**被置底的
    窗口重新压回底部（实测有效，自检里由子进程制造真实前台切换来验证）。`Ctrl+Shift+Alt+B` 解除全部置底。
    收尾时钩子按需卸载、扩展样式还原

### 文件与资源管理器

- [x] **P1-05 右键菜单扩展** — `file.context_menu` · S · 无提权
  - 实现：写 `HKCU\Software\Classes\{*,Directory,Directory\Background}\shell\WinEase.*\command`
  - 菜单项：复制路径、在此处打开终端、计算文件哈希、以 WinEase 重命名
  - ⚠ 注意：Windows 11 会折叠进「显示更多选项」；**不做 COM Shell Extension**（需注册+提权+易崩 explorer）
  - 验收：卸载功能时注册表项被完整清理，无残留
  - 产出：`plugins/file_context_menu/`。三个可勾选项，安装 6 个键（复制完整路径×3 / 计算 SHA-256×1 /
    在此处打开终端×2）；只写 **HKCU**（全程不需要管理员权限），同时写默认值与 `MUIVerb`（新旧资源管理器都认）。
    **完整清理**做了两层：按配置里记录的键路径删除 + 逐个 root 扫 `WinEase.*` 前缀兜底
    （防上次异常退出留残渣）；退出程序时也清理（菜单项只在 WinEase 运行期间存在）。
    **实现偏差（有意）**：第 4 项「以 WinEase 重命名」没做 —— 它需要主程序提供 CLI 入口
    与一套重命名 UI（插件契约里没有"被命令行唤起"这条通道），留到 P2 的文件重命名功能一起做。
    自检断言：6 个键都在、命令内容正确（含 `%~I` 去引号写法）、没碰 HKLM、移除/重装可来回切换、
    **停用后注册表里一个 WinEase 项都不剩**

### 监控与工具

- [x] **P1-06 颜色拾取器** — `monitor.color_picker` · S · 无提权
  - 关键 API：`GetCursorPos` + `BitBlt` 抓屏取像素（**按物理像素取色**，不能按逻辑坐标）
  - 功能：放大镜预览、HEX/RGB/HSL/CMYK 复制、取色历史、快捷键触发
  - 验收：125% 缩放下取色值与画图工具一致
  - 产出：`plugins/monitor_color_picker/`。快捷键 `Ctrl+Alt+C`（按默认格式复制）、
    `Ctrl+Alt+Shift+C`（固定复制 HEX）；设置面板含**实时放大镜**（最近邻放大，每 100ms 采样
    `Win32::probePixel`）、格式下拉、取色历史（去重、最近 12 条、点击即复制）。
    取色与坐标**全程物理像素**，插件侧不做任何 DPI 换算。
    自检用一块**置顶纯色窗口当标准色卡**，断言"复制出来的颜色值正确"且"附带坐标落在色卡的
    **物理**矩形内"——若插件误用逻辑坐标，那个点会落到卡外（本机缩放 1.5，偏差约 1/3 屏）；
    同时用"按逻辑坐标取色会取到别的颜色"做反向对照。

### 显示辅助

- [x] **P1-07 屏幕标尺** — `display.ruler` · S · 无提权
  - 实现：OverlayKit 透明置顶窗口 + `QPainter` 刻度 + 拖拽/旋转
  - 功能：水平/垂直/斜向测距、单位切换（px / 自定义）、多标尺并存
  - 验收：全屏置顶且点击穿透开关可切换
  - 产出：`plugins/display_ruler/`（`ruler_plugin.*` + `ruler_overlay.*`）。快捷键 `Ctrl+Alt+R` 显示/隐藏、
    `Ctrl+Shift+Alt+R` 切单位；**跟随鼠标**模式：落一个点，终点一直跟着光标（量距离只需"落点 → 移鼠标"），
    可切"可拖拽"（关闭穿透）；`pin` 钉住当前标尺即可量下一条（多标尺并存）。
    单位 px / cm / in（cm·in 按系统 DPI 估算，界面上如实标注"估算"——显示器物理尺寸系统给不准）。
    停用/清除即回收悬浮层，不留置顶残影。
    自检断言：悬浮层数=显示器数、OverlayKit 身份标记、TOPMOST/LAYERED/NOACTIVATE/TRANSPARENT 四个样式、
    铺满显示器（≤2px）、z 序在普通窗口之上；再用**真实光标**量一段 400px 的垂距（容差 ±2px）、
    切单位后长度立刻按新单位重算、穿透开关能来回切、清除后登记表归零。

- [x] **P1-08 焦点高亮** — `display.focus_highlight` · S · 无提权
  - 实现：OverlayKit 全屏层 + 鼠标位置高亮圈 / 聚光灯（`SetWindowRgn` 挖洞或分层 alpha）
  - 交互：快捷键切换、呼吸动画、圆环大小/颜色可配
  - 验收：录屏时高亮位置与鼠标一致，且不干扰被点击窗口
  - 产出：`plugins/display_focus_highlight/`（`focus_highlight_plugin.*` + `spotlight_overlay.*`）。
    快捷键 `Ctrl+Alt+H` 开关、`Ctrl+Shift+Alt+H` 切样式（聚光灯 ⇄ 只画圆环）；30fps 跟光标，
    **每帧只重绘圆环那一带的脏区**（整屏半透明窗口全量重绘会卡）；呼吸动画 ±8% 半径；
    半径/压暗强度/圆环颜色/呼吸开关都在设置面板可调。
    打洞用 `QPainter::CompositionMode_Clear`（这是分层窗口里唯一能把 alpha 真正清零的办法）。
    验收"不干扰被点击窗口"落在两行上：悬浮层始终 `setClickThrough(true)` + `setNoActivate(true)`。
    自检的硬证据是**同一像素的 A/B**：光标压在背景板上时该像素不失真；只把光标挪开，
    同一像素被压暗到 0.57 倍 —— 这直接证明"高亮跟着鼠标"，且停用后像素完全复原（无残影）。
    注：拖拽（P1-07）与呼吸动画（P1-08）属于**手测项**，自检只验到样式/配置层面，不注入鼠标。

### 启动器

- [x] **P1-09 网页快速搜索** — `launcher.web_search` · S · 无提权
  - 实现：模拟 `Ctrl+C` 取选中文本 → `QDesktopServices::openUrl` 打开搜索引擎 URL
  - 交互：多引擎可切换（默认 + 快捷前缀）；取词失败时回退用剪贴板现有内容
  - 验收：在浏览器/记事本选中文本后触发，正确打开搜索结果页
  - 产出：`plugins/launcher_web_search/`。快捷键 `Ctrl+Alt+S`（默认引擎）、`Ctrl+Shift+Alt+S`（只复制链接）；
    取词复用 `ClipboardTools::acquireText()`（与格式化/编码插件同一套"选中 → 回落剪贴板"语义）。
    前缀语法 `<别名>: 关键词`，**中英文冒号都认**（中文输入法下很容易打出全角），
    别名支持多个（`gh`/`github`、`bd`/`baidu`…），引擎表可在配置里改（`别名[,别名…]|名称|URL模板`）。
    不认识的前缀按普通关键词搜（不把用户的话吃掉）。
    **dryRun（只复制链接）既是实用模式，也是自检能逐字符校验 URL 的关键**。
    自检断言：空格编码成 `%20`、全角冒号+中文 UTF-8 编码、自定义引擎生效、空输入给出明确原因

### 开发运维

- [x] **P1-10 JSON/XML 格式化** — `dev.text_format` · S · 无提权
  - 实现：`QJsonDocument` / `QXmlStreamReader`；选中文本经剪贴板就地替换
  - 功能：格式化、压缩、转义/去转义、JSONPath 简易查询
  - 验收：非法 JSON 给出精确错误行列号，不改动原文
  - 产出：`plugins/dev_text_format/`。快捷键 `Ctrl+Alt+J`（自动识别 JSON/XML 后格式化）、
    `Ctrl+Shift+Alt+J`（压缩）；设置面板含强制 JSON/XML、转义/去转义、JSONPath 查询、缩进设置。
    **实现偏差（有意）**：格式化/压缩不用 `QJsonDocument::toJson()` 往返 —— `QJsonObject` 内部按
    key 排序，往返会把用户的键顺序改掉；改为自写**保序词法扫描**（`QJsonDocument` 只用来校验取错误）。
    错误行列号按**字符**计数（`QJsonParseError::offset` 是 UTF-8 字节偏移，直接当列号在中文文档上会偏）。
    `minify(format(x)) == x` 是自检里的往返不变量。
    **硬规则：转换失败绝不动剪贴板**，用户原文留在原地，才能照着"第 2 行第 9 列"去改。

- [x] **P1-11 编码转换** — `dev.encoder` · S · 无提权
  - 实现：`QByteArray::toBase64` / URL 编解码 / `QCryptographicHash`（MD5/SHA1/SHA256/SHA512）
  - 交互：输入框即时双向转换，一键复制
  - 验收：与在线工具结果逐字节一致
  - 产出：`plugins/dev_encoder/`。快捷键 `Ctrl+Alt+E`（Base64 编码）、`Ctrl+Shift+Alt+E`
    （一次算 MD5/SHA-1/SHA-256/SHA-512，四行带算法名）；设置面板做 Base64/URL 双向即时转换 + 哈希表。
    Base64 解码**刻意宽松**：容忍换行、URL 安全字母表（`-_`）、缺省 `=`（先把填充剥掉再判长度，
    否则 `====` 会被当成合法输入）。
    自检用固定向量钉死：`Base64("abc")=YWJj`、`Base64("hello 世界")=aGVsbG8g5LiW55WM`、
    `Base64(U+1F600)=8J+YgA==`，以及 FIPS-180 的 `MD5/SHA-1/SHA-256/SHA-512("abc")`。

- [x] **P1-12 端口占用查看** — `dev.port_viewer` · M · 部分 ▲提权
  - 关键 API：`GetExtendedTcpTable` / `GetExtendedUdpTable` → PID → `QueryFullProcessImageName`
  - 功能：按端口/PID 查询、列出监听与连接、结束占用进程（系统进程走提权助手）
  - 验收：能查出被占用端口对应的进程名与完整路径
  - 产出：`plugins/dev_port_viewer/` + 平台层 `win32/NetUtils`。快捷键 `Ctrl+Alt+P`：
    查**剪贴板里的端口**并把结果（端口 + 进程名 + 完整路径）写回剪贴板 —— 排查"端口被占"时
    最常见的动作就是"复制个端口号想知道是谁"。设置面板可按端口/PID/进程名查询、列出全部端点、
    选中一行**结束进程**（经 `ElevationService` → `WinEaseHelper`，首次弹一次 UAC）。
    `requiresAdmin` 仍为 false：**枚举是只读的，普通权限就够**，提权只发生在点"结束进程"时。
    自检自己开一个**回环监听套接字**（TCP+UDP，端口由系统分配，不用猜哪个端口空着），
    再断言"查得到该端口 + PID 是本进程 + 进程名与完整路径正确"。

### 个性化

- [x] **P1-13 主题切换** — `personal.theme_switch` · S · 无提权
  - 实现：写 `AppsUseLightTheme` / `SystemUsesLightTheme` + 广播 `WM_SETTINGCHANGE`
  - 功能：深色/浅色/跟随系统/自定义强调色；处理 Win11「自定义模式」（应用浅色 + 系统深色）
  - 验收：切换后资源管理器与开始菜单立即跟随，无需重启
  - 产出：`plugins/personal_theme_switch/`。快捷键 `Ctrl+Alt+T` 在深/浅之间反转；
    设置面板另有深色 / 浅色 / 自定义 / 恢复四个按钮。**两个值每次都一起写**（只写一个就会把用户
    带进第三种状态），写完广播 `WM_SETTINGCHANGE("ImmersiveColorSet")`。
    界面上明确写着"改的是 Windows 自己的主题，WinEase 界面固定深色不受影响"，
    以及还原契约（停用恢复 / 退出不影响）。
    **实现偏差（有意）**：没做"自定义强调色"—— 强调色在 `HKCU\...\Explorer\Accent` 的二进制
    `AccentPalette` 里（要写 32 字节调色板 + 改 `ColorSet_Version3`），不属于"低风险快赢"，
    留给 P2 的个性化主题功能。
    自检断言：浅色=两处都 1、深色=两处都 0、自定义=1/0；**用隐藏窗口真的抓到一次
    `WM_SETTINGCHANGE("ImmersiveColorSet")` 广播**（否则"立即生效"就是空话）；
    停用后两个值**逐项还原**

- [x] **P1-14 壁纸自动切换** — `personal.wallpaper` · M · 无提权
  - 实现：公开 COM 接口 `IDesktopWallpaper`（**支持多显示器独立壁纸**）；`SystemParametersInfo` 兜底
  - 功能：定时/随机/按序切换、按显示器指定文件夹、历史回滚、开机恢复
  - 验收：双显示器分别设置不同壁纸成功，且重启后保持
  - 产出：`plugins/personal_wallpaper/` + 平台层 `win32/DesktopWallpaper`。
    快捷键 `Ctrl+Alt+W` 下一张、`Ctrl+Shift+Alt+W` 上一张；设置面板可挑文件夹、按序/随机、
    自动轮换间隔（0=只手动）、演练模式、以及"只换光标所在那块屏"（多显示器）。
    随机模式**连着不重复**（否则用户会以为没反应）；换图历史留最近 10 条（可回滚）。
    还原契约：**停用功能 → 换回启用前那张**；退出 WinEase → 保持（验收原文就是"重启后保持"）。
    **实现偏差（有意）**：没做"启动时兜底还原"—— 那会把用户重启后想保留的壁纸翻回去；
    "开机恢复"在本实现里体现为"原壁纸路径落盘，随时可一键恢复"。
    多显示器分屏设置在 API 层已实现（`setWallpaperForMonitor` + `perMonitor` 开关），
    但本机只有单屏，**分屏那条路径属手测项**。
    自检断言：按文件名顺序切 3 张并绕回、上一张回滚、恢复原壁纸、
    **停用后壁纸还原成启用前那张**（每次都用平台层读回真实路径比对，路径先归一化）

---
