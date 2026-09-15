#pragma once

// ============================================================================
//  PlayerModel —— 媒体控制面板的纯函数层（插件与 feature_smoke 编译同一份）
//
//  这里只做三件事：把平台层读到的会话整理成"面板要显示的行"、算进度、
//  以及在**读不到**的时候说清楚读不到什么。
//
//  纪律（自检钉的就是这几条）：
//      1. **读不到就写原因**：位置读不到不许显示 0:00，时长缺失不许显示 0%；
//         「没提供曲目名」和「读曲目失败」是两件事，文案必须分开。
//      2. **进度只在时长已知时才有百分比**：很多直播/网页播放器就是不给 EndTime。
//      3. 面板显示"这一刻"的位置靠**外推**（采样时刻 + 已播时间），
//         外推是纯函数，能被自检直接钉住（不许倒着走、不许超过时长）。
// ============================================================================

#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::Player {

/// 平台层读到的原始会话（调用方从 `WinEase::Win32::MediaSessionInfo` 填过来）
struct PlayerSessionInput {
    QString sessionId;
    QString appName;
    quint32 pid = 0;
    bool current = false;

    bool playing = false;   ///< 播放状态是不是"正在播放"（外推用）
    QString playbackText;   ///< 播放状态的中文描述；空 = 读不到
    QString playbackError;  ///< 读不到播放状态的原因

    QString title;
    QString artist;
    QString albumTitle;
    QString propertiesError; ///< 读曲目失败的原因（空 = 读到了，播放器可能确实没给名字）

    qint64 positionMs = -1;  ///< <0 = 读不到
    qint64 durationMs = -1;  ///< <=0 = 播放器没给时长（不能当成 0）
    qint64 updatedAtMs = -1; ///< 位置采样时刻（Unix 毫秒）；<=0 = 未知
    QString timelineError;   ///< 读不到进度的原因

    bool canPlay = false;
    bool canPause = false;
    bool canNext = false;
    bool canPrevious = false;
};

/// 一行"可控制的播放会话"
struct PlayerRow {
    QString key;      ///< 定位串（= sessionId），面板把它挂在列表项的 UserRole 上
    QString title;    ///< 曲目名；播放器没给就写清"没给"，失败就写失败原因
    QString subtitle; ///< 艺术家 · 专辑；读不到就写原因
    QString appText;  ///< "chrome.exe（PID 1234）"
    QString statusText;
    QString progressText;
    int progressPercent = -1; ///< <0 = 进度未知（界面必须显示"未知"，不许画 0%）

    qint64 positionMs = -1;
    qint64 durationMs = -1;
    qint64 updatedAtMs = -1; ///< 位置采样时刻（面板按它外推"这一刻"的位置）
    bool hasTimeline = false;
    bool current = false;
    bool playing = false;
    bool canPlay = false;
    bool canPause = false;
    bool canNext = false;
    bool canPrevious = false;
    QString tooltip;
};

struct PlayerOptions {
    /// 只看正在播放的会话（默认关：暂停中的会话也是要能控制的）
    bool playingOnly = false;
};

struct PlayerSnapshot {
    QList<PlayerRow> rows;
    int hiddenNotPlaying = 0; ///< 被"只看正在播放"藏起来的行数
    int unreadableCount = 0;  ///< 状态或进度读不到的会话数（面板要如实说）
    QString currentTitle;     ///< 系统"当前会话"（媒体键的作用对象）的描述；空 = 没有
    QStringList warnings;     ///< 需要如实告诉用户的事
};

PlayerSnapshot buildSnapshot(const QList<PlayerSessionInput> &sessions,
                             const PlayerOptions &options);

/// 把毫秒转成 "1:23" / "1:02:03"；**负数（= 读不到）返回空串**
/// （`0` 是合法位置，返回 "0:00" —— "读不到"由调用方用原因文案表达，不能靠 0 冒充）
QString formatClock(qint64 ms);

/// 这一刻的播放位置（毫秒）。未知返回 -1；
/// 正在播放时按"采样时刻 + 已过去的真实时间"外推，并夹在 [0, 时长] 内
qint64 projectedPositionMs(qint64 positionMs,
                           qint64 updatedAtMs,
                           qint64 nowMs,
                           bool playing,
                           qint64 durationMs);

/// 进度百分比（0~100）；位置或时长任一未知返回 -1
int progressPercent(qint64 positionMs, qint64 durationMs);

/// 面板顶部那一行（"当前：chrome.exe — 歌曲名（正在播放）"/"系统里没有媒体会话"）
QString statusLine(const PlayerSnapshot &snapshot);

/// 列表为空时的说明文案（要说清"是没会话"还是"被筛选藏起来了"）
QString emptyListText(const PlayerSnapshot &snapshot, const PlayerOptions &options);

/// 会话行的 tooltip（界面上截断的话在这里说全）
QString rowTooltip(const PlayerRow &row);

/// 读不到某个字段时的统一文案："<字段>读不到（<原因>）"
QString unavailableText(const QString &field, const QString &error);

} // namespace WinEase::FeaturePlugins::Player
