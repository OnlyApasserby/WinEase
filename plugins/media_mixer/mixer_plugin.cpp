#include "mixer_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/ComApartment.h"
#include "win32/CoreAudio.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

using WinEase::FeaturePlugins::Mixer::MixerOptions;
using WinEase::FeaturePlugins::Mixer::MixerRow;
using WinEase::FeaturePlugins::Mixer::MixerSessionInput;
using WinEase::FeaturePlugins::Mixer::MixerSnapshot;
using WinEase::Win32::AudioSessionInfo;

/// 会话轮询间隔的默认值与范围（秒）。默认 2 秒：每轮只是"枚举 + 读若干会话音量"，
/// 微秒级；与 mic_mute 的 2 秒同步同口径 —— 够快（应用退出后行很快消失），
/// 又不让 CPU 有可测量的占用。
constexpr int kDefaultPollSeconds = 2;
constexpr int kMinPollSeconds = 1;
constexpr int kMaxPollSeconds = 30;

/// 记忆条目上限：防止长期使用后配置段无限膨胀
constexpr int kMaxRememberedEntries = 200;

/// 音量变化阈值：小于它不写配置（应用自己微调音量时不至于每轮刷盘）
constexpr double kRememberEpsilon = 0.01;

/// 列表项上的自检数据槽（见 mixer_plugin.h 顶部说明）
constexpr int kRowKeyRole = Qt::UserRole;
constexpr int kPidRole = Qt::UserRole + 1;
constexpr int kInstanceIdsRole = Qt::UserRole + 2;

/// 记忆键：进程名小写。**刻意不用路径**：
/// 同一应用的不同版本/不同安装位置应当共用一条记忆，否则用户会撞见"改了没记住"。
QString memoryKey(const QString &processName)
{
    return processName.trimmed().toLower();
}

MixerSessionInput toInput(const AudioSessionInfo &session)
{
    MixerSessionInput input;
    input.instanceId = session.instanceId;
    input.pid = session.pid;
    input.processName = session.processName;
    input.displayName = session.displayName;
    input.executablePath = session.executablePath;
    input.volume = static_cast<double>(session.volume);
    input.volumeError = session.volumeError;
    input.muted = session.muted;
    input.muteValid = session.muteValid;
    input.muteError = session.muteError;
    input.expired = session.expired;
    input.active = session.active;
    input.systemSounds = session.systemSounds;
    input.stateText = session.stateText;
    return input;
}

QList<MixerSessionInput> toInputs(const QList<AudioSessionInfo> &sessions)
{
    QList<MixerSessionInput> inputs;
    inputs.reserve(sessions.size());
    for (const AudioSessionInfo &session : sessions) {
        inputs.append(toInput(session));
    }
    return inputs;
}

} // namespace

MixerPlugin::MixerPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

MixerPlugin::~MixerPlugin() = default;

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString MixerPlugin::id() const
{
    return QStringLiteral("media.mixer");
}

QString MixerPlugin::name() const
{
    return QStringLiteral("音量混合器");
}

QString MixerPlugin::description() const
{
    return QStringLiteral("按应用单独调音量与静音；同一应用的多个会话会一起调整，"
                          "不再出现「拖了滑块听不出变化」");
}

QIcon MixerPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Media);
}

WinEase::FeatureCategory MixerPlugin::category() const
{
    return WinEase::FeatureCategory::Media;
}

QString MixerPlugin::version() const
{
    return QStringLiteral("1.0.0");
}

QString MixerPlugin::author() const
{
    return QStringLiteral("WinEase");
}

