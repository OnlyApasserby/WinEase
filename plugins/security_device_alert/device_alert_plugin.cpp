#include "device_alert_plugin.h"

#include "sdk/PluginServices.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <iterator>

namespace {

using WinEase::Common::DeviceKind;
using WinEase::Common::DeviceUsageEntry;

/// 轮询周期（默认 3 秒：路线图验收要求"打开相机后 5 秒内提示"）
constexpr int kDefaultPollSeconds = 3;
/// 托盘徽标颜色（提醒性质，用橙色；不用红色是因为它不代表故障）
const QString kBadgeColor = QStringLiteral("#e67e22");

} // namespace

DeviceAlertPlugin::DeviceAlertPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString DeviceAlertPlugin::id() const
{
    return QStringLiteral("security.device_alert");
}

QString DeviceAlertPlugin::name() const
{
    return QStringLiteral("摄像头麦克风提醒");
}

QString DeviceAlertPlugin::description() const
{
    return QStringLiteral("正有应用使用摄像头/麦克风时托盘亮起并弹提示，可记住已批准的应用");
}

QIcon DeviceAlertPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Security);
}

WinEase::FeatureCategory DeviceAlertPlugin::category() const
{
    return WinEase::FeatureCategory::Security;
}

QStringList DeviceAlertPlugin::tags() const
{
    return { QStringLiteral("摄像头"), QStringLiteral("麦克风"), QStringLiteral("隐私"),
             QStringLiteral("提醒"), QStringLiteral("webcam"), QStringLiteral("shexiangtou") };
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool DeviceAlertPlugin::initialize()
{
    // 配置只在 initialize() 读一次，之后改动走面板控件（踩坑 #26）
    if (WinEase::PluginServices *svc = services()) {
        m_webcamEnabled = svc->configValue(id(), QStringLiteral("webcamEnabled"), true).toBool();
        m_microphoneEnabled = svc->configValue(id(), QStringLiteral("microphoneEnabled"), true).toBool();
        m_notify = svc->configValue(id(), QStringLiteral("notify"), true).toBool();
        m_pollSeconds = qBound(1,
                               svc->configValue(id(), QStringLiteral("pollSeconds"),
                                                kDefaultPollSeconds).toInt(),
                               60);
        m_approved = svc->configValue(id(), QStringLiteral("approved"), QStringList()).toStringList();
    }

    if (m_timer == nullptr) {
        m_timer = new QTimer(this);
        m_timer->setInterval(m_pollSeconds * 1000);
        connect(m_timer, &QTimer::timeout, this, [this] { poll(); });
    }

    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("设备使用提醒已就绪（每 %1 秒轮询一次，已批准 %2 个应用）")
                   .arg(m_pollSeconds)
                   .arg(m_approved.size()));
    return true;
}

void DeviceAlertPlugin::shutdown()
{
    if (m_timer != nullptr) {
        m_timer->stop();
    }
}

bool DeviceAlertPlugin::canEnable(QString *reason) const
{
    // 没有硬前提：摄像头/麦克风列表总是读得到的（父键不存在只是"没人用过"）。
    // 真正的读取错误由 poll() 逐次如实报出，而不是一刀切不让启用
    Q_UNUSED(reason)
    return true;
}

