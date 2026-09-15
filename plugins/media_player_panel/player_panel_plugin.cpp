#include "player_panel_plugin.h"

#include "sdk/FeatureCategory.h"
#include "sdk/PluginServices.h"
#include "win32/ComApartment.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

using WinEase::FeaturePlugins::Player::PlayerOptions;
using WinEase::FeaturePlugins::Player::PlayerRow;
using WinEase::FeaturePlugins::Player::PlayerSessionInput;
using WinEase::Win32::MediaCommand;
using WinEase::Win32::MediaPlaybackState;
using WinEase::Win32::MediaSessionInfo;

/// 列表项的定位串数据槽（自检按它取"这一行是哪个会话"）
constexpr int kKeyRole = Qt::UserRole;

constexpr int kSeekSliderMaximum = 1000;

constexpr int kDefaultPollSeconds = 2;
constexpr int kMinPollSeconds = 1;
constexpr int kMaxPollSeconds = 10;

PlayerSessionInput toInput(const MediaSessionInfo &session)
{
    PlayerSessionInput input;
    input.sessionId = session.sessionId;
    input.appName = session.appName;
    input.pid = session.pid;
    input.current = session.current;
    input.playing = (session.playback == MediaPlaybackState::Playing);
    input.playbackError = session.playbackError;
    if (session.playback != MediaPlaybackState::Unknown) {
        input.playbackText = WinEase::Win32::mediaPlaybackStateText(session.playback);
    } else if (input.playbackError.isEmpty()) {
        input.playbackError = QStringLiteral("系统没有给出状态");
    }

    input.title = session.title;
    input.artist = session.artist;
    input.albumTitle = session.albumTitle;
    input.propertiesError = session.propertiesError;

    input.positionMs = session.positionMs;
    input.durationMs = session.durationMs;
    input.updatedAtMs = session.updatedAtMs;
    input.timelineError = session.timelineError;

    input.canPlay = session.canPlay;
    input.canPause = session.canPause;
    input.canNext = session.canNext;
    input.canPrevious = session.canPrevious;
    return input;
}

/// 进度文案：位置按时长外推（"这一刻"的位置），未知的部分照实写原因
QString liveProgressText(const PlayerRow &row)
{
    using namespace WinEase::FeaturePlugins::Player;
    if (row.key.isEmpty()) {
        return QStringLiteral("（未选择会话）");
    }
    if (row.positionMs < 0) {
        return row.progressText;
    }
    const qint64 position = projectedPositionMs(row.positionMs,
                                               row.updatedAtMs,
                                               QDateTime::currentMSecsSinceEpoch(),
                                               row.playing,
                                               row.durationMs);
    if (row.durationMs <= 0) {
        return QStringLiteral("%1 / 时长未知（该播放器没有提供）").arg(formatClock(position));
    }
    return QStringLiteral("%1 / %2").arg(formatClock(position), formatClock(row.durationMs));
}

int liveProgressPermille(const PlayerRow &row)
{
    using namespace WinEase::FeaturePlugins::Player;
    if (row.positionMs < 0 || row.durationMs <= 0) {
        return 0;
    }
    const qint64 position = projectedPositionMs(row.positionMs,
                                               row.updatedAtMs,
                                               QDateTime::currentMSecsSinceEpoch(),
                                               row.playing,
                                               row.durationMs);
    const int percent = progressPercent(position, row.durationMs);
    return (percent < 0) ? 0 : percent * (kSeekSliderMaximum / 100);
}

} // namespace

MediaPlayerPanelPlugin::MediaPlayerPanelPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

MediaPlayerPanelPlugin::~MediaPlayerPanelPlugin() = default;

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString MediaPlayerPanelPlugin::id() const
{
    return QStringLiteral("media.player_panel");
}

QString MediaPlayerPanelPlugin::name() const
{
    return QStringLiteral("媒体控制面板");
}

QString MediaPlayerPanelPlugin::description() const
{
    return QStringLiteral("播放/暂停、上一首/下一首、进度与封面；"
                          "「控制选中的会话」与「系统媒体键」分成两组，不会混着用");
}

QIcon MediaPlayerPanelPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Media);
}

WinEase::FeatureCategory MediaPlayerPanelPlugin::category() const
{
    return WinEase::FeatureCategory::Media;
}

QString MediaPlayerPanelPlugin::version() const
{
    return QStringLiteral("1.0.0");
}

