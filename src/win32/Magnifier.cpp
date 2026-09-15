#include "win32/Magnifier.h"

#include "win32/Win32Error.h"

#include <algorithm>

#include <windows.h>

#include <magnification.h> // 需要 windows.h 在前；随 Windows SDK 提供，链接 Magnification.lib

namespace WinEase::Win32 {

namespace {

const wchar_t *const kHostClassName = L"WinEaseMagnifierHost";
const wchar_t *const kHostTitle = L"WinEase 放大镜";

/// 尺寸/倍率的合法区间（面板上的控件也按这个区间约束，
/// 免得有人手改配置写出一个"源矩形比屏幕还大"的倍率）
constexpr int kMinSizePx = 100;
constexpr int kMaxSizePx = 800;
constexpr double kMinFactor = 1.2;
constexpr double kMaxFactor = 10.0;

/// 宿主窗口过程。唯一职责是**点击穿透**：
/// `WM_NCHITTEST` 返回 `HTTRANSPARENT` 是文档化的做法，鼠标消息会落到下方窗口，
/// 于是用户能一边开着放大镜、一边正常操作它底下的东西（放大镜是"看"的工具，
/// 不该顺手把点击吃掉）。
LRESULT CALLBACK magnifierHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

/// 注册宿主窗口类。窗口类本身就是进程级资源，所以这里**不用静态标志位**：
/// 每次注册，把"已经注册过"当成功处理（平台层的约定是没有模块级全局状态）。
bool ensureHostClassRegistered(QString *errorOut)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &magnifierHostProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kHostClassName;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);

    if (::RegisterClassExW(&windowClass) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            if (errorOut != nullptr) {
                *errorOut = describeFailure(QStringLiteral("注册放大镜宿主窗口类"), error);
            }
            return false;
        }
    }
    return true;
}

} // namespace

QString magnifierHostClassName()
{
    return QString::fromWCharArray(kHostClassName);
}

QString magnifierShapeKey(MagnifierShape shape)
{
    switch (shape) {
    case MagnifierShape::Rectangle:
        return QStringLiteral("rect");
    case MagnifierShape::Rounded:
        return QStringLiteral("rounded");
    case MagnifierShape::Circle:
        return QStringLiteral("circle");
    }
    return QStringLiteral("rounded");
}

MagnifierShape magnifierShapeFromKey(const QString &key)
{
    if (key == QLatin1String("rect")) {
        return MagnifierShape::Rectangle;
    }
    if (key == QLatin1String("circle")) {
        return MagnifierShape::Circle;
    }
    return MagnifierShape::Rounded; // 缺省/无法识别 → 圆角（最常用的观感）
}

bool Magnifier::apiAvailable(QString *errorOut)
{
    if (::MagInitialize() == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("初始化 Magnification API"), ::GetLastError());
        }
        return false;
    }
    ::MagUninitialize();
    return true;
}

Magnifier::Magnifier() = default;

Magnifier::~Magnifier()
{
    destroy();
}

bool Magnifier::create(int sizePx, double factor, MagnifierShape shape, QString *errorOut)
{
    destroy();

    m_sizePx = std::clamp(sizePx, kMinSizePx, kMaxSizePx);
    m_factor = std::clamp(factor, kMinFactor, kMaxFactor);
    m_shape = shape;

    if (::MagInitialize() == FALSE) {
        // 典型原因：本进程不是 DPI 感知的，或桌面合成被禁用
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("初始化 Magnification API"), ::GetLastError());
        }
        return false;
    }
    m_apiInitialized = true;

    if (!ensureHostClassRegistered(errorOut)) {
        destroy();
        return false;
    }

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    // 宿主：置顶弹层、不进任务栏、不抢焦点（放大镜不该打断用户正在打的字）
    m_host = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               kHostClassName,
                               kHostTitle,
                               WS_POPUP,
                               0,
                               0,
                               m_sizePx,
                               m_sizePx,
                               nullptr,
                               nullptr,
                               instance,
                               nullptr);
    if (m_host == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("创建放大镜宿主窗口"), ::GetLastError());
        }
        destroy();
        return false;
    }

    m_magnifier = ::CreateWindowExW(0,
                                    WC_MAGNIFIER,
                                    L"WinEaseMagnifier",
                                    WS_CHILD | WS_VISIBLE,
                                    0,
                                    0,
                                    m_sizePx,
                                    m_sizePx,
                                    m_host,
                                    nullptr,
                                    instance,
                                    nullptr);
    if (m_magnifier == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("创建放大镜子窗口（WC_MAGNIFIER）"),
                                        ::GetLastError());
        }
        destroy();
        return false;
    }

    // 把放大窗自己排除在放大内容之外：否则窗口一旦压到源区域上就会"镜中镜"无限递归
    HWND excluded[1] = { m_host };
    if (::MagSetWindowFilterList(m_magnifier, MW_FILTERMODE_EXCLUDE, 1, excluded) == FALSE
        && errorOut != nullptr) {
        *errorOut = describeFailure(QStringLiteral("设置放大镜排除窗口"), ::GetLastError());
    }

    // 放大内容里带上系统光标：用户就是盯着光标底下那点东西看的
    ::MagShowSystemCursor(TRUE);

    applyRegion();

    // 建窗前先摆好位置再显示，避免先在 (0,0) 闪一下
    ::SetWindowPos(m_host, HWND_TOPMOST, 0, 0, m_sizePx, m_sizePx, SWP_NOACTIVATE);
    QString followError;
    followCursor(&followError);
    ::ShowWindow(m_host, SW_SHOWNOACTIVATE);

    return isValid();
}