QString MixerPlugin::detailedDescription() const
{
    // ⚠ 这一份是给用户看的：写清**做不到什么、为什么**，不要只写宣传语
    return QStringLiteral(
               "把 Windows 自带音量合成器里「每个应用一根滑块」搬到主界面面板里。\n"
               "\n"
               "与本工具其它音量功能的分工：\n"
               "· 滚轮调音量：改的是**系统主音量**（整台机器）\n"
               "· 本功能：改的是**会话音量**（某一个应用），**不会**动主音量\n"
               "\n"
               "比自带合成器多做的事：\n"
               "· 同一应用在这一台输出设备上开了好几份会话时，合并成一行、一起调整 —— "
               "只调其中一份正是「拖了滑块却听不出变化」的原因\n"
               "· 音量读不到时写出原因（例如跨权限的进程），不拿 0% 冒充「已静音」\n"
               "· 可选的「记住每个应用的音量」：下次该应用出现时自动恢复\n"
               "\n"
               "能力边界（做不到的事，以及原因）：\n"
               "· 只管**默认输出设备**上的会话。切换默认设备后会自动重新枚举；"
               "非默认设备上的会话不在列表里\n"
               "· 「让某个应用从耳机出声、另一个从音箱出声」（每应用指定输出设备）"
               "需要 Win11 未公开的 IAudioPolicyConfigFactory，本版不做\n"
               "· 「记住音量」只对**首次出现**的会话套用一次。若应用启动后自己又改了音量，"
               "以应用为准（我们不会反复覆盖它）\n"
               "· 已过期的会话（进程已退出）默认隐藏，可在面板上打开开关查看\n"
               "\n"
               "停用本功能**不会还原**任何音量：音量是用户可见、在用的系统设置，"
               "被工具悄悄改回去属于静默破坏。停用只做两件事：停止轮询、清掉本功能的界面状态。");
}

QStringList MixerPlugin::tags() const
{
    return { QStringLiteral("音量"), QStringLiteral("混音"), QStringLiteral("合成器"),
             QStringLiteral("应用音量"), QStringLiteral("静音"), QStringLiteral("mixer"),
             QStringLiteral("yl") };
}

bool MixerPlugin::requiresAdmin() const
{
    return false;
}

bool MixerPlugin::supportsHotkey() const
{
    // 面板型功能：动作都是"选中某一行再调"，没有可以一个键完成的语义
    return false;
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool MixerPlugin::initialize()
{
    // 只在 initialize() 读一次配置，之后改配置走面板控件
    if (WinEase::PluginServices *svc = services()) {
        m_groupByProcess = svc->configValue(id(), QStringLiteral("groupByProcess"), true).toBool();
        m_showExpired = svc->configValue(id(), QStringLiteral("showExpired"), false).toBool();
        m_rememberVolume = svc->configValue(id(), QStringLiteral("rememberVolume"), true).toBool();
        m_pollSeconds = svc->configValue(id(), QStringLiteral("pollSeconds"), kDefaultPollSeconds).toInt();
        m_pollSeconds = std::clamp(m_pollSeconds, kMinPollSeconds, kMaxPollSeconds);
    }
    loadRememberedVolumes();

    if (m_pollTimer == nullptr) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(m_pollSeconds * 1000);
        connect(m_pollTimer, &QTimer::timeout, this, [this] { pollOnce(); });
    }

    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("音量混合器已就绪"));
    return true;
}

void MixerPlugin::shutdown()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    // 退出时把记忆落盘（平时是"有变化才写"，退出时兜一次底）
    saveRememberedVolumes();

    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("groupByProcess"), m_groupByProcess);
        svc->setConfigValue(id(), QStringLiteral("showExpired"), m_showExpired);
        svc->setConfigValue(id(), QStringLiteral("rememberVolume"), m_rememberVolume);
        svc->syncConfig();
    }
    // ⚠ 不碰任何会话音量/静音：退出保持现状（纪律 3）
}

bool MixerPlugin::canEnable(QString *reason) const
{
    // 与 mic_mute 同口径：先如实说清"环境不满足"，不要让用户启用成功却什么都看不到
    if (!WinEase::Win32::isThreadComReady()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("当前线程未初始化 COM，无法访问音频会话");
        }
        return false;
    }

    QString error;
    const WinEase::Win32::AudioEndpoint probe =
        WinEase::Win32::AudioEndpoint::defaultEndpoint(WinEase::Win32::AudioDirection::Render, &error);
    if (!probe.isValid()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("没有检测到可用的默认输出设备：%1").arg(error);
        }
        return false;
    }
    return true;
}

