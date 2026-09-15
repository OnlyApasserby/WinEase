#pragma once

// ============================================================================
//  SpotlightOverlay —— 聚光灯绘制层（P1-08 的显示部分）
//
//  画法：整屏压暗 + 在光标处**打一个全透明的洞** + 洞边一圈彩色圆环。
//      打洞用 QPainter::CompositionMode_Clear —— 这是"擦掉"而不是"再画一层"，
//      是分层窗口里唯一能把 alpha 真正清零的办法（OverlayWindow::paintEvent
//      已经把绘制表面清成透明，paintOverlay 里 Clear 出来的洞就是全透明的）。
//
//  性能：整屏半透明窗口全量重绘很贵，因此每帧只请求"圆环外接矩形"的脏区，
//      并且用 requestOverlayUpdate(dirty) 而不是 update()。
//
//  ⚠ 本类的数据一律是**物理像素**，painter 是**逻辑坐标** → 换算走
//    plugins/common/OverlayGeometry.h
// ============================================================================

#include "sdk/OverlayWindow.h"

#include <QColor>
#include <QPoint>
#include <QRect>

class SpotlightOverlay : public WinEase::OverlayWindow
{
public:
    struct Style {
        int radius = 90;                          ///< 亮区半径（物理像素）
        int veilAlpha = 110;                      ///< 压暗强度（0 = 不压暗）
        QColor ringColor = QColor(255, 193, 7);   ///< 圆环颜色
        bool ringOnly = false;                    ///< 只画圆环（不压暗整屏）
        bool breathing = true;                    ///< 呼吸动画
    };

    explicit SpotlightOverlay(QWidget *parent = nullptr);

    void setStyle(const Style &style);
    Style style() const { return m_style; }

    /// 设置高亮中心（**物理像素**屏幕坐标）
    void setFocusPoint(const QPoint &physicalPoint);
    QPoint focusPoint() const { return m_focus; }

    /// 呼吸相位（0..1），由插件的定时器推进
    void setBreathingPhase(qreal phase);

    /// 本层是否包含该物理点（多显示器时只有光标所在那层需要打洞）
    bool containsPhysicalPoint(const QPoint &physicalPoint) const;

protected:
    void paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect) override;

private:
    qreal effectiveRadius() const;
    QColor veilColor() const;

    /// 当前圆环（含呼吸）的**逻辑**外接矩形 —— 用作脏区。
    ///
    /// ⚠ 必须是逻辑坐标：`requestOverlayUpdate()` 收的是逻辑矩形，
    ///   把物理坐标的脏区传进去，就会去重绘**另一块地方**，真正要更新的那一带
    ///   永远不重绘 —— 症状是"洞根本没挖在光标上"（自检里逐像素比对抓到的）。
    QRect ringBoundsLogical() const;

    Style m_style;
    QPoint m_focus;
    qreal m_phase = 0.0;
};
