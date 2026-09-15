#include "win32/WindowUtils.h"

#include "win32/ProcessUtils.h"

#include <QStringList>

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <string>

// 部分 SDK 版本未定义 DWMWA_CLOAKED，自行兜底（值为 14，长期稳定）
#ifndef DWMWA_CLOAKED
#    define DWMWA_CLOAKED 14
#endif

// 同上：标题栏深色属性在旧 SDK 中缺失（值为 20，Win10 20H1+ 使用）
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#    define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace WinEase::Win32 {

namespace {

/// 透明度允许范围：低于 0.1 的窗口几乎不可见且难以找回，直接收敛
constexpr qreal kMinOpacity = 0.1;
constexpr qreal kMaxOpacity = 1.0;

/// 取标题/类名的缓冲区大小
constexpr int kTextBufferSize = 512;

/// 应被"窗口管理"类功能忽略的外壳窗口类名（全部小写比较）
const QStringList &shellWindowClasses()
{
    static const QStringList classes{
        QStringLiteral("progman"),                            // 桌面
        QStringLiteral("workerw"),                            // 壁纸层
        QStringLiteral("shell_traywnd"),                      // 主任务栏
        QStringLiteral("shell_secondarytraywnd"),             // 副屏任务栏
        QStringLiteral("tasklistthumbnailwnd"),               // 任务栏缩略图
        QStringLiteral("foregroundstaging"),                  // 前台切换中转窗口
        QStringLiteral("xamlexplorerhostislandwindow"),       // 系统外壳岛窗口
        QStringLiteral("windows.ui.composition.desktopwindowcontentbridge"),
        QStringLiteral("ime"),
        QStringLiteral("default ime"),
        QStringLiteral("msctfime ui"),
        QStringLiteral("tf_floatinglangbar_wndtitle"),        // 输入法悬浮条
    };
    return classes;
}

using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);

/// 动态解析 Shcore!GetDpiForMonitor。
/// 只为"查询显示器 DPI"这一个辅助能力，不值得引入硬链接依赖，因此运行时解析。
/// 函数内静态缓存的是操作系统事实（只读），不构成本模块的可变全局状态。
GetDpiForMonitorFn resolveGetDpiForMonitor()
{
    static const GetDpiForMonitorFn resolve = [] () -> GetDpiForMonitorFn {
        const HMODULE module = ::LoadLibraryW(L"Shcore.dll");
        if (module == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForMonitorFn>(::GetProcAddress(module, "GetDpiForMonitor"));
    }();
    return resolve;
}

/// 查询显示器 DPI（受调用进程的 DPI 感知级别影响）
int dpiOfMonitor(HMONITOR monitor)
{
    constexpr int kMdtEffectiveDpi = 0; // MDT_EFFECTIVE_DPI
    if (const GetDpiForMonitorFn resolve = resolveGetDpiForMonitor();
        resolve != nullptr && monitor != nullptr) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(resolve(monitor, kMdtEffectiveDpi, &dpiX, &dpiY)) && dpiX > 0U) {
            return static_cast<int>(dpiX);
        }
    }

    const UINT systemDpi = ::GetDpiForSystem();
    return systemDpi > 0U ? static_cast<int>(systemDpi) : 96;
}

/// 查询显示器**真实** DPI（MDT_RAW_DPI，不受 DPI 虚拟化影响）。
/// 关键点：在 DPI 不感知进程里 GetDpiForSystem() 只会返回 96，
/// 拿不到用户实际的缩放比例；只有 MDT_RAW_DPI 能绕过虚拟化拿到真实值。
int rawDpiOfMonitor(HMONITOR monitor)
{
    constexpr int kMdtRawDpi = 2; // MDT_RAW_DPI
    if (const GetDpiForMonitorFn resolve = resolveGetDpiForMonitor();
        resolve != nullptr && monitor != nullptr) {
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(resolve(monitor, kMdtRawDpi, &dpiX, &dpiY)) && dpiX > 0U) {
            return static_cast<int>(dpiX);
        }
    }
    return 96;
}