bool MixerPlugin::onEnable()
{
    // 清空"上一轮存在的会话"：启用即重新接管，现有应用立刻就套上记忆音量
    m_lastInstanceIds.clear();
    m_deviceId.clear();

    // ⚠ 这里**不能**用 isEnabled() 做判断：setEnabled(true) 内部才调用 onEnable，
    //   此刻 m_enabled 还是旧值（踩坑 #28/#51/#57）。所以写入类动作（记忆套用）
    //   由本次显式调用完成，而不是靠在函数里"读自身状态"决定。
    applyMemoryPass();
    refreshSessions(false);

    if (m_pollTimer != nullptr) {
        m_pollTimer->start();
    }

    // 面板重画排到事件循环下一轮：那时 isEnabled() 才是真值，控件启用状态才正确
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    return true;
}

void MixerPlugin::onDisable()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    m_lastInstanceIds.clear();
    m_deviceId.clear();

    // ⚠ 不还原任何音量/静音（纪律 3）。这里只清掉"本功能的运行态"
    saveRememberedVolumes();

    Q_EMIT statusMessage(QStringLiteral("已停用（各应用的音量与静音保持不变）"));

    // 同样排到事件循环下一轮：onDisable 里 isEnabled() 仍是旧的 true（踩坑 #28）
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
//  轮询与刷新
// ---------------------------------------------------------------------------

void MixerPlugin::pollOnce()
{
    if (!isEnabled()) {
        return;
    }
    applyMemoryPass();      // 先套用记忆（这一轮会改系统状态）
    refreshSessions(false); // 再读**真实**状态重画 —— 面板上显示的永远是读回来的值
}

void MixerPlugin::refreshSessions(bool fromUser)
{
    Q_UNUSED(fromUser)

    QString error;
    const QList<AudioSessionInfo> sessions = WinEase::Win32::audioSessions(&error);
    if (!error.isEmpty() && error != m_enumError) {
        // 只在"错误内容变了"时报一次：轮询每 2 秒一次，
        // 无去重会把同一条错误刷满日志与卡片（也没人会读重复 100 遍的东西）
        if (sessions.isEmpty()) {
            reportFailure(QStringLiteral("枚举音频会话失败：%1").arg(error));
        } else {
            logMessage(WinEase::PluginLogLevel::Warning, error);
        }
    }
    m_enumError = error;

    // ---- 设备热插拔判据：默认输出设备变了就把"已出现"集合整体作废 ----
    // 会话实例 id 是"每设备"的，换了设备之后旧 id 不会再回来
    const QString deviceId = WinEase::Win32::defaultRenderDeviceId();
    const QString deviceName = WinEase::Win32::defaultRenderDeviceName();
    if (deviceId != m_deviceId) {
        if (!m_deviceId.isEmpty()) {
            logMessage(WinEase::PluginLogLevel::Info,
                       QStringLiteral("默认输出设备已更换（%1 → %2），重新枚举会话")
                           .arg(m_deviceName.isEmpty() ? QStringLiteral("（无）") : m_deviceName,
                                deviceName.isEmpty() ? QStringLiteral("（无）") : deviceName));
        }
        m_lastInstanceIds.clear();
        m_selectedKey.clear();
        m_deviceId = deviceId;
    }
    if (!deviceName.isEmpty()) {
        m_deviceName = deviceName;
    }

    MixerOptions options;
    options.groupByProcess = m_groupByProcess;
    options.showExpired = m_showExpired;
    m_snapshot = WinEase::FeaturePlugins::Mixer::buildSnapshot(toInputs(sessions), options);

    rebuildList(m_snapshot);
    restoreSelection(m_selectedKey);

    QString status = WinEase::FeaturePlugins::Mixer::statusLine(m_deviceName, m_snapshot);
    if (!m_enumError.isEmpty()) {
        status = QStringLiteral("%1 · %2").arg(m_enumError, status);
    }
    publishStatus(status);

    if (fromUser) {
        clearLastError();
    }
}

