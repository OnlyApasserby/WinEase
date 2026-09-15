#include "ruler_overlay.h"

#include "OverlayGeometry.h"

#include <QFont>
#include <QFontMetrics>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <cmath>

using WinEase::FeaturePlugins::OverlayGeometry::toLogical;

namespace {

// 绘制尺寸（逻辑像素）
constexpr int kBandWidth = 26;      ///< 标尺带半透明底
constexpr int kMajorTick = 14;      ///< 每 50 逻辑像素的长刻度
constexpr int kMinorTick = 7;       ///< 每 10 逻辑像素的短刻度
constexpr int kTickStep = 10;
constexpr int kMajorEvery = 5;      ///< 每 5 个小格一个长刻度
constexpr int kHandleRadius = 6;
constexpr int kTextMargin = 6;

const QColor kBandColor(0, 0, 0, 110);        ///< 半透明底（不是全透明，但要挡得住背景噪点）
const QColor kLineColor(0, 200, 255, 255);    ///< 主线：不透明 —— 拖拽命中就靠它
const QColor kTickColor(0, 200, 255, 200);
const QColor kActiveColor(255, 193, 7, 255);  ///< 活动标尺用琥珀色，便于区分多条
const QColor kTextColor(255, 255, 255, 255);
const QColor kTextBgColor(0, 0, 0, 200);

} // namespace

qreal RulerOverlay::Ruler::length() const
{
    return std::hypot(static_cast<qreal>(end.x() - start.x()),
                      static_cast<qreal>(end.y() - start.y()));
}

QRect RulerOverlay::Ruler::bounds() const
{
    // 至少留 1 像素：零长标尺也要有可命中的区域，否则"刚落点"时看不到也点不到
    return QRect(start, end).normalized().adjusted(-kHandleRadius, -kHandleRadius,
                                                   kHandleRadius, kHandleRadius);
}

RulerOverlay::RulerOverlay(QWidget *parent)
    : WinEase::OverlayWindow(parent)
{
}

void RulerOverlay::setRulers(const QList<Ruler> &rulers)
{
    m_rulers = rulers;
    requestOverlayUpdate();
}

void RulerOverlay::setUnit(const QString &unitName, qreal physicalPixelsPerUnit)
{
    m_unitName = unitName;
    m_pixelsPerUnit = (physicalPixelsPerUnit > 0.0) ? physicalPixelsPerUnit : 1.0;
    requestOverlayUpdate();
}

void RulerOverlay::setInteractive(bool interactive)
{
    m_interactive = interactive;
    requestOverlayUpdate();
}

void RulerOverlay::setActiveIndex(int index)
{
    m_activeIndex = index;
    requestOverlayUpdate();
}

QString RulerOverlay::lengthText(qreal physicalPixels) const
{
    const qreal value = physicalPixels / m_pixelsPerUnit;
    if (m_unitName == QLatin1String("px")) {
        return QStringLiteral("%1 px").arg(qRound(value));
    }
    // cm / in 的小数位多一点才看得出差别（物理尺寸由系统 DPI 估算）
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(m_unitName);
}

void RulerOverlay::paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect)
{
    Q_UNUSED(dirtyLogicalRect);

    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_rulers.size(); ++i) {
        paintOneRuler(painter, m_rulers.at(i), i == m_activeIndex);
    }
}

void RulerOverlay::paintOneRuler(QPainter &painter, const Ruler &ruler, bool active) const
{
    const QPointF a = toLogical(*this, ruler.start);
    const QPointF b = toLogical(*this, ruler.end);

    QLineF line(a, b);
    const qreal logicalLength = line.length();
    if (logicalLength < 0.5) {
        // 零长标尺：只画一个起点手柄（用户"刚落点"时能看到自己点在哪）
        painter.setPen(Qt::NoPen);
        painter.setBrush(kActiveColor);
        painter.drawEllipse(a, kHandleRadius, kHandleRadius);
        return;
    }

    const QColor lineColor = active ? kActiveColor : kLineColor;

    // ---- 半透明"带"（沿线的矩形；用旋转坐标系画，省得手算法线）----
    painter.save();
    painter.translate(a);
    painter.rotate(-line.angle()); // Qt 的角度是逆时针为正，y 轴向下 → 取负
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBandColor);
    painter.drawRect(QRectF(0, -kBandWidth / 2.0, logicalLength, kBandWidth));

    // ---- 刻度：每 kTickStep 一小格，每 kMajorEvery 格一个长刻度 ----
    // 刻度密度按逻辑像素算，视觉上在各缩放级别都一致
    painter.setPen(QPen(kTickColor, 1));
    int index = 0;
    for (qreal position = kTickStep; position < logicalLength; position += kTickStep, ++index) {
        const qreal half = ((index + 1) % kMajorEvery == 0) ? kMajorTick / 2.0 : kMinorTick / 2.0;
        painter.drawLine(QPointF(position, -half), QPointF(position, half));
    }

    // ---- 主线：不透明、稍粗 —— 这就是"可拖拽区域"的本体 ----
    painter.setPen(QPen(lineColor, 3));
    painter.drawLine(QPointF(0, 0), QPointF(logicalLength, 0));
    painter.restore();

    // ---- 两端手柄（不透明的实心圆，命中测试依赖它）----
    painter.setPen(Qt::NoPen);
    painter.setBrush(lineColor);
    painter.drawEllipse(a, kHandleRadius, kHandleRadius);
    painter.drawEllipse(b, kHandleRadius, kHandleRadius);

    // ---- 长度气泡：画在中点上方，**不随线旋转**（斜向时也保持正立好读）----
    const QPointF middle = (a + b) / 2.0;
    const QString text = lengthText(ruler.length());
    const QFontMetrics metrics(painter.font());
    const QRect textRect = metrics.boundingRect(text).adjusted(-kTextMargin, -kTextMargin / 2,
                                                               kTextMargin, kTextMargin / 2);
    QRectF bubble(middle.x() - textRect.width() / 2.0,
                  middle.y() - kBandWidth / 2.0 - textRect.height() - 6,
                  textRect.width(),
                  textRect.height());

    // 贴着屏幕上方时气泡会跑出图层 → 挪到线的下方
    if (bubble.top() < 2) {
        bubble.moveTop(middle.y() + kBandWidth / 2.0 + 6);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(kTextBgColor);
    painter.drawRoundedRect(bubble, 4, 4);
    painter.setPen(kTextColor);
    painter.drawText(bubble, Qt::AlignCenter, text);
}