QString MediaPlayerPanelPlugin::author() const
{
    return QStringLiteral("WinEase");
}

QString MediaPlayerPanelPlugin::detailedDescription() const
{
    return QStringLiteral(
        "读的是 Windows 的「系统媒体会话」（SMTC）——就是按音量键时弹出的那个媒体浮出控件里的条目。\n"
        "\n"
        "能做到：\n"
        "· 列出当前所有注册了媒体会话的播放器（Chrome / Spotify / 系统播放器 / 各类 UWP 播放器…），"
        "读出曲目、艺术家、专辑与封面；\n"
        "· 对**选中的那一个会话**精确控制：播放/暂停、上一首/下一首、停止、跳到指定进度；\n"
        "· 另有一组「发送媒体键」，等价于按键盘上的媒体键 —— 由系统决定交给哪个会话，"
        "**不一定是列表里选中的那个**（两组分开摆就是不想让人误会）。\n"
        "\n"
        "做不到 / 会照实说的地方：\n"
        "· 没有注册媒体会话的播放器（很老的本地播放器）看不到、也控不了；\n"
        "· 「系统声音」这类只有音频流、没有媒体会话的东西不在这里（那是音量混合器的事）；\n"
        "· 有些网页播放器不给时长、不给封面、不给曲目名 —— 面板会写清是「播放器没提供」"
        "还是「读取失败」，绝不拿 0:00 / 0% 冒充读数；\n"
        "· 最后能不能生效由播放器决定：面板的按钮只是把请求交给它，"
        "被拒绝时会明说「该操作没有被播放器接受」。\n"
        "\n"
        "停用本功能**不会**改变任何播放状态（不会替你暂停，也不会替你继续放）。");
}

QStringList MediaPlayerPanelPlugin::tags() const
{
    return { QStringLiteral("媒体控制"),   QStringLiteral("播放器"), QStringLiteral("SMTC"),
             QStringLiteral("播放暂停"),   QStringLiteral("上一首"), QStringLiteral("下一首"),
             QStringLiteral("进度"),       QStringLiteral("封面"),   QStringLiteral("mtkz"),
             QStringLiteral("bfzt"),       QStringLiteral("fm") };
}

// ---------------------------------------------------------------------------
//  能力标记
// ---------------------------------------------------------------------------

bool MediaPlayerPanelPlugin::requiresAdmin() const
{
    return false;
}

bool MediaPlayerPanelPlugin::supportsHotkey() const
{
    // 面板型功能：快捷键的价值全在"不用打开面板"，而本功能本来就是看着面板操作的
    return false;
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool MediaPlayerPanelPlugin::initialize()
{
    // 只在 initialize() 读一次配置，之后改配置走面板控件
    if (WinEase::PluginServices *svc = services()) {
        m_autoRefresh = svc->configValue(id(), QStringLiteral("autoRefresh"), true).toBool();
        m_playingOnly = svc->configValue(id(), QStringLiteral("playingOnly"), false).toBool();
        m_pollSeconds = svc->configValue(id(), QStringLiteral("pollSeconds"), kDefaultPollSeconds).toInt();
        m_pollSeconds = std::clamp(m_pollSeconds, kMinPollSeconds, kMaxPollSeconds);
    }

    if (m_pollTimer == nullptr) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(m_pollSeconds * 1000);
        connect(m_pollTimer, &QTimer::timeout, this, [this] { pollOnce(); });
    }
    if (m_tickTimer == nullptr) {
        // 进度条要看起来"在走"：轮询 2 秒一次太粗，这里只重算**纯函数外推**，不碰系统
        m_tickTimer = new QTimer(this);
        m_tickTimer->setInterval(500);
        connect(m_tickTimer, &QTimer::timeout, this, [this] {
            if (isEnabled()) {
                updateProgressView();
            }
        });
    }

    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("媒体控制面板已就绪"));
    return true;
}

void MediaPlayerPanelPlugin::shutdown()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    if (m_tickTimer != nullptr) {
        m_tickTimer->stop();
    }
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("autoRefresh"), m_autoRefresh);
        svc->setConfigValue(id(), QStringLiteral("playingOnly"), m_playingOnly);
        svc->setConfigValue(id(), QStringLiteral("pollSeconds"), m_pollSeconds);
        svc->syncConfig();
    }
    // ⚠ 不碰任何播放状态：退出时保持现状（纪律 1）
}

