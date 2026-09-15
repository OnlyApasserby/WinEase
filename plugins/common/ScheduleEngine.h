#pragma once

// ============================================================================
//  ScheduleEngine.h —— 定时任务的**时间决策**（纯函数，无 Q_OBJECT、无定时器）
//
//  为什么把"什么时候该触发"抽成纯函数：
//      路线图的验收是"**跨零点与系统休眠恢复后调度不丢任务**"。
//      这两件事如果靠"等到那个时刻"来验证，自检就得跑一整天；而且系统休眠这件事
//      根本没法在自检里制造。把决策抽成 `nextDailyFire(time, now)` 与
//      `dueTasks(tasks, now)` 之后，休眠三小时、跨零点、跨月、跨年全都能用
//      **固定的 now** 当场断言 —— 插件那边只剩"每秒把当前时间喂进来"。
//
//  关键语义（决定了休眠恢复会不会丢任务）：
//      任务存的是**绝对触发时刻** `nextFireAt`，而不是"每天几点"。
//      "每天 09:00" 只是"排定 nextFireAt"的一种规则；一旦排定，调度器只看
//      `now >= nextFireAt` —— 于是系统睡了三小时醒来，错过的那条任务照样会被捞出来
//      执行（`ScheduleTask::missed == true`，界面要如实标注"补触发"）。
//
//  ⚠ 反过来说：**用户新设一个"每天 09:00"的任务时，如果现在已经 09:01，
//     不能立刻触发**（那叫惊吓不叫提醒）——`nextDailyFire()` 因此严格晚于 now。
// ============================================================================

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>

namespace WinEase::Common {

/// 一条定时任务
struct ScheduleTask {
    QString id;              ///< 稳定标识（配置里用来定位/删除）
    QString action;          ///< "remind" / "mute" / "unmute" / 电源动作键（"shutdown"…）
    QString title;           ///< 提醒文本 / 状态栏说明
    QTime dailyTime;         ///< "每天 HH:mm" 规则用（一次性任务不用）
    bool repeatDaily = true; ///< true=每天，false=只触发一次
    QDateTime nextFireAt;    ///< **绝对**触发时刻（排定结果；休眠恢复全靠它）
    bool enabled = true;
    /// 本次触发是否属于"错过后的补触发"（调度器写入，仅供界面说明，不参与决策）
    bool missed = false;

    bool isValid() const { return !id.isEmpty() && nextFireAt.isValid(); }
};

/// "每天 HH:mm" 的下一次触发时刻：**严格晚于 now**（跨零点/跨月/跨年都正确）
QDateTime nextDailyFire(const QTime &time, const QDateTime &now);

/// 该触发（或补触发）的任务：`now >= nextFireAt` 且启用。
/// 休眠/挂起期间错过的任务会被它原样捞出来，并把 `missed` 置 true
QList<ScheduleTask> dueTasks(const QList<ScheduleTask> &tasks, const QDateTime &now);

/// 触发之后推进任务：每天重复 → 排到下一次；一次性 → 关闭（`enabled = false`）
ScheduleTask advanceSchedule(const ScheduleTask &task, const QDateTime &now);

/// 下一个将要触发的时刻（界面"还有多久"用；没有可触发的任务返回无效值）
QDateTime nextFireTime(const QList<ScheduleTask> &tasks, const QDateTime &now);

/// 动作键的中文名（"remind" → "提醒"；电源动作走 PowerControl 的 powerActionText）
QString scheduleActionText(const QString &action);
/// 动作键的候选列表（界面下拉用）
QStringList scheduleActionKeys();

/// "3 小时 12 分" / "12 秒" 这类人话时长
QString humanizeDuration(qint64 seconds);
/// 一句话描述任务（"每天 09:00 提醒：起来活动一下"）
QString humanizeTask(const ScheduleTask &task);

QJsonObject taskToJson(const ScheduleTask &task);
ScheduleTask taskFromJson(const QJsonObject &object, bool *ok = nullptr);
/// 配置里存 QStringList，每项一个 JSON 对象（`SettingsManager` 直接支持）
QStringList tasksToList(const QList<ScheduleTask> &tasks);
QList<ScheduleTask> tasksFromList(const QStringList &items);

/// 生成一个不重复的任务 id（"t<时间戳><序号>"）
QString makeTaskId(const QList<ScheduleTask> &existing, const QDateTime &now);

} // namespace WinEase::Common