MonitorInfo makeMonitorInfo(HMONITOR monitor)
{
    MonitorInfo info;
    if (monitor == nullptr) {
        return info;
    }

    MONITORINFOEXW nativeInfo{};
    nativeInfo.cbSize = sizeof(MONITORINFOEXW);
    if (::GetMonitorInfoW(monitor, &nativeInfo) == FALSE) {
        return info;
    }

    info.valid = true;
    info.nativeHandle = monitor;
    info.deviceName = QString::fromWCharArray(nativeInfo.szDevice);
    info.primary = (nativeInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

    const RECT &full = nativeInfo.rcMonitor;
    info.geometry = QRect(full.left, full.top, full.right - full.left, full.bottom - full.top);

    const RECT &work = nativeInfo.rcWork;
    info.workArea = QRect(work.left, work.top, work.right - work.left, work.bottom - work.top);

    info.dpi = dpiOfMonitor(monitor);
    info.scaleFactor = static_cast<qreal>(info.dpi) / 96.0;
    return info;
}

/// DWM 窗口边界在 DPI 不感知进程中的坐标换算系数。
///
/// 已知陷阱：DWM 的 DWMWA_EXTENDED_FRAME_BOUNDS **始终返回物理像素**，
/// 而同一进程里 GetWindowRect 返回的是被 Windows 虚拟化后的像素。
/// 若进程是 DPI 不感知的（未声明 per-monitor DPI aware，例如控制台工具），
/// 两个 API 的坐标系会相差一个缩放倍数，直接比较会出现"视觉边界比窗口还大"的荒谬结果。
///
/// 这里统一把 DWM 结果换算到与 GetWindowRect 相同的坐标系，保证本模块内部语义自洽：
///   * 进程为 per-monitor DPI aware（WinEase.exe 的正常状态）→ 系数 1.0，即物理像素
///   * 进程为 DPI 不感知 → 用主显示器的**真实** DPI 折算，与 GetWindowRect 一致
///
/// 说明：DPI 不感知进程的坐标是被**统一**虚拟化的（哪怕是混合 DPI 多屏），
/// 因此这里用主显示器的真实 DPI，而不是窗口所在显示器的。
qreal dwmBoundsScaleFactor()
{
    const DPI_AWARENESS awareness =
        ::GetAwarenessFromDpiAwarenessContext(::GetThreadDpiAwarenessContext());
    if (awareness != DPI_AWARENESS_UNAWARE) {
        return 1.0;
    }

    const HMONITOR primary = ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    const int rawDpi = rawDpiOfMonitor(primary);
    return rawDpi > 0 ? static_cast<qreal>(rawDpi) / 96.0 : 1.0;
}

/// EnumDisplayMonitors 回调：把结果收集到 QList
BOOL CALLBACK collectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM userData)
{
    auto *list = reinterpret_cast<QList<MonitorInfo> *>(userData);
    if (list != nullptr) {
        const MonitorInfo info = makeMonitorInfo(monitor);
        if (info.valid) {
            list->append(info);
        }
    }
    return TRUE; // 继续枚举
}

/// 修改扩展样式；返回是否成功
bool modifyExStyle(HWND hwnd, LONG_PTR mask, LONG_PTR value)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }

    const LONG_PTR current = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR updated = (current & ~mask) | (value & mask);
    if (updated == current) {
        return true; // 已经是目标状态
    }

    // SetWindowLongPtr 不会在成功时清零错误码，必须先自行清零再检查
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, updated);
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        return false;
    }

    // 样式变化需要重绘生效
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

} // namespace

// ============================================================================
//  窗口基础信息
// ============================================================================

bool isValidWindow(WindowHandle hwnd)
{
    return hwnd != nullptr && ::IsWindow(hwnd) != FALSE;
}

bool isWindowVisible(WindowHandle hwnd)
{
    return isValidWindow(hwnd) && ::IsWindowVisible(hwnd) != FALSE;
}

bool isWindowCloaked(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return true;
    }
    DWORD cloaked = 0;
    const HRESULT hr = ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (FAILED(hr)) {
        return false; // 查询失败时保守认为"可见"
    }
    return cloaked != 0U;
}

WindowHandle foregroundWindow()
{
    return ::GetForegroundWindow();
}

WindowHandle topLevelWindowAt(const QPoint &screenPoint)
{
    POINT point{ screenPoint.x(), screenPoint.y() };
    HWND hwnd = ::WindowFromPoint(point);
    if (hwnd == nullptr) {
        return nullptr;
    }
    // WindowFromPoint 返回的是子窗口，需要向上找到顶层窗口
    return ::GetAncestor(hwnd, GA_ROOT);
}

