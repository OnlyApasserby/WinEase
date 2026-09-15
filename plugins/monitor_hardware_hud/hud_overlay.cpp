#include "hud_overlay.h"

#include "win32/WindowUtils.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QStringList>

#include <algorithm>

namespace WinEase::FeaturePlugins {

namespace {

/// 吸附阈值（逻辑像素）：拖到离工作区边缘这么近就自动贴上去
constexpr int kSnapDistanceLogical = 16;

/// 投影层数（逐层递减 alpha，叠出柔和外圈）
constexpr int kShadowLayers = 3;

} // namespace

// ============================================================================
//  构造
// ============================================================================

HudOverlay::HudOverlay(QWidget *parent)
    : WinEase::OverlayWindow(parent)
{
    setObjectName(QStringLiteral("WinEaseHardwareHudOverlay"));

    // 默认整层点击穿透：常驻面板不该抢走它下面的点击。
    // 需要拖动时由插件切到 setClickThrough(false)——那时"能点到"的范围就是面板本体
    // （分层窗口逐像素按 alpha 命中，见头文件与 OverlayWindow.h 的说明）
    setClickThrough(true);
    setNoActivate(true);

    // 悬浮层基类的鼠标信号只在"关掉穿透"时才会发出；这里直接订阅，
    // 无需重写 mousePressEvent 等虚函数（那些已经由基类翻译成信号）
    connect(this, &WinEase::OverlayWindow::overlayMousePressed,
            this, &HudOverlay::onOverlayPressed);
    connect(this, &WinEase::OverlayWindow::overlayMouseMoved,
            this, &HudOverlay::onOverlayMoved);
    connect(this, &WinEase::OverlayWindow::overlayMouseReleased,
            this, &HudOverlay::onOverlayReleased);
}

// ============================================================================
//  内容
// ============================================================================

void HudOverlay::setSnapshot(const Hud::HudSnapshot &snapshot)
{
    const QSize before = panelLogicalSize();
    m_snapshot = snapshot;
    const QSize after = panelLogicalSize();

    // 行数变化 → 窗口高度变化。锚点（左上角）保持不动，所以只重设矩形即可
    if (after != before) {
        applyGeometry(false);
    }

    if (isOverlayVisible()) {
        // 窗口本身就只有面板大小，脏区取整块面板即可（不存在"整屏重绘"的代价问题）
        requestOverlayUpdate(panelLogicalRect());
    }

    Q_EMIT snapshotChanged();
}

const Hud::HudSnapshot &HudOverlay::snapshot() const
{
    return m_snapshot;
}

int HudOverlay::metricRowCount() const
{
    return static_cast<int>(m_snapshot.metrics.size());
}

QString HudOverlay::snapshotText() const
{
    // ⚠ 这里刻意把"可用 / 不可用"也写进去：自检要能一眼看出
    //   "这一行是真实读数还是不可用原因"，而后者的判定依据就是 available
    QStringList parts;
    parts.reserve(m_snapshot.metrics.size());
    for (const Hud::MetricReading &reading : m_snapshot.metrics) {
        parts.append(QStringLiteral("%1=%2%3")
                         .arg(reading.label, reading.valueText,
                              reading.available ? QString() : QStringLiteral("[不可用]")));
    }
    if (parts.isEmpty()) {
        return QStringLiteral("（未选择任何指标）");
    }
    // ⚠ 行分隔用**换行**而不是 " · "：数值里本来就会出现 " · "
    //   （内存一行是 "61% · 10.2 GB/15.9 GB"），用点号分隔会让"按行解析"变成猜
    return parts.join(QLatin1Char('\n'));
}

void HudOverlay::setStyle(const Style &style)
{
    m_style = style;
    if (isOverlayVisible()) {
        requestOverlayUpdate();
    }
}

const HudOverlay::Style &HudOverlay::style() const
{
    return m_style;
}

// ============================================================================
//  位置
// ============================================================================

void HudOverlay::setAnchor(const QPoint &physicalTopLeft, bool allowSnap)
{
    m_anchor = physicalTopLeft;
    applyGeometry(allowSnap);
    Q_EMIT anchorChanged();
}

QPoint HudOverlay::anchorPhysical() const
{
    return m_anchor;
}

QSize HudOverlay::panelLogicalSize() const
{
    // 一个指标都没开时也要占一行：面板得能显示"未选择任何指标"的空态提示
    const int rows = std::max(1, static_cast<int>(m_snapshot.metrics.size()));
    const int height = m_style.titleHeight + m_style.paddingY * 2 + rows * m_style.rowHeight;
    return QSize(std::max(140, m_style.panelWidth), height);
}

QRect HudOverlay::panelLogicalRect() const
{
    const int margin = m_style.shadowMargin;
    return overlayLogicalRect().adjusted(margin, margin, -margin, -margin);
}

QRect HudOverlay::panelPhysicalRect() const
{
    const qreal scale = overlayScaleFactor();
    const int margin = qMax(0, qRound(m_style.shadowMargin * scale));
    return overlayGeometry().adjusted(margin, margin, -margin, -margin);
}

bool HudOverlay::isDragging() const
{
    return m_dragging;
}

void HudOverlay::applyGeometry(bool allowSnap)
{
    const QSize logicalSize = panelLogicalSize();
    if (logicalSize.isEmpty()) {
        return;
    }

    // 缩放系数取"面板所在的显示器"的，而不是当前窗口所在显示器的——
    // 跨 DPI 多屏时按当前窗口算会把面板放到错误的位置
    const WinEase::Win32::MonitorInfo monitor = WinEase::Win32::monitorForPoint(m_anchor);
    const qreal scale = (monitor.valid && monitor.scaleFactor > 0.0) ? monitor.scaleFactor : 1.0;

    const QSize panelPhysical(qMax(1, qRound(logicalSize.width() * scale)),
                              qMax(1, qRound(logicalSize.height() * scale)));

    if (allowSnap) {
        m_anchor = snappedAnchor(m_anchor, panelPhysical);
    }

    const int marginPhysical = qMax(0, qRound(m_style.shadowMargin * scale));
    const QRect windowRect(QPoint(m_anchor.x() - marginPhysical, m_anchor.y() - marginPhysical),
                           QSize(panelPhysical.width() + marginPhysical * 2,
                                 panelPhysical.height() + marginPhysical * 2));

    // setOverlayGeometry 内部会按目标显示器的缩放把物理矩形换算成逻辑几何
    // （取整策略是"宁大勿小"，所以我们不回读它来当锚点，见 anchorPhysical()）
    setOverlayGeometry(windowRect);
}

QPoint HudOverlay::snappedAnchor(const QPoint &panelTopLeft, const QSize &panelPhysicalSize) const
{
    WinEase::Win32::MonitorInfo monitor = WinEase::Win32::monitorForPoint(panelTopLeft);
    if (!monitor.valid) {
        monitor = WinEase::Win32::primaryMonitor();
    }
    if (!monitor.valid) {
        return panelTopLeft;
    }

    // 分屏/多屏必须用 workArea（已排除任务栏），否则面板会贴到任务栏底下
    QRect area = monitor.workArea;
    if (area.isEmpty()) {
        area = monitor.geometry;
    }
    if (area.isEmpty()) {
        return panelTopLeft;
    }

    const qreal scale = (monitor.scaleFactor > 0.0) ? monitor.scaleFactor : 1.0;
    const int snap = qMax(1, qRound(kSnapDistanceLogical * scale));

    // 先夹进工作区（面板不许跑到屏幕外），再按阈值吸附到最近的边
    const int maxX = area.right() + 1 - panelPhysicalSize.width();
    const int maxY = area.bottom() + 1 - panelPhysicalSize.height();

    int x = panelTopLeft.x();
    int y = panelTopLeft.y();
    x = (maxX >= area.left()) ? qBound(area.left(), x, maxX) : area.left();
    y = (maxY >= area.top()) ? qBound(area.top(), y, maxY) : area.top();

    if (qAbs(x - area.left()) <= snap) {
        x = area.left();
    } else if (qAbs(area.right() + 1 - (x + panelPhysicalSize.width())) <= snap) {
        x = maxX;
    }
    if (qAbs(y - area.top()) <= snap) {
        y = area.top();
    } else if (qAbs(area.bottom() + 1 - (y + panelPhysicalSize.height())) <= snap) {
        y = maxY;
    }

    return QPoint(x, y);
}

// ============================================================================
//  拖拽
// ============================================================================

void HudOverlay::onOverlayPressed(const QPoint &logicalPos, int button)
{
    if (button != Qt::LeftButton || !panelLogicalRect().contains(logicalPos)) {
        return;
    }

    // 整块面板都是拖拽把手：面板无论如何都会吃掉落在它身上的点击，
    // 那不如让这些点击直接有用（拖动），而不是变成一个"点了没反应"的死区
    m_dragging = true;
    m_pressLogical = logicalPos;
    m_pressAnchor = m_anchor;
    setCursor(Qt::SizeAllCursor);
    requestOverlayUpdate();
}

void HudOverlay::onOverlayMoved(const QPoint &logicalPos)
{
    if (!m_dragging) {
        return;
    }

    const qreal scale = overlayScaleFactor();
    const QPoint logicalDelta = logicalPos - m_pressLogical;
    const QPoint physicalDelta(qRound(logicalDelta.x() * scale), qRound(logicalDelta.y() * scale));

    // 拖动过程中不吸附（吸附只在松手时发生，否则会被"吸住"拖不动）
    m_anchor = m_pressAnchor + physicalDelta;
    applyGeometry(false);
    requestOverlayUpdate();
}

void HudOverlay::onOverlayReleased(const QPoint &logicalPos, int button)
{
    Q_UNUSED(logicalPos)
    if (!m_dragging || button != Qt::LeftButton) {
        return;
    }

    m_dragging = false;
    unsetCursor();
    // 松手时收敛 + 吸附一次，并把最终位置报给插件持久化
    applyGeometry(true);
    requestOverlayUpdate();
    Q_EMIT anchorCommitted(m_anchor);
}

// ============================================================================
//  绘制
// ============================================================================

void HudOverlay::paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect)
{
    Q_UNUSED(dirtyLogicalRect)
    const QRect panel = panelLogicalRect();
    if (panel.isEmpty()) {
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, true);

    // ---- 投影：几层递减 alpha 的圆角矩形（画在面板外圈，落在阴影边距里）----
    painter.setPen(Qt::NoPen);
    for (int layer = kShadowLayers; layer >= 1; --layer) {
        QColor shadow(0, 0, 0, 10 + layer * 5);
        painter.setBrush(shadow);
        const qreal spread = m_style.shadowMargin * (layer / qreal(kShadowLayers + 1));
        painter.drawRoundedRect(QRectF(panel).adjusted(-spread, -spread + 1, spread, spread + 1),
                                m_style.cornerRadius + spread,
                                m_style.cornerRadius + spread);
    }

    // ---- 面板底 + 细描边 ----
    // ⚠ 面板底 alpha 必须 > 0：分层窗口逐像素按 alpha 命中，全透明区域点了没反应
    painter.setBrush(m_style.panelColor);
    painter.setPen(QPen(m_style.borderColor, 1));
    const QRectF panelRect = QRectF(panel).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(panelRect, m_style.cornerRadius, m_style.cornerRadius);

    // ---- 标题条：只有上面两个角是圆的 ----
    const int radius = m_style.cornerRadius;
    const QRectF titleRect(panelRect.left(), panelRect.top(), panelRect.width(), m_style.titleHeight);
    QPainterPath titlePath;
    titlePath.moveTo(titleRect.bottomLeft());
    titlePath.lineTo(titleRect.left(), titleRect.top() + radius);
    titlePath.quadTo(titleRect.topLeft(), QPointF(titleRect.left() + radius, titleRect.top()));
    titlePath.lineTo(titleRect.right() - radius, titleRect.top());
    titlePath.quadTo(QPointF(titleRect.right(), titleRect.top()),
                     QPointF(titleRect.right(), titleRect.top() + radius));
    titlePath.lineTo(titleRect.bottomRight());
    titlePath.closeSubpath();

    painter.setPen(Qt::NoPen);
    painter.setBrush(m_dragging ? m_style.titleBarColor.lighter(140) : m_style.titleBarColor);
    painter.drawPath(titlePath);

    // ---- 标题文字 + 右侧拖拽提示点 ----
    QFont titleFont = painter.font();
    titleFont.setPixelSize(m_style.fontSizePx);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(m_style.titleTextColor);
    painter.drawText(QRect(panel.left() + m_style.paddingX, panel.top(),
                           panel.width() - m_style.paddingX * 2, m_style.titleHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, m_snapshot.title);

    painter.setPen(Qt::NoPen);
    painter.setBrush(m_style.mutedColor);
    for (int column = 0; column < 2; ++column) {
        for (int row = 0; row < 3; ++row) {
            painter.drawEllipse(QPointF(panel.right() - m_style.paddingX + 1 + column * 3.0,
                                        panel.top() + m_style.titleHeight / 2.0 - 4 + row * 4.0),
                                1.0, 1.0);
        }
    }

    // ---- 指标行：名称左对齐、数值右对齐 ----
    QFont rowFont = painter.font();
    rowFont.setPixelSize(m_style.fontSizePx);
    rowFont.setBold(false);
    painter.setFont(rowFont);
    const QFontMetrics metrics(rowFont);

    const int rowWidth = panel.width() - m_style.paddingX * 2;
    int rowTop = panel.top() + m_style.titleHeight + m_style.paddingY;

    if (m_snapshot.metrics.isEmpty()) {
        painter.setPen(m_style.mutedColor);
        painter.drawText(QRect(panel.left() + m_style.paddingX, rowTop, rowWidth, m_style.rowHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("未选择任何指标"));
        return;
    }

    for (const Hud::MetricReading &reading : m_snapshot.metrics) {
        const QRect rowRect(panel.left() + m_style.paddingX, rowTop, rowWidth, m_style.rowHeight);

        painter.setPen(reading.available ? m_style.textColor : m_style.mutedColor);
        painter.drawText(rowRect, Qt::AlignLeft | Qt::AlignVCenter, reading.label);

        const int labelWidth = metrics.horizontalAdvance(reading.label) + 8;
        const QRect valueRect(rowRect.left() + labelWidth, rowRect.top(),
                              rowRect.width() - labelWidth, rowRect.height());
        if (valueRect.width() > 0) {
            // 不可用项用琥珀色：与"真实读数"一眼可分（值本身是原因文本，绝不显示 0）
            painter.setPen(reading.available ? m_style.textColor : m_style.warningColor);
            painter.drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter,
                             metrics.elidedText(reading.valueText, Qt::ElideRight,
                                                valueRect.width()));
        }

        rowTop += m_style.rowHeight;
    }
}

} // namespace WinEase::FeaturePlugins
