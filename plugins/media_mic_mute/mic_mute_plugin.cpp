#include "mic_mute_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/ComApartment.h"

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::AudioDirection;
using WinEase::Win32::AudioEndpoint;

/// 外部状态同步间隔。外部改动（耳机线上的麦键、Windows 声音设置、会议软件
/// 自动闭麦/开麦）只能靠轮询发现；2 秒是"够快"与"几乎不耗电"的折中 ——
/// 每轮只是一次 `GetMute()` COM 调用，微秒级。
constexpr int kSyncIntervalMs = 2000;

/// 徽标：静音=红「静」，可用=绿「麦」，读不到=灰「？」
/// （常驻而不是只在静音时出现：徽标消失到底是"没静音"还是"插件没跑"，
///   用户没法分辨，而这件事恰恰需要一眼看清）
const QString kBadgeMutedText = QStringLiteral("静");
const QString kBadgeMutedColor = QStringLiteral("#E5484D");
const QString kBadgeLiveText = QStringLiteral("麦");
const QString kBadgeLiveColor = QStringLiteral("#30A46C");
const QString kBadgeUnknownText = QStringLiteral("？");
const QString kBadgeUnknownColor = QStringLiteral("#8B8D98");

} // namespace

MicMutePlugin::MicMutePlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString MicMutePlugin::id() const
{
    return QStringLiteral("media.mic_mute");
}

QString MicMutePlugin::name() const
{
    return QStringLiteral("麦克风一键静音");
}

QString MicMutePlugin::description() const
{
    return QStringLiteral("一个快捷键闭麦/开麦；托盘图标常驻显示系统真实的麦克风状态");
}

QIcon MicMutePlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Media);
}

WinEase::FeatureCategory MicMutePlugin::category() const
{
    return WinEase::FeatureCategory::Media;
}

QStringList MicMutePlugin::tags() const
{
    return { QStringLiteral("静音"), QStringLiteral("麦克风"), QStringLiteral("闭麦"),
             QStringLiteral("mic"), QStringLiteral("mute"), QStringLiteral("bm") };
}

bool MicMutePlugin::supportsHotkey() const
{
    return true;
}

QKeySequence MicMutePlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+M"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool MicMutePlugin::initialize()
{
    // 插件只在 initialize() 读一次配置，之后改配置走面板控件
    m_notifyOnToggle = services() != nullptr
                           ? services()->configValue(id(), QStringLiteral("notifyOnToggle"), true)
                                 .toBool()
                           : true;

    if (m_syncTimer == nullptr) {
        m_syncTimer = new QTimer(this);
        m_syncTimer->setInterval(kSyncIntervalMs);
        connect(m_syncTimer, &QTimer::timeout, this, [this] { syncExternalState(); });
    }

    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("麦克风一键静音已就绪"));
    return true;
}

void MicMutePlugin::shutdown()
{
    if (m_syncTimer != nullptr) {
        m_syncTimer->stop();
    }
    m_publishedValid = false;

    // 卸载时把徽标摘掉，否则托盘上会永远挂着一个没人负责的标记。
    // ⚠ 只摘徽标：麦克风的静音状态保持原样（fail-closed，理由见头文件）
    if (WinEase::PluginServices *svc = services()) {
        svc->setTrayBadge(id(), QString(), QString(), QString());
    }
}

bool MicMutePlugin::canEnable(QString *reason) const
{
    // 与 P2-05 一致：音频调用依赖宿主主线程的 COM 套间，这里先如实说清楚
    if (!WinEase::Win32::isThreadComReady()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("当前线程未初始化 COM，无法访问音频设备");
        }
        return false;
    }

    QString error;
    const AudioEndpoint probe = AudioEndpoint::defaultEndpoint(AudioDirection::Capture, &error);
    if (!probe.isValid()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("没有检测到可用的麦克风（默认采集端点）：%1").arg(error);
        }
        return false;
    }
    return true;
}

bool MicMutePlugin::onEnable()
{
    QString error;
    if (!ensureEndpoint(&error)) {
        setLastError(error);
        return false;
    }

    // 一启用就把真实状态报出去：徽标常驻，用户随时能看一眼"现在是不是闭麦"，
    // 而不是"等按了快捷键才出现"
    bool muted = false;
    if (readMuteState(&muted, &error)) {
        m_publishedValid = false; // 强制重报一次
        publishState(muted, true);
    } else {
        publishUnknownState(error);
    }

    if (m_syncTimer != nullptr) {
        m_syncTimer->start();
    }
    return true;
}

