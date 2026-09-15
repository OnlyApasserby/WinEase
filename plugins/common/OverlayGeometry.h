#pragma once

// ============================================================================
//  OverlayGeometry.h —— 悬浮层坐标换算（P1-07 / P1-08 共用）
//
//  为什么单独抽出来：
//      OverlayKit 的坐标契约是"**物理像素进来、逻辑坐标画出去**"
//      （见 src/sdk/OverlayWindow.h 的坐标契约一节）。标尺与焦点高亮都要把
//      "光标/窗口的物理坐标"换算成"本层内的逻辑坐标"，
//      这个换算只要写错一次，症状就是"高 DPI 下图形整体偏移 1/3 屏"。
//      与其在两个插件里各推导一遍，不如只有一处实现。
//
//  ⚠ 本头文件不含 Q_OBJECT，且全部为 inline 纯函数。
// ============================================================================

#include "sdk/OverlayWindow.h"

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

namespace WinEase::FeaturePlugins::OverlayGeometry {

/// 物理屏幕坐标 → 某悬浮层的**逻辑**坐标（原点 = 该层左上角）
inline QPointF toLogical(const WinEase::OverlayWindow &overlay, const QPoint &physicalPoint)
{
    const QRect physicalRect = overlay.overlayGeometry();
    const qreal scale = overlay.overlayScaleFactor();
    if (scale <= 0.0) {
        return QPointF(physicalPoint);
    }
    return QPointF((physicalPoint.x() - physicalRect.left()) / scale,
                   (physicalPoint.y() - physicalRect.top()) / scale);
}

/// 逻辑长度 → 该层的物理像素长度（用于把绘制出来的尺寸换回屏幕尺度）
inline qreal toPhysicalLength(const WinEase::OverlayWindow &overlay, qreal logicalLength)
{
    const qreal scale = overlay.overlayScaleFactor();
    return (scale > 0.0) ? logicalLength * scale : logicalLength;
}

/// 该悬浮层内的一块逻辑矩形（由物理矩形换算，含向外扩张的边距）
inline QRect logicalRectWithMargin(const WinEase::OverlayWindow &overlay,
                                   const QRect &physicalRect,
                                   int physicalMargin)
{
    const qreal scale = overlay.overlayScaleFactor();
    if (scale <= 0.0) {
        return physicalRect;
    }
    const QRect expanded = physicalRect.adjusted(-physicalMargin, -physicalMargin,
                                                 physicalMargin, physicalMargin);
    const QPointF topLeft = toLogical(overlay, expanded.topLeft());
    const QPointF bottomRight = toLogical(overlay, expanded.bottomRight());
    return QRect(topLeft.toPoint(), bottomRight.toPoint());
}

} // namespace WinEase::FeaturePlugins::OverlayGeometry