void Magnifier::destroy()
{
    if (m_magnifier != nullptr && ::IsWindow(m_magnifier) != FALSE) {
        ::DestroyWindow(m_magnifier);
    }
    if (m_host != nullptr && ::IsWindow(m_host) != FALSE) {
        ::DestroyWindow(m_host);
    }
    m_magnifier = nullptr;
    m_host = nullptr;
    m_hasSource = false;
    m_lastSource.reset(); // 新窗口没有"上一次的源矩形"，第一次必须真的设下去

    // 与 create() 里的 MagInitialize 成对：窗口全销毁之后再反初始化
    if (m_apiInitialized) {
        ::MagUninitialize();
        m_apiInitialized = false;
    }
}

bool Magnifier::isValid() const
{
    return m_host != nullptr && m_magnifier != nullptr && ::IsWindow(m_host) != FALSE
           && ::IsWindow(m_magnifier) != FALSE;
}

WindowHandle Magnifier::hostWindow() const
{
    return isValid() ? m_host : nullptr;
}

WindowHandle Magnifier::magnifierWindow() const
{
    return isValid() ? m_magnifier : nullptr;
}

bool Magnifier::followCursor(QString *errorOut)
{
    if (!isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("放大镜窗口尚未创建");
        }
        return false;
    }

    POINT cursor{};
    if (::GetCursorPos(&cursor) == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("读取光标位置"), ::GetLastError());
        }
        return false;
    }
    const QPoint point(static_cast<int>(cursor.x), static_cast<int>(cursor.y));

    const MonitorInfo monitor = monitorForPoint(point);
    const QRect workArea = monitor.valid
                               ? monitor.workArea
                               : QRect(0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                                       ::GetSystemMetrics(SM_CYSCREEN));
    const QPoint target = preferredTopLeft(point, QSize(m_sizePx, m_sizePx), workArea);

    if (windowRect().topLeft() != target) {
        ::SetWindowPos(m_host, HWND_TOPMOST, target.x(), target.y(), m_sizePx, m_sizePx,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    return applySource(point, errorOut);
}

bool Magnifier::setSourceCenter(const QPoint &screenPoint, QString *errorOut)
{
    if (!isValid()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("放大镜窗口尚未创建");
        }
        return false;
    }
    return applySource(screenPoint, errorOut);
}

bool Magnifier::applySource(const QPoint &center, QString *errorOut)
{
    if (m_magnifier == nullptr || ::IsWindow(m_magnifier) == FALSE) {
        return false;
    }

    // Magnification API 里"倍率"的表达方式：源矩形边长 = 窗口边长 / 倍率
    const int sourceSize = std::max(1, qRound(static_cast<double>(m_sizePx) / m_factor));
    QRect source(center.x() - sourceSize / 2, center.y() - sourceSize / 2, sourceSize, sourceSize);

    // 源矩形必须落在显示器范围内，越界 MagSetWindowSource 直接失败（屏幕边缘很常见）
    const MonitorInfo monitor = monitorForPoint(center);
    if (monitor.valid) {
        const QRect bounds = monitor.geometry;
        if (bounds.width() >= sourceSize) {
            source.moveLeft(
                std::clamp(source.left(), bounds.left(), bounds.right() + 1 - sourceSize));
        }
        if (bounds.height() >= sourceSize) {
            source.moveTop(
                std::clamp(source.top(), bounds.top(), bounds.bottom() + 1 - sourceSize));
        }
    }

    const RECT rect{ source.left(), source.top(), source.right() + 1, source.bottom() + 1 };

    // 只在**源矩形真的变了**时才调 MagSetWindowSource。
    // 每 30ms 无条件重设一次是纯浪费：光标没动、倍率没改时它带来的只有一次
    // 无意义的跨进程/内核往返（实测这套"无变化也照设"占了跟随成本的大头）。
    // 注意重绘（InvalidateRect）**不能**省 —— 源区没变不代表里面的内容没变
    // （用户正在打字、页面在滚动），省掉它放大窗就会停在旧画面上。
    if (!m_lastSource.has_value() || *m_lastSource != source) {
        if (::MagSetWindowSource(m_magnifier, rect) == FALSE) {
            if (errorOut != nullptr) {
                *errorOut = describeFailure(QStringLiteral("设置放大源区域"), ::GetLastError());
            }
            return false;
        }
        m_lastSource = source;
    }

    // 官方示例每个刷新周期都会让放大窗重绘一次：源内容变了（打字、滚动、动画）
    // 才会跟着变。注意 FALSE = 不擦背景（擦背景反而会闪）。
    ::InvalidateRect(m_magnifier, nullptr, FALSE);

    m_sourceCenter = center;
    m_hasSource = true;
    return true;
}