// ---------------------------------------------------------------------------
//  音量记忆
// ---------------------------------------------------------------------------

void MixerPlugin::applyMemoryPass()
{
    QString error;
    const QList<AudioSessionInfo> sessions = WinEase::Win32::audioSessions(&error);
    if (!error.isEmpty() && sessions.isEmpty()) {
        // 枚举失败：这一轮别改也别记。宁可"这一轮不做"，也不要拿半份名单去改会话音量
        return;
    }

    QSet<QString> current;
    for (const AudioSessionInfo &session : sessions) {
        if (!session.instanceId.isEmpty()) {
            current.insert(session.instanceId);
        }
    }

    int applied = 0;
    if (m_rememberVolume) {
        // 同一个记忆值归成一批：不同应用记得的音量不同，不能一锅端
        QMap<double, QStringList> pending;
        for (const AudioSessionInfo &session : sessions) {
            if (session.expired || session.systemSounds || session.instanceId.isEmpty()) {
                continue; // 过期会话/系统声音不参与记忆
            }
            if (m_lastInstanceIds.contains(session.instanceId)) {
                continue; // 不是"首次出现"：只套用一次，之后以用户/应用为准
            }
            const QString key = memoryKey(session.processName);
            if (key.isEmpty()) {
                continue;
            }
            const auto found = m_remembered.constFind(key);
            if (found == m_remembered.constEnd()) {
                continue;
            }
            pending[found.value()].append(session.instanceId);
        }

        for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
            QStringList failures;
            const int succeeded = WinEase::Win32::applySessionVolume(
                it.value(), static_cast<float>(it.key()), &failures);
            applied += succeeded;
            if (succeeded > 0) {
                logMessage(WinEase::PluginLogLevel::Info,
                           QStringLiteral("新出现的 %1 份会话已按记忆恢复为 %2")
                               .arg(succeeded)
                               .arg(WinEase::FeaturePlugins::Mixer::formatVolume(it.key())));
            }
            for (const QString &failure : failures) {
                logMessage(WinEase::PluginLogLevel::Warning,
                           QStringLiteral("按记忆恢复音量失败：%1").arg(failure));
            }
        }
    }

    // 记下"本轮存在的会话"：下一轮靠它判断谁是新出现的
    m_lastInstanceIds = current;

    if (!m_rememberVolume) {
        return;
    }

    if (applied > 0) {
        // 套用后**重读**再观察：否则会把"我们以为设成了多少"当成真实值记进配置
        QString rereadError;
        observeVolumesForMemory(WinEase::Win32::audioSessions(&rereadError));
    } else {
        observeVolumesForMemory(sessions);
    }
}

void MixerPlugin::observeVolumesForMemory(const QList<AudioSessionInfo> &sessions)
{
    bool changed = false;
    for (const AudioSessionInfo &session : sessions) {
        if (!session.adjustable() || session.systemSounds) {
            continue; // 过期会话、系统声音不参与记忆
        }
        if (session.volume < 0.0F) {
            continue; // 读不到就不记：宁可没有记忆，也不要记一个假值
        }
        const QString key = memoryKey(session.processName);
        if (key.isEmpty()) {
            continue;
        }

        const double value = static_cast<double>(session.volume);
        const auto found = m_remembered.constFind(key);
        if (found != m_remembered.constEnd() && std::abs(found.value() - value) < kRememberEpsilon) {
            continue;
        }
        m_remembered.insert(key, value);
        changed = true;
    }

    if (changed) {
        m_rememberedDirty = true;
        saveRememberedVolumes();
    }
}