bool MediaPlayerPanelPlugin::canEnable(QString *reason) const
{
    if (!WinEase::Win32::isThreadComReady()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("当前线程尚未初始化 COM，无法读取系统媒体会话");
        }
        return false;
    }
    return true;
}

bool MediaPlayerPanelPlugin::onEnable()
{
    refreshSessions(false);

    // ⚠ 这里不能用 isEnabled() 做判断：setEnabled(true) 内部才调用 onEnable，
    //   此刻 m_enabled 还是旧值（踩坑 #28/#51/#57）
    if (m_autoRefresh && m_pollTimer != nullptr) {
        m_pollTimer->start();
    }
    if (m_tickTimer != nullptr) {
        m_tickTimer->start();
    }

    // 面板重画排到事件循环下一轮：那时 isEnabled() 才是真值，按钮启用状态才正确
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    return true;
}

void MediaPlayerPanelPlugin::onDisable()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    if (m_tickTimer != nullptr) {
        m_tickTimer->stop();
    }
    m_seeking = false;

    // ⚠ 不碰任何播放状态（纪律 1）。这里只清掉"本功能的运行态"
    Q_EMIT statusMessage(QStringLiteral("已停用（播放状态保持不变）"));

    // 同样排到事件循环下一轮：onDisable 里 isEnabled() 仍是旧的 true（踩坑 #28）
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *MediaPlayerPanelPlugin::createSettingsWidget(QWidget *parent)
{
    m_panel = new QWidget(parent);
    m_panel->setObjectName(QStringLiteral("playerPanel"));
    auto *layout = new QVBoxLayout(m_panel);

    m_statusLabel = new QLabel(m_panel);
    m_statusLabel->setObjectName(QStringLiteral("playerStatusLabel"));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_sessionList = new QListWidget(m_panel);
    m_sessionList->setObjectName(QStringLiteral("playerSessionList"));
    m_sessionList->setMinimumHeight(120);
    m_sessionList->setAlternatingRowColors(true);
    layout->addWidget(m_sessionList, 1);

    // ---- 选中会话的详情：封面 + 曲目 + 进度 ----
    auto *detailLayout = new QHBoxLayout();
    m_artworkLabel = new QLabel(m_panel);
    m_artworkLabel->setObjectName(QStringLiteral("playerArtworkLabel"));
    m_artworkLabel->setFixedSize(96, 96);
    m_artworkLabel->setAlignment(Qt::AlignCenter);
    m_artworkLabel->setFrameShape(QFrame::StyledPanel);
    m_artworkLabel->setWordWrap(true);
    detailLayout->addWidget(m_artworkLabel);

    auto *textLayout = new QVBoxLayout();
    m_titleLabel = new QLabel(m_panel);
    m_titleLabel->setObjectName(QStringLiteral("playerTitleLabel"));
    m_titleLabel->setWordWrap(true);
    textLayout->addWidget(m_titleLabel);

    m_artistLabel = new QLabel(m_panel);
    m_artistLabel->setObjectName(QStringLiteral("playerArtistLabel"));
    m_artistLabel->setWordWrap(true);
    textLayout->addWidget(m_artistLabel);

    m_progressSlider = new QSlider(Qt::Horizontal, m_panel);
    m_progressSlider->setObjectName(QStringLiteral("playerProgressSlider"));
    m_progressSlider->setRange(0, kSeekSliderMaximum);
    m_progressSlider->setSingleStep(10);
    m_progressSlider->setPageStep(50);
    m_progressSlider->setToolTip(QStringLiteral("拖动到某处后松手 = 跳到该进度（该播放器是否接受由它决定）"));
    textLayout->addWidget(m_progressSlider);

    m_progressLabel = new QLabel(m_panel);
    m_progressLabel->setObjectName(QStringLiteral("playerProgressLabel"));
    m_progressLabel->setWordWrap(true);
    textLayout->addWidget(m_progressLabel);
    textLayout->addStretch(1);
    detailLayout->addLayout(textLayout, 1);
    layout->addLayout(detailLayout);

    // ---- 第一组：控制**选中的会话** ----
    auto *commandLabel = new QLabel(QStringLiteral("控制选中的会话（精确作用于列表里选中的那一个）"), m_panel);
    commandLabel->setObjectName(QStringLiteral("playerCommandGroupLabel"));
    layout->addWidget(commandLabel);

    auto *commandLayout = new QHBoxLayout();
    m_previousButton = new QPushButton(QStringLiteral("上一首"), m_panel);
    m_previousButton->setObjectName(QStringLiteral("playerPreviousButton"));
    commandLayout->addWidget(m_previousButton);
    m_toggleButton = new QPushButton(QStringLiteral("播放/暂停"), m_panel);
    m_toggleButton->setObjectName(QStringLiteral("playerToggleButton"));
    commandLayout->addWidget(m_toggleButton);
    m_nextButton = new QPushButton(QStringLiteral("下一首"), m_panel);
    m_nextButton->setObjectName(QStringLiteral("playerNextButton"));
    commandLayout->addWidget(m_nextButton);
    m_stopButton = new QPushButton(QStringLiteral("停止"), m_panel);
    m_stopButton->setObjectName(QStringLiteral("playerStopButton"));
    commandLayout->addWidget(m_stopButton);
    commandLayout->addStretch(1);
    layout->addLayout(commandLayout);

    // ---- 第二组：发系统媒体键（打给"系统当前会话"，与上面那一组**不是**一回事）----
    auto *keyLabel = new QLabel(QStringLiteral("发送媒体键（等价于按键盘上的媒体键，由系统挑一个会话）"), m_panel);
    keyLabel->setObjectName(QStringLiteral("playerSendKeyGroupLabel"));
    layout->addWidget(keyLabel);

    auto *keyLayout = new QHBoxLayout();
    m_sendPlayPauseButton = new QPushButton(QStringLiteral("播放/暂停键"), m_panel);
    m_sendPlayPauseButton->setObjectName(QStringLiteral("playerSendPlayPauseButton"));
    keyLayout->addWidget(m_sendPlayPauseButton);
    m_sendPrevButton = new QPushButton(QStringLiteral("上一首键"), m_panel);
    m_sendPrevButton->setObjectName(QStringLiteral("playerSendPrevButton"));
    keyLayout->addWidget(m_sendPrevButton);
    m_sendNextButton = new QPushButton(QStringLiteral("下一首键"), m_panel);
    m_sendNextButton->setObjectName(QStringLiteral("playerSendNextButton"));
    keyLayout->addWidget(m_sendNextButton);
    keyLayout->addStretch(1);
    layout->addLayout(keyLayout);

    // ---- 刷新与选项 ----
    auto *optionLayout = new QHBoxLayout();
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), m_panel);
    m_refreshButton->setObjectName(QStringLiteral("playerRefreshButton"));
    optionLayout->addWidget(m_refreshButton);
    m_autoRefreshCheck = new QCheckBox(QStringLiteral("自动刷新"), m_panel);
    m_autoRefreshCheck->setObjectName(QStringLiteral("playerAutoRefreshCheck"));
    m_autoRefreshCheck->setChecked(m_autoRefresh);
    optionLayout->addWidget(m_autoRefreshCheck);
    m_playingOnlyCheck = new QCheckBox(QStringLiteral("只看正在播放"), m_panel);
    m_playingOnlyCheck->setObjectName(QStringLiteral("playerPlayingOnlyCheck"));
    m_playingOnlyCheck->setChecked(m_playingOnly);
    optionLayout->addWidget(m_playingOnlyCheck);
    optionLayout->addStretch(1);
    layout->addLayout(optionLayout);

    m_detailLabel = new QLabel(m_panel);
    m_detailLabel->setObjectName(QStringLiteral("playerDetailLabel"));
    m_detailLabel->setWordWrap(true);
    m_detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_detailLabel);

    auto *hint = new QLabel(
        QStringLiteral(
            "· 这里列的是系统「媒体会话」（按音量键弹出的那个浮出控件里的条目），"
            "没有注册会话的播放器看不到、也控不了\n"
            "· 上面的四个按钮只作用于**列表里选中的会话**；下面的媒体键由系统自己挑一个会话 —— "
            "想精确控制某个播放器就用上面那组\n"
            "· 有些播放器不提供时长或封面：面板会写清是「播放器没提供」还是「读取失败」，"
            "不会拿 0:00 / 0% 冒充读数\n"
            "· 停用本功能不会改变任何播放状态"),
        m_panel);
    hint->setObjectName(QStringLiteral("playerHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    // ---- 连接（放在设完初值之后，避免程序化赋值触发回调）----
    connect(m_sessionList, &QListWidget::currentRowChanged, this, [this](int) {
        if (m_syncingUi || m_sessionList.isNull()) {
            return;
        }
        const QListWidgetItem *item = m_sessionList->currentItem();
        m_selectedKey = (item != nullptr) ? item->data(kKeyRole).toString() : QString();
        updateDetail();
        updateProgressView();
        updateStatusLabel();
    });
    connect(m_progressSlider, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this] {
        m_seeking = false;
        applySeek(m_progressSlider->value());
    });
    connect(m_progressSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_seeking && m_progressLabel != nullptr) {
            const PlayerRow row = currentRow();
            if (row.durationMs > 0) {
                m_progressLabel->setText(QStringLiteral("松手后跳到 %1")
                                             .arg(WinEase::FeaturePlugins::Player::formatClock(
                                                 row.durationMs * value / kSeekSliderMaximum)));
            }
        }
    });
    connect(m_toggleButton, &QPushButton::clicked, this, [this] {
        applyCommand(MediaCommand::TogglePlayPause, QStringLiteral("播放/暂停"));
    });
    connect(m_previousButton, &QPushButton::clicked, this, [this] {
        applyCommand(MediaCommand::Previous, QStringLiteral("上一首"));
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this] {
        applyCommand(MediaCommand::Next, QStringLiteral("下一首"));
    });
    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        applyCommand(MediaCommand::Stop, QStringLiteral("停止"));
    });
    connect(m_sendPlayPauseButton, &QPushButton::clicked, this, [this] {
        applyMediaKey(WinEase::Win32::MediaKey::PlayPause);
    });
    connect(m_sendPrevButton, &QPushButton::clicked, this, [this] {
        applyMediaKey(WinEase::Win32::MediaKey::PreviousTrack);
    });
    connect(m_sendNextButton, &QPushButton::clicked, this, [this] {
        applyMediaKey(WinEase::Win32::MediaKey::NextTrack);
    });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refreshPanel(); });
    connect(m_autoRefreshCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_autoRefresh = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("autoRefresh"), checked);
        }
        if (m_pollTimer != nullptr) {
            if (checked && isEnabled()) {
                m_pollTimer->start();
            } else {
                m_pollTimer->stop();
            }
        }
        publishStatus(checked ? QStringLiteral("自动刷新已打开")
                              : QStringLiteral("自动刷新已关闭（列表不会自己更新，点「刷新」）"));
    });
    connect(m_playingOnlyCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_playingOnly = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("playingOnly"), checked);
        }
        refreshPanel();
    });

    refreshPanel();
    return m_panel;
}

