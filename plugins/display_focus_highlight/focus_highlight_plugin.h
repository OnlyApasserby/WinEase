#pragma once

// ============================================================================
//  FocusHighlightPlugin —— P1-08 焦点高亮（聚光灯）
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+H）开关；开启后整屏压暗，光标周围留一圈亮区
//    * Ctrl+Shift+Alt+H 切换样式（聚光灯 ⇄ 只画圆环）
//    * 30fps 跟随光标（只在圆环那一带重绘），带呼吸动画
//    * 半径 / 压暗强度 / 圆环颜色 / 是否呼吸 都可在设置面板里调
//
//  "不干扰被点击窗口"是怎么做到的（这一条是验收项）：
//      悬浮层始终 **点击穿透 + 不抢焦点**（WS_EX_TRANSPARENT | WS_EX_NOACTIVATE），
//      压暗与圆环只是视觉效果，鼠标事件照样落到下层窗口上；
//      全过程不改动任何被高亮的窗口本身。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "spotlight_overlay.h"

#include <QElapsedTimer>
#include <QTimer>

class FocusHighlightPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "focus_highlight_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit FocusHighlightPlugin(QObject *parent = nullptr);

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
    bool showSpotlight();
    bool hideSpotlight();
    bool toggleSpotlight();
    bool toggleStyle();
    void ensureOverlays();
    void destroyOverlays();
    /// 定时器：跟随光标 + 推进呼吸相位
    void tick();
    void applyStyle();
    void persistStyle();

    SpotlightOverlay::Style m_style;
    bool m_active = false;
    QTimer m_tickTimer;
    qreal m_phase = 0.0;
    QPoint m_lastReported;     ///< 位置去重：只在真的移动时报状态
    QElapsedTimer m_reportTimer; ///< 位置上报节流（最快 200ms 一条）
};