void MixerPlugin::loadRememberedVolumes()
{
    m_remembered.clear();

    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    const QStringList entries =
        svc->configValue(id(), QStringLiteral("rememberedVolumes"), QStringList()).toStringList();
    for (const QString &entry : entries) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        if (separator <= 0) {
            continue;
        }
        bool ok = false;
        const double value = entry.mid(separator + 1).toDouble(&ok);
        if (!ok || value < 0.0 || value > 1.0) {
            continue; // 坏数据直接丢，不猜
        }
        m_remembered.insert(memoryKey(entry.left(separator)), value);
    }
}

void MixerPlugin::saveRememberedVolumes()
{
    if (!m_rememberedDirty) {
        return;
    }
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    QStringList entries;
    entries.reserve(m_remembered.size());
    for (auto it = m_remembered.constBegin(); it != m_remembered.constEnd(); ++it) {
        entries.append(QStringLiteral("%1=%2").arg(it.key(), QString::number(it.value(), 'f', 3)));
    }
    entries.sort(); // 落盘顺序稳定：便于人工查看与比对
    if (entries.size() > kMaxRememberedEntries) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("已记住的音量条目超过 %1 条，仅保留前 %1 条")
                       .arg(kMaxRememberedEntries));
        entries = entries.mid(0, kMaxRememberedEntries);
    }

    svc->setConfigValue(id(), QStringLiteral("rememberedVolumes"), entries);
    svc->syncConfig();
    m_rememberedDirty = false;
}