// ---------------------------------------------------------------------------
//  轮询与刷新（**只读**：这里绝不发控制命令）
// ---------------------------------------------------------------------------

void MediaPlayerPanelPlugin::pollOnce()
{
    if (!isEnabled()) {
        return;
    }
    refreshSessions(false);
    rebuildList();
    restoreSelection();
    updateDetail();
    updateProgressView();
    updateStatusLabel();
}

void MediaPlayerPanelPlugin::refreshSessions(bool fromUser)
{
    Q_UNUSED(fromUser);

    QString error;
    const QList<MediaSessionInfo> sessions = WinEase::Win32::mediaSessions(&error);
    m_enumError = error;

    if (!error.isEmpty()) {
        // 枚举失败：如实说，并且**保留**上一次的行，但状态里写清"这是上一次的结果"
        publishStatus(QStringLiteral("读不到媒体会话列表：%1（下面显示的是上一次的结果）").arg(error));
        return;
    }

    QList<PlayerSessionInput> inputs;
    inputs.reserve(sessions.size());
    for (const MediaSessionInfo &session : sessions) {
        inputs.append(toInput(session));
    }

    PlayerOptions options;
    options.playingOnly = m_playingOnly;
    m_snapshot = WinEase::FeaturePlugins::Player::buildSnapshot(inputs, options);

    // 选中的会话消失了：清掉选中，别让命令打到一个已经不存在的会话上
    bool selectedStillThere = false;
    for (const PlayerRow &row : m_snapshot.rows) {
        if (row.key == m_selectedKey) {
            selectedStillThere = true;
            break;
        }
    }
    if (!m_selectedKey.isEmpty() && !selectedStillThere) {
        publishStatus(QStringLiteral("之前选中的会话「%1」已消失，请重新选择").arg(m_selectedKey));
        m_selectedKey.clear();
        m_artworkKey.clear();
    }
}

