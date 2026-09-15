#pragma once

// ============================================================================
//  WindowUtils.h —— 窗口 / 显示器 / DPI 平台能力
//
//  ⚠ 坐标约定（违反必然导致高 DPI 下错位）：
//      本模块所有 QRect / QPoint **一律为物理像素、屏幕坐标**。
//      Qt 的 QWidget::geometry() / QScreen::geometry() 是**逻辑像素**。
//      两者换算使用本模块的 toLogical() / toPhysical()，
//      缩放系数用 scaleFactorForWindow()。
//
//  ⚠ 关于"物理像素"的前提：
//      上述约定成立的前提是**进程为 per-monitor DPI aware**（WinEase.exe 由 Qt 设置，
//      满足该前提）。若进程是 DPI 不感知的（例如未声明清单的控制台工具），
//      Windows 会虚拟化 GetWindowRect 等 API 的返回值，此时坐标是"系统缩放后的像素"。
//      本模块已做内部自洽处理（见 visualWindowRect 的实现说明），
//      调用方如需判断当前处于哪种模式，可用 dpiAwarenessText()。
//
//  ⚠ 换算损耗：toLogical() / toPhysical() 存在**最多 1 像素的取整损耗**，
//      不要依赖它们做精确往返（例如 640@150% → 427 → 641）。
//
//  为什么不用 Qt 的 QWindow 而直接走 Win32：
//      窗口置顶/置底/透明度/分屏需要操作**其它进程的任意窗口**，
//      Qt 的 QWindow 只能描述本进程创建的窗口，覆盖不到需求。
//
//  本模块全部为无状态纯函数，可安全被主程序与所有插件共同链接。
// ============================================================================

#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

#include <windows.h>

