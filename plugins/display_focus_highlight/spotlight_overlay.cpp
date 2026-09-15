#include "spotlight_overlay.h"

#include "OverlayGeometry.h"

#include <QPainter>

#include <cmath>

using WinEase::FeaturePlugins::OverlayGeometry::toLogical;

namespace {

constexpr int kRingWidth = 4;            ///< 圆环线宽（逻辑像素）
constexpr qreal kBreathAmplitude = 0.08; ///< 呼吸幅度（±8% 半径）
/// 自己写 2π：MSVC 的 <cmath> 默认不定义 M_PI（需要 _USE_MATH_DEFINES，别为这个改全局宏）
constexpr qreal kTwoPi = 6.283185307179586;

} // namespace

SpotlightOverlay::SpotlightOverlay(QWidget *parent)
    : WinEase::OverlayWindow(parent)
{
}

void SpotlightOverlay::setStyle(const Style &style)
{
    m_style = style;
    requestOverlayUpdate();
}

void SpotlightOverlay::setFocusPoint(const QPoint &physicalPoint)
{
    if (m_focus == physicalPoint) {
        return;
    }
    // 脏区必须取"旧位置 ∪ 新位置"：只刷新新位置的话，
    // 旧的那个洞会一直留在屏幕上（分层窗口不会自己清理没重绘过的像素）
    const QRect previous = ringBoundsLogical();
    m_focus = physicalPoint;
    requestOverlayUpdate(previous.united(ringBoundsLogical()));
}

void SpotlightOverlay::setBreathingPhase(qreal phase)
{
    if (m_style.breathing) {
        const QRect previous = ringBoundsLogical();
        m_phase = phase;
        requestOverlayUpdate(previous.united(ringBoundsLogical()));
        return;
    }
    m_phase = phase;
}

bool SpotlightOverlay::containsPhysicalPoint(const QPoint &physicalPoint) const
{
    return overlayGeometry().contains(physicalPoint);
}

qreal SpotlightOverlay::effectiveRadius() const
{
    qreal radius = qMax(1, m_style.radius);
    if (m_style.breathing) {
        radius *= 1.0 + kBreathAmplitude * std::sin(m_phase * kTwoPi);
    }
    return radius;
}

QColor SpotlightOverlay::veilColor() const
{
    const int alpha = qBound(0, m_style.veilAlpha, 255);
    return QColor(0, 0, 0, alpha);
}

QRect SpotlightOverlay::ringBoundsLogical() const
{
    const qreal scale = overlayScaleFactor();
    const qreal physicalRadius = effectiveRadius() + kRingWidth * 4.0;
    const qreal logicalRadius = (scale > 0.0) ? physicalRadius / scale : physicalRadius;
    const QPointF center = toLogical(*this, m_focus);
    const int inset = static_cast<int>(std::ceil(logicalRadius)) + 1;
    return QRect(center.toPoint() - QPoint(inset, inset), QSize(inset * 2, inset * 2));
}

void SpotlightOverlay::paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect)
{
    Q_UNUSED(dirtyLogicalRect);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal scale = overlayScaleFactor();
    const bool focusOnThisScreen = containsPhysicalPoint(m_focus);

    // ---- 压暗：光标不在本层时也压暗（多显示器下"别的屏也要变暗"才符合直觉）----
    if (!m_style.ringOnly && m_style.veilAlpha > 0) {
        painter.fillRect(rect(), veilColor());
    }

    if (!focusOnThisScreen) {
        return;
    }

    const QPointF center = toLogical(*this, m_focus);
    const qreal logicalRadius = (scale > 0.0) ? effectiveRadius() / scale : effectiveRadius();

    // ---- 打洞：把洞里的 alpha 清零（这才是"聚光灯"而不是"再盖一层"）----
    if (!m_style.ringOnly && m_style.veilAlpha > 0) {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawEllipse(center, logicalRadius, logicalRadius);
        painter.restore();
    }

    // ---- 洞边圆环 ----
    if (m_style.ringColor.alpha() > 0) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_style.ringColor, kRingWidth));
        const qreal ringRadius = logicalRadius + kRingWidth / 2.0;
        painter.drawEllipse(center, ringRadius, ringRadius);
    }
}