QString windowTitle(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return QString();
    }
    // GetWindowTextW 对其它进程窗口取的是缓存标题，不会发送 WM_GETTEXT，安全
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return QString();
    }

    std::wstring buffer(static_cast<size_t>(length) + 1U, L'\0');
    const int copied = ::GetWindowTextW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0) {
        return QString();
    }
    return QString::fromWCharArray(buffer.data(), copied);
}

QString windowClassName(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return QString();
    }
    wchar_t buffer[kTextBufferSize] = {};
    const int copied = ::GetClassNameW(hwnd, buffer, kTextBufferSize);
    if (copied <= 0) {
        return QString();
    }
    return QString::fromWCharArray(buffer, copied);
}

quint32 windowProcessId(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return 0U;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    return static_cast<quint32>(pid);
}

QString windowProcessName(WindowHandle hwnd)
{
    const quint32 pid = windowProcessId(hwnd);
    if (pid == 0U) {
        return QString();
    }
    return processName(pid);
}

bool isOwnWindow(WindowHandle hwnd)
{
    return windowProcessId(hwnd) == static_cast<quint32>(::GetCurrentProcessId());
}

bool isShellWindow(WindowHandle hwnd)
{
    const QString className = windowClassName(hwnd).toLower();
    if (className.isEmpty()) {
        return false;
    }
    return shellWindowClasses().contains(className);
}

bool isManageableWindow(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd) || !isWindowVisible(hwnd)) {
        return false;
    }
    if (isWindowCloaked(hwnd) || isShellWindow(hwnd) || isOwnWindow(hwnd)) {
        return false;
    }

    // 尺寸为零的窗口（隐藏的宿主窗口等）不适合作为操作目标
    const QRect rect = windowRect(hwnd);
    return rect.width() > 0 && rect.height() > 0;
}

// ============================================================================
//  几何
// ============================================================================

QRect windowRect(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return QRect();
    }
    RECT rect{};
    if (::GetWindowRect(hwnd, &rect) == FALSE) {
        return QRect();
    }
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

QRect visualWindowRect(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return QRect();
    }

    // Win10/11 的窗口在 GetWindowRect 里包含约 7px 的不可见投影边框，
    // 直接用它做分屏会导致窗口之间出现缝隙，因此优先取 DWM 的视觉边界。
    RECT rect{};
    const HRESULT hr = ::DwmGetWindowAttribute(hwnd,
                                               DWMWA_EXTENDED_FRAME_BOUNDS,
                                               &rect,
                                               sizeof(rect));
    if (SUCCEEDED(hr) && (rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0) {
        const QRect bounds(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        // 统一坐标系（见 dwmBoundsScaleFactor 的说明）
        return toLogical(bounds, dwmBoundsScaleFactor());
    }
    return windowRect(hwnd);
}

QRect windowRectForVisualRect(WindowHandle hwnd, const QRect &visualTarget)
{
    if (!isValidWindow(hwnd) || !visualTarget.isValid()) {
        return visualTarget;
    }

    // visualWindowRect() 的逆运算：把"我希望窗口看起来占哪块区域"换算成
    // "应该传给 moveWindow() 的矩形"。
    // 不可直接用 moveWindow(目标区域)：Win10/11 的窗口在 GetWindowRect 里
    // 含约 7px 不可见投影边框，结果相邻窗口之间会漏出缝隙、彼此重叠。
    const QRect window = windowRect(hwnd);
    const QRect visual = visualWindowRect(hwnd);
    if (!window.isValid() || !visual.isValid() || visual.isEmpty()) {
        return visualTarget;
    }

    const int leftInset = visual.left() - window.left();
    const int topInset = visual.top() - window.top();
    const int rightInset = window.right() - visual.right();
    const int bottomInset = window.bottom() - visual.bottom();

    return QRect(visualTarget.left() - leftInset,
                 visualTarget.top() - topInset,
                 visualTarget.width() + leftInset + rightInset,
                 visualTarget.height() + topInset + bottomInset);
}

QRect clientRectOnScreen(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return QRect();
    }
    RECT client{};
    if (::GetClientRect(hwnd, &client) == FALSE) {
        return QRect();
    }

    POINT topLeft{ client.left, client.top };
    if (::ClientToScreen(hwnd, &topLeft) == FALSE) {
        return QRect();
    }

    POINT bottomRight{ client.right, client.bottom };
    if (::ClientToScreen(hwnd, &bottomRight) == FALSE) {
        return QRect();
    }

    return QRect(topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y);
}

