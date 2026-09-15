#include "MixerModel.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace WinEase::FeaturePlugins::Mixer {

namespace {

QString rowTitle(const MixerSessionInput &session)
{
    if (session.systemSounds) {
        return QStringLiteral("系统声音");
    }
    if (!session.processName.isEmpty()) {
        return session.processName;
    }
    if (!session.displayName.isEmpty()) {
        return session.displayName;
    }
    return QStringLiteral("未知进程（PID %1）").arg(session.pid);
}

MixerRowKind kindOf(const MixerSessionInput &session)
{
    if (session.systemSounds) {
        return MixerRowKind::SystemSounds;
    }
    if (session.processName.isEmpty()) {
        return MixerRowKind::Unknown;
    }
    return MixerRowKind::Application;
}

/// 聚合键：同一进程的多份会话合并到一行 —— 这就是"按应用调音量"的落点
QString groupKey(const MixerSessionInput &session)
{
    if (session.systemSounds) {
        return QStringLiteral("sys");
    }
    if (!session.processName.isEmpty()) {
        return QStringLiteral("p:") + session.processName.trimmed().toLower();
    }
    return QStringLiteral("pid:%1").arg(session.pid);
}

/// 逐会话键：关闭"按应用合并"时一行一份会话
QString instanceKey(const MixerSessionInput &session)
{
    if (!session.instanceId.isEmpty()) {
        return QStringLiteral("i:") + session.instanceId;
    }
    return QStringLiteral("pid:%1").arg(session.pid);
}

QString muteText(bool muteValid, bool muted, bool mixed)
{
    if (!muteValid) {
        return QStringLiteral("静音状态读不到");
    }
    if (mixed) {
        return QStringLiteral("部分会话已静音");
    }
    return muted ? QStringLiteral("已静音") : QStringLiteral("未静音");
}

/// 聚合过程中的临时状态
struct Accumulator {
    MixerRow row;
    bool anyVolume = false;
    double maxVolume = -1.0;
    QString firstVolumeError;
    bool anyMuteKnown = false;
    bool allMuted = true;
    bool anyMuted = false;
    QString firstMuteError;
    int expiredCount = 0;
    int unreadable = 0;
};

void accumulate(Accumulator &acc, const MixerSessionInput &session, bool wantInstanceId)
{
    // ⚠ 从 MixerRow 的默认值 0 起步累加（踩坑 #74：默认写成 1 会让每个应用
    //   都多报一份会话，并让 row.expired 永远算不出来）
    ++acc.row.sessionCount;

    // ⚠ 只有可操作的会话才把实例 id 收进来：过期会话的实例 id 调不动，
    //   混进 instanceIds 会让"按应用调音量"必然失败几条
    if (wantInstanceId) {
        acc.row.instanceIds.append(session.instanceId);
    }
    if (session.expired) {
        ++acc.expiredCount;
    }
    if (session.active) {
        acc.row.active = true;
    }
    if (session.pid != 0 && acc.row.pid == 0) {
        acc.row.pid = session.pid;
    }
    if (acc.row.executablePath.isEmpty() && !session.executablePath.isEmpty()) {
        acc.row.executablePath = session.executablePath;
    }

    // ---- 音量：代表值取**最响**的一份 ----
    // 为什么是 max 而不是 min：用户拖滑块是想把"听到的"调小，而听到的是最响的那一份；
    // 取 min 会长在"看着已经很小、实际还很响"的状态上。
    if (session.volume >= 0.0) {
        if (!acc.anyVolume || session.volume > acc.maxVolume) {
            acc.maxVolume = session.volume;
        }
        acc.anyVolume = true;
    } else {
        ++acc.unreadable;
        if (acc.firstVolumeError.isEmpty()) {
            acc.firstVolumeError = session.volumeError;
        }
    }

    // ---- 静音：只有**读得到**的会话参与判定 ----
    if (session.muteValid) {
        acc.anyMuteKnown = true;
        acc.allMuted = acc.allMuted && session.muted;
        acc.anyMuted = acc.anyMuted || session.muted;
    } else if (acc.firstMuteError.isEmpty()) {
        acc.firstMuteError = session.muteError;
    }

    // ---- 逐会话明细（进 tooltip） ----
    const QString volumeText = session.volume >= 0.0
                                   ? formatVolume(session.volume)
                                   : unavailableVolumeText(session.volumeError);
    QString detail = QStringLiteral("· %1 · %2 · %3")
                         .arg(session.stateText.isEmpty() ? QStringLiteral("状态未知")
                                                          : session.stateText,
                              volumeText,
                              muteText(session.muteValid, session.muted, false));
    if (!session.executablePath.isEmpty()) {
        detail += QStringLiteral(" · %1").arg(session.executablePath);
    }
    acc.row.sessionDetails.append(detail);
}

QString buildSubtitle(const MixerRow &row)
{
    QStringList parts;
    if (row.kind != MixerRowKind::SystemSounds && row.pid != 0) {
        parts << QStringLiteral("PID %1").arg(row.pid);
    }
    if (row.sessionCount > 1) {
        parts << QStringLiteral("%1 个会话").arg(row.sessionCount);
    }
    parts << (row.volume >= 0.0 ? formatVolume(row.volume)
                                : unavailableVolumeText(row.volumeError));
    if (row.muteValid && row.muted) {
        parts << QStringLiteral("已静音");
    } else if (row.muteMixed) {
        parts << QStringLiteral("部分会话已静音");
    }
    if (row.expired) {
        parts << QStringLiteral("已过期");
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace

// ============================================================================
//  组装
// ============================================================================

MixerSnapshot buildSnapshot(const QList<MixerSessionInput> &sessions,
                            const MixerOptions &options)
{
    MixerSnapshot snapshot;

    QHash<QString, int> indexOfKey;
    QList<Accumulator> accumulators;

    for (const MixerSessionInput &session : sessions) {
        if (session.expired && !options.showExpired) {
            ++snapshot.hiddenExpired;
            continue;
        }

        const bool adjustable = !session.expired && !session.instanceId.isEmpty();
        const QString key = options.groupByProcess ? groupKey(session) : instanceKey(session);

        const auto found = indexOfKey.constFind(key);
        if (found == indexOfKey.constEnd()) {
            Accumulator acc;
            acc.row.key = key;
            acc.row.kind = kindOf(session);
            acc.row.title = rowTitle(session);
            accumulate(acc, session, adjustable);
            indexOfKey.insert(key, accumulators.size());
            accumulators.append(acc);
            continue;
        }
        accumulate(accumulators[found.value()], session, adjustable);
    }

    snapshot.rows.reserve(accumulators.size());
    for (Accumulator &acc : accumulators) {
        MixerRow &row = acc.row;
        row.volume = acc.anyVolume ? acc.maxVolume : -1.0;
        row.volumeError = acc.anyVolume ? QString() : acc.firstVolumeError;
        row.muteValid = acc.anyMuteKnown;
        row.muted = acc.anyMuteKnown && acc.allMuted;
        row.muteMixed = acc.anyMuteKnown && acc.anyMuted && !acc.allMuted;
        row.muteError = acc.anyMuteKnown ? QString() : acc.firstMuteError;
        row.expired = (acc.expiredCount == row.sessionCount);
        snapshot.unreadableCount += acc.unreadable;

        // 系统声音不显示 PID：0 对用户没有意义，反而像"数据缺了"
        if (row.kind == MixerRowKind::SystemSounds) {
            row.pid = 0;
        }
        row.subtitle = buildSubtitle(row);
        row.tooltip = rowTooltip(row);
        snapshot.rows.append(row);
    }

    // 排序：应用在前、系统声音在后，同类按标题 —— 会话来去时行序不跳
    std::stable_sort(snapshot.rows.begin(),
                     snapshot.rows.end(),
                     [](const MixerRow &left, const MixerRow &right) {
                         const bool leftSystem = (left.kind == MixerRowKind::SystemSounds);
                         const bool rightSystem = (right.kind == MixerRowKind::SystemSounds);
                         if (leftSystem != rightSystem) {
                             return !leftSystem;
                         }
                         const int byTitle = QString::compare(left.title,
                                                              right.title,
                                                              Qt::CaseInsensitive);
                         if (byTitle != 0) {
                             return byTitle < 0;
                         }
                         return QString::compare(left.key, right.key) < 0;
                     });

    return snapshot;
}

QString rowTooltip(const MixerRow &row)
{
    QStringList lines;
    lines << row.title;

    if (row.kind == MixerRowKind::SystemSounds) {
        lines << QStringLiteral("Windows 自己的提示音：不属于任何应用（PID 0）");
    } else if (row.pid != 0) {
        lines << QStringLiteral("PID：%1").arg(row.pid);
    }

    if (!row.executablePath.isEmpty()) {
        lines << QStringLiteral("路径：%1").arg(row.executablePath);
    } else if (row.kind != MixerRowKind::SystemSounds) {
        lines << QStringLiteral("路径：读不到（该进程可能属于其它用户或权限更高）");
    }

    lines << QStringLiteral("会话：%1 份%2")
                 .arg(row.sessionCount)
                 .arg(row.active ? QStringLiteral("（正在出声）") : QString());
    lines << QStringLiteral("音量：%1")
                 .arg(row.volume >= 0.0 ? formatVolume(row.volume)
                                        : unavailableVolumeText(row.volumeError));
    lines << QStringLiteral("静音：%1")
                 .arg(muteText(row.muteValid, row.muted, row.muteMixed));

    if (row.sessionCount > 1) {
        lines << QString();
        lines << QStringLiteral("该应用在这一台输出设备上开了 %1 份会话。按应用合并时，"
                                "拖滑块会**同时调整这几份**；只调其中一份就会出现"
                                "「拖了滑块却听不出变化」。")
                     .arg(row.sessionCount);
    }

    if (!row.sessionDetails.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("逐会话明细：");
        lines.append(row.sessionDetails);
    }

    if (!row.adjustable()) {
        lines << QString();
        lines << QStringLiteral("本行当前不可调整：%1")
                     .arg(row.expired ? QStringLiteral("会话已过期（进程已退出）")
                                      : QStringLiteral("拿不到会话实例标识"));
    }
    return lines.join(QLatin1Char('\n'));
}

// ============================================================================
//  格式化
// ============================================================================

QString formatVolume(double volume)
{
    const double clamped = std::clamp(volume, 0.0, 1.0);
    const int percent = static_cast<int>(std::lround(clamped * 100.0));
    return QStringLiteral("%1%").arg(percent);
}

QString unavailableVolumeText(const QString &error)
{
    return error.isEmpty() ? QStringLiteral("音量读不到（系统未给出原因）") : error;
}

QString noSelectionText()
{
    return QStringLiteral("（在列表里选中一个应用后再调音量或静音）");
}

QString statusLine(const QString &deviceName, const MixerSnapshot &snapshot)
{
    QStringList parts;
    parts << QStringLiteral("输出设备：%1").arg(deviceName.isEmpty()
                                                   ? QStringLiteral("没有可用的输出设备")
                                                   : deviceName);
    parts << QStringLiteral("可调项 %1 个").arg(snapshot.rows.size());
    if (snapshot.hiddenExpired > 0) {
        parts << QStringLiteral("已隐藏 %1 个过期会话").arg(snapshot.hiddenExpired);
    }
    if (snapshot.unreadableCount > 0) {
        parts << QStringLiteral("%1 个会话读不到音量").arg(snapshot.unreadableCount);
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace WinEase::FeaturePlugins::Mixer