void MediaPlayerPanelPlugin::rebuildList()
{
    if (m_sessionList == nullptr) {
        return;
    }

    // 列表重建期间要挡住 currentRowChanged：否则边清边选会触发一串"选中变化"回调
    const QSignalBlocker blocker(m_sessionList);
    m_sessionList->clear();

    PlayerOptions options;
    options.playingOnly = m_playingOnly;

    if (m_snapshot.rows.isEmpty()) {
        auto *item = new QListWidgetItem(WinEase::FeaturePlugins::Player::emptyListText(m_snapshot, options),
                                        m_sessionList);
        item->setFlags(Qt::NoItemFlags);
        item->setToolTip(QStringLiteral("系统里没有媒体会话时，任何控制按钮都没有作用对象"));
        return;
    }

    for (const PlayerRow &row : m_snapshot.rows) {
        QString text = row.title;
        if (!row.subtitle.isEmpty()) {
            text += QStringLiteral(" · ") + row.subtitle;
        }
        if (row.current) {
            text = QStringLiteral("▶ ") + text;
        }
        auto *item = new QListWidgetItem(QStringLiteral("%1　[%2]").arg(text, row.statusText),
                                        m_sessionList);
        item->setData(kKeyRole, row.key);
        item->setToolTip(row.tooltip);
    }
}