bool moveWindow(WindowHandle hwnd, const QRect &targetRect)
{
    if (!isValidWindow(hwnd) || !targetRect.isValid()) {
        return false;
    }

    // 最大化窗口无法被自由移动，先还原
    restoreIfMaximized(hwnd);

    const BOOL ok = ::SetWindowPos(hwnd,
                                   nullptr,
                                   targetRect.x(),
                                   targetRect.y(),
                                   targetRect.width(),
                                   targetRect.height(),
                                   SWP_NOZORDER | SWP_NOACTIVATE);
    return ok != FALSE;
}

bool centerWindowOnMonitor(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    const MonitorInfo monitor = monitorForWindow(hwnd);
    if (!monitor.valid) {
        return false;
    }

    restoreIfMaximized(hwnd);

    // "居中"的语义是**看起来**居中，所以量的是视觉边界（visualWindowRect），
    // 而 moveWindow() 收的是窗口矩形 —— 两者相差不可见投影边框。
    // 若把视觉宽高直接交给 moveWindow()，每居中一次窗口就会缩小一圈
    // （实测 150% DPI 下每次缩约 18x9 px，且窗口越大缩得越明显），
    // 因此必须经 windowRectForVisualRect() 换算回窗口矩形。
    const QRect current = visualWindowRect(hwnd);
    const QRect area = monitor.workArea;
    const int width = std::min(current.width(), area.width());
    const int height = std::min(current.height(), area.height());
    const QRect visualTarget(area.x() + (area.width() - width) / 2,
                             area.y() + (area.height() - height) / 2,
                             width,
                             height);

    return moveWindow(hwnd, windowRectForVisualRect(hwnd, visualTarget));
}

bool restoreIfMaximized(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd) || !isWindowMaximized(hwnd)) {
        return false;
    }
    ::ShowWindow(hwnd, SW_RESTORE);
    return true;
}

bool isWindowMaximized(WindowHandle hwnd)
{
    return isValidWindow(hwnd) && ::IsZoomed(hwnd) != FALSE;
}

bool isWindowMinimized(WindowHandle hwnd)
{
    return isValidWindow(hwnd) && ::IsIconic(hwnd) != FALSE;
}

// ============================================================================
//  Z 序
// ============================================================================

bool isTopMost(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (exStyle & WS_EX_TOPMOST) != 0;
}

bool setTopMost(WindowHandle hwnd, bool on)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    if (isTopMost(hwnd) == on) {
        return true;
    }

    const BOOL ok = ::SetWindowPos(hwnd,
                                   on ? HWND_TOPMOST : HWND_NOTOPMOST,
                                   0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return ok != FALSE;
}

bool setBottom(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    // HWND_BOTTOM 只是放进 Z 序最底部；若要"像动态壁纸"一样长期贴底，
    // 调用方还需配合 SetWinEventHook 持续维持层级（属于功能实现，不在本层）
    const BOOL ok = ::SetWindowPos(hwnd,
                                   HWND_BOTTOM,
                                   0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return ok != FALSE;
}

// ============================================================================
//  扩展样式
// ============================================================================

ExStyleSnapshot captureExStyle(WindowHandle hwnd)
{
    ExStyleSnapshot snapshot;
    if (!isValidWindow(hwnd)) {
        return snapshot;
    }
    snapshot.exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    snapshot.valid = true;
    return snapshot;
}

bool restoreExStyle(WindowHandle hwnd, const ExStyleSnapshot &snapshot)
{
    if (!isValidWindow(hwnd) || !snapshot.valid) {
        return false;
    }
    return modifyExStyle(hwnd, static_cast<LONG_PTR>(-1), snapshot.exStyle);
}

bool setClickThrough(WindowHandle hwnd, bool on)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    if (on) {
        // 点击穿透必须同时具备 WS_EX_TRANSPARENT 与 WS_EX_LAYERED
        return modifyExStyle(hwnd, WS_EX_TRANSPARENT | WS_EX_LAYERED,
                             WS_EX_TRANSPARENT | WS_EX_LAYERED);
    }
    return modifyExStyle(hwnd, WS_EX_TRANSPARENT, 0);
}

