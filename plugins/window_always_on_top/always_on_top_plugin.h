#pragma once

// ============================================================================
//  AlwaysOnTopPlugin —— P1-01 窗口置顶
//
//  行为：
//    * 快捷键（默认 Ctrl+Alt+P）切换**目标窗口**的置顶状态
//    * 目标窗口由 WindowTargetState 解析：跟随鼠标下窗口，或锁定某个窗口反复操作
//    * 记住每个被置顶窗口的**原始状态**，功能停用/程序退出时一律还原
//      （有些窗口本来就被别的程序置顶了，例如 OBS 预览，不能一律取消置顶）
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include "WindowFeatureState.h"

#include <QHash>

class AlwaysOnTopPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "always_on_top_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit AlwaysOnTopPlugin(QObject *parent = nullptr);

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
    /// 切换目标窗口的置顶状态
    void toggleTargetTopMost();
    /// 还原所有被本功能置顶的窗口
    void restoreAll();
    /// 把当前置顶清单推给卡片与托盘提示
    void reportStatus();

    /// 被本功能改动过的窗口记录：还原时需要"原来是不是置顶"以及**身份校验**信息
    struct PinRecord {
        bool wasTopMost = false; ///< 改动前是否已置顶
        quint32 processId = 0;   ///< 用于确认句柄没被系统回收给别的窗口
        QString className;
    };

    WinEase::FeaturePlugins::WindowTargetState m_target;
    QHash<quintptr, PinRecord> m_pinned;
};
