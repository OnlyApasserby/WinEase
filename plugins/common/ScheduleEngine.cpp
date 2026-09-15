#include "ScheduleEngine.h"

#include "PowerControl.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace WinEase::Common {

QDateTime nextDailyFire(const QTime &time, const QDateTime &now)
{
    if (!time.isValid() || !now.isValid()) {
        return QDateTime();
    }

    // QDateTime(QDate, QTime) 在 Qt6 里就是**本地时间**，正是定时任务要的语义
    QDateTime candidate(now.date(), time);
    // **严格**晚于 now：用户 09:01 设"每天 09:00"时，指的显然是明天那一次，
    // 不能因为"今天这个点刚过 1 分钟"就立刻弹一个提醒出来
    if (candidate <= now) {
        candidate = candidate.addDays(1);
    }
    return candidate;
}

QList<ScheduleTask> dueTasks(const QList<ScheduleTask> &tasks, const QDateTime &now)
{
    QList<ScheduleTask> due;
    if (!now.isValid()) {
        return due;
    }

    for (const ScheduleTask &task : tasks) {
        if (!task.enabled || !task.nextFireAt.isValid()) {
            continue;
        }
        if (now < task.nextFireAt) {
            continue;
        }
        ScheduleTask fired = task;
        // 迟到超过 1 个 tick 就算"补触发"：系统休眠、进程被挂起、机器卡住都会走到这里
        fired.missed = task.nextFireAt.secsTo(now) >= 2;
        due.append(fired);
    }
    return due;
}

ScheduleTask advanceSchedule(const ScheduleTask &task, const QDateTime &now)
{
    ScheduleTask next = task;
    next.missed = false;

    if (!task.repeatDaily || !task.dailyTime.isValid()) {
        next.enabled = false; // 一次性任务：触发即完成
        return next;
    }

    // 以"排定时刻"为基准往后推，而不是以 now 推：否则机器睡了三小时醒来，
    // 每天 09:00 的任务会被推成"明天 12:00"，越睡越偏
    const QDateTime base = task.nextFireAt.isValid() ? task.nextFireAt : now;
    QDateTime candidate(base.date(), task.dailyTime);
    while (candidate <= now) {
        candidate = candidate.addDays(1);
    }
    next.nextFireAt = candidate;
    return next;
}

QDateTime nextFireTime(const QList<ScheduleTask> &tasks, const QDateTime &now)
{
    QDateTime earliest;
    for (const ScheduleTask &task : tasks) {
        if (!task.enabled || !task.nextFireAt.isValid()) {
            continue;
        }
        if (task.nextFireAt <= now) {
            return task.nextFireAt; // 已经到点了（或错过了）——那就是"现在"
        }
        if (!earliest.isValid() || task.nextFireAt < earliest) {
            earliest = task.nextFireAt;
        }
    }
    return earliest;
}

QString scheduleActionText(const QString &action)
{
    if (action == QLatin1String("remind")) {
        return QStringLiteral("提醒");
    }
    if (action == QLatin1String("mute")) {
        return QStringLiteral("静音麦克风");
    }
    if (action == QLatin1String("unmute")) {
        return QStringLiteral("取消麦克风静音");
    }
    bool ok = false;
    const PowerAction power = powerActionFromKey(action, &ok);
    return ok ? powerActionText(power) : action;
}

QStringList scheduleActionKeys()
{
    QStringList keys{ QStringLiteral("remind"), QStringLiteral("mute"), QStringLiteral("unmute") };
    for (const PowerAction action : allPowerActions()) {
        keys << powerActionKey(action);
    }
    return keys;
}