void MicMutePlugin::onDisable()
{
    if (m_syncTimer != nullptr) {
        m_syncTimer->stop();
    }
    m_publishedValid = false;

    if (WinEase::PluginServices *svc = services()) {
        // 摘徽标 = 停止对外宣称状态；**不动麦克风本身**：
        // 静音是 fail-closed 的隐私状态，停用功能时悄悄把麦打开，比留下一个静音危险
        svc->setTrayBadge(id(), QString(), QString(), QString());
    }
    Q_EMIT statusMessage(QStringLiteral("已停用（麦克风静音状态保持不变，不会被自动打开）"));
}

void MicMutePlugin::onHotkey(const QString &hotkeyId)
{
    // <id>::default 在插件**加载时**就注册好了，停用期间也可能被分发到，先挡住
    if (!isEnabled()) {
        return;
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("default") || action == QLatin1String("toggle")) {
        toggleFromUser();
    }
}

// ---------------------------------------------------------------------------
//  功能实现
// ---------------------------------------------------------------------------

bool MicMutePlugin::ensureEndpoint(QString *errorOut)
{
    if (m_endpoint.isValid()) {
        return true;
    }

    QString error;
    m_endpoint = AudioEndpoint::defaultEndpoint(AudioDirection::Capture, &error);
    if (m_endpoint.isValid()) {
        return true;
    }

    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("绑定默认麦克风失败：%1").arg(error);
    }
    return false;
}

bool MicMutePlugin::readMuteState(bool *mutedOut, QString *errorOut) const
{
    QString error;
    if (!m_endpoint.muteState(mutedOut, &error)) {
        if (errorOut != nullptr) {
            *errorOut = error;
        }
        return false;
    }
    return true;
}

bool MicMutePlugin::toggleMute(bool *stateOut, QString *errorOut)
{
    QString error;
    if (!ensureEndpoint(&error)) {
        if (errorOut != nullptr) {
            *errorOut = error;
        }
        return false;
    }

    // 先读**系统**当前值再取反：本地缓存一旦被外部改动（耳机麦键/会议软件）就会骗人
    bool current = false;
    if (!readMuteState(&current, &error)) {
        // 句柄可能已失效（设备换了）→ 丢掉，下次重建
        m_endpoint = AudioEndpoint();
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("读取麦克风状态失败：%1").arg(error);
        }
        return false;
    }

    const bool target = !current;
    if (!m_endpoint.setMuted(target, &error)) {
        m_endpoint = AudioEndpoint();
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("设置麦克风静音失败：%1").arg(error);
        }
        return false;
    }

    // 写成功 ≠ 生效（独占模式下有些端点会接受 SetMute 却不变状态）→ 回读确认
    bool applied = false;
    if (!readMuteState(&applied, &error)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("写入后回读麦克风状态失败：%1").arg(error);
        }
        return false;
    }

    if (stateOut != nullptr) {
        *stateOut = applied;
    }

    if (applied != target) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("系统没有接受这次切换（仍为「%1」）")
                            .arg(applied ? QStringLiteral("已静音") : QStringLiteral("未静音"));
        }
        return false;
    }
    return true;
}

void MicMutePlugin::toggleFromUser()
{
    bool muted = false;
    QString error;
    if (!toggleMute(&muted, &error)) {
        setLastError(error);
        logMessage(WinEase::PluginLogLevel::Warning, error);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), error);
        }
        // 失败时不能把旧徽标留在托盘上假装还能信
        publishUnknownState(error);
        return;
    }

    clearLastError();
    publishState(muted, true);

    if (WinEase::PluginServices *svc = services(); svc != nullptr && m_notifyOnToggle) {
        svc->notify(name(),
                    muted ? QStringLiteral("已静音（系统麦克风已关闭）")
                          : QStringLiteral("已取消静音（麦克风已打开）"));
    }
}

void MicMutePlugin::publishState(bool muted, bool force)
{
    if (!force && m_publishedValid && m_publishedMuted == muted) {
        return; // 状态没变就不重画图标（重画会让托盘闪一下）
    }

    m_publishedMuted = muted;
    m_publishedValid = true;

    if (WinEase::PluginServices *svc = services()) {
        svc->setTrayBadge(id(),
                          muted ? kBadgeMutedText : kBadgeLiveText,
                          muted ? kBadgeMutedColor : kBadgeLiveColor,
                          muted ? QStringLiteral("麦克风已静音") : QStringLiteral("麦克风可用"));
    }

    Q_EMIT statusMessage(muted ? QStringLiteral("麦克风已静音") : QStringLiteral("麦克风可用"));
    refreshPanel();
}

void MicMutePlugin::publishUnknownState(const QString &reason)
{
    // "读不到"也是必须说出去的信息：留着旧徽标等于对外说谎
    m_publishedValid = false;

    if (WinEase::PluginServices *svc = services()) {
        svc->setTrayBadge(id(), kBadgeUnknownText, kBadgeUnknownColor,
                          QStringLiteral("麦克风状态未知：%1").arg(reason));
    }

    Q_EMIT statusMessage(QStringLiteral("麦克风状态未知：%1").arg(reason));
    refreshPanel();
}