void MediaPlayerPanelPlugin::restoreSelection()
{
    if (m_sessionList == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_sessionList);

    int target = -1;
    for (int i = 0; i < m_sessionList->count(); ++i) {
        const QListWidgetItem *item = m_sessionList->item(i);
        const QString key = item->data(kKeyRole).toString();
        if (key.isEmpty()) {
            continue;
        }
        if (key == m_selectedKey) {
            target = i;
            break;
        }
    }
    // 没有选中过就默认选第一行：用户打开面板的下一步动作几乎总是"控制它"
    if (target < 0 && m_selectedKey.isEmpty()) {
        for (int i = 0; i < m_sessionList->count(); ++i) {
            if (!m_sessionList->item(i)->data(kKeyRole).toString().isEmpty()) {
                target = i;
                break;
            }
        }
    }
    if (target >= 0) {
        m_sessionList->setCurrentRow(target);
        m_selectedKey = m_sessionList->item(target)->data(kKeyRole).toString();
    } else {
        m_sessionList->setCurrentRow(-1);
    }
}

void MediaPlayerPanelPlugin::refreshPanel()
{
    // 选项勾选框：成员是唯一真相，控件只是它的显示（程序化赋值必须挡住信号）
    if (m_autoRefreshCheck != nullptr) {
        const QSignalBlocker blocker(m_autoRefreshCheck);
        m_autoRefreshCheck->setChecked(m_autoRefresh);
    }
    if (m_playingOnlyCheck != nullptr) {
        const QSignalBlocker blocker(m_playingOnlyCheck);
        m_playingOnlyCheck->setChecked(m_playingOnly);
    }

    m_syncingUi = true;
    refreshSessions(false);
    rebuildList();
    restoreSelection();
    m_syncingUi = false;

    updateDetail();
    updateProgressView();
    updateStatusLabel();

    const bool enabled = isEnabled();
    if (m_toggleButton != nullptr) {
        m_toggleButton->setEnabled(enabled);
    }
    if (m_previousButton != nullptr) {
        m_previousButton->setEnabled(enabled);
    }
    if (m_nextButton != nullptr) {
        m_nextButton->setEnabled(enabled);
    }
    if (m_stopButton != nullptr) {
        m_stopButton->setEnabled(enabled);
    }
    if (m_sendPlayPauseButton != nullptr) {
        m_sendPlayPauseButton->setEnabled(enabled);
    }
    if (m_sendPrevButton != nullptr) {
        m_sendPrevButton->setEnabled(enabled);
    }
    if (m_sendNextButton != nullptr) {
        m_sendNextButton->setEnabled(enabled);
    }
    if (m_refreshButton != nullptr) {
        // 刷新是只读的，停用后也可以点（否则用户看不到"现在有没有会话"）
        m_refreshButton->setEnabled(true);
    }
}

