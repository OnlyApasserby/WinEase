#pragma once

// ============================================================================
//  hud_overlay.h —— 硬件监控悬浮窗的"面板"（P3-07 第一批）
//
//  职责只有两件：把一份 HudSnapshot 画出来、处理拖拽与边缘吸附。
//  它**不知道任何 Win32 取数细节**——采样在插件里，显示模型在 HudMetrics 里。
//
//  ---------------------------------------------------------------------------
//  为什么窗口尺寸 = 面板尺寸 + 阴影边距，而不是铺满整个显示器（重要设计选择）
//
//    1. 常驻 HUD 若铺满整屏，等于让 DWM 每帧合成一个全屏分层窗口——代价白付；
//       贴住面板大小的窗口则只有几百像素。
//    2. 移动面板 = 移动窗口本身。旧位置由 DWM 重新合成，**天然不会留残影**；
//       而在全屏层里改绘制位置的话，旧位置的像素得靠调用方自己擦。
//    3. 面板尺寸随"逐项开关"变化时，只需要重设一次窗口矩形，不需要动脏区策略。
//
//  ⚠ alpha 命中测试（OverlayWindow.h 已注明，这里再强调一次）：
//      分层窗口是**逐像素按 alpha 命中**的。所以"能不能拖"取决于
//      **面板本体有没有画出不透明内容**——本类把面板底画成 alpha 205 的深色，
//      并把标题条画成近乎不透明，命中区因此是"整块面板"。
//      面板之外的投影/边距区域 alpha 很低，点击自然穿透到下层窗口。
//
//  ⚠ 面板会吃掉落在它自己身上的点击（这是"能拖动"的必然代价）。
//      不想让它挡事时切"穿透模式"（setClickThrough(true)），此时整块面板都不参与命中。
// ============================================================================

#include "HudMetrics.h"
#include "sdk/OverlayWindow.h"

#include <QColor>
#include <QPoint>
#include <QRect>
#include <QSize>

namespace WinEase::FeaturePlugins {

/// 硬件监控面板的绘制 + 拖拽/吸附
class HudOverlay : public WinEase::OverlayWindow
{
    Q_OBJECT

    // ------------------------------------------------------------------------
    //  只读属性：**面板对外可观测状态的唯一入口**
    //
    //  为什么需要它们（而不是让自检直接调 snapshot()/anchorPhysical()）：
    //      插件是独立 DLL，**不导出 C++ 符号**；面板类只在插件内部可见，
    //      自检拿到的只是一个 QObject*。Qt 的**运行期元对象**（QObject::property）
    //      是天然跨 DLL 的，所以"面板现在几行、写的是什么、落在哪"必须从这里读。
    //      这也是自检唯一诚实的读法：读的是**真面板**的状态，
    //      而不是自检自己造一个影子对象来自证。
    //
    //  ⚠ 改动这三个属性等于改动自检与面板的契约，升级时两者必须一起改。
    // ------------------------------------------------------------------------
    Q_PROPERTY(int metricRowCount READ metricRowCount NOTIFY snapshotChanged)
    Q_PROPERTY(QString snapshotText READ snapshotText NOTIFY snapshotChanged)
    Q_PROPERTY(QPoint anchorPhysical READ anchorPhysical NOTIFY anchorChanged)

public:
    /// 视觉参数（逻辑像素；面板宽度固定，高度由行数决定）
    struct Style {
        QColor panelColor{18, 20, 26, 205};      ///< 面板底（alpha 必须 > 0，否则点不到）
        QColor borderColor{255, 255, 255, 38};   ///< 1px 细描边
        QColor titleBarColor{32, 37, 48, 240};   ///< 标题条：拖拽热区，刻意画得更实
        QColor titleTextColor{232, 236, 245};
        QColor textColor{222, 228, 238};
        QColor mutedColor{148, 156, 170};
        QColor warningColor{255, 193, 7};        ///< 不可用项：琥珀色警示
        int panelWidth = 300;                    ///< 面板逻辑宽度（固定，行数决定高度）
        int titleHeight = 28;
        int rowHeight = 22;
        int paddingX = 12;
        int paddingY = 10;
        int shadowMargin = 8;                    ///< 四周留白（画投影用，也是不可命中的边距）
        int cornerRadius = 10;
        int fontSizePx = 12;
    };

    explicit HudOverlay(QWidget *parent = nullptr);

    // ---------------- 内容 ----------------

    /// 更新面板内容。行数变化时自动重算窗口尺寸（保持左上角锚点不动）
    void setSnapshot(const Hud::HudSnapshot &snapshot);
    const Hud::HudSnapshot &snapshot() const;

    // ---------------- 对外可观测状态（跨 DLL 只读属性，见类头注释）----------------

    /// 当前面板上的指标行数（0 = 空态提示那一行）
    int metricRowCount() const;
    /// 面板上每一行 "名称=值" 的拼接文本（自检断言"写的是不是真话"用；
    /// 界面上不显示它，它只是把面板内容变成可断言的字符串）
    QString snapshotText() const;

    void setStyle(const Style &style);
    const Style &style() const;

    // ---------------- 位置 ----------------

    /// 设置面板左上角（物理像素 + 屏幕坐标）。
    /// @param allowSnap true 时会先收敛到显示器工作区内，并吸附到最近的边缘
    void setAnchor(const QPoint &physicalTopLeft, bool allowSnap = false);
    /// 请求值（不是回读值）——拖动时以它为基准累加，避免"宁大勿小"的取整误差累积
    QPoint anchorPhysical() const;

    QRect panelPhysicalRect() const; ///< 面板本体的物理矩形（不含阴影边距）
    QRect panelLogicalRect() const;  ///< 面板本体的逻辑矩形（窗口内坐标，绘制/脏区用）
    QSize panelLogicalSize() const;  ///< 面板逻辑尺寸（标题条 + 各行）

    bool isDragging() const;

Q_SIGNALS:
    /// 位置定案（拖动结束或吸附完成）——插件据此持久化
    void anchorCommitted(const QPoint &physicalTopLeft);

    /// 面板内容更新（只读属性的 NOTIFY；自检据此确认"面板真的刷了"）
    void snapshotChanged();
    /// 面板位置变化
    void anchorChanged();

protected:
    void paintOverlay(QPainter &painter, const QRect &dirtyLogicalRect) override;

private:
    void onOverlayPressed(const QPoint &logicalPos, int button);
    void onOverlayMoved(const QPoint &logicalPos);
    void onOverlayReleased(const QPoint &logicalPos, int button);

    /// 按当前锚点 + 面板尺寸重设窗口矩形（物理像素 → 本层逻辑坐标由 OverlayKit 换算）
    void applyGeometry(bool allowSnap);
    /// 收敛到工作区内并吸附到边缘（物理像素）
    QPoint snappedAnchor(const QPoint &panelTopLeft, const QSize &panelPhysicalSize) const;

    Hud::HudSnapshot m_snapshot;
    Style m_style;

    QPoint m_anchor;         ///< 面板左上角（物理像素；请求值即真值）
    QPoint m_pressLogical;   ///< 按下时鼠标在本层内的逻辑坐标
    QPoint m_pressAnchor;    ///< 按下时的锚点（拖动累加的基准）
    bool m_dragging = false;
};

} // namespace WinEase::FeaturePlugins