bool DeviceAlertPlugin::onEnable()
{
    // ⚠ 时序（踩坑 #51 的变体）：启用瞬间就要检查一次（用户往往是"先发现摄像头亮了、
    //    才想起装个提醒"），但**不能在这里同步调 poll()** —— 此刻宿主的"已启用"状态
    //    还没落定，`isEnabled()` 仍是 false，而 poll() 用它来决定"能不能弹通知"，
    //    结果是徽标亮了、气泡却没弹（用户恰好就是那种"没在看托盘"的人）。
    //    所以排到事件循环下一轮再检查。
    QMetaObject::invokeMethod(this, [this] { poll(); }, Qt::QueuedConnection);

    if (m_timer != nullptr) {
        m_timer->start();
    }

    Q_EMIT statusMessage(statusText());

    // ⚠ 时序（踩坑 #51）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void DeviceAlertPlugin::onDisable()
{
    if (m_timer != nullptr) {
        m_timer->stop();
    }
    m_alerts.clear();
    m_notifiedKeys.clear();
    updateBadge();
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  轮询与提醒
// ---------------------------------------------------------------------------

bool DeviceAlertPlugin::kindEnabled(DeviceKind kind) const
{
    return kind == DeviceKind::Webcam ? m_webcamEnabled : m_microphoneEnabled;
}

void DeviceAlertPlugin::poll()
{
    const QDateTime now = QDateTime::currentDateTime();

    m_entries.clear();
    m_alerts.clear();
    m_readError.clear();
    QSet<QString> activeKeys;

    for (const DeviceKind kind : WinEase::Common::allDeviceKinds()) {
        if (!kindEnabled(kind)) {
            continue;
        }

        const WinEase::Common::ConsentStoreSnapshot snapshot =
            WinEase::Common::readConsentStore(kind);
        if (!snapshot.ok) {
            // ⚠ "读不到"必须如实说：安全类功能里，"没有警报"和"看不见"完全不同
            m_readError = snapshot.error;
            continue;
        }

        const QList<DeviceUsageEntry> entries =
            WinEase::Common::buildUsageEntries(snapshot.entries, m_approved, now);
        for (const DeviceUsageEntry &entry : entries) {
            m_entries.append(entry);
        }

        for (const DeviceUsageEntry &entry : WinEase::Common::alertEntries(entries)) {
            m_alerts.append(Alert{ kind, entry });
        }
        for (const DeviceUsageEntry &entry : entries) {
            if (entry.inUse) {
                activeKeys.insert(QStringLiteral("%1|%2").arg(WinEase::Common::deviceKindKey(kind),
                                                              entry.keyName));
            }
        }
    }

    // 通知按 (设备, 应用) 去重：只在**新出现**时弹一次 ——
    // 3 秒一次的轮询要是每次都弹，用户第一件事就是把这个功能关掉
    WinEase::PluginServices *svc = services();
    if (isEnabled() && m_notify && svc != nullptr) {
        for (const Alert &alert : m_alerts) {
            const QString identity = QStringLiteral("%1|%2")
                                         .arg(WinEase::Common::deviceKindKey(alert.kind),
                                              alert.entry.keyName);
            if (m_notifiedKeys.contains(identity)) {
                continue;
            }
            m_notifiedKeys.insert(identity);
            svc->notify(QStringLiteral("%1正在被使用").arg(WinEase::Common::deviceKindText(alert.kind)),
                        QStringLiteral("%1\n（不想每次都被提醒？在面板里把它加入已批准名单）")
                            .arg(alert.entry.displayName));
            m_lastEvent = QStringLiteral("%1：%2 开始使用")
                              .arg(WinEase::Common::deviceKindText(alert.kind),
                                   alert.entry.displayName);
        }
    }

    // 不再活跃的应用从"已提醒"集合里摘掉：它下次再用时应该再提醒一轮
    for (auto it = m_notifiedKeys.begin(); it != m_notifiedKeys.end();) {
        it = activeKeys.contains(*it) ? std::next(it) : m_notifiedKeys.erase(it);
    }

    if (m_alerts.isEmpty()) {
        m_lastEvent = m_readError.isEmpty()
                          ? QStringLiteral("当前没有应用在使用摄像头/麦克风")
                          : QStringLiteral("读取设备使用记录失败：%1").arg(m_readError);
    }

    updateBadge();
    if (isEnabled()) {
        Q_EMIT statusMessage(statusText());
    }
    refreshPanel();
}

void DeviceAlertPlugin::setApproved(const QStringList &approved)
{
    m_approved = approved;
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("approved"), m_approved);
        svc->syncConfig();
    }
}

