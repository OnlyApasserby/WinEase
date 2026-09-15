#include "PlayerModel.h"

#include <algorithm>
#include <cmath>

namespace WinEase::FeaturePlugins::Player {

namespace {

constexpr qint64 kMsPerSecond = 1000;
constexpr qint64 kMsPerMinute = 60 * kMsPerSecond;
constexpr qint64 kMsPerHour = 60 * kMsPerMinute;

QString titleOf(const PlayerSessionInput &session)
{
    if (!session.propertiesError.isEmpty()) {
        return unavailableText(QStringLiteral("曲目名"), session.propertiesError);
    }
    if (session.title.trimmed().isEmpty()) {
        return QStringLiteral("（该播放器没有提供曲目名）");
    }
    return session.title;
}

QString subtitleOf(const PlayerSessionInput &session)
{
    if (!session.propertiesError.isEmpty()) {
        // 标题那一行已经把失败原因说清楚了，这里不再重复
        return QString();
    }
    QStringList parts;
    if (!session.artist.trimmed().isEmpty()) {
        parts << session.artist;
    }
    if (!session.albumTitle.trimmed().isEmpty()) {
        parts << session.albumTitle;
    }
    if (parts.isEmpty()) {
        return QStringLiteral("（该播放器没有提供艺术家/专辑）");
    }
    return parts.join(QStringLiteral(" · "));
}

QString statusOf(const PlayerSessionInput &session)
{
    if (!session.playbackError.isEmpty()) {
        return unavailableText(QStringLiteral("播放状态"), session.playbackError);
    }
    if (session.playbackText.isEmpty()) {
        return unavailableText(QStringLiteral("播放状态"), QStringLiteral("系统没有给出状态"));
    }
    return session.playbackText;
}

QString progressOf(const PlayerSessionInput &session)
{
    if (session.positionMs < 0) {
        const QString reason = session.timelineError.isEmpty()
                                   ? QStringLiteral("系统没有给出进度")
                                   : session.timelineError;
        return unavailableText(QStringLiteral("播放进度"), reason);
    }
    const QString position = formatClock(session.positionMs);
    if (session.durationMs <= 0) {
        // 直播 / 网页播放器常见：位置有，时长就是不给 —— 不能说成 0:00
        return QStringLiteral("%1 / 时长未知（该播放器没有提供）").arg(position);
    }
    return QStringLiteral("%1 / %2").arg(position, formatClock(session.durationMs));
}

QString capabilityText(const PlayerSessionInput &session)
{
    QStringList parts;
    parts << (session.canPlay ? QStringLiteral("可播放") : QStringLiteral("不可播放"));
    parts << (session.canPause ? QStringLiteral("可暂停") : QStringLiteral("不可暂停"));
    parts << (session.canPrevious ? QStringLiteral("可上一首") : QStringLiteral("不可上一首"));
    parts << (session.canNext ? QStringLiteral("可下一首") : QStringLiteral("不可下一首"));
    return parts.join(QStringLiteral(" / "));
}

PlayerRow buildRow(const PlayerSessionInput &session)
{
    PlayerRow row;
    row.key = session.sessionId;
    row.title = titleOf(session);
    row.subtitle = subtitleOf(session);
    row.appText = (session.pid != 0)
                      ? QStringLiteral("%1（PID %2）").arg(session.appName).arg(session.pid)
                      : session.appName;
    row.statusText = statusOf(session);
    row.progressText = progressOf(session);
    row.progressPercent = progressPercent(session.positionMs, session.durationMs);
    row.positionMs = session.positionMs;
    row.durationMs = session.durationMs;
    row.updatedAtMs = session.updatedAtMs;
    row.hasTimeline = (session.positionMs >= 0);
    row.current = session.current;
    row.playing = session.playing;
    row.canPlay = session.canPlay;
    row.canPause = session.canPause;
    row.canNext = session.canNext;
    row.canPrevious = session.canPrevious;
    row.tooltip = rowTooltip(row);
    return row;
}

} // namespace

PlayerSnapshot buildSnapshot(const QList<PlayerSessionInput> &sessions, const PlayerOptions &options)
{
    PlayerSnapshot snapshot;

    for (const PlayerSessionInput &session : sessions) {
        if (options.playingOnly && !session.playing) {
            ++snapshot.hiddenNotPlaying;
            continue;
        }
        PlayerRow row = buildRow(session);
        if (!session.playbackError.isEmpty() || session.positionMs < 0) {
            ++snapshot.unreadableCount;
        }
        if (row.current && snapshot.currentTitle.isEmpty()) {
            snapshot.currentTitle = QStringLiteral("%1 — %2（%3）")
                                        .arg(row.appText, row.title, row.statusText);
        }
        snapshot.rows.append(row);
    }

    // 排序：当前会话置顶 → 正在播放 → 应用名 → 曲目名
    std::stable_sort(snapshot.rows.begin(),
                     snapshot.rows.end(),
                     [](const PlayerRow &left, const PlayerRow &right) {
                         if (left.current != right.current) {
                             return left.current;
                         }
                         if (left.playing != right.playing) {
                             return left.playing;
                         }
                         const int byApp = QString::compare(left.appText,
                                                            right.appText,
                                                            Qt::CaseInsensitive);
                         if (byApp != 0) {
                             return byApp < 0;
                         }
                         const int byTitle = QString::compare(left.title,
                                                              right.title,
                                                              Qt::CaseInsensitive);
                         if (byTitle != 0) {
                             return byTitle < 0;
                         }
                         return QString::compare(left.key, right.key) < 0;
                     });

    if (snapshot.unreadableCount > 0) {
        snapshot.warnings << QStringLiteral("有 %1 个会话的状态或进度读不到 —— "
                                            "面板里写的是原因，不是猜出来的数字")
                                 .arg(snapshot.unreadableCount);
    }
    if (snapshot.hiddenNotPlaying > 0 && !options.playingOnly) {
        snapshot.warnings << QStringLiteral("有 %1 个会话当前不在播放（它们仍在列表里）")
                                 .arg(snapshot.hiddenNotPlaying);
    }

    return snapshot;
}

QString formatClock(qint64 ms)
{
    if (ms < 0) {
        return QString();
    }
    const qint64 hours = ms / kMsPerHour;
    const qint64 minutes = (ms % kMsPerHour) / kMsPerMinute;
    const qint64 seconds = (ms % kMsPerMinute) / kMsPerSecond;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

qint64 projectedPositionMs(qint64 positionMs,
                           qint64 updatedAtMs,
                           qint64 nowMs,
                           bool playing,
                           qint64 durationMs)
{
    if (positionMs < 0) {
        return -1;
    }
    if (!playing || updatedAtMs <= 0 || nowMs <= updatedAtMs) {
        return positionMs;
    }
    qint64 projected = positionMs + (nowMs - updatedAtMs);
    if (projected < 0) {
        projected = 0;
    }
    if (durationMs > 0 && projected > durationMs) {
        projected = durationMs;
    }
    return projected;
}

int progressPercent(qint64 positionMs, qint64 durationMs)
{
    if (positionMs < 0 || durationMs <= 0) {
        return -1;
    }
    const double ratio = static_cast<double>(positionMs) / static_cast<double>(durationMs);
    const int percent = static_cast<int>(std::lround(ratio * 100.0));
    return std::clamp(percent, 0, 100);
}

QString statusLine(const PlayerSnapshot &snapshot)
{
    if (snapshot.rows.isEmpty()) {
        return QStringLiteral("面板里没有可控制的会话");
    }
    const QString count = QStringLiteral("共 %1 个会话").arg(snapshot.rows.size());
    if (!snapshot.currentTitle.isEmpty()) {
        return QStringLiteral("当前：%1 · %2").arg(snapshot.currentTitle, count);
    }
    return QStringLiteral("系统还没有指定「当前会话」（媒体键由系统自己挑一个） · %1").arg(count);
}

QString emptyListText(const PlayerSnapshot &snapshot, const PlayerOptions &options)
{
    if (options.playingOnly && snapshot.hiddenNotPlaying > 0) {
        return QStringLiteral("列表为空：有 %1 个会话当前不在播放（取消勾选「只看正在播放」可以看到它们）")
            .arg(snapshot.hiddenNotPlaying);
    }
    return QStringLiteral("系统里现在一个媒体会话都没有 —— "
                          "先用播放器（浏览器放音频也算）播一下，再点「刷新」");
}

QString rowTooltip(const PlayerRow &row)
{
    QStringList lines;
    lines << QStringLiteral("应用：%1%2").arg(row.appText, row.current ? QStringLiteral("（当前会话）") : QString());
    lines << QStringLiteral("曲目：%1").arg(row.title);
    if (!row.subtitle.isEmpty()) {
        lines << QStringLiteral("艺术家/专辑：%1").arg(row.subtitle);
    }
    lines << QStringLiteral("状态：%1").arg(row.statusText);
    lines << QStringLiteral("进度：%1").arg(row.progressText);
    QStringList capabilities;
    capabilities << (row.canPlay ? QStringLiteral("可播放") : QStringLiteral("不可播放"));
    capabilities << (row.canPause ? QStringLiteral("可暂停") : QStringLiteral("不可暂停"));
    capabilities << (row.canPrevious ? QStringLiteral("可上一首") : QStringLiteral("不可上一首"));
    capabilities << (row.canNext ? QStringLiteral("可下一首") : QStringLiteral("不可下一首"));
    lines << QStringLiteral("播放器声明的能力：%1").arg(capabilities.join(QStringLiteral(" / ")));
    lines << QStringLiteral("定位串：%1").arg(row.key);
    return lines.join(QLatin1Char('\n'));
}

QString unavailableText(const QString &field, const QString &error)
{
    if (error.isEmpty()) {
        return QStringLiteral("%1读不到").arg(field);
    }
    return QStringLiteral("%1读不到（%2）").arg(field, error);
}

} // namespace WinEase::FeaturePlugins::Player