void MediaPlayerPanelPlugin::updateDetail()
{
    const PlayerRow row = currentRow();
    const bool hasRow = !row.key.isEmpty();

    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(hasRow ? row.title : QStringLiteral("（未选择会话）"));
    }
    if (m_artistLabel != nullptr) {
        m_artistLabel->setText(hasRow ? row.subtitle : QString());
    }
    if (m_detailLabel != nullptr) {
        if (!hasRow) {
            m_detailLabel->setText(WinEase::FeaturePlugins::Player::emptyListText(
                m_snapshot,
                PlayerOptions{ m_playingOnly }));
        } else {
            QStringList lines;
            lines << QStringLiteral("应用：%1").arg(row.appText);
            lines << QStringLiteral("进度：%1").arg(row.progressText);
            lines << QStringLiteral("定位串：%1").arg(row.key);
            if (m_snapshot.unreadableCount > 0) {
                lines << m_snapshot.warnings.join(QStringLiteral("；"));
            }
            if (!m_lastAction.isEmpty()) {
                lines << QStringLiteral("最近一次动作：%1").arg(m_lastAction);
            }
            m_detailLabel->setText(lines.join(QLatin1Char('\n')));
        }
    }

    // 按钮跟着"播放器自己声明的能力"走，而不是我们自己猜
    const bool enabled = isEnabled();
    if (m_toggleButton != nullptr) {
        m_toggleButton->setEnabled(enabled && hasRow && (row.canPlay || row.canPause));
    }
    if (m_previousButton != nullptr) {
        m_previousButton->setEnabled(enabled && hasRow && row.canPrevious);
    }
    if (m_nextButton != nullptr) {
        m_nextButton->setEnabled(enabled && hasRow && row.canNext);
    }
    if (m_stopButton != nullptr) {
        m_stopButton->setEnabled(enabled && hasRow && (row.canPlay || row.canPause));
    }

    loadArtwork(row.key);
}

void MediaPlayerPanelPlugin::updateProgressView()
{
    const PlayerRow row = currentRow();
    const bool hasRow = !row.key.isEmpty();
    const bool hasDuration = hasRow && (row.durationMs > 0) && (row.positionMs >= 0);

    if (m_progressLabel != nullptr) {
        m_progressLabel->setText(hasRow ? liveProgressText(row) : QString());
    }

    if (m_progressSlider == nullptr || m_seeking) {
        return;
    }
    const QSignalBlocker blocker(m_progressSlider);
    m_progressSlider->setEnabled(isEnabled() && hasDuration);
    m_progressSlider->setValue(hasDuration ? liveProgressPermille(row) : 0);
}

void MediaPlayerPanelPlugin::updateStatusLabel()
{
    if (m_statusLabel == nullptr) {
        return;
    }
    const QString summary = WinEase::FeaturePlugins::Player::statusLine(m_snapshot);
    if (!isEnabled()) {
        // 未启用时明确写出"已停用"，而不是留一行旧数据让人以为还在跑
        m_statusLabel->setText(QStringLiteral("已停用（播放状态保持不变） · %1").arg(summary));
        return;
    }
    m_statusLabel->setText(m_statusLine.isEmpty()
                               ? summary
                               : QStringLiteral("%1 · %2").arg(m_statusLine, summary));
}

// ---------------------------------------------------------------------------
//  动作（只有用户点了按钮才会走到这里）
// ---------------------------------------------------------------------------

void MediaPlayerPanelPlugin::applyCommand(MediaCommand command, const QString &actionName)
{
    const QString key = selectedKey();
    if (key.isEmpty()) {
        publishStatus(QStringLiteral("请先在列表里选择一个会话"));
        return;
    }

    QString error;
    if (!WinEase::Win32::controlMediaSession(key, command, &error)) {
        reportFailure(error);
        publishStatus(QStringLiteral("控制未生效：%1").arg(error));
        refreshSessions(false);
        updateStatusLabel();
        return;
    }

    m_lastAction = QStringLiteral("%1（对选中的会话）").arg(actionName);
    publishStatus(QStringLiteral("已对「%1」执行：%2 —— 等它自己把状态报回来")
                      .arg(currentRow().appText, actionName));
    scheduleFollowUpRefresh();
}