bool Magnifier::setFactor(double factor, QString *errorOut)
{
    m_factor = std::clamp(factor, kMinFactor, kMaxFactor);
    if (!isValid() || !m_hasSource) {
        return true; // 没开窗时只改参数，下次创建时生效
    }
    return applySource(m_sourceCenter, errorOut);
}

bool Magnifier::setSizePx(int sizePx, QString *errorOut)
{
    m_sizePx = std::clamp(sizePx, kMinSizePx, kMaxSizePx);
    if (!isValid()) {
        return true;
    }
    applyRegion(); // 圆角半径/圆形都跟尺寸有关，尺寸一变遮罩要重做
    return followCursor(errorOut);
}

bool Magnifier::setShape(MagnifierShape shape, QString *errorOut)
{
    Q_UNUSED(errorOut)
    m_shape = shape;
    if (!isValid()) {
        return true;
    }
    applyRegion();
    return true;
}

void Magnifier::applyRegion()
{
    if (m_host == nullptr || ::IsWindow(m_host) == FALSE) {
        return;
    }

    const int size = m_sizePx;
    HRGN region = nullptr;
    switch (m_shape) {
    case MagnifierShape::Rectangle:
        region = nullptr; // 空区域 = 无遮罩（整块矩形）
        break;
    case MagnifierShape::Rounded: {
        const int diameter = std::max(16, size / 4); // CreateRoundRectRgn 收的是椭圆**直径**
        region = ::CreateRoundRectRgn(0, 0, size + 1, size + 1, diameter, diameter);
        break;
    }
    case MagnifierShape::Circle:
        region = ::CreateEllipticRgn(0, 0, size + 1, size + 1);
        break;
    }

    // SetWindowRgn 成功时窗口接管区域对象的所有权（不能再删）；失败则要自己删，否则泄漏 GDI 对象
    if (::SetWindowRgn(m_host, region, TRUE) == 0 && region != nullptr) {
        ::DeleteObject(region);
    }
}

QRect Magnifier::sourceRect() const
{
    RECT rect{};
    if (m_magnifier == nullptr || ::IsWindow(m_magnifier) == FALSE
        || ::MagGetWindowSource(m_magnifier, &rect) == FALSE) {
        return QRect();
    }
    return QRect(static_cast<int>(rect.left),
                 static_cast<int>(rect.top),
                 static_cast<int>(rect.right - rect.left),
                 static_cast<int>(rect.bottom - rect.top));
}

QRect Magnifier::windowRect() const
{
    RECT rect{};
    if (m_host == nullptr || ::IsWindow(m_host) == FALSE || ::GetWindowRect(m_host, &rect) == FALSE) {
        return QRect();
    }
    return QRect(static_cast<int>(rect.left),
                 static_cast<int>(rect.top),
                 static_cast<int>(rect.right - rect.left),
                 static_cast<int>(rect.bottom - rect.top));
}

QPoint Magnifier::preferredTopLeft(const QPoint &cursor, const QSize &size, const QRect &workArea)
{
    constexpr int kGap = 24; // 与光标拉开一点距离：光标底下那块正是用户要看的内容

    int x = cursor.x() + kGap;
    int y = cursor.y() + kGap;

    // 靠边翻转：右下放不下就翻到左上，目标始终是"整块放大窗都在屏幕里"
    if (x + size.width() > workArea.right() + 1) {
        x = cursor.x() - kGap - size.width();
    }
    if (y + size.height() > workArea.bottom() + 1) {
        y = cursor.y() - kGap - size.height();
    }

    // 兜底夹取：光标紧贴边角时，"翻转"也可能越界
    const int maxX = std::max(workArea.left(), workArea.right() + 1 - size.width());
    const int maxY = std::max(workArea.top(), workArea.bottom() + 1 - size.height());
    x = std::clamp(x, workArea.left(), maxX);
    y = std::clamp(y, workArea.top(), maxY);
    return QPoint(x, y);
}

} // namespace WinEase::Win32
