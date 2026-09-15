#include "sdk/OverlayWindow.h"

#include "win32/WindowUtils.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QThread>
#include <QWindow>

#include <windows.h>

namespace WinEase {

namespace {

/// 悬浮层窗口标记（宿主与自检据此识别"这是不是 WinEase 残留的悬浮层"）
constexpr const wchar_t *kOverlayMarker = L"WinEaseOverlayWindow";

/// 由 Qt 的 WId 取出原生 HWND
HWND nativeHandleOf(const QWidget *widget)
{
    if (widget == nullptr) {
        return nullptr;
    }
    // WId 在不同 Qt 版本/平台下可能是 HWND 或 quintptr，统一经 quintptr 转换
    return reinterpret_cast<HWND>(
        static_cast<quintptr>(const_cast<QWidget *>(widget)->winId()));
}

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

OverlayWindow::OverlayWindow(QWidget *parent)
    : QWidget(parent)
{
    // 悬浮层只能在 GUI 线程创建
    Q_ASSERT(QCoreApplication::instance() != nullptr);
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    setObjectName(QStringLiteral("WinEaseOverlay"));

    // 无边框 + 置顶 + 工具窗口（不进任务栏、不出现在 Alt+Tab）
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool;
    // 这两个标志在原生窗口创建时决定 WS_EX_TRANSPARENT / WS_EX_NOACTIVATE，
    // 是"点击穿透"与"不抢焦点"的主路径；ensureNativeWindow() 里还会再兜底一次
    if (m_clickThrough) {
        flags |= Qt::WindowTransparentForInput;
    }
    if (m_noActivate) {
        flags |= Qt::WindowDoesNotAcceptFocus;
    }
    setWindowFlags(flags);

    // 半透明背景：悬浮层的绘制内容自带 alpha 通道
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
}

OverlayWindow::~OverlayWindow()
{
    // 尽量在销毁前先隐藏：分层窗口未隐藏就销毁时，DWM 可能残留一帧画面
    if (isVisible()) {
        hide();
    }
    delete m_backingLayer;
    m_backingLayer = nullptr;
}

// ============================================================================
//  原生窗口与扩展样式
// ============================================================================

void OverlayWindow::ensureNativeWindow()
{
    const HWND hwnd = nativeHandleOf(this);
    if (hwnd == nullptr) {
        return;
    }

    // 打标记（幂等）：宿主与自检据此判断是否有悬浮层窗口泄漏
    if (::GetPropW(hwnd, kOverlayMarker) == nullptr) {
        ::SetPropW(hwnd, kOverlayMarker, reinterpret_cast<HANDLE>(1));
    }

    // 与窗口标志双保险。若 Qt 未来改变了标志→扩展样式的映射，这里仍能保证语义
    Win32::setClickThrough(hwnd, m_clickThrough);
    Win32::setNoActivate(hwnd, m_noActivate);
}

bool OverlayWindow::isClickThrough() const
{
    return m_clickThrough;
}

void OverlayWindow::setClickThrough(bool on)
{
    if (m_clickThrough == on) {
        return;
    }
    m_clickThrough = on;

    if (!isVisible()) {
        // 尚未显示：改窗口标志即可（还没有原生窗口，代价为零）
        Qt::WindowFlags flags = windowFlags();
        flags.setFlag(Qt::WindowTransparentForInput, on);
        setWindowFlags(flags);
        return;
    }

    // 已显示：直接改扩展样式。刻意不用 setWindowFlags——那会重建原生窗口导致闪烁
    Win32::setClickThrough(nativeHandleOf(this), on);
}

bool OverlayWindow::isNoActivate() const
{
    return m_noActivate;
}

void OverlayWindow::setNoActivate(bool on)
{
    if (m_noActivate == on) {
        return;
    }
    m_noActivate = on;

    if (!isVisible()) {
        Qt::WindowFlags flags = windowFlags();
        flags.setFlag(Qt::WindowDoesNotAcceptFocus, on);
        setWindowFlags(flags);
        return;
    }

    Win32::setNoActivate(nativeHandleOf(this), on);
}

// ============================================================================
//  放置（物理像素 → 逻辑像素）
// ============================================================================

bool OverlayWindow::setOverlayGeometry(const QRect &physicalScreenRect)
{
    if (!physicalScreenRect.isValid() || physicalScreenRect.isEmpty()) {
        return false;
    }

    // 目标显示器决定缩放系数。注意必须用"目标位置所在显示器"的系数，
    // 而不是当前所在显示器的——这正是跨 DPI 多屏下不错位的关键
    const Win32::MonitorInfo monitor = Win32::monitorForPoint(physicalScreenRect.center());
    const qreal scale = (monitor.valid && monitor.scaleFactor > 0.0) ? monitor.scaleFactor : 1.0;

    ensureNativeWindow();
    setGeometry(Win32::toLogical(physicalScreenRect, scale));
    return true;
}

bool OverlayWindow::coverMonitor(int monitorIndex)
{
    const QList<Win32::MonitorInfo> all = Win32::monitors();
    if (monitorIndex < 0 || monitorIndex >= all.size() || !all.at(monitorIndex).valid) {
        return false;
    }
    return setOverlayGeometry(all.at(monitorIndex).geometry);
}

bool OverlayWindow::coverMonitorAt(const QPoint &physicalScreenPoint)
{
    const Win32::MonitorInfo monitor = Win32::monitorForPoint(physicalScreenPoint);
    if (!monitor.valid) {
        return false;
    }
    return setOverlayGeometry(monitor.geometry);
}

bool OverlayWindow::coverAllScreens()
{
    const QList<Win32::MonitorInfo> all = Win32::monitors();
    QRect unionRect;
    for (const Win32::MonitorInfo &monitor : all) {
        if (!monitor.valid) {
            continue;
        }
        unionRect = unionRect.isNull() ? monitor.geometry : unionRect.united(monitor.geometry);
    }
    if (unionRect.isNull() || unionRect.isEmpty()) {
        return false;
    }
    return setOverlayGeometry(unionRect);
}

QRect OverlayWindow::overlayGeometry() const
{
    // 读回实际生效的物理矩形，而不是回读请求值——这样"设置是否真的生效"可被验证
    const HWND hwnd = nativeHandleOf(this);
    if (hwnd == nullptr) {
        return QRect();
    }
    return Win32::windowRect(hwnd);
}

qreal OverlayWindow::overlayScaleFactor() const
{
    if (const QScreen *current = screen()) {
        const qreal ratio = current->devicePixelRatio();
        return ratio > 0.0 ? ratio : 1.0;
    }
    return 1.0;
}

QRect OverlayWindow::overlayLogicalRect() const
{
    return QRect(QPoint(0, 0), size());
}

// ============================================================================
//  绘制
// ============================================================================

void OverlayWindow::paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect)
{
    Q_UNUSED(painter)
    Q_UNUSED(dirtyLogicalRect)
    // 默认什么都不画：纯透明覆盖层
}

void OverlayWindow::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform, true);

    // 先清空本次要重绘的区域。
    // 半透明顶层窗口的内容是带 alpha 的，若不清底，上一次的画面会残留在更新区域里
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(event->rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (m_autoPaintBackingLayer) {
        paintBackingLayer(painter);
    }

    // event->rect() 是逻辑坐标（QWidget 坐标系），与绘制契约一致
    paintOverlay(painter, event->rect());
}