namespace WinEase::Win32 {

/// 窗口句柄别名（保持与 Win32 一致，便于与其它 SDK 混用）
using WindowHandle = HWND;

/// 显示器信息（物理像素 + 屏幕坐标）
struct MonitorInfo {
    QString deviceName;        ///< 设备名，如 "\\.\DISPLAY1"
    QRect geometry;            ///< 完整区域
    QRect workArea;            ///< 可用区域（已排除任务栏），分屏必须用它
    bool primary = false;      ///< 是否主显示器
    bool valid = false;        ///< 是否查到了有效信息
    int dpi = 96;              ///< 该显示器的 DPI（受调用进程 DPI 感知级别影响）
    qreal scaleFactor = 1.0;   ///< dpi / 96
    /// 原生显示器句柄（HMONITOR）。
    /// 以 void* 暴露，供 DDC/CI、每显示器 API 等需要 HMONITOR 的场景使用，
    /// 同时避免让本头文件的使用者必须包含 windows.h。
    void *nativeHandle = nullptr;
};

// ============================================================================
//  窗口基础信息
// ============================================================================

bool isValidWindow(WindowHandle hwnd);
bool isWindowVisible(WindowHandle hwnd);

/// 是否被 DWM "隐藏"：UWP 应用最小化、非当前虚拟桌面的窗口都会是 cloaked，
/// 用于避免对不可见窗口做置顶/分屏等操作
bool isWindowCloaked(WindowHandle hwnd);

WindowHandle foregroundWindow();

/// 屏幕坐标处的最上层窗口（会向上追溯到顶层窗口）
WindowHandle topLevelWindowAt(const QPoint &screenPoint);

QString windowTitle(WindowHandle hwnd);
QString windowClassName(WindowHandle hwnd);
quint32 windowProcessId(WindowHandle hwnd);
QString windowProcessName(WindowHandle hwnd); ///< 失败返回空串

/// 是否属于本进程（避免把自己的窗口当作操作目标）
bool isOwnWindow(WindowHandle hwnd);

/// 是否为应当忽略的系统外壳窗口（桌面、任务栏、输入法候选窗等）
bool isShellWindow(WindowHandle hwnd);

/// 综合判断：该窗口是否适合作为"窗口管理"类功能的操作目标
bool isManageableWindow(WindowHandle hwnd);

// ============================================================================
//  几何
// ============================================================================

/// 含边框的窗口矩形（物理像素）
QRect windowRect(WindowHandle hwnd);

/// 视觉边界：剔除 Win10/11 窗口的不可见投影边框，
/// 分屏对齐必须用它，否则窗口之间会留出约 7px 的缝隙
QRect visualWindowRect(WindowHandle hwnd);

/// 客户区在屏幕坐标下的矩形（物理像素）
QRect clientRectOnScreen(WindowHandle hwnd);

/// 移动并调整窗口大小；若窗口处于最大化会先自动还原
bool moveWindow(WindowHandle hwnd, const QRect &targetRect);

/// visualWindowRect() 的逆运算：给定"希望窗口看起来占用的区域"，
/// 返回应该传给 moveWindow() 的矩形（自动扣掉不可见投影边框）。
/// 分屏/对齐必须用它，否则相邻窗口之间会漏缝隙或互相重叠。
QRect windowRectForVisualRect(WindowHandle hwnd, const QRect &visualTarget);

/// 把窗口居中到它所在的显示器工作区
bool centerWindowOnMonitor(WindowHandle hwnd);

bool restoreIfMaximized(WindowHandle hwnd);
bool isWindowMaximized(WindowHandle hwnd);
bool isWindowMinimized(WindowHandle hwnd);

// ============================================================================
//  Z 序（置顶 / 置底）
// ============================================================================

bool isTopMost(WindowHandle hwnd);

/// 置顶 / 取消置顶
bool setTopMost(WindowHandle hwnd, bool on);

/// 置底（钉在桌面层之上、其它窗口之下）
bool setBottom(WindowHandle hwnd);

// ============================================================================
//  扩展样式（点击穿透 / 不抢焦点 / 透明度都要动 GWL_EXSTYLE）
// ============================================================================

/// 扩展样式快照：功能停用时用它恢复原样
struct ExStyleSnapshot {
    LONG_PTR exStyle = 0;
    bool valid = false;
};

ExStyleSnapshot captureExStyle(WindowHandle hwnd);
bool restoreExStyle(WindowHandle hwnd, const ExStyleSnapshot &snapshot);

/// 点击穿透（鼠标事件穿透到下层窗口）—— OverlayKit 悬浮层必需
bool setClickThrough(WindowHandle hwnd, bool on);
bool isClickThrough(WindowHandle hwnd);

/// 不抢焦点（点击不激活、不出现在 Alt+Tab）
bool setNoActivate(WindowHandle hwnd, bool on);

// ============================================================================
//  标题栏深色
//
//  用途：WinEase 的配色是**强制深色**的，不能出现"纯黑界面配一条浅色标题栏"。
//
//  为什么必须走 Win32：
//      标题栏由 DWM 绘制，**不受 Qt 样式表与调色板控制**。
//      Windows 默认让标题栏跟随系统"应用模式"偏好，
//      因此系统处于浅色模式时标题栏会是浅色的。
//      设置 DWMWA_USE_IMMERSIVE_DARK_MODE 可以强制它为深色。
//
//  说明：Qt 6.8 起也可以用 QStyleHints::setColorScheme(Qt::ColorScheme::Dark)
//        声明深色方案（本项目已同时使用），但那是"请求平台配合"；
//        本函数是**直接落到底层属性**，不依赖 Qt 内部是否真的转发了该请求。
// ============================================================================

/// 强制窗口标题栏使用深色绘制；Windows 10 1809 以下不支持时返回 false
bool setDarkTitleBar(WindowHandle hwnd, bool on);

// ============================================================================
//  窗口透明度
//
//  实现说明（重要）：
//      使用 SetLayeredWindowAttributes(LWA_ALPHA)，它会让**窗口内容整体**
//      半透明——这正是"方便偷看背后内容"所需的语义。
//      不要用 SetWindowCompositionAttribute 的 ACCENT 策略替代：那只影响
//      窗口背景的着色/模糊，内容依旧不透明，语义完全不同。
//
//  已知限制：
//      * 需要给窗口加 WS_EX_LAYERED，少数窗口（UWP、已分层窗口、独占全屏）
//        可能无效或表现异常，调用方需容忍失败
//      * 被操作窗口若频繁重绘可能出现轻微闪烁
// ============================================================================

/// 透明度原始状态快照
struct OpacitySnapshot {
    bool valid = false;
    bool wasLayered = false;        ///< 原本是否已是分层窗口
    BYTE originalAlpha = 255;       ///< 原本的 alpha（仅当 wasLayered 有效）
    LONG_PTR originalExStyle = 0;
};

OpacitySnapshot captureOpacityState(WindowHandle hwnd);

/// 恢复透明度原始状态（停用功能时调用，满足"不留副作用"要求）
bool restoreOpacityState(WindowHandle hwnd, const OpacitySnapshot &snapshot);

/// 设置透明度，取值会被收敛到 [0.1, 1.0]；1.0 表示完全不透明
bool setOpacity(WindowHandle hwnd, qreal opacity);

/// 读取当前透明度；非分层窗口返回 1.0
qreal opacity(WindowHandle hwnd);

// ============================================================================
//  显示器
// ============================================================================

/// 枚举全部显示器（按 Windows 的枚举顺序）
QList<MonitorInfo> monitors();

MonitorInfo monitorForWindow(WindowHandle hwnd);
MonitorInfo monitorForPoint(const QPoint &screenPoint);
MonitorInfo primaryMonitor();
MonitorInfo monitorFromNativeHandle(void *nativeMonitor); ///< HMONITOR

// ============================================================================
//  DPI 与坐标换算
// ============================================================================

/// 窗口所在显示器的 DPI；hwnd 无效时回退到系统 DPI
int dpiForWindow(WindowHandle hwnd);

/// dpi / 96
qreal scaleFactorForWindow(WindowHandle hwnd);

/// 当前进程的 DPI 感知级别（只读诊断；Qt 已负责设置，不要自行修改）
QString dpiAwarenessText();

QRect toLogical(const QRect &physicalRect, qreal scaleFactor);
QRect toPhysical(const QRect &logicalRect, qreal scaleFactor);
QPoint toLogical(const QPoint &physicalPoint, qreal scaleFactor);
QPoint toPhysical(const QPoint &logicalPoint, qreal scaleFactor);

/// 当前进程是否为管理员权限（窗口管理类功能用于判断能否操作系统级窗口）
bool isProcessElevated();

} // namespace WinEase::Win32