void MediaPlayerPanelPlugin::applyMediaKey(WinEase::Win32::MediaKey key)
{
    QString error;
    if (!WinEase::Win32::sendMediaKey(key, &error)) {
        reportFailure(error);
        publishStatus(QStringLiteral("媒体键没有发出去：%1").arg(error));
        return;
    }

    const QString currentId = WinEase::Win32::currentMediaSessionId();
    m_lastAction = QStringLiteral("媒体键 %1（打给系统当前会话）")
                       .arg(WinEase::Win32::mediaKeyName(key));
    if (currentId.isEmpty()) {
        // 如实说：键发出去了，但系统当前没有会话接它
        publishStatus(QStringLiteral("已发送媒体键：%1 —— 系统当前没有会话接收（键被系统收下了）")
                          .arg(WinEase::Win32::mediaKeyName(key)));
    } else {
        publishStatus(QStringLiteral("已发送媒体键：%1 —— 系统交给的会话是「%2」")
                          .arg(WinEase::Win32::mediaKeyName(key), currentId));
    }
    scheduleFollowUpRefresh();
}

void MediaPlayerPanelPlugin::applySeek(int permille)
{
    const PlayerRow row = currentRow();
    if (row.key.isEmpty()) {
        publishStatus(QStringLiteral("请先在列表里选择一个会话"));
        return;
    }
    if (row.durationMs <= 0) {
        publishStatus(QStringLiteral("该播放器没有提供时长，无法跳转"));
        return;
    }

    const qint64 target = row.durationMs * permille / kSeekSliderMaximum;
    QString error;
    if (!WinEase::Win32::seekMediaSession(row.key, target, &error)) {
        reportFailure(error);
        publishStatus(QStringLiteral("跳转没有生效：%1").arg(error));
        return;
    }
    m_lastAction = QStringLiteral("跳到 %1").arg(WinEase::FeaturePlugins::Player::formatClock(target));
    publishStatus(QStringLiteral("已请求跳到 %1（该播放器是否接受由它决定）")
                      .arg(WinEase::FeaturePlugins::Player::formatClock(target)));
    scheduleFollowUpRefresh();
}

void MediaPlayerPanelPlugin::scheduleFollowUpRefresh()
{
    // 播放状态是**播放器**改的，不是我们改的：给它一点时间再回读，
    // 否则面板会显示"按了没反应"（其实只是还没轮到我们读）
    QTimer::singleShot(300, this, [this] {
        if (!isEnabled()) {
            return;
        }
        pollOnce();
    });
}

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

WinEase::FeaturePlugins::Player::PlayerRow MediaPlayerPanelPlugin::currentRow() const
{
    for (const PlayerRow &row : m_snapshot.rows) {
        if (row.key == m_selectedKey) {
            return row;
        }
    }
    return PlayerRow();
}

QString MediaPlayerPanelPlugin::selectedKey() const
{
    if (!m_selectedKey.isEmpty()) {
        return m_selectedKey;
    }
    // 面板上没有任何选中（例如列表刚重建）时，退回"当前列表的第一行"
    if (!m_snapshot.rows.isEmpty()) {
        return m_snapshot.rows.first().key;
    }
    return QString();
}

void MediaPlayerPanelPlugin::loadArtwork(const QString &sessionKey)
{
    if (m_artworkLabel == nullptr) {
        return;
    }
    if (sessionKey.isEmpty()) {
        m_artworkKey.clear();
        m_artworkLabel->setPixmap(QPixmap());
        m_artworkLabel->setText(QStringLiteral("（未选择会话）"));
        return;
    }
    if (sessionKey == m_artworkKey) {
        return;
    }
    m_artworkKey = sessionKey;

    QString error;
    const QImage artwork = WinEase::Win32::mediaSessionArtwork(sessionKey, &error);
    m_artworkLabel->setPixmap(QPixmap());
    if (!artwork.isNull()) {
        m_artworkLabel->setText(QString());
        m_artworkLabel->setPixmap(QPixmap::fromImage(artwork).scaled(m_artworkLabel->size(),
                                                                     Qt::KeepAspectRatio,
                                                                     Qt::SmoothTransformation));
        return;
    }
    if (!error.isEmpty()) {
        // 读失败：写原因，不许留一个空白方块让人以为是"没有封面"
        m_artworkLabel->setText(QStringLiteral("封面读不到：%1").arg(error));
        return;
    }
    m_artworkLabel->setText(QStringLiteral("该播放器没有提供封面"));
}

void MediaPlayerPanelPlugin::publishStatus(const QString &text)
{
    m_statusLine = text;
    updateStatusLabel();
    Q_EMIT statusMessage(text);
}

void MediaPlayerPanelPlugin::reportFailure(const QString &text)
{
    setLastError(text);
    logMessage(WinEase::PluginLogLevel::Warning, text);
    Q_EMIT errorOccurred(text);
}