void OverlayWindow::requestOverlayUpdate(const QRect &dirtyLogicalRect)
{
    if (!isVisible()) {
        return;
    }
    if (dirtyLogicalRect.isNull() || dirtyLogicalRect.isEmpty()) {
        update();
        return;
    }
    update(dirtyLogicalRect);
}

void OverlayWindow::updateBackingLayerSize()
{
    const qreal devicePixelRatio = overlayScaleFactor();
    const QSize logicalSize = size();
    if (logicalSize.isEmpty()) {
        return;
    }

    const QSize physicalSize(qMax(1, qRound(logicalSize.width() * devicePixelRatio)),
                             qMax(1, qRound(logicalSize.height() * devicePixelRatio)));

    if (m_backingLayer != nullptr && m_backingLayer->size() == physicalSize
        && qFuzzyCompare(m_backingLayer->devicePixelRatio(), devicePixelRatio)) {
        return;
    }

    // 尺寸或缩放变化：重建。内容无法安全保留，调用方需要重画（头文件已注明）
    auto *replacement = new QPixmap(physicalSize);
    replacement->setDevicePixelRatio(devicePixelRatio);
    replacement->fill(Qt::transparent);

    delete m_backingLayer;
    m_backingLayer = replacement;
}

QPixmap *OverlayWindow::backingLayer()
{
    updateBackingLayerSize();
    if (m_backingLayer == nullptr) {
        // 窗口尺寸尚未确定（未显示/未放置）：给个 1×1 占位，避免调用方拿到空指针
        m_backingLayer = new QPixmap(1, 1);
        m_backingLayer->fill(Qt::transparent);
    }
    return m_backingLayer;
}

