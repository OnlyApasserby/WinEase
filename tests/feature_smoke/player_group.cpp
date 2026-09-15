#include "player_group.h"

#include "PlayerModel.h"

#include "app/core/PluginManager.h"
#include "sdk/FeatureCategory.h"
#include "sdk/IFeaturePlugin.h"
#include "win32/MediaSessions.h"

#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <QWidget>

namespace FeatureSmoke {
namespace {

using WinEase::FeaturePlugins::Player::PlayerOptions;
using WinEase::FeaturePlugins::Player::PlayerRow;
using WinEase::FeaturePlugins::Player::PlayerSessionInput;
using WinEase::FeaturePlugins::Player::PlayerSnapshot;
using WinEase::Win32::MediaCommand;
using WinEase::Win32::MediaPlaybackState;
using WinEase::Win32::MediaSessionInfo;

const QString kPluginId = QStringLiteral("media.player_panel");
constexpr int kKeyRole = Qt::UserRole;

// ---------------------------------------------------------------------------
//  平台层回读的小工具
// ---------------------------------------------------------------------------

/// 系统里现在有没有"探针造的那一份会话"；找到就填进 out
bool probeSessionIn(MediaSessionInfo *out, QString *errorOut)
{
    QString error;
    const QList<MediaSessionInfo> sessions = WinEase::Win32::mediaSessions(&error);
    if (errorOut != nullptr) {
        *errorOut = error;
    }
    if (!error.isEmpty()) {
        return false;
    }
    for (const MediaSessionInfo &info : sessions) {
        const bool sameSource =
            info.sourceAppId.compare(MediaSessionProbe::appUserModelId(), Qt::CaseInsensitive) == 0;
        if (sameSource || info.title == MediaSessionProbe::titleText()) {
            if (out != nullptr) {
                *out = info;
            }
            return true;
        }
    }
    return false;
}

QString playbackTextOf(const MediaSessionInfo &info)
{
    return WinEase::Win32::mediaPlaybackStateText(info.playback);
}

/// 等某个会话的播放状态稳定为期望值（连续稳定，不是"某一次采样命中"）
bool waitForPlayback(const QString &sessionId, MediaPlaybackState expected, int timeoutMs = 5000)
{
    MediaSessionInfo last;
    return waitForStable(
        [&] {
            MediaSessionInfo info;
            if (!probeSessionIn(&info, nullptr) || info.sessionId != sessionId) {
                return false;
            }
            last = info;
            return info.playback == expected;
        },
        timeoutMs,
        3,
        80);
}

int rowIndexOf(const QListWidget *list, const QString &key)
{
    if (list == nullptr || key.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(kKeyRole).toString() == key) {
            return i;
        }
    }
    return -1;
}

QStringList panelRowKeys(const QListWidget *list)
{
    QStringList keys;
    if (list == nullptr) {
        return keys;
    }
    for (int i = 0; i < list->count(); ++i) {
        const QString key = list->item(i)->data(kKeyRole).toString();
        if (!key.isEmpty()) {
            keys << key;
        }
    }
    return keys;
}

// ---------------------------------------------------------------------------
//  面板控件抓取（插件不导出符号，自检按 objectName 定位）
// ---------------------------------------------------------------------------

struct PlayerPanel {
    QWidget *panel = nullptr;
    QLabel *status = nullptr;
    QListWidget *list = nullptr;
    QLabel *title = nullptr;
    QLabel *artist = nullptr;
    QLabel *artwork = nullptr;
    QLabel *progressLabel = nullptr;
    QSlider *progressSlider = nullptr;
    QPushButton *previous = nullptr;
    QPushButton *toggle = nullptr;
    QPushButton *next = nullptr;
    QPushButton *stop = nullptr;
    QPushButton *sendPlayPause = nullptr;
    QPushButton *sendPrev = nullptr;
    QPushButton *sendNext = nullptr;
    QPushButton *refresh = nullptr;
    QCheckBox *autoRefresh = nullptr;
    QCheckBox *playingOnly = nullptr;
    QLabel *detail = nullptr;
    QLabel *sendKeyGroupLabel = nullptr;