void MixerPlugin::forgetRememberedVolumes()
{
    m_remembered.clear();
    m_rememberedDirty = true;
    saveRememberedVolumes();
    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("已清除记住的应用音量"));
    Q_EMIT statusMessage(QStringLiteral("已清除记住的应用音量"));
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QWidget *MixerPlugin::createSettingsWidget(QWidget *parent)
{
    m_panel = new QWidget(parent);
    m_panel->setObjectName(QStringLiteral("mixerPanel"));
    auto *layout = new QVBoxLayout(m_panel);

    m_statusLabel = new QLabel(m_panel);
    m_statusLabel->setObjectName(QStringLiteral("mixerStatusLabel"));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_sessionList = new QListWidget(m_panel);
    m_sessionList->setObjectName(QStringLiteral("mixerSessionList"));
    m_sessionList->setMinimumHeight(150);
    m_sessionList->setAlternatingRowColors(true);
    layout->addWidget(m_sessionList, 1);

    auto *volumeLayout = new QHBoxLayout();
    volumeLayout->addWidget(new QLabel(QStringLiteral("该应用音量"), m_panel));
    m_volumeSlider = new QSlider(Qt::Horizontal, m_panel);
    m_volumeSlider->setObjectName(QStringLiteral("mixerVolumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setSingleStep(5);
    m_volumeSlider->setPageStep(10);
    volumeLayout->addWidget(m_volumeSlider, 1);
    m_volumeValueLabel = new QLabel(m_panel);
    m_volumeValueLabel->setObjectName(QStringLiteral("mixerVolumeValueLabel"));
    m_volumeValueLabel->setMinimumWidth(72);
    volumeLayout->addWidget(m_volumeValueLabel);
    layout->addLayout(volumeLayout);

    m_muteCheck = new QCheckBox(QStringLiteral("静音该应用（它的全部会话一起生效）"), m_panel);
    m_muteCheck->setObjectName(QStringLiteral("mixerMuteCheck"));
    layout->addWidget(m_muteCheck);

    m_detailLabel = new QLabel(m_panel);
    m_detailLabel->setObjectName(QStringLiteral("mixerDetailLabel"));
    m_detailLabel->setWordWrap(true);
    m_detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_detailLabel);

    auto *actionLayout = new QHBoxLayout();
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), m_panel);
    m_refreshButton->setObjectName(QStringLiteral("mixerRefreshButton"));
    actionLayout->addWidget(m_refreshButton);
    m_forgetButton = new QPushButton(QStringLiteral("清除已记住的音量"), m_panel);
    m_forgetButton->setObjectName(QStringLiteral("mixerForgetButton"));
    actionLayout->addWidget(m_forgetButton);
    actionLayout->addStretch(1);
    layout->addLayout(actionLayout);

    auto *switchLayout = new QHBoxLayout();
    m_showExpiredCheck = new QCheckBox(QStringLiteral("显示已过期的会话"), m_panel);
    m_showExpiredCheck->setObjectName(QStringLiteral("mixerShowExpiredCheck"));
    m_showExpiredCheck->setChecked(m_showExpired);
    switchLayout->addWidget(m_showExpiredCheck);
    m_groupCheck = new QCheckBox(QStringLiteral("按应用合并同类会话"), m_panel);
    m_groupCheck->setObjectName(QStringLiteral("mixerGroupCheck"));
    m_groupCheck->setChecked(m_groupByProcess);
    switchLayout->addWidget(m_groupCheck);
    m_rememberCheck = new QCheckBox(QStringLiteral("记住每个应用的音量"), m_panel);
    m_rememberCheck->setObjectName(QStringLiteral("mixerRememberCheck"));
    m_rememberCheck->setChecked(m_rememberVolume);
    switchLayout->addWidget(m_rememberCheck);
    switchLayout->addStretch(1);
    layout->addLayout(switchLayout);

    auto *hint = new QLabel(
        QStringLiteral(
            "· 这里调的是**某个应用**的音量，不会动系统主音量（主音量请用「滚轮调音量」）\n"
            "· 同一应用在这一台输出设备上开了多份会话时合并成一行、一起调整 —— "
            "只调其中一份就会出现「拖了滑块却听不出变化」\n"
            "· 「记住每个应用的音量」打开时，会对当前所有应用立即恢复一次已记住的音量，"
            "之后只对新出现的应用套用一次\n"
            "· 停用本功能不会还原任何音量（音量是你在用的系统设置）"),
        m_panel);
    hint->setObjectName(QStringLiteral("mixerHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // ---- 连接（放在设完初值之后，避免程序化赋值触发回调）----
    connect(m_sessionList, &QListWidget::currentRowChanged, this, [this](int) {
        if (m_syncingUi || m_sessionList.isNull()) {
            return;
        }
        const QListWidgetItem *item = m_sessionList->currentItem();
        m_selectedKey = (item != nullptr) ? item->data(kRowKeyRole).toString() : QString();
        updateDetailForCurrentRow();
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        applyVolumeToCurrentRow(value);
    });
    connect(m_muteCheck, &QCheckBox::toggled, this, [this](bool checked) {
        applyMuteToCurrentRow(checked);
    });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refreshPanel(); });
    connect(m_showExpiredCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_showExpired = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("showExpired"), checked);
        }
        refreshPanel();
    });
    connect(m_groupCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_groupByProcess = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("groupByProcess"), checked);
        }
        refreshPanel();
    });
    connect(m_rememberCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_rememberVolume = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("rememberVolume"), checked);
        }
        if (checked) {
            // 刚打开开关：把当前所有应用都当成"新出现的"，让记忆立刻生效一次
            m_lastInstanceIds.clear();
        }
        refreshPanel();
    });
    connect(m_forgetButton, &QPushButton::clicked, this, [this] { forgetRememberedVolumes(); });

    refreshPanel();
    return m_panel;
}