void OverlayWindow::clearBackingLayer()
{
    if (m_backingLayer == nullptr) {
        return;
    }
    m_backingLayer->fill(Qt::transparent);
}

void OverlayWindow::paintBackingLayer(QPainter &painter)
{
    if (m_backingLayer == nullptr || m_backingLayer->isNull()) {
        return;
    }
    // 目标矩形用逻辑尺寸：位图已带 devicePixelRatio，会自动缩放到 1:1 物理像素
    painter.drawPixmap(QRect(QPoint(0, 0), size()), *m_backingLayer);
}

void OverlayWindow::setBackingLayerAutoPaint(bool on)
{
    m_autoPaintBackingLayer = on;
}

bool OverlayWindow::isBackingLayerAutoPaint() const
{
    return m_autoPaintBackingLayer;
}

// ============================================================================
//  内容控件
// ============================================================================

QWidget *OverlayWindow::setContentWidget(QWidget *content)
{
    if (m_contentWidget == content) {
        return m_contentWidget;
    }
    if (m_contentWidget != nullptr) {
        m_contentWidget->setParent(nullptr);
        m_contentWidget->deleteLater();
        m_contentWidget = nullptr;
    }

    m_contentWidget = content;
    if (m_contentWidget != nullptr) {
        m_contentWidget->setParent(this);
        m_contentWidget->setGeometry(rect());
        m_contentWidget->show();
    }
    return m_contentWidget;
}

// ============================================================================
//  事件
// ============================================================================

void OverlayWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 原生窗口此时已存在：打标记并确保扩展样式生效
    ensureNativeWindow();
    updateBackingLayerSize();
    Q_EMIT overlayShown();
}

void OverlayWindow::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    Q_EMIT overlayHidden();
}

void OverlayWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateBackingLayerSize();
    if (m_contentWidget != nullptr) {
        m_contentWidget->setGeometry(rect());
    }
}

void OverlayWindow::mousePressEvent(QMouseEvent *event)
{
    if (m_clickThrough) {
        event->ignore(); // 正常情况下根本收不到事件，这里只是防御
        return;
    }
    Q_EMIT overlayMousePressed(event->position().toPoint(), static_cast<int>(event->button()));
    event->accept();
}

void OverlayWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_clickThrough) {
        event->ignore();
        return;
    }
    Q_EMIT overlayMouseMoved(event->position().toPoint());
    event->accept();
}

void OverlayWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_clickThrough) {
        event->ignore();
        return;
    }
    Q_EMIT overlayMouseReleased(event->position().toPoint(), static_cast<int>(event->button()));
    event->accept();
}

// ============================================================================
//  生命周期
// ============================================================================

bool OverlayWindow::isOverlayVisible() const
{
    return isVisible();
}

void OverlayWindow::closeOverlay()
{
    // 顺序很关键：先隐藏 → 走一次事件循环让隐藏真正提交给合成器 → 再销毁。
    // 半透明分层窗口若在可见状态下直接销毁，DWM 可能残留一帧画面（俗称残影）
    if (isVisible()) {
        hide();
    }
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    deleteLater();
}

bool OverlayWindow::isOverlayWindow(void *nativeHandle)
{
    if (nativeHandle == nullptr) {
        return false;
    }
    return ::GetPropW(static_cast<HWND>(nativeHandle), overlayMarkerProperty()) != nullptr;
}

const wchar_t *OverlayWindow::overlayMarkerProperty()
{
    return kOverlayMarker;
}

} // namespace WinEase