void MicMutePlugin::syncExternalState()
{
    if (!isEnabled()) {
        return;
    }

    bool muted = false;
    QString error;
    if (!readMuteState(&muted, &error)) {
        // 读不到（设备被拔掉等）：丢掉可能已失效的句柄，下一轮重建后再判
        m_endpoint = AudioEndpoint();
        if (m_publishedValid) {
            publishUnknownState(error);
        }
        return;
    }

    if (m_publishedValid && m_publishedMuted == muted) {
        return;
    }

    // 走到这里说明状态被**外部**改了（耳机麦键 / 系统设置 / 会议软件自动闭麦）。
    // 徽标必须跟着变：否则用户看托盘得出的结论是错的
    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("检测到麦克风状态被外部改变：%1")
                   .arg(muted ? QStringLiteral("已静音") : QStringLiteral("已取消静音")));
    publishState(muted, true);
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *MicMutePlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("micMutePanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *stateLabel = new QLabel(widget);
    stateLabel->setObjectName(QStringLiteral("micMuteStateLabel"));
    stateLabel->setWordWrap(true);
    layout->addWidget(stateLabel);

    auto *deviceLabel = new QLabel(widget);
    deviceLabel->setObjectName(QStringLiteral("micMuteDeviceLabel"));
    deviceLabel->setWordWrap(true);
    layout->addWidget(deviceLabel);

    auto *toggleButton = new QPushButton(widget);
    toggleButton->setObjectName(QStringLiteral("micMuteToggleButton"));
    layout->addWidget(toggleButton, 0, Qt::AlignLeft);

    auto *notifyCheck = new QCheckBox(QStringLiteral("切换时弹气泡提示"), widget);
    notifyCheck->setObjectName(QStringLiteral("micMuteNotifyCheck"));
    notifyCheck->setChecked(m_notifyOnToggle);
    layout->addWidget(notifyCheck);

    auto *hint = new QLabel(
        QStringLiteral("快捷键：%1（可在主界面「快捷键」里修改）。\n"
                       "状态每次都直接读系统，不保存本地状态，所以面板与托盘图标会"
                       "跟着外部改动（耳机麦键、声音设置、会议软件）一起变。\n"
                       "停用本功能不会自动取消静音 —— 静音属于隐私状态，"
                       "被工具悄悄打开比留下一个静音更危险。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText)),
        widget);
    hint->setObjectName(QStringLiteral("micMuteHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_stateLabel = stateLabel;
    m_deviceLabel = deviceLabel;
    m_toggleButton = toggleButton;
    m_notifyCheck = notifyCheck;

    connect(toggleButton, &QPushButton::clicked, widget, [this] {
        // 面板按钮与快捷键走**同一条路**（同一条实现，不会出现"按钮能用的场景快捷键不能用"）
        toggleFromUser();
    });
    connect(notifyCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_notifyOnToggle = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("notifyOnToggle"), checked);
            svc->syncConfig();
        }
    });

    refreshPanel();
    return widget;
}

void MicMutePlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    // 面板可以在"未启用"时被打开：这里只读不写，绑定端点不产生任何系统状态
    QString bindError;
    if (!ensureEndpoint(&bindError)) {
        if (!m_stateLabel.isNull()) {
            m_stateLabel->setText(QStringLiteral("当前状态：%1").arg(bindError));
        }
        if (!m_deviceLabel.isNull()) {
            m_deviceLabel->setText(QStringLiteral("设备：未绑定"));
        }
        if (!m_toggleButton.isNull()) {
            m_toggleButton->setText(QStringLiteral("静音（闭麦）"));
            m_toggleButton->setEnabled(false);
        }
        return;
    }

    // 面板显示的状态也必须现读系统，而不是拿 m_publishedMuted 顶上：
    // 那个值只在"报出去的时刻"有效，用户打开面板的这一刻未必还是它
    bool muted = false;
    QString error;
    const bool known = readMuteState(&muted, &error);

    if (!m_stateLabel.isNull()) {
        m_stateLabel->setText(known
                                  ? (muted ? QStringLiteral("当前状态：麦克风已静音")
                                           : QStringLiteral("当前状态：麦克风可用（未静音）"))
                                  : QStringLiteral("当前状态：读不到（%1）").arg(error));
    }

    if (!m_deviceLabel.isNull()) {
        m_deviceLabel->setText(m_endpoint.isValid()
                                   ? QStringLiteral("设备：%1").arg(m_endpoint.deviceName())
                                   : QStringLiteral("设备：未绑定"));
    }

    if (!m_toggleButton.isNull()) {
        m_toggleButton->setText(muted && known ? QStringLiteral("取消静音（开麦）")
                                               : QStringLiteral("静音（闭麦）"));
        m_toggleButton->setEnabled(known);
    }
}