bool isClickThrough(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    return (::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
}

bool setNoActivate(WindowHandle hwnd, bool on)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }
    if (on) {
        return modifyExStyle(hwnd, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                             WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    }
    return modifyExStyle(hwnd, WS_EX_NOACTIVATE, 0);
}

bool setDarkTitleBar(WindowHandle hwnd, bool on)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }

    const BOOL value = on ? TRUE : FALSE;

    // 属性号 20 是 Windows 10 20H1 及以后版本使用的 DWMWA_USE_IMMERSIVE_DARK_MODE；
    // 1809~1903 上该属性号是 19（因此必须做一次回退，否则老系统上静默无效）。
    // 注意：两个版本都不会"报错成功却无效果"，失败时可安全回退。
    if (SUCCEEDED(::DwmSetWindowAttribute(
            hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value)))) {
        return true;
    }
    return SUCCEEDED(::DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value)));
}

// ============================================================================
//  透明度
// ============================================================================

OpacitySnapshot captureOpacityState(WindowHandle hwnd)
{
    OpacitySnapshot snapshot;
    if (!isValidWindow(hwnd)) {
        return snapshot;
    }

    snapshot.originalExStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    snapshot.wasLayered = (snapshot.originalExStyle & WS_EX_LAYERED) != 0;

    if (snapshot.wasLayered) {
        BYTE alpha = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        if (::GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) != FALSE
            && (flags & LWA_ALPHA) != 0U) {
            snapshot.originalAlpha = alpha;
        }
    }

    snapshot.valid = true;
    return snapshot;
}

bool restoreOpacityState(WindowHandle hwnd, const OpacitySnapshot &snapshot)
{
    if (!isValidWindow(hwnd) || !snapshot.valid) {
        return false;
    }

    if (!snapshot.wasLayered) {
        // 原本不是分层窗口：清掉我们加的 WS_EX_LAYERED
        return modifyExStyle(hwnd, WS_EX_LAYERED, 0);
    }

    // 原本就是分层窗口：只还原 alpha，保留样式
    return ::SetLayeredWindowAttributes(hwnd, 0, snapshot.originalAlpha, LWA_ALPHA) != FALSE;
}