void DeviceAlertPlugin::approveCurrentAlert()
{
    if (m_alerts.isEmpty()) {
        m_lastEvent = QStringLiteral("当前没有需要批准的应用");
        refreshPanel();
        return;
    }

    const Alert alert = m_alerts.first();
    if (!m_approved.contains(alert.entry.keyName)) {
        m_approved.append(alert.entry.keyName);
        setApproved(m_approved);
        m_lastEvent = QStringLiteral("已批准：%1（以后不再提示它）").arg(alert.entry.displayName);
        Q_EMIT statusMessage(m_lastEvent);
    }

    poll(); // 立刻按新名单重算，用户马上就看不到那条提醒了
}

void DeviceAlertPlugin::forgetSelected()
{
    if (m_approvedList.isNull()) {
        return;
    }

    QListWidgetItem *item = m_approvedList->currentItem();
    if (item == nullptr) {
        m_lastEvent = QStringLiteral("请先在名单里选中一个应用");
        refreshPanel();
        return;
    }

    const QString keyName = item->data(Qt::UserRole).toString();
    if (m_approved.removeAll(keyName) > 0) {
        setApproved(m_approved);
        // 移出名单 = 用户反悔了"不想再被打扰"这件事 → 把去重标记也清掉，
        // 否则它虽然会重新亮徽标，却再也不会弹一次通知（用户会觉得"移出了没用"）
        for (const WinEase::Common::DeviceKind kind : WinEase::Common::allDeviceKinds()) {
            m_notifiedKeys.remove(QStringLiteral("%1|%2")
                                      .arg(WinEase::Common::deviceKindKey(kind), keyName));
        }
        m_lastEvent = QStringLiteral("已移出名单：%1（它的使用会重新触发提醒）")
                          .arg(WinEase::Common::displayNameForAppKey(keyName));
        Q_EMIT statusMessage(m_lastEvent);
    }

    poll();
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QList<DeviceKind> DeviceAlertPlugin::alertKinds() const
{
    QList<DeviceKind> kinds;
    for (const Alert &alert : m_alerts) {
        if (!kinds.contains(alert.kind)) {
            kinds.append(alert.kind);
        }
    }
    return kinds;
}

void DeviceAlertPlugin::updateBadge()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    const QList<DeviceKind> kinds = alertKinds();
    if (kinds.isEmpty()) {
        // 警报解除 → 徽标立刻摘掉（验收里的"关闭后提示自动解除"就是这一步）
        svc->setTrayBadge(id(), QString(), QString(), QString());
        return;
    }

    QStringList parts;
    for (const Alert &alert : m_alerts) {
        parts << QStringLiteral("%1（%2）")
                     .arg(alert.entry.displayName,
                          WinEase::Common::deviceKindText(alert.kind));
    }
    svc->setTrayBadge(id(),
                      WinEase::Common::deviceBadgeText(kinds),
                      kBadgeColor,
                      QStringLiteral("正在使用：%1").arg(parts.join(QStringLiteral("、"))));
}

QString DeviceAlertPlugin::statusText() const
{
    if (!m_readError.isEmpty()) {
        return QStringLiteral("读取设备使用记录失败：%1").arg(m_readError);
    }
    if (m_alerts.isEmpty()) {
        if (!m_entries.isEmpty()) {
            return QStringLiteral("当前没有应用在使用摄像头/麦克风"
                                  "（已批准 %1 个应用，它们的使用不再打扰你）")
                .arg(m_approved.size());
        }
        return QStringLiteral("当前没有应用在使用摄像头/麦克风");
    }

    QStringList parts;
    for (const Alert &alert : m_alerts) {
        parts << QStringLiteral("%1：%2（%3）")
                     .arg(WinEase::Common::deviceKindText(alert.kind),
                          alert.entry.displayName,
                          WinEase::Common::humanizeUsageDuration(
                              alert.entry, QDateTime::currentDateTime()));
    }
    return QStringLiteral("⚠ %1").arg(parts.join(QStringLiteral("；")));
}

