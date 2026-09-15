#pragma once

// ============================================================================
//  device_alert_plugin.h —— P2-11 摄像头/麦克风使用提醒（security.device_alert）
//
//  数据来源是系统"隐私和安全性"页面同源的注册表（`ConsentStore\...\NonPackaged`），
//  每 3 秒轮询一次（可配），有人用摄像头/麦克风就**托盘亮起 + 弹气泡**，停用即解除。
//
//  ---------------------------------------------------------------------------
//  四条纪律：
//
//   1. **"读不到" ≠ "没人用"**：父键不存在是正常的（这台机器还没有任何应用碰过
//      这个设备），但**读取出错**必须如实说明 —— 对安全类功能来说，
//      "没有警报"和"看不见"是两件完全不同的事（与 P2-10 麦克风静音同一条纪律）。
//   2. **别反复唠叨**：轮询是 3 秒一次，可"某个应用开始用摄像头"只发生一次。
//      所以通知按 (设备, 应用) 去重：只在**新出现**时弹一次；那个应用停用后再用，
//      才算新的一轮。否则用户每 3 秒被弹一次，第一件事就是把这个功能关掉。
//   3. **"记住已批准"是降低打扰的唯一手段**（路线图要求）：名单存的是注册表键名，
//      批准之后该应用不再触发提醒，但**仍然显示在明细里**（看得到，不打扰）。
//   4. **停用即停手**：停用插件就停轮询、摘徽标、不再弹任何东西；
//      不写任何注册表（本功能对外部世界**只读**）——除了用户自己点的那份批准名单。
//
//  ⚠ 本插件在真实注册表上**只读**：`ConsentStore` 一个字节都不写。
//     端到端自检会临时造一个自检专用条目，用完立刻删除（见 device_group 的说明）。
// ============================================================================

#include "DeviceUsageMonitor.h"
#include "sdk/IFeaturePlugin.h"

#include <QList>
#include <QListWidgetItem>
#include <QPointer>
#include <QSet>
#include <QStringList>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;

class DeviceAlertPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "device_alert_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit DeviceAlertPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;

private:
    /// 一条提醒：哪个设备 + 哪个应用
    struct Alert {
        WinEase::Common::DeviceKind kind = WinEase::Common::DeviceKind::Webcam;
        WinEase::Common::DeviceUsageEntry entry;
    };

    /// 读一遍注册表、重算警报、更新徽标/通知/面板
    void poll();
    /// 面板按钮：批准当前正在提醒的那个应用
    void approveCurrentAlert();
    /// 面板按钮：把列表里选中的条目移出已批准名单
    void forgetSelected();
    void setApproved(const QStringList &approved);

    bool kindEnabled(WinEase::Common::DeviceKind kind) const;

    void refreshPanel();
    void updateBadge();
    QString statusText() const;
    QString detailText() const;
    QString approvedText() const;
    /// 警报徽标与托盘提示用
    QList<WinEase::Common::DeviceKind> alertKinds() const;

    QList<Alert> m_alerts; ///< 当前需要提醒的条目
    QList<WinEase::Common::DeviceUsageEntry> m_entries; ///< 全部条目（面板明细用）
    QSet<QString> m_notifiedKeys; ///< 已经弹过通知的 (设备|键名)，避免每 3 秒重弹
    QStringList m_approved;       ///< 已批准名单（存注册表键名）
    QString m_lastEvent;
    QString m_readError; ///< 上一轮读取失败的原因（空 = 没出错）

    bool m_webcamEnabled = true;
    bool m_microphoneEnabled = true;
    bool m_notify = true;
    int m_pollSeconds = 3;

    QTimer *m_timer = nullptr;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_detailLabel;
    QPointer<QListWidget> m_approvedList;
    QPointer<QPushButton> m_approveButton;
    QPointer<QPushButton> m_forgetButton;
    QPointer<QSpinBox> m_pollSpin;
    QPointer<QCheckBox> m_notifyCheck;
    QPointer<QCheckBox> m_webcamCheck;
    QPointer<QCheckBox> m_microphoneCheck;
};