bool setOpacity(WindowHandle hwnd, qreal opacity)
{
    if (!isValidWindow(hwnd)) {
        return false;
    }

    const qreal clamped = std::clamp(opacity, kMinOpacity, kMaxOpacity);
    const BYTE alpha = static_cast<BYTE>(std::lround(clamped * 255.0));

    if (alpha >= 255) {
        // 回到完全不透明：确保窗口处于正常状态
        if ((::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) {
            return true;
        }
        const BOOL ok = ::SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        return ok != FALSE;
    }

    // 需要 WS_EX_LAYERED 才能应用 alpha
    if ((::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) {
        if (!modifyExStyle(hwnd, WS_EX_LAYERED, WS_EX_LAYERED)) {
            return false;
        }
    }

    const BOOL ok = ::SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
    return ok != FALSE;
}

qreal opacity(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return 1.0;
    }
    if ((::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) {
        return 1.0;
    }

    BYTE alpha = 255;
    DWORD flags = 0;
    COLORREF key = 0;
    if (::GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags) == FALSE
        || (flags & LWA_ALPHA) == 0U) {
        return 1.0;
    }
    return static_cast<qreal>(alpha) / 255.0;
}

// ============================================================================
//  显示器
// ============================================================================

QList<MonitorInfo> monitors()
{
    QList<MonitorInfo> result;
    ::EnumDisplayMonitors(nullptr, nullptr, collectMonitorProc, reinterpret_cast<LPARAM>(&result));
    return result;
}

MonitorInfo monitorForWindow(WindowHandle hwnd)
{
    if (!isValidWindow(hwnd)) {
        return MonitorInfo();
    }
    return makeMonitorInfo(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
}

MonitorInfo monitorForPoint(const QPoint &screenPoint)
{
    POINT point{ screenPoint.x(), screenPoint.y() };
    return makeMonitorInfo(::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
}

MonitorInfo primaryMonitor()
{
    // 主显示器：坐标 (0,0) 所在的显示器
    return monitorForPoint(QPoint(0, 0));
}

MonitorInfo monitorFromNativeHandle(void *nativeMonitor)
{
    return makeMonitorInfo(static_cast<HMONITOR>(nativeMonitor));
}

// ============================================================================
//  DPI 与坐标换算
// ============================================================================

int dpiForWindow(WindowHandle hwnd)
{
    if (isValidWindow(hwnd)) {
        const UINT dpi = ::GetDpiForWindow(hwnd);
        if (dpi > 0U) {
            return static_cast<int>(dpi);
        }
    }
    const UINT systemDpi = ::GetDpiForSystem();
    return systemDpi > 0U ? static_cast<int>(systemDpi) : 96;
}

qreal scaleFactorForWindow(WindowHandle hwnd)
{
    return static_cast<qreal>(dpiForWindow(hwnd)) / 96.0;
}

QString dpiAwarenessText()
{
    const DPI_AWARENESS_CONTEXT context = ::GetThreadDpiAwarenessContext();
    switch (::GetAwarenessFromDpiAwarenessContext(context)) {
    case DPI_AWARENESS_UNAWARE:
        return QStringLiteral("DPI 不感知（系统缩放拉伸）");
    case DPI_AWARENESS_SYSTEM_AWARE:
        return QStringLiteral("系统 DPI 感知");
    case DPI_AWARENESS_PER_MONITOR_AWARE:
        return QStringLiteral("按显示器 DPI 感知（推荐）");
    default:
        break;
    }
    return QStringLiteral("未知 DPI 感知级别");
}

QRect toLogical(const QRect &physicalRect, qreal scaleFactor)
{
    if (scaleFactor <= 0.0 || qFuzzyCompare(scaleFactor, 1.0)) {
        return physicalRect;
    }
    const auto scale = [scaleFactor](int value) {
        return static_cast<int>(std::lround(static_cast<qreal>(value) / scaleFactor));
    };
    return QRect(scale(physicalRect.x()), scale(physicalRect.y()),
                 scale(physicalRect.width()), scale(physicalRect.height()));
}

QRect toPhysical(const QRect &logicalRect, qreal scaleFactor)
{
    if (scaleFactor <= 0.0 || qFuzzyCompare(scaleFactor, 1.0)) {
        return logicalRect;
    }
    const auto scale = [scaleFactor](int value) {
        return static_cast<int>(std::lround(static_cast<qreal>(value) * scaleFactor));
    };
    return QRect(scale(logicalRect.x()), scale(logicalRect.y()),
                 scale(logicalRect.width()), scale(logicalRect.height()));
}

QPoint toLogical(const QPoint &physicalPoint, qreal scaleFactor)
{
    if (scaleFactor <= 0.0 || qFuzzyCompare(scaleFactor, 1.0)) {
        return physicalPoint;
    }
    return QPoint(static_cast<int>(std::lround(static_cast<qreal>(physicalPoint.x()) / scaleFactor)),
                  static_cast<int>(std::lround(static_cast<qreal>(physicalPoint.y()) / scaleFactor)));
}

QPoint toPhysical(const QPoint &logicalPoint, qreal scaleFactor)
{
    if (scaleFactor <= 0.0 || qFuzzyCompare(scaleFactor, 1.0)) {
        return logicalPoint;
    }
    return QPoint(static_cast<int>(std::lround(static_cast<qreal>(logicalPoint.x()) * scaleFactor)),
                  static_cast<int>(std::lround(static_cast<qreal>(logicalPoint.y()) * scaleFactor)));
}

// ============================================================================
//  权限
// ============================================================================

bool isProcessElevated()
{
    // 结果只取决于进程令牌，进程生命周期内不会改变，缓存一次即可
    static const bool cached = [] {
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
            return false;
        }

        TOKEN_ELEVATION elevation{};
        DWORD returned = 0;
        const BOOL ok = ::GetTokenInformation(token,
                                              TokenElevation,
                                              &elevation,
                                              sizeof(elevation),
                                              &returned);
        ::CloseHandle(token);
        return ok != FALSE && elevation.TokenIsElevated != 0U;
    }();
    return cached;
}

} // namespace WinEase::Win32