QString DeviceAlertPlugin::detailText() const
{
    if (m_entries.isEmpty()) {
        return QStringLiteral("最近没有任何应用使用过摄像头/麦克风");
    }

    QStringList lines;
    for (const DeviceUsageEntry &entry : m_entries) {
        lines << QStringLiteral("%1 · %2 · %3")
                     .arg(entry.displayName,
                          WinEase::Common::humanizeUsageDuration(entry, QDateTime::currentDateTime()),
                          entry.approved ? QStringLiteral("已批准") : QStringLiteral("未批准"));
    }
    return lines.join(QStringLiteral("\n"));
}

QString DeviceAlertPlugin::approvedText() const
{
    return QStringLiteral("已批准名单（%1 个）：这些应用的使用不会再提醒你").arg(m_approved.size());
}

QWidget *DeviceAlertPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("deviceAlertPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("deviceAlertStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *detailLabel = new QLabel(widget);
    detailLabel->setObjectName(QStringLiteral("deviceAlertDetailLabel"));
    detailLabel->setWordWrap(true);
    layout->addWidget(detailLabel);

    auto *approveButton = new QPushButton(QStringLiteral("批准当前应用"), widget);
    approveButton->setObjectName(QStringLiteral("deviceAlertApproveButton"));
    layout->addWidget(approveButton, 0, Qt::AlignLeft);

    auto *listTitle = new QLabel(approvedText(), widget);
    listTitle->setObjectName(QStringLiteral("deviceAlertApprovedTitle"));
    layout->addWidget(listTitle);

    auto *approvedList = new QListWidget(widget);
    approvedList->setObjectName(QStringLiteral("deviceAlertApprovedList"));
    approvedList->setMaximumHeight(120);
    layout->addWidget(approvedList);

    auto *forgetButton = new QPushButton(QStringLiteral("移出名单"), widget);
    forgetButton->setObjectName(QStringLiteral("deviceAlertForgetButton"));
    layout->addWidget(forgetButton, 0, Qt::AlignLeft);

    auto *optionRow = new QHBoxLayout();
    auto *webcamCheck = new QCheckBox(QStringLiteral("监视摄像头"), widget);
    webcamCheck->setObjectName(QStringLiteral("deviceAlertWebcamCheck"));
    webcamCheck->setChecked(m_webcamEnabled);
    optionRow->addWidget(webcamCheck);

    auto *microphoneCheck = new QCheckBox(QStringLiteral("监视麦克风"), widget);
    microphoneCheck->setObjectName(QStringLiteral("deviceAlertMicrophoneCheck"));
    microphoneCheck->setChecked(m_microphoneEnabled);
    optionRow->addWidget(microphoneCheck);

    optionRow->addSpacing(12);
    auto *notifyCheck = new QCheckBox(QStringLiteral("弹气泡提示"), widget);
    notifyCheck->setObjectName(QStringLiteral("deviceAlertNotifyCheck"));
    notifyCheck->setChecked(m_notify);
    optionRow->addWidget(notifyCheck);

    optionRow->addSpacing(12);
    optionRow->addWidget(new QLabel(QStringLiteral("轮询间隔："), widget));
    auto *pollSpin = new QSpinBox(widget);
    pollSpin->setObjectName(QStringLiteral("deviceAlertPollSpin"));
    pollSpin->setRange(1, 60);
    pollSpin->setSuffix(QStringLiteral(" 秒"));
    pollSpin->setValue(m_pollSeconds);
    optionRow->addWidget(pollSpin);
    optionRow->addStretch(1);
    layout->addLayout(optionRow);

    auto *hint = new QLabel(
        QStringLiteral("数据来源与系统「隐私和安全性」页面**同源**：\n"
                       "HKCU\\...\\CapabilityAccessManager\\ConsentStore\\<设备>\\NonPackaged。\n"
                       "判定「正在使用」只有一个依据：LastUsedTimeStop == 0（还没停下）；"
                       "从没用过的空键不会被当成警报。\n"
                       "同一个应用只提醒一次；想彻底安静就把它加入已批准名单"
                       "（名单只影响本插件的提示，不改系统的任何权限设置）。\n"
                       "本插件对注册表**只读**：它只是把系统隐私页面里的同一份数据读出来及时告诉你。"),
        widget);
    hint->setObjectName(QStringLiteral("deviceAlertHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_detailLabel = detailLabel;
    m_approvedList = approvedList;
    m_approveButton = approveButton;
    m_forgetButton = forgetButton;
    m_pollSpin = pollSpin;
    m_notifyCheck = notifyCheck;
    m_webcamCheck = webcamCheck;
    m_microphoneCheck = microphoneCheck;

    connect(approveButton, &QPushButton::clicked, widget, [this] { approveCurrentAlert(); });
    connect(forgetButton, &QPushButton::clicked, widget, [this] { forgetSelected(); });
    connect(webcamCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_webcamEnabled = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("webcamEnabled"), checked);
            svc->syncConfig();
        }
        poll(); // 改开关立刻按新设置重算（用户要马上看到效果）
    });
    connect(microphoneCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_microphoneEnabled = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("microphoneEnabled"), checked);
            svc->syncConfig();
        }
        poll();
    });
    connect(notifyCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_notify = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("notify"), checked);
            svc->syncConfig();
        }
        refreshPanel();
    });
    connect(pollSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_pollSeconds = qBound(1, value, 60);
        if (m_timer != nullptr) {
            m_timer->setInterval(m_pollSeconds * 1000);
        }
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("pollSeconds"), m_pollSeconds);
            svc->syncConfig();
        }
        refreshPanel();
    });

    refreshPanel();
    return widget;
}

void DeviceAlertPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    const bool running = isEnabled();

    if (!m_statusLabel.isNull()) {
        m_statusLabel->setText(isEnabled() ? statusText() : QStringLiteral("功能已停用"));
    }
    if (!m_detailLabel.isNull()) {
        m_detailLabel->setText(detailText());
    }

    // "批准"按钮直接写出它会批准谁 —— 用户不该猜这个按钮作用在哪个应用上
    if (!m_approveButton.isNull()) {
        const bool hasAlert = running && !m_alerts.isEmpty();
        m_approveButton->setEnabled(hasAlert);
        m_approveButton->setText(hasAlert
                                     ? QStringLiteral("批准：%1（以后不再提示）")
                                           .arg(m_alerts.first().entry.displayName)
                                     : QStringLiteral("批准当前应用"));
    }

    if (!m_approvedList.isNull()) {
        // 重填名单（条目少，直接重建；顺带保证顺序与配置一致）
        m_approvedList->clear();
        for (const QString &keyName : m_approved) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1    %2")
                    .arg(WinEase::Common::displayNameForAppKey(keyName),
                         WinEase::Common::appPathFromKeyName(keyName)),
                m_approvedList.data());
            item->setData(Qt::UserRole, keyName);
            item->setToolTip(WinEase::Common::appPathFromKeyName(keyName));
        }
        m_approvedList->setEnabled(running && !m_approved.isEmpty());
    }
    if (!m_forgetButton.isNull()) {
        m_forgetButton->setEnabled(running && !m_approved.isEmpty());
    }

    if (!m_pollSpin.isNull()) {
        m_pollSpin->setEnabled(running);
        if (m_pollSpin->value() != m_pollSeconds) {
            QSignalBlocker blocker(m_pollSpin.data());
            m_pollSpin->setValue(m_pollSeconds);
        }
    }
    if (!m_notifyCheck.isNull()) {
        m_notifyCheck->setEnabled(running);
    }
    if (!m_webcamCheck.isNull()) {
        m_webcamCheck->setEnabled(running);
    }
    if (!m_microphoneCheck.isNull()) {
        m_microphoneCheck->setEnabled(running);
    }
}