    QStringList missing() const
    {
        QStringList absent;
        if (status == nullptr) {
            absent << QStringLiteral("playerStatusLabel");
        }
        if (list == nullptr) {
            absent << QStringLiteral("playerSessionList");
        }
        if (title == nullptr) {
            absent << QStringLiteral("playerTitleLabel");
        }
        if (artist == nullptr) {
            absent << QStringLiteral("playerArtistLabel");
        }
        if (artwork == nullptr) {
            absent << QStringLiteral("playerArtworkLabel");
        }
        if (progressLabel == nullptr) {
            absent << QStringLiteral("playerProgressLabel");
        }
        if (progressSlider == nullptr) {
            absent << QStringLiteral("playerProgressSlider");
        }
        if (previous == nullptr) {
            absent << QStringLiteral("playerPreviousButton");
        }
        if (toggle == nullptr) {
            absent << QStringLiteral("playerToggleButton");
        }
        if (next == nullptr) {
            absent << QStringLiteral("playerNextButton");
        }
        if (stop == nullptr) {
            absent << QStringLiteral("playerStopButton");
        }
        if (sendPlayPause == nullptr) {
            absent << QStringLiteral("playerSendPlayPauseButton");
        }
        if (sendPrev == nullptr) {
            absent << QStringLiteral("playerSendPrevButton");
        }
        if (sendNext == nullptr) {
            absent << QStringLiteral("playerSendNextButton");
        }
        if (refresh == nullptr) {
            absent << QStringLiteral("playerRefreshButton");
        }
        if (autoRefresh == nullptr) {
            absent << QStringLiteral("playerAutoRefreshCheck");
        }
        if (playingOnly == nullptr) {
            absent << QStringLiteral("playerPlayingOnlyCheck");
        }
        if (detail == nullptr) {
            absent << QStringLiteral("playerDetailLabel");
        }
        if (sendKeyGroupLabel == nullptr) {
            absent << QStringLiteral("playerSendKeyGroupLabel");
        }
        return absent;
    }
};

PlayerPanel grabPanel(QWidget *panel)
{
    PlayerPanel ui;
    ui.panel = panel;
    if (panel == nullptr) {
        return ui;
    }
    ui.status = panel->findChild<QLabel *>(QStringLiteral("playerStatusLabel"));
    ui.list = panel->findChild<QListWidget *>(QStringLiteral("playerSessionList"));
    ui.title = panel->findChild<QLabel *>(QStringLiteral("playerTitleLabel"));
    ui.artist = panel->findChild<QLabel *>(QStringLiteral("playerArtistLabel"));
    ui.artwork = panel->findChild<QLabel *>(QStringLiteral("playerArtworkLabel"));
    ui.progressLabel = panel->findChild<QLabel *>(QStringLiteral("playerProgressLabel"));
    ui.progressSlider = panel->findChild<QSlider *>(QStringLiteral("playerProgressSlider"));
    ui.previous = panel->findChild<QPushButton *>(QStringLiteral("playerPreviousButton"));
    ui.toggle = panel->findChild<QPushButton *>(QStringLiteral("playerToggleButton"));
    ui.next = panel->findChild<QPushButton *>(QStringLiteral("playerNextButton"));
    ui.stop = panel->findChild<QPushButton *>(QStringLiteral("playerStopButton"));
    ui.sendPlayPause = panel->findChild<QPushButton *>(QStringLiteral("playerSendPlayPauseButton"));
    ui.sendPrev = panel->findChild<QPushButton *>(QStringLiteral("playerSendPrevButton"));
    ui.sendNext = panel->findChild<QPushButton *>(QStringLiteral("playerSendNextButton"));
    ui.refresh = panel->findChild<QPushButton *>(QStringLiteral("playerRefreshButton"));
    ui.autoRefresh = panel->findChild<QCheckBox *>(QStringLiteral("playerAutoRefreshCheck"));
    ui.playingOnly = panel->findChild<QCheckBox *>(QStringLiteral("playerPlayingOnlyCheck"));
    ui.detail = panel->findChild<QLabel *>(QStringLiteral("playerDetailLabel"));
    ui.sendKeyGroupLabel = panel->findChild<QLabel *>(QStringLiteral("playerSendKeyGroupLabel"));
    return ui;
}

// ---------------------------------------------------------------------------
//  纯函数层：一组固定的"读不到"输入 + 一组正常输入
// ---------------------------------------------------------------------------

PlayerSessionInput makeInput(const QString &id,
                             const QString &app,
                             const QString &playbackText,
                             bool playing)
{
    PlayerSessionInput input;
    input.sessionId = id;
    input.appName = app;
    input.playbackText = playbackText;
    input.playing = playing;
    return input;
}

QList<PlayerSessionInput> fixtureInputs(qint64 nowMs)
{
    // ① 正常在放：曲目/艺术家/专辑都有，时长已知，是系统当前会话
    PlayerSessionInput playing = makeInput(QStringLiteral("A"), QStringLiteral("chrome.exe"),
                                          QStringLiteral("正在播放"), true);
    playing.pid = 4321;
    playing.current = true;
    playing.title = QStringLiteral("一首歌");
    playing.artist = QStringLiteral("某歌手");
    playing.albumTitle = QStringLiteral("某专辑");
    playing.positionMs = 30000;
    playing.durationMs = 120000;
    playing.updatedAtMs = nowMs;
    playing.canPlay = true;
    playing.canPause = true;
    playing.canNext = true;
    playing.canPrevious = true;

    // ② 网页播放器常见：位置有、**时长就是不给**（不能显示成 0:00 / 0%）
    PlayerSessionInput live = makeInput(QStringLiteral("B"), QStringLiteral("msedge.exe"),
                                       QStringLiteral("正在播放"), true);
    live.title = QStringLiteral("直播曲目");
    live.positionMs = 5000;
    live.durationMs = -1;
    live.updatedAtMs = nowMs;

    // ③ 状态与进度都读不到（要说清"读不到 + 原因"）
    PlayerSessionInput unreadable = makeInput(QStringLiteral("C"), QStringLiteral("oldplayer.exe"),
                                             QString(), false);
    unreadable.playbackError = QStringLiteral("读取播放状态失败（错误码 5）");
    unreadable.timelineError = QStringLiteral("读取播放进度失败（错误码 5）");
    unreadable.positionMs = -1;
    unreadable.durationMs = -1;

    // ④ 暂停中的会话（要能控制，但排在"正在播放"后面）
    PlayerSessionInput paused = makeInput(QStringLiteral("D"), QStringLiteral("spotify.exe"),
                                         QStringLiteral("已暂停"), false);
    paused.title = QStringLiteral("另一首歌");
    paused.positionMs = 1000;
    paused.durationMs = 90000;
    paused.updatedAtMs = nowMs;

    return { playing, live, unreadable, paused };
}

int runPureModelTests(Reporter &reporter)
{
    using namespace WinEase::FeaturePlugins::Player;

    const int before = reporter.failures();
    reporter.info(QStringLiteral("---- 纯函数层：进度/时长读不到时必须说清「读不到」，不许拿 0 冒充 ----"));

    reporter.check(formatClock(0) == QStringLiteral("0:00"),
                   QStringLiteral("时间格式：0 毫秒是 0:00"),
                   formatClock(0));
    reporter.check(formatClock(83000) == QStringLiteral("1:23"),
                   QStringLiteral("时间格式：83 秒是 1:23"),
                   formatClock(83000));
    reporter.check(formatClock(3723000) == QStringLiteral("1:02:03"),
                   QStringLiteral("时间格式：超过一小时是 1:02:03"),
                   formatClock(3723000));
    reporter.check(formatClock(-1).isEmpty(),
                   QStringLiteral("★ 时间格式：负数（= 读不到）返回空串，**不是** 0:00"),
                   formatClock(-1));

    reporter.check(progressPercent(30000, 120000) == 25,
                   QStringLiteral("进度百分比：30 秒 / 2 分钟 = 25%"),
                   QString::number(progressPercent(30000, 120000)));
    reporter.check(progressPercent(0, 0) == -1,
                   QStringLiteral("★ 进度百分比：时长未知（0）= 没有百分比（-1），不是 0%"),
                   QString::number(progressPercent(0, 0)));
    reporter.check(progressPercent(1000, -1) == -1,
                   QStringLiteral("★ 进度百分比：位置未知 = -1"),
                   QString::number(progressPercent(1000, -1)));
    reporter.check(progressPercent(240000, 120000) == 100,
                   QStringLiteral("进度百分比：位置超出时长会被夹到 100%"),
                   QString::number(progressPercent(240000, 120000)));

    reporter.check(projectedPositionMs(10000, 1000, 4000, true, 60000) == 13000,
                   QStringLiteral("进度外推：采样在 10 秒、采样后又过了 3 秒 → 13 秒"),
                   QString::number(projectedPositionMs(10000, 1000, 4000, true, 60000)));
    reporter.check(projectedPositionMs(10000, 1000, 4000, false, 60000) == 10000,
                   QStringLiteral("进度外推：暂停时**不**往前推"),
                   QString::number(projectedPositionMs(10000, 1000, 4000, false, 60000)));
    reporter.check(projectedPositionMs(59000, 1000, 90000, true, 60000) == 60000,
                   QStringLiteral("进度外推：不会超过曲目时长"),
                   QString::number(projectedPositionMs(59000, 1000, 90000, true, 60000)));
    reporter.check(projectedPositionMs(-1, 1000, 4000, true, 60000) == -1,
                   QStringLiteral("进度外推：位置读不到就还是读不到（不编 0）"),
                   QString::number(projectedPositionMs(-1, 1000, 4000, true, 60000)));
    reporter.check(projectedPositionMs(10000, -1, 4000, true, 60000) == 10000,
                   QStringLiteral("进度外推：没有采样时刻就原样返回（不外推）"),
                   QString::number(projectedPositionMs(10000, -1, 4000, true, 60000)));

    // ---- 面板要显示的行 ----
    const qint64 now = 1700000000000LL;
    PlayerOptions options;
    const PlayerSnapshot snapshot = buildSnapshot(fixtureInputs(now), options);

    reporter.check(snapshot.rows.size() == 4,
                   QStringLiteral("快照：4 份会话 → 4 行"),
                   QString::number(snapshot.rows.size()));
    reporter.check(!snapshot.rows.isEmpty() && snapshot.rows.first().key == QStringLiteral("A"),
                   QStringLiteral("快照：系统「当前会话」排在第一行"),
                   snapshot.rows.isEmpty() ? QString() : snapshot.rows.first().key);
    reporter.check(snapshot.rows.first().current,
                   QStringLiteral("快照：第一行标着「当前会话」"));

    const auto findRow = [&snapshot](const QString &key) {
        for (const PlayerRow &row : snapshot.rows) {
            if (row.key == key) {
                return row;
            }
        }
        return PlayerRow();
    };

    const PlayerRow live = findRow(QStringLiteral("B"));
    reporter.check(live.progressPercent == -1,
                   QStringLiteral("★★ 时长未知的会话：百分比是「没有」（-1），不是 0%"),
                   QString::number(live.progressPercent));
    reporter.check(live.progressText.contains(QStringLiteral("0:05"))
                       && live.progressText.contains(QStringLiteral("时长未知")),
                   QStringLiteral("★★ 时长未知的会话：进度写「0:05 / 时长未知（该播放器没有提供）」"),
                   live.progressText);
    reporter.check(live.statusText == QStringLiteral("正在播放"),
                   QStringLiteral("快照：播放状态用系统给的中文描述"),
                   live.statusText);

    const PlayerRow unreadable = findRow(QStringLiteral("C"));
    reporter.check(unreadable.statusText.contains(QStringLiteral("读不到")),
                   QStringLiteral("★★ 状态读不到的行：写「读不到」而不是留空"),
                   unreadable.statusText);
    reporter.check(unreadable.statusText.contains(QStringLiteral("错误码 5")),
                   QStringLiteral("状态读不到的行：原因（错误码）也带给用户"),
                   unreadable.statusText);
    reporter.check(unreadable.progressText.contains(QStringLiteral("读不到"))
                       && unreadable.progressText.contains(QStringLiteral("错误码 5")),
                   QStringLiteral("★★ 进度读不到的行：连原因一起写"),
                   unreadable.progressText);
    reporter.check(snapshot.unreadableCount == 1,
                   QStringLiteral("快照：读不到的会话数被统计出来（面板要如实说）"),
                   QString::number(snapshot.unreadableCount));
    reporter.check(!snapshot.warnings.isEmpty()
                       && snapshot.warnings.join(QString()).contains(QStringLiteral("读不到")),
                   QStringLiteral("快照：把「有会话读不到」这条提示带给面板"),
                   snapshot.warnings.join(QStringLiteral("；")));

    const PlayerRow paused = findRow(QStringLiteral("D"));
    reporter.check(!paused.playing,
                   QStringLiteral("快照：暂停中的会话标着「不在播放」"));
    reporter.check(progressPercent(paused.positionMs, paused.durationMs) == 1,
                   QStringLiteral("进度百分比：1 秒 / 90 秒 ≈ 1%"),
                   QString::number(progressPercent(paused.positionMs, paused.durationMs)));

    PlayerOptions playingOnly;
    playingOnly.playingOnly = true;
    const PlayerSnapshot filtered = buildSnapshot(fixtureInputs(now), playingOnly);
    reporter.check(filtered.rows.size() == 2 && filtered.hiddenNotPlaying == 2,
                   QStringLiteral("★ 「只看正在播放」：留下 2 行、记下藏了 2 行"),
                   QStringLiteral("行 %1 / 藏 %2")
                       .arg(filtered.rows.size())
                       .arg(filtered.hiddenNotPlaying));

    PlayerSnapshot empty;
    reporter.check(emptyListText(empty, options).contains(QStringLiteral("一个媒体会话都没有")),
                   QStringLiteral("空列表文案：说清「系统里没有会话」，而不是留一块空白"),
                   emptyListText(empty, options));
    reporter.check(emptyListText(PlayerSnapshot(), playingOnly).contains(QStringLiteral("一个媒体会话都没有")),
                   QStringLiteral("空列表文案：真没有会话时不说「被筛选藏起来了」"));
    PlayerSnapshot hiddenOnly;
    hiddenOnly.hiddenNotPlaying = 2;
    reporter.check(emptyListText(hiddenOnly, playingOnly).contains(QStringLiteral("不在播放")),
                   QStringLiteral("空列表文案：被筛选藏起来时要说明原因"),
                   emptyListText(hiddenOnly, playingOnly));

    reporter.check(statusLine(snapshot).contains(QStringLiteral("共 4 个会话"))
                       && statusLine(snapshot).contains(QStringLiteral("当前")),
                   QStringLiteral("状态行：带会话数 + 当前会话"),
                   statusLine(snapshot));
    reporter.check(statusLine(empty).contains(QStringLiteral("没有可控制的会话")),
                   QStringLiteral("状态行：没有会话时如实说"),
                   statusLine(empty));

    reporter.check(unavailableText(QStringLiteral("播放进度"), QStringLiteral("系统没有给出进度"))
                       == QStringLiteral("播放进度读不到（系统没有给出进度）"),
                   QStringLiteral("读不到的文案格式统一：「<字段>读不到（<原因>）」"),
                   unavailableText(QStringLiteral("播放进度"), QStringLiteral("系统没有给出进度")));

    reporter.check(rowTooltip(findRow(QStringLiteral("A"))).contains(QStringLiteral("定位串")),
                   QStringLiteral("行 tooltip：把界面上截断的信息（含定位串）说全"));

    return reporter.failures() - before;
}

} // namespace

