#pragma once

// ============================================================================
//  power_panel_plugin.h —— P2-06 快速关机 / 重启 / 休眠（monitor.power_panel）
//
//  六个动作：锁定 / 注销 / 睡眠 / 休眠 / 重启 / 关机。
//
//  三条纪律：
//   1. **不可撤销的操作必须留出可撤销的时间窗**：除"锁定"外，所有动作都先走
//      倒计时（默认 60 秒，可配），期间托盘显示倒数、面板给出「取消」。
//      这是路线图 P2-06 的必备项，也是这个功能唯一"出错就会被骂"的地方。
//   2. **关机前先把状态落盘**：发起电源请求之前调一次 `syncConfig()` ——
//      用户点关机是不给程序留退路的（`WM_QUERYENDSESSION` 之后可能只剩几秒），
//      "已启用哪些插件"这种状态必须先写进 config.ini。
//   3. **助手不可用就如实报错**（fail-closed）：关机/重启/etc 要 `SE_SHUTDOWN_NAME`，
//      按 D2 决策交给提权助手；拿不到助手时绝不能假装"已关机"——
//      用户以为按了关机然后走开，回来发现机器还开着，比直接报错糟糕得多。
//      例外是"锁定"：`LockWorkStation` 普通权限即可，不依赖助手。
//
//  ⚠ 取消（撤销通道）走的是平台层本地实现，不经过助手：
//     机器正在倒计时关闭时，那道保险不能依赖另一个进程还活着。
// ============================================================================

#include "PowerControl.h"
#include "sdk/IFeaturePlugin.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>

class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;

class PowerPanelPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "power_panel_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit PowerPanelPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 用户点了某个动作：二次确认 → 倒计时（或立即执行）
    void requestAction(WinEase::Common::PowerAction action);
    /// 真正把请求送出去（倒计时结束后的那一步，或"倒计时 = 0"时的直通）
    void executeAction(WinEase::Common::PowerAction action);
    void tickCountdown();
    void cancelCountdown(const QString &reason);

    void refreshPanel();
    void updateBadge();
    QString statusText() const;
    QString helperText() const;
    /// 托盘/状态栏里那句"还剩几秒"的话
    QString countdownText() const;

    WinEase::Common::PowerCountdown m_countdown;
    QTimer *m_tick = nullptr;

    int m_countdownSeconds = 60; ///< 来自配置；0 = 立即执行
    bool m_confirm = true;       ///< 二次确认（自检把它关掉，否则会卡在没人点的模态框上）
    QString m_lastEvent;

    /// 动作 → 面板按钮（自检按 objectName 找，这里只是方便统一刷新启用状态）
    QHash<int, QPointer<QPushButton>> m_actionButtons;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_helperLabel;
    QPointer<QLabel> m_hintLabel;
    QPointer<QSpinBox> m_countdownSpin;
    QPointer<QPushButton> m_cancelButton;
};
