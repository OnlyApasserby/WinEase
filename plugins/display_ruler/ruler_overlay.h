#pragma once

// ============================================================================
//  RulerOverlay —— 屏幕标尺的绘制层（P1-07 的显示部分）
//
//  与插件逻辑分开的原因：插件只管"标尺数据 + 快捷键 + 配置"，
//  绘制细节（刻度、气泡、手柄）集中在这里，改样式不会碰到逻辑。
//
//  ⚠ 两条必须遵守的约定（都来自 sdk/OverlayWindow.h）：
//    1. 本类的数据一律是**物理像素**，`paintOverlay()` 里的 painter 是**逻辑坐标**
//       → 换算统一走 plugins/common/OverlayGeometry.h
//    2. 悬浮层是分层窗口，**alpha == 0 的区域永远不被命中**。
//       所以"可拖拽"必须建立在"真的画了不透明内容"之上 —— 本类画的
//       标尺带、刻度、手柄都是不透明的，而空白处保持全透明（点击自然穿透到下层）。
// ============================================================================

#include "sdk/OverlayWindow.h"

#include <QList>
#include <QPoint>
#include <QString>

class RulerOverlay : public WinEase::OverlayWindow
{
public:
    /// 一条标尺（物理像素坐标）
    struct Ruler {
        QPoint start;
        QPoint end;

        bool isValid() const { return start != QPoint(0, 0) || end != QPoint(0, 0); }
        /// 物理像素长度
        qreal length() const;
        QRect bounds() const; ///< 物理外接矩形（含最小尺寸，避免零长标尺退化成点）
    };

    explicit RulerOverlay(QWidget *parent = nullptr);

    void setRulers(const QList<Ruler> &rulers);
    QList<Ruler> rulers() const { return m_rulers; }

    /// 显示用的单位文本（"px" / "cm" / "in"）与"1 个单位等于多少物理像素"
    void setUnit(const QString &unitName, qreal physicalPixelsPerUnit);
    /// 交互模式（可拖拽）：把标尺画得更醒目，便于看清可点区域
    void setInteractive(bool interactive);
    /// 活动标尺（跟随鼠标的那条）的下标；-1 表示没有
    void setActiveIndex(int index);

protected:
    void paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect) override;

private:
    void paintOneRuler(QPainter &painter, const Ruler &ruler, bool active) const;
    QString lengthText(qreal physicalPixels) const;

    QList<Ruler> m_rulers;
    QString m_unitName = QStringLiteral("px");
    qreal m_pixelsPerUnit = 1.0;
    bool m_interactive = false;
    int m_activeIndex = -1;
};