QString humanizeDuration(qint64 seconds)
{
    if (seconds < 0) {
        return QStringLiteral("已过期");
    }
    if (seconds < 60) {
        return QStringLiteral("%1 秒").arg(seconds);
    }
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return QStringLiteral("%1 分").arg(minutes);
    }
    const qint64 hours = minutes / 60;
    const qint64 restMinutes = minutes % 60;
    if (hours < 24) {
        return restMinutes == 0
                   ? QStringLiteral("%1 小时").arg(hours)
                   : QStringLiteral("%1 小时 %2 分").arg(hours).arg(restMinutes);
    }
    const qint64 days = hours / 24;
    return QStringLiteral("%1 天 %2 小时").arg(days).arg(hours % 24);
}

QString humanizeTask(const ScheduleTask &task)
{
    const QString when = task.repeatDaily && task.dailyTime.isValid()
                             ? QStringLiteral("每天 %1")
                                   .arg(task.dailyTime.toString(QStringLiteral("HH:mm")))
                             : QStringLiteral("仅一次");
    const QString what = scheduleActionText(task.action);
    const QString extra = task.title.isEmpty() ? QString() : QStringLiteral("：%1").arg(task.title);
    return QStringLiteral("%1 %2%3").arg(when, what, extra);
}

QJsonObject taskToJson(const ScheduleTask &task)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), task.id);
    object.insert(QStringLiteral("action"), task.action);
    object.insert(QStringLiteral("title"), task.title);
    if (task.dailyTime.isValid()) {
        object.insert(QStringLiteral("dailyTime"), task.dailyTime.toString(QStringLiteral("HH:mm")));
    }
    object.insert(QStringLiteral("repeatDaily"), task.repeatDaily);
    object.insert(QStringLiteral("nextFireAt"),
                  task.nextFireAt.isValid() ? task.nextFireAt.toString(Qt::ISODate)
                                            : QString());
    object.insert(QStringLiteral("enabled"), task.enabled);
    return object;
}

ScheduleTask taskFromJson(const QJsonObject &object, bool *ok)
{
    ScheduleTask task;
    task.id = object.value(QStringLiteral("id")).toString();
    task.action = object.value(QStringLiteral("action")).toString();
    task.title = object.value(QStringLiteral("title")).toString();
    if (object.contains(QStringLiteral("dailyTime"))) {
        task.dailyTime = QTime::fromString(object.value(QStringLiteral("dailyTime")).toString(),
                                           QStringLiteral("HH:mm"));
    }
    task.repeatDaily = object.value(QStringLiteral("repeatDaily")).toBool(true);
    task.nextFireAt = QDateTime::fromString(object.value(QStringLiteral("nextFireAt")).toString(),
                                            Qt::ISODate);
    task.enabled = object.value(QStringLiteral("enabled")).toBool(true);

    if (ok != nullptr) {
        *ok = task.isValid();
    }
    return task;
}

QStringList tasksToList(const QList<ScheduleTask> &tasks)
{
    QStringList items;
    items.reserve(tasks.size());
    for (const ScheduleTask &task : tasks) {
        items << QString::fromUtf8(QJsonDocument(taskToJson(task)).toJson(QJsonDocument::Compact));
    }
    return items;
}

QList<ScheduleTask> tasksFromList(const QStringList &items)
{
    QList<ScheduleTask> tasks;
    tasks.reserve(items.size());
    for (const QString &item : items) {
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(item.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            continue; // 坏掉的条目跳过：宁可少一条任务，也不要整个调度器起不来
        }
        bool ok = false;
        const ScheduleTask task = taskFromJson(document.object(), &ok);
        if (ok) {
            tasks.append(task);
        }
    }
    return tasks;
}

QString makeTaskId(const QList<ScheduleTask> &existing, const QDateTime &now)
{
    const QString prefix = QStringLiteral("t%1").arg(now.toMSecsSinceEpoch());
    QString candidate = prefix;
    int suffix = 1;
    for (;;) {
        bool taken = false;
        for (const ScheduleTask &task : existing) {
            if (task.id == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
        candidate = QStringLiteral("%1_%2").arg(prefix).arg(++suffix);
    }
}

} // namespace WinEase::Common
