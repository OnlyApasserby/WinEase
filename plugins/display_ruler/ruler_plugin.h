#pragma once

// ============================================================================
//  RulerPlugin —— P1-07 屏幕标尺
//
//  交互模型（为什么这样设计）：
//    * 标尺是**全屏透明置顶层**，默认**点击穿透** —— 量尺寸时用户往往还要
//      正常操作下层窗口，挡住鼠标就没法用了
//    * 默认模式"跟随鼠标"：起点由快捷键落下，终点**始终跟着光标**，
//      于是量一条线只需要"落点 → 移鼠标"两个动作，无需按住拖动
//    * 需要精确微调时切到"可拖拽"（关闭穿透）：此时标尺自己画出来的
//      主线/刻度/手柄是不透明的，会被命中；**其余区域 alpha == 0，
//      点击照样穿透到下层**（Windows 对分层窗口是逐像素判定命中）
//    * 多条标尺并存：`pin` 钉住当前这条，下一次"落点"就开始新的一条
//
//  单位换算（cm / 英寸）说明：
//      显示器**物理尺寸**无法从系统可靠获得（EDID 里也常常是估算值），
//      因此按"系统 DPI"换算：1 英寸 = dpi 物理像素、1 cm = dpi / 2.54。
//      界面上如实标注"按系统 DPI 估算"，而不是假装精确。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "ruler_overlay.h"

#include <QList>
#include <QTimer>

class RulerPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "ruler_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit RulerPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    // ---------------- 供快捷键与设置面板共用 ----------------
    bool toggleRuler();
    bool dropStartPoint();       ///< 落点（新建/重设活动标尺的起点）
    bool pinActiveRuler();       ///< 钉住当前标尺（停止跟随）
    bool cycleUnit();
    bool toggleInteractive();    ///< 点击穿透开关
    bool clearRulers();

    /// 创建（每显示器一个）并显示悬浮层；已存在时直接复用
    bool ensureOverlays();
    void destroyOverlays();
    /// 把最新标尺数据推给所有悬浮层
    void refreshOverlays();
    /// 定时器回调：让活动标尺的终点跟着光标走，长度变化时更新状态文本
    void followCursor();
    /// 按当前单位重报一次活动标尺的长度（换单位后要立刻生效，不能等鼠标再动）
    void reportLength();

    QString formatLength(qreal physicalPixels) const;
    qreal pixelsPerUnit() const;
    QString unitName() const;
    void emitStatus(const QString &text);
    /// 悬浮层所在显示器（用于单位换算与"全屏"判断）
    QRect monitorRect() const;

    QList<RulerOverlay::Ruler> m_rulers;
    int m_activeIndex = -1;          ///< 跟随光标的标尺下标；-1 表示都钉住了
    bool m_visible = false;
    bool m_interactive = false;      ///< true = 关闭穿透，标尺可拖拽
    QString m_unit = QStringLiteral("px");

    QTimer m_followTimer;
    QString m_lastLengthText;        ///< 去重：长度文本没变就不刷状态
};