void MixerPlugin::rebuildList(const MixerSnapshot &snapshot)
{
    if (m_sessionList.isNull()) {
        return;
    }

    m_syncingUi = true;
    m_sessionList->clear();
    for (const MixerRow &row : snapshot.rows) {
        QString text = row.title;
        if (row.sessionCount > 1) {
            text += QStringLiteral("  ×%1").arg(row.sessionCount);
        }
        text += QLatin1Char('\n') + row.subtitle;

        auto *item = new QListWidgetItem(text, m_sessionList);
        item->setToolTip(row.tooltip);
        item->setData(kRowKeyRole, row.key);
        item->setData(kPidRole, static_cast<quint32>(row.pid));
        item->setData(kInstanceIdsRole, row.instanceIds);
        if (!row.adjustable()) {
            item->setForeground(QBrush(QColor(0x8B, 0x8D, 0x98))); // 灰掉：过期行不可调
        }
    }
    m_syncingUi = false;
}

void MixerPlugin::restoreSelection(const QString &rowKey)
{
    if (m_sessionList.isNull()) {
        return;
    }

    int index = -1;
    if (!rowKey.isEmpty()) {
        for (int row = 0; row < m_sessionList->count(); ++row) {
            if (m_sessionList->item(row)->data(kRowKeyRole).toString() == rowKey) {
                index = row;
                break;
            }
        }
    }

    m_syncingUi = true;
    if (index >= 0) {
        m_sessionList->setCurrentRow(index);
    } else {
        // 选中的应用消失了就**如实清空**，不要"顺手选中别的" —— 否则用户下一次
        // 拖滑块会作用到一个他并没有选中的应用上
        m_sessionList->setCurrentRow(-1);
        m_selectedKey.clear();
    }
    m_syncingUi = false;

    updateDetailForCurrentRow();
}

void MixerPlugin::updateDetailForCurrentRow()
{
    const MixerRow row = currentRow();
    const bool running = isEnabled();
    const bool adjustable = running && row.adjustable();

    if (m_volumeSlider != nullptr) {
        // ⚠ 拖动中不要回写滑块：会和用户的手指抢（拖到一半被拉回真实值）
        if (!m_volumeSlider->isSliderDown()) {
            const QSignalBlocker blocker(m_volumeSlider);
            if (row.volume >= 0.0) {
                m_volumeSlider->setValue(static_cast<int>(std::lround(row.volume * 100.0)));
            }
        }
        // 音量读不到时滑块不启用：总得有个位置，但我们**不知道**位置在哪
        m_volumeSlider->setEnabled(adjustable && row.volume >= 0.0);
    }

    if (m_volumeValueLabel != nullptr) {
        m_volumeValueLabel->setText(
            row.volume >= 0.0
                ? WinEase::FeaturePlugins::Mixer::formatVolume(row.volume)
                : WinEase::FeaturePlugins::Mixer::unavailableVolumeText(row.volumeError));
    }

    if (m_muteCheck != nullptr) {
        const QSignalBlocker blocker(m_muteCheck);
        m_muteCheck->setChecked(row.muteValid && row.muted);
        m_muteCheck->setEnabled(adjustable && row.muteValid);
    }

    if (m_detailLabel != nullptr) {
        m_detailLabel->setText(row.key.isEmpty()
                                   ? WinEase::FeaturePlugins::Mixer::noSelectionText()
                                   : row.tooltip);
    }
}

void MixerPlugin::refreshPanel()
{
    // 选项勾选框：成员是唯一真相，控件只是它的显示（程序化赋值必须挡住信号）
    if (m_showExpiredCheck != nullptr) {
        const QSignalBlocker blocker(m_showExpiredCheck);
        m_showExpiredCheck->setChecked(m_showExpired);
    }
    if (m_groupCheck != nullptr) {
        const QSignalBlocker blocker(m_groupCheck);
        m_groupCheck->setChecked(m_groupByProcess);
    }
    if (m_rememberCheck != nullptr) {
        const QSignalBlocker blocker(m_rememberCheck);
        m_rememberCheck->setChecked(m_rememberVolume);
    }

    refreshSessions(false);

    updateStatusLabel();
    if (m_forgetButton != nullptr) {
        m_forgetButton->setEnabled(isEnabled());
    }
    updateDetailForCurrentRow();
}