int runPlayerGroupTests(Reporter &reporter,
                        WinEase::PluginManager &manager,
                        StubServices &services)
{
    Q_UNUSED(services);

    const int before = reporter.failures();
    reporter.info(QStringLiteral("=== P3-11 媒体控制面板（media.player_panel）==="));

    runPureModelTests(reporter);

    // -----------------------------------------------------------------------
    //  1. 造一份**真实**的媒体会话（自检当播放器）
    // -----------------------------------------------------------------------
    reporter.info(QStringLiteral("---- 系统媒体会话：自检自己注册一份真实会话（SMTC 发布方）----"));

    MediaSessionProbe probe;
    QString probeError;
    const bool probeStarted = probe.start(&probeError);
    reporter.check(probeStarted,
                   QStringLiteral("前置：自检能自己造出一份真实的系统媒体会话"),
                   probeStarted ? QStringLiteral("子进程 PID %1 · AUMID %2")
                                      .arg(probe.pid())
                                      .arg(MediaSessionProbe::appUserModelId())
                                : probeError);
    if (!probeStarted) {
        reporter.info(QStringLiteral("前置不成立：会话与面板的断言全部跳过"
                                     "（这是**如实失败**，不是静默跳过）"));
        return reporter.failures() - before;
    }

    MediaSessionInfo probeSession;
    QString enumError;
    const bool visible = waitForStable([&] { return probeSessionIn(&probeSession, &enumError); },
                                       8000,
                                       3,
                                       100);
    reporter.check(visible,
                   QStringLiteral("★★ 系统媒体会话列表里能找到自检刚造的那一份"),
                   visible ? QStringLiteral("%1 · 标题「%2」").arg(probeSession.describe(), probeSession.title)
                           : QStringLiteral("枚举不到：%1").arg(enumError));
    reporter.check(probe.sessionId() == MediaSessionProbe::appUserModelId(),
                   QStringLiteral("会话定位串就是我们给它设的 AUMID（定位规则自证）"),
                   probe.sessionId());

    reporter.check(visible && probeSession.title == MediaSessionProbe::titleText(),
                   QStringLiteral("★★ 曲目名读回来与播放器报的一致"),
                   probeSession.title);
    reporter.check(visible && probeSession.artist == MediaSessionProbe::artistText(),
                   QStringLiteral("★★ 艺术家读回来一致"),
                   probeSession.artist);
    reporter.check(visible && probeSession.albumTitle == MediaSessionProbe::albumText(),
                   QStringLiteral("★ 专辑读回来一致"),
                   probeSession.albumTitle);
    reporter.check(visible && probeSession.playback == MediaPlaybackState::Playing,
                   QStringLiteral("★ 播放状态读回来是「正在播放」（发布方就是这么声明的）"),
                   playbackTextOf(probeSession));

    const bool durationOk = visible
                            && qAbs(probeSession.durationMs - MediaSessionProbe::durationMs()) <= 1000;
    reporter.check(durationOk,
                   QStringLiteral("★★ 时长读回来等于发布方声明的 3:00（tick → 毫秒 换算正确）"),
                   QStringLiteral("期望 %1 ms，读到 %2 ms")
                       .arg(MediaSessionProbe::durationMs())
                       .arg(probeSession.durationMs));
    reporter.check(visible && probeSession.positionMs >= 0 && probeSession.positionMs < 10000,
                   QStringLiteral("★ 位置读到的是真实位置（刚发布时接近 0）"),
                   QStringLiteral("%1 ms").arg(probeSession.positionMs));
    reporter.check(visible && probeSession.updatedAtMs > 0,
                   QStringLiteral("★ 位置带回了采样时刻（面板靠它外推「这一刻」的位置）"),
                   QStringLiteral("%1").arg(probeSession.updatedAtMs));

    QString artworkError;
    const QImage artwork = WinEase::Win32::mediaSessionArtwork(probe.sessionId(), &artworkError);
    const QColor centerColor = artwork.isNull()
                                   ? QColor()
                                   : artwork.pixelColor(artwork.width() / 2, artwork.height() / 2);
    reporter.check(!artwork.isNull() && centerColor == MediaSessionProbe::artworkColor(),
                   QStringLiteral("★★ 封面读回来了，颜色与发布方给的那张一致"),
                   artwork.isNull() ? QStringLiteral("空图：%1").arg(artworkError)
                                    : QStringLiteral("%1×%2，中心像素 %3（期望 %4）")
                                          .arg(artwork.width())
                                          .arg(artwork.height())
                                          .arg(centerColor.name())
                                          .arg(MediaSessionProbe::artworkColor().name()));

    const bool isCurrent = visible && probeSession.current;
    reporter.check(isCurrent,
                   QStringLiteral("前置：探针会话就是系统「当前会话」（媒体键必须打到它身上）"),
                   isCurrent ? QStringLiteral("是（%1）").arg(probeSession.sessionId)
                             : QStringLiteral("系统当前会话是别的 —— 可能有别的播放器正在播放"));

    // -----------------------------------------------------------------------
    //  2. 平台层控制链路：请求 → 播放器收到 → 状态/位置变了 → 我们回读
    // -----------------------------------------------------------------------
    reporter.info(QStringLiteral("---- 控制链路：发命令 → 播放器收到 → 回读真实状态 ----"));

    QString error;
    const int buttonBaseline = probe.receivedButtons().size();
    reporter.check(WinEase::Win32::controlMediaSession(probe.sessionId(), MediaCommand::Pause, &error),
                   QStringLiteral("★ 暂停请求被系统接受"),
                   error);
    reporter.check(waitForStable(
                       [&] {
                           const QStringList buttons = probe.receivedButtons();
                           return buttons.size() > buttonBaseline
                                  && buttons.last() == QStringLiteral("Pause");
                       },
                       5000,
                       3,
                       80),
                   QStringLiteral("★★ 播放器真的收到了暂停请求（子进程把收到的键记了下来）"),
                   probe.receivedButtons().join(QStringLiteral("，")));
    reporter.check(waitForPlayback(probe.sessionId(), MediaPlaybackState::Paused),
                   QStringLiteral("★★ 回读真实播放状态：已暂停（是播放器自己改的，不是我们猜的）"),
                   playbackTextOf(probeSession));

    reporter.check(WinEase::Win32::controlMediaSession(probe.sessionId(), MediaCommand::Play, &error),
                   QStringLiteral("★ 播放请求被系统接受"),
                   error);
    reporter.check(waitForPlayback(probe.sessionId(), MediaPlaybackState::Playing),
                   QStringLiteral("★ 回读真实播放状态：回到「正在播放」"));

    const qint64 seekTarget = MediaSessionProbe::durationMs() / 3;
    const int positionBaseline = probe.requestedPositions().size();
    reporter.check(WinEase::Win32::seekMediaSession(probe.sessionId(), seekTarget, &error),
                   QStringLiteral("★ 跳转请求被系统接受（跳到 1:00）"),
                   error);
    reporter.check(waitForStable(
                       [&] {
                           const QList<qint64> positions = probe.requestedPositions();
                           return positions.size() > positionBaseline
                                  && positions.last() == seekTarget;
                       },
                       5000,
                       3,
                       80),
                   QStringLiteral("★★ 播放器收到的跳转位置正是我们要求的那个（1:00）"),
                   QStringLiteral("收到：%1")
                       .arg(seekTarget));
    reporter.check(waitForStable(
                       [&] {
                           MediaSessionInfo info;
                           return probeSessionIn(&info, nullptr)
                                  && qAbs(info.positionMs - seekTarget) <= 1500;
                       },
                       5000,
                       3,
                       80),
                   QStringLiteral("★★ 回读真实位置：已经跳到 1:00"));

    QString missingError;
    const bool missingRejected =
        !WinEase::Win32::controlMediaSession(QStringLiteral("WinEase.Nonexistent.Session"),
                                             MediaCommand::Pause,
                                             &missingError);
    reporter.check(missingRejected,
                   QStringLiteral("★ 对不存在的会话下命令：如实失败（不假装成功）"),
                   missingError);
    reporter.check(missingRejected && missingError.contains(QStringLiteral("已消失")),
                   QStringLiteral("★ 失败原因说清了「会话已消失」"),
                   missingError);

    // -----------------------------------------------------------------------
    //  3. 真实插件 DLL：面板 → 动作 → 真实状态
    // -----------------------------------------------------------------------
    reporter.info(QStringLiteral("---- 插件与面板：用户点按钮这条链路 ----"));

    WinEase::IFeaturePlugin *plugin = manager.plugin(kPluginId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("插件：media.player_panel 已由真实 DLL 加载"),
                   kPluginId);
    if (plugin == nullptr) {
        probe.stop();
        return reporter.failures() - before;
    }

    reporter.check(plugin->name() == QStringLiteral("媒体控制面板"),
                   QStringLiteral("插件：卡片名称"),
                   plugin->name());
    reporter.check(plugin->category() == WinEase::FeatureCategory::Media,
                   QStringLiteral("插件：分类是「媒体」"));
    reporter.check(!plugin->requiresAdmin(),
                   QStringLiteral("插件：不需要管理员权限"));
    reporter.check(!plugin->supportsHotkey(),
                   QStringLiteral("插件：面板型，不注册全局快捷键"));
    reporter.check(!plugin->detailedDescription().isEmpty(),
                   QStringLiteral("插件：有给用户看的完整说明（帮助页里说清做不到什么）"));
    reporter.check(plugin->tags().contains(QStringLiteral("SMTC")),
                   QStringLiteral("插件：搜索标签里含 SMTC（找得到）"),
                   plugin->tags().join(QStringLiteral("，")));

    StatusLog log;
    log.attach(plugin);
    reporter.check(manager.setPluginEnabled(kPluginId, true),
                   QStringLiteral("插件：能启用"),
                   manager.pluginFailureReason(kPluginId));
    settleEvents(200);

    QWidget *panelWidget = manager.createSettingsWidget(kPluginId);
    reporter.check(panelWidget != nullptr, QStringLiteral("插件：能拿到设置面板"));
    const PlayerPanel ui = grabPanel(panelWidget);
    reporter.check(ui.missing().isEmpty(),
                   QStringLiteral("面板：关键控件齐全（自检按 objectName 定位）"),
                   ui.missing().isEmpty() ? QStringLiteral("全部到位")
                                          : QStringLiteral("缺：%1").arg(ui.missing().join(QStringLiteral("，"))));

    // 轮询会自己发现新会话（这里没有点过「刷新」）
    const bool listed = waitForStable([&] { return rowIndexOf(ui.list, probe.sessionId()) >= 0; },
                                      8000,
                                      3,
                                      100);
    reporter.check(listed,
                   QStringLiteral("★★ 面板列表里出现了探针会话（自动轮询发现的，没点刷新）"),
                   ui.list == nullptr ? QString() : panelRowKeys(ui.list).join(QStringLiteral("，")));
    reporter.check(ui.list != nullptr
                       && ui.list->item(rowIndexOf(ui.list, probe.sessionId()))->text().contains(
                           MediaSessionProbe::titleText()),
                   QStringLiteral("★★ 那一行的文字里就是播放器报的曲目名（不是我们编的）"),
                   ui.list == nullptr || rowIndexOf(ui.list, probe.sessionId()) < 0
                       ? QString()
                       : ui.list->item(rowIndexOf(ui.list, probe.sessionId()))->text());
    reporter.check(waitForStable(
                       [&] {
                           return ui.list != nullptr
                                  && ui.list->count() == WinEase::Win32::mediaSessions().size();
                       },
                       3000,
                       3,
                       80),
                   QStringLiteral("★ 面板列出的行数 == 系统会话数（不藏行、不多行）"));

    reporter.check(ui.title != nullptr && ui.title->text() == MediaSessionProbe::titleText(),
                   QStringLiteral("面板：详情区显示的是选中会话的曲目"),
                   ui.title == nullptr ? QString() : ui.title->text());
    reporter.check(ui.artist != nullptr && ui.artist->text().contains(MediaSessionProbe::artistText()),
                   QStringLiteral("面板：详情区显示艺术家（专辑跟在一起）"),
                   ui.artist == nullptr ? QString() : ui.artist->text());
    reporter.check(ui.progressLabel != nullptr
                       && ui.progressLabel->text().contains(QStringLiteral("3:00")),
                   QStringLiteral("面板：进度文案里是真实时长 3:00（tick 换算到界面这一层也对）"),
                   ui.progressLabel == nullptr ? QString() : ui.progressLabel->text());
    reporter.check(ui.progressSlider != nullptr && ui.progressSlider->isEnabled(),
                   QStringLiteral("面板：时长已知时进度条可用（说明「能跳转」）"));
    const QImage panelArtwork =
        (ui.artwork != nullptr && !ui.artwork->pixmap().isNull())
            ? ui.artwork->pixmap().toImage()
            : QImage();
    reporter.check(!panelArtwork.isNull()
                       && panelArtwork.pixelColor(panelArtwork.width() / 2,
                                                  panelArtwork.height() / 2)
                              == MediaSessionProbe::artworkColor(),
                   QStringLiteral("★★ 面板封面区显示的是真实封面（像素颜色对得上）"),
                   panelArtwork.isNull() ? QStringLiteral("封面区是空的")
                                         : QStringLiteral("%1×%2")
                                               .arg(panelArtwork.width())
                                               .arg(panelArtwork.height()));
    reporter.check(ui.sendKeyGroupLabel != nullptr
                       && ui.sendKeyGroupLabel->text().contains(QStringLiteral("系统")),
                   QStringLiteral("★ 面板上「发送媒体键」那一组写清了它打给「系统当前会话」"
                                  "（不跟「控制选中的会话」混着说）"),
                   ui.sendKeyGroupLabel == nullptr ? QString() : ui.sendKeyGroupLabel->text());

    // ---- 点「播放/暂停」：当前是播放中 → 应当打成暂停 ----
    const int buttonBaseline2 = probe.receivedButtons().size();
    log.clear();
    if (ui.toggle != nullptr) {
        ui.toggle->click();
    }
    settleEvents(150);
    reporter.check(waitForStable(
                       [&] {
                           const QStringList buttons = probe.receivedButtons();
                           return buttons.size() > buttonBaseline2
                                  && buttons.last() == QStringLiteral("Pause");
                       },
                       5000,
                       3,
                       80),
                   QStringLiteral("★★ 点面板「播放/暂停」→ 播放器收到暂停（用户点按钮的完整链路）"),
                   probe.receivedButtons().join(QStringLiteral("，")));
    reporter.check(waitForPlayback(probe.sessionId(), MediaPlaybackState::Paused),
                   QStringLiteral("★★ 面板点了按钮之后，回读到的真实状态是「已暂停」"));
    reporter.check(log.contains(QStringLiteral("播放/暂停")),
                   QStringLiteral("面板：状态提示写清了刚做了什么"),
                   log.last());

    // ---- 点「下一首」：播放器侧记录到 Next ----
    const int buttonBaseline3 = probe.receivedButtons().size();
    if (ui.next != nullptr) {
        ui.next->click();
    }
    settleEvents(150);
    reporter.check(waitForStable(
                       [&] {
                           const QStringList buttons = probe.receivedButtons();
                           return buttons.size() > buttonBaseline3
                                  && buttons.last() == QStringLiteral("Next");
                       },
                       5000,
                       3,
                       80),
                   QStringLiteral("★★ 点面板「下一首」→ 播放器收到下一首"),
                   probe.receivedButtons().join(QStringLiteral("，")));

    // ---- 媒体键：由**系统**路由到当前会话 ----
    if (isCurrent) {
        log.clear();
        const int buttonBaseline4 = probe.receivedButtons().size();
        if (ui.sendPlayPause != nullptr) {
            ui.sendPlayPause->click();
        }
        settleEvents(150);
        reporter.check(waitForStable(
                           [&] { return probe.receivedButtons().size() > buttonBaseline4; },
                           6000,
                           3,
                           100),
                       QStringLiteral("★★ 点「发送媒体键」→ 系统把媒体键路由到了当前会话"
                                      "（探针收到按键事件）"),
                       QStringLiteral("收到：%1")
                           .arg(probe.receivedButtons().join(QStringLiteral("，"))));
        reporter.check(log.contains(QStringLiteral("媒体键")),
                       QStringLiteral("★ 面板：状态提示写的是「媒体键」，不是「控制选中的会话」"),
                       log.last());
    } else {
        reporter.check(false,
                       QStringLiteral("前置不成立：探针不是当前会话，媒体键无法回读"
                                      "（这是如实失败，不是跳过）"));
    }

    // ---- 纪律：停用不改变播放状态 ----
    reporter.info(QStringLiteral("---- 纪律：停用插件不该动用户的播放状态 ----"));
    reporter.check(WinEase::Win32::controlMediaSession(probe.sessionId(), MediaCommand::Play, &error),
                   QStringLiteral("准备：先把探针恢复成「正在播放」"),
                   error);
    reporter.check(waitForPlayback(probe.sessionId(), MediaPlaybackState::Playing),
                   QStringLiteral("准备：探针确认在播放"));

    log.clear();
    reporter.check(manager.setPluginEnabled(kPluginId, false),
                   QStringLiteral("插件：能停用"));
    settleEvents(300);
    reporter.check(waitForStable(
                       [&] {
                           MediaSessionInfo info;
                           return probeSessionIn(&info, nullptr)
                                  && info.playback == MediaPlaybackState::Playing;
                       },
                       1500,
                       3,
                       200),
                   QStringLiteral("★★ 停用插件后播放状态**没有**被改动（没替你暂停，也没替你继续）"));
    reporter.check(log.contains(QStringLiteral("已停用")),
                   QStringLiteral("★ 停用时会明确告诉用户「播放状态保持不变」"),
                   log.last());

    // -----------------------------------------------------------------------
    //  4. 会话消失之后：面板必须说清"之前的会话没了"
    // -----------------------------------------------------------------------
    reporter.info(QStringLiteral("---- 会话消失：不许拿旧数据当真 ----"));
    reporter.check(manager.setPluginEnabled(kPluginId, true),
                   QStringLiteral("插件：能重新启用"));
    settleEvents(200);

    probe.stop();
    settleEvents(300);
    if (ui.refresh != nullptr) {
        ui.refresh->click();
    }
    settleEvents(300);
    reporter.check(waitForStable(
                       [&] {
                           return ui.status != nullptr
                                  && ui.status->text().contains(QStringLiteral("已消失"));
                       },
                       5000,
                       3,
                       100),
                   QStringLiteral("★ 选中的会话消失后，面板如实说明（而不是继续拿旧数据当真）"),
                   ui.status == nullptr ? QString() : ui.status->text());
    reporter.check(!panelRowKeys(ui.list).contains(probe.sessionId()),
                   QStringLiteral("★ 会话消失后，列表里不再留下那一行（没有幽灵行）"),
                   panelRowKeys(ui.list).join(QStringLiteral("，")));

    manager.setPluginEnabled(kPluginId, false);
    return reporter.failures() - before;
}

} // namespace FeatureSmoke
