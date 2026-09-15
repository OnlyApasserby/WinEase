#pragma once

// ============================================================================
//  WindowOpacityPlugin —— P1-03 窗口透明度
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+Minus）把**目标窗口**变淡一档，Ctrl+Alt+Plus 变回一档，
//      Ctrl+Alt+0 恢复完全不透明
//    * 步长可配置（5% ~ 50%，默认 10%）
//    * 每个被调整过的窗口都保存 **OpacitySnapshot**（是否原本就是分层窗口、
//      原始 alpha、原始扩展样式）；功能停用或程序退出时逐窗口还原
//      —— 满足"可完整卸载、不留副作用"
//
//  为什么用 SetLayeredWindowAttributes(LWA_ALPHA)：
//      语义是"窗口内容整体半透明"，正好是"偷看背后内容"需要的；
//      SetWindowCompositionAttribute 的 ACCENT 只改背景着色/模糊，内容依旧不透明。
//      （平台层已封装，见 WinEaseWin32/WindowUtils 的透明度一段说明）
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/WindowUtils.h"

#include "WindowFeatureState.h"

#include <QHash>

class WindowOpacityPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "window_opacity_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit WindowOpacityPlugin(QObject *parent = nullptr);

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

    /// 按当前步长调整目标窗口透明度（delta 为 ±步长）
    bool adjustOpacity(qreal delta);
    /// 把已记录的窗口全部还原成完全不透明
    void resetAll();

private:
    struct OpacityRecord {
        WinEase::Win32::OpacitySnapshot snapshot; ///< 用于停用时"恢复原样"
        quint32 processId = 0;                    ///< 句柄复用校验
        QString className;
    };

    WinEase::FeaturePlugins::WindowTargetState m_target;
    QHash<quintptr, OpacityRecord> m_adjusted;
    qreal m_step = 0.10;
};