WinEase::FeaturePlugins::Mixer::MixerRow MixerPlugin::currentRow() const
{
    WinEase::FeaturePlugins::Mixer::MixerRow row;
    if (m_sessionList.isNull() || m_sessionList->currentItem() == nullptr) {
        return row;
    }
    rowByKey(m_sessionList->currentItem()->data(kRowKeyRole).toString(), &row);
    return row;
}

bool MixerPlugin::rowByKey(const QString &rowKey,
                           WinEase::FeaturePlugins::Mixer::MixerRow *rowOut) const
{
    if (rowKey.isEmpty()) {
        return false;
    }
    for (const MixerRow &row : m_snapshot.rows) {
        if (row.key == rowKey) {
            if (rowOut != nullptr) {
                *rowOut = row;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  写入
// ---------------------------------------------------------------------------

void MixerPlugin::applyVolumeToCurrentRow(int percent)
{
    if (m_syncingUi || !isEnabled()) {
        return;
    }

    const MixerRow row = currentRow();
    if (row.instanceIds.isEmpty()) {
        return;
    }

    const float volume = static_cast<float>(std::clamp(percent, 0, 100) / 100.0);
    QStringList failures;
    const int succeeded = WinEase::Win32::applySessionVolume(row.instanceIds, volume, &failures);

    // 立刻反馈，不等下一轮轮询（轮询最长 2 秒，用户会觉得滑块"没反应"）
    if (m_volumeValueLabel != nullptr) {
        m_volumeValueLabel->setText(WinEase::FeaturePlugins::Mixer::formatVolume(volume));
    }

    if (succeeded != row.instanceIds.size()) {
        reportFailure(QStringLiteral("调整「%1」音量：%2 份会话里有 %3 份失败 —— %4")
                          .arg(row.title)
                          .arg(row.instanceIds.size())
                          .arg(row.instanceIds.size() - succeeded)
                          .arg(failures.join(QStringLiteral("；"))));
        return;
    }
    clearLastError();
}

void MixerPlugin::applyMuteToCurrentRow(bool muted)
{
    if (m_syncingUi || !isEnabled()) {
        return;
    }

    const MixerRow row = currentRow();
    if (row.instanceIds.isEmpty()) {
        return;
    }

    QStringList failures;
    const int succeeded = WinEase::Win32::applySessionMuted(row.instanceIds, muted, &failures);
    if (succeeded != row.instanceIds.size()) {
        reportFailure(QStringLiteral("调整「%1」静音：%2 份会话里有 %3 份失败 —— %4")
                          .arg(row.title)
                          .arg(row.instanceIds.size())
                          .arg(row.instanceIds.size() - succeeded)
                          .arg(failures.join(QStringLiteral("；"))));
        return;
    }

    clearLastError();
    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("已把「%1」的 %2 份会话设为%3")
                   .arg(row.title)
                   .arg(row.instanceIds.size())
                   .arg(muted ? QStringLiteral("静音") : QStringLiteral("取消静音")));
}

// ---------------------------------------------------------------------------
//  状态
// ---------------------------------------------------------------------------

void MixerPlugin::publishStatus(const QString &text)
{
    m_statusLine = text;
    updateStatusLabel();
    Q_EMIT statusMessage(text);
}

void MixerPlugin::updateStatusLabel()
{
    if (m_statusLabel == nullptr) {
        return;
    }
    if (isEnabled() || m_statusLine.isEmpty()) {
        m_statusLabel->setText(m_statusLine);
        return;
    }
    // 未启用时明确写出"已停用"，而不是留一行旧数据让人以为还在跑
    m_statusLabel->setText(
        QStringLiteral("已停用（各应用的音量与静音保持不变） · %1").arg(m_statusLine));
}

void MixerPlugin::reportFailure(const QString &text)
{
    setLastError(text);
    logMessage(WinEase::PluginLogLevel::Warning, text);
    Q_EMIT errorOccurred(text);
}
