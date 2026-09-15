#include "power_group.h"

#include "PowerControl.h"
#include "ScheduleEngine.h"

#include "win32/CoreAudio.h"
#include "win32/PowerUtils.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QTime>
#include <QTimeEdit>
#include <QWidget>

namespace FeatureSmoke {

namespace {

namespace Win32 = WinEase::Win32;
using WinEase::Common::PowerAction;
using WinEase::Common::ScheduleTask;

const QString kPowerId = QStringLiteral("monitor.power_panel");
const QString kSchedulerId = QStringLiteral("monitor.scheduler");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kPowerPanelName = QStringLiteral("powerPanel");
const QString kPowerStatusLabel = QStringLiteral("powerStatusLabel");
const QString kPowerHelperLabel = QStringLiteral("powerHelperLabel");
const QString kPowerHintLabel = QStringLiteral("powerHintLabel");
const QString kPowerCountdownSpin = QStringLiteral("powerCountdownSpin");
const QString kPowerCancelButton = QStringLiteral("powerCancelButton");

const QString kSchedulerPanelName = QStringLiteral("schedulerPanel");
const QString kSchedulerStatusLabel = QStringLiteral("schedulerStatusLabel");
const QString kSchedulerCountdownLabel = QStringLiteral("schedulerCountdownLabel");
const QString kSchedulerTable = QStringLiteral("schedulerTable");
const QString kSchedulerActionCombo = QStringLiteral("schedulerActionCombo");
const QString kSchedulerTimeEdit = QStringLiteral("schedulerTimeEdit");
const QString kSchedulerRepeatCheck = QStringLiteral("schedulerRepeatCheck");
const QString kSchedulerTitleEdit = QStringLiteral("schedulerTitleEdit");
const QString kSchedulerAddButton = QStringLiteral("schedulerAddButton");
const QString kSchedulerRemoveButton = QStringLiteral("schedulerRemoveButton");
const QString kSchedulerToggleButton = QStringLiteral("schedulerToggleButton");
const QString kSchedulerCancelButton = QStringLiteral("schedulerCancelButton");

/// 自检预置的倒计时秒数（main.cpp 里写进配置）——刻意不是默认的 60，
/// 这样"配置真的被读走"是被断言出来的，而且用例不用等一分钟
constexpr int kPresetCountdown = 3;
/// 预置的任务条数（与 main.cpp 的种子一致；改一边就得改另一边，断在明处）
constexpr int kPresetTaskCount = 4;

/// 静默等一会儿（照常转事件循环）
void settle(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

template <typename T>
T *childNamed(QWidget *root, const QString &objectName)
{
    return root != nullptr ? root->findChild<T *>(objectName) : nullptr;
}

/// 从面板状态文本里读"下一个任务还有多久"——这里只需要"存在一句话"这种弱断言
QString shorten(const QString &text, int limit = 120)
{
    return text.size() <= limit ? text : text.left(limit) + QStringLiteral("…");
}

/// 麦克风静音守卫：P2-07 的"定时静音"任务会**真的**改系统麦克风 ——
/// 用户的麦克风状态是他自己/会议软件调出来的，用例结束必须还回去并回读确认
class MicMuteGuard
{
public:
    MicMuteGuard()
    {
        QString error;
        m_mic = Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Capture, &error);
        if (!m_mic.isValid()) {
            m_error = error;
            return;
        }
        if (!m_mic.muteState(&m_muted, &m_error)) {
            return;
        }
        m_valid = true;
    }

    ~MicMuteGuard() { restore(); }

    /// 显式还原。
    /// ⚠ 用例必须**先调它再断言**："析构时会还原"听着没错，但析构发生在函数返回之后 ——
    ///    在它之前去断言"已经还原了"，验到的是**还没还原**的状态（本用例就踩过这一脚）
    bool restore()
    {
        if (!m_valid || m_restored) {
            return m_restored;
        }
        QString error;
        m_restored = m_mic.setMuted(m_muted, &error);
        return m_restored;
    }

    bool valid() const { return m_valid; }
    bool originalMuted() const { return m_muted; }
    QString error() const { return m_error; }

    /// 从系统读当前静音状态（读不到时 okOut 为 false，绝不当成"未静音"）
    static bool readMuted(bool *mutedOut, QString *errorOut = nullptr)
    {
        Win32::AudioEndpoint mic =
            Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Capture, errorOut);
        if (!mic.isValid()) {
            return false;
        }
        return mic.muteState(mutedOut, errorOut);
    }

private:
    Win32::AudioEndpoint m_mic;
    bool m_valid = false;
    bool m_muted = false;
    bool m_restored = false;
    QString m_error;
};

// ===========================================================================
//  P2-06 快速关机（monitor.power_panel）
//
//  这一项唯一"出错就会挨骂"的地方是**撤销通道**：用户按了关机、又按了取消，
//  机器就必须继续开着。所以本段的重心全在"取消之后请求有没有真的被拦下来"。
//
//  ⚠ 两个刻意不点的地方：
//    * **「锁定」按钮一次都不点** —— 点了会真的锁屏，用户得重新登录。
//      "锁定不需要提权"这条决策用纯函数断言（`powerActionNeedsElevation`）；
//    * 电源请求打到**记录型提权桩**上（`RecordingElevationService`）：整条链路
//      （倒计时 → 参数组装 → 请求）都真实跑，只在"系统真的关机"之前截住。
// ===========================================================================

void runPowerPanelCase(Reporter &reporter, WinEase::PluginManager &manager, StubServices &services)
{
    WinEase::IFeaturePlugin *plugin = manager.plugin(kPowerId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-06 插件已从插件目录加载（monitor.power_panel）"));
    if (plugin == nullptr) {
        return;
    }

    // ---- 纯函数：谁需要提权（"锁定"是唯一例外，也是唯一本地能做的动作）----
    reporter.check(!WinEase::Common::powerActionNeedsElevation(PowerAction::Lock),
                   QStringLiteral("P2-06 「锁定」不需要提权（走本地 LockWorkStation，"
                                  "不依赖另一个进程）"));
    bool allOthersNeedElevation = true;
    for (const PowerAction action : WinEase::Common::allPowerActions()) {
        if (action != PowerAction::Lock
            && !WinEase::Common::powerActionNeedsElevation(action)) {
            allOthersNeedElevation = false;
        }
    }
    reporter.check(allOthersNeedElevation,
                   QStringLiteral("P2-06 其余五个动作都要经提权助手（`SE_SHUTDOWN_NAME`）"));

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kPowerPanelName,
                   QStringLiteral("P2-06 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kPowerStatusLabel);
    auto *helperLabel = childNamed<QLabel>(settings, kPowerHelperLabel);
    auto *hintLabel = childNamed<QLabel>(settings, kPowerHintLabel);
    auto *countdownSpin = childNamed<QSpinBox>(settings, kPowerCountdownSpin);
    auto *cancelButton = childNamed<QPushButton>(settings, kPowerCancelButton);

    QHash<QString, QPushButton *> actionButtons;
    for (const PowerAction action : WinEase::Common::allPowerActions()) {
        const QString name = QStringLiteral("powerBtn_%1")
                                 .arg(WinEase::Common::powerActionKey(action));
        actionButtons.insert(name, childNamed<QPushButton>(settings, name));
    }
    bool allButtonsFound = true;
    for (const QPushButton *button : actionButtons) {
        if (button == nullptr) {
            allButtonsFound = false;
        }
    }

    reporter.check(statusLabel != nullptr && helperLabel != nullptr && hintLabel != nullptr
                       && countdownSpin != nullptr && cancelButton != nullptr && allButtonsFound,
                   QStringLiteral("P2-06 面板控件按对象名全部找到（含六个动作按钮）"));
    if (statusLabel == nullptr || countdownSpin == nullptr || cancelButton == nullptr
        || !allButtonsFound) {
        delete settings;
        return;
    }

    QPushButton *shutdownButton = actionButtons.value(QStringLiteral("powerBtn_shutdown"));

    const auto cleanup = [&]() {
        manager.setPluginEnabled(kPowerId, false);
        delete settings;
    };

    // ---- ① 配置驱动的倒计时秒数 ----
    reporter.check(countdownSpin->value() == kPresetCountdown,
                   QStringLiteral("P2-06 面板上的倒计时是配置里的 %1 秒（不是代码默认的 60）")
                       .arg(kPresetCountdown),
                   QStringLiteral("面板显示 %1 秒").arg(countdownSpin->value()));

    reporter.check(manager.setPluginEnabled(kPowerId, true),
                   QStringLiteral("P2-06 插件启用成功"), plugin->lastError());
    settle(200);

    // ---- ② 提权助手不可用时：必须如实报错（fail-closed）----
    services.elevation()->setAvailable(false);
    services.elevation()->clearCalls();
    countdownSpin->setValue(0); // 0 = 不等待，直接执行（这条要验的是"报错"，不是"倒计时"）
    settle(120);
    shutdownButton->click();
    settle(250);

    reporter.check(!plugin->lastError().isEmpty()
                       && plugin->lastError().contains(QStringLiteral("提权助手")),
                   QStringLiteral("P2-06 助手不可用时**如实报错**（不假装已经关机）"),
                   plugin->lastError());
    reporter.check(statusLabel->text().contains(QStringLiteral("失败")),
                   QStringLiteral("P2-06 面板状态栏也把失败说清楚"), shorten(statusLabel->text()));
    reporter.check(services.notificationCount() > 0,
                   QStringLiteral("P2-06 失败时弹出提示（用户可能没在看面板）"));
    reporter.check(services.elevation()->callCount() > 0
                       && !services.elevation()->calls().last().operation.isEmpty(),
                   QStringLiteral("P2-06 助手不可用是**助手侧**的结论（请求确实到了它那里）"),
                   services.elevation()->operations().join(QStringLiteral(",")));

    // ---- ③ 倒计时中点「取消」：请求一次都不能发出去 ----
    services.elevation()->setAvailable(true);
    services.elevation()->clearCalls();
    countdownSpin->setValue(kPresetCountdown);
    settle(150);

    shutdownButton->click();
    settle(200);
    reporter.check(statusLabel->text().contains(QStringLiteral("秒后执行关机")),
                   QStringLiteral("P2-06 点「关机」→ 进入倒计时（不立刻关机）"),
                   shorten(statusLabel->text()));
    reporter.check(!services.trayBadgeText(kPowerId).isEmpty(),
                   QStringLiteral("P2-06 倒计时期间托盘上出现倒数徽标"),
                   services.trayBadgeText(kPowerId));
    reporter.check(cancelButton->isEnabled(),
                   QStringLiteral("P2-06 倒计时期间「取消倒计时」可用"));
    reporter.check(!shutdownButton->isEnabled(),
                   QStringLiteral("P2-06 倒计时期间动作按钮被禁用（避免两个倒计时抢一个屏）"));

    const int badgeWhileCounting = services.trayBadgeText(kPowerId).toInt();
    settle(1200);
    const int badgeAfterOneTick = services.trayBadgeText(kPowerId).toInt();
    reporter.check(badgeAfterOneTick >= 0 && badgeAfterOneTick < badgeWhileCounting,
                   QStringLiteral("P2-06 托盘徽标每秒递减（用户看得见还剩多久）"),
                   QStringLiteral("%1 → %2").arg(badgeWhileCounting).arg(badgeAfterOneTick));

    cancelButton->click();
    settle(250);
    reporter.check(statusLabel->text().contains(QStringLiteral("已取消")),
                   QStringLiteral("P2-06 点「取消倒计时」→ 状态如实变成已取消"),
                   shorten(statusLabel->text()));
    reporter.check(services.elevation()->callCount(QStringLiteral("powerAction")) == 0,
                   QStringLiteral("P2-06 ★ 取消之后**电源请求一次都没发出去**"
                                  "（这正是「取消倒计时后系统不关机」）"),
                   QStringLiteral("桩收到 %1 次 powerAction")
                       .arg(services.elevation()->callCount(QStringLiteral("powerAction"))));
    reporter.check(services.trayBadgeText(kPowerId).isEmpty(),
                   QStringLiteral("P2-06 取消后托盘徽标被清掉"));

    // ---- ④ 让它走到点：请求带着正确的参数交到助手那里 ----
    shutdownButton->click();
    settle(150);
    const bool dispatched = waitFor(
        [&services] { return services.elevation()->callCount(QStringLiteral("powerAction")) > 0; },
        kPresetCountdown * 1000 + 4000);
    reporter.check(dispatched,
                   QStringLiteral("P2-06 倒计时归零 → 电源请求真的发出去了（到点即执行）"),
                   shorten(statusLabel->text()));

    const QVariantMap arguments = services.elevation()->lastArguments();
    reporter.check(arguments.value(QStringLiteral("action")).toString() == QStringLiteral("shutdown"),
                   QStringLiteral("P2-06 请求里的 action 是 shutdown"),
                   arguments.value(QStringLiteral("action")).toString());
    reporter.check(arguments.contains(QStringLiteral("timeout"))
                       && arguments.value(QStringLiteral("timeout")).toInt() >= 0,
                   QStringLiteral("P2-06 请求带上了系统级反悔窗口 timeout"),
                   QStringLiteral("timeout = %1").arg(arguments.value(QStringLiteral("timeout")).toInt()));

    // ---- ⑤ 停用时若正在倒计时：必须撤销（不能留下"关着的功能还在倒数"）----
    services.elevation()->clearCalls();
    shutdownButton->click();
    settle(200);
    reporter.check(statusLabel->text().contains(QStringLiteral("秒后执行")),
                   QStringLiteral("P2-06 再次进入倒计时（准备验证停用撤销）"),
                   shorten(statusLabel->text()));
    reporter.check(manager.setPluginEnabled(kPowerId, false),
                   QStringLiteral("P2-06 倒计时期间停用插件成功"));
    settle(200);
    reporter.check(services.trayBadgeText(kPowerId).isEmpty(),
                   QStringLiteral("P2-06 停用后托盘徽标被清掉"));
    settle(kPresetCountdown * 1000 + 500);
    reporter.check(services.elevation()->callCount(QStringLiteral("powerAction")) == 0,
                   QStringLiteral("P2-06 ★ 停用撤销了倒计时：等到原定时刻也没有发出请求"),
                   QStringLiteral("桩收到 %1 次").arg(services.elevation()->callCount()));

    // ---- ⑥ 收尾：确认本机此刻没有挂起的关机（自检不留危险状态）----
    bool pending = true;
    QString probeError;
    const bool probed = Win32::hasPendingShutdown(&pending, &probeError);
    reporter.check(probed && !pending,
                   QStringLiteral("P2-06 收尾：本机当前没有挂起的关机/重启"),
                   probeError.isEmpty() ? QStringLiteral("无挂起") : probeError);

    cleanup();
}

// ===========================================================================
//  P2-07 定时任务（monitor.scheduler）
//
//  两段：**纯函数**（跨零点 / 跨月 / 休眠补触发 —— 全都用固定 now 断言，
//  不用等到明天、也不用真的让机器睡一觉）+ **端到端**（真通知、真麦克风、
//  真电源倒计时）。
//
//  ⚠ 预置任务（main.cpp）的 `nextFireAt` 刻意落在**过去**：这就是"系统休眠三小时
//    后唤醒"的同一种状态（调度器只认绝对触发时刻），于是"要不要补触发、有没有丢"
//    这件事能在 1 秒内验完。
// ===========================================================================

void runSchedulerCase(Reporter &reporter, WinEase::PluginManager &manager, StubServices &services)
{
    const QDateTime now = QDateTime::currentDateTime();

    // ---- ① 纯函数：每天 HH:mm 的下一次触发（跨零点 / 跨月 / 严格晚于 now）----
    const QDateTime lateNight(QDate(2026, 9, 14), QTime(23, 30, 0));
    reporter.check(WinEase::Common::nextDailyFire(QTime(7, 0), lateNight)
                       == QDateTime(QDate(2026, 9, 15), QTime(7, 0)),
                   QStringLiteral("P2-07 跨零点：23:30 排定「每天 07:00」→ 落在次日 07:00"));

    const QDateTime earlyMorning(QDate(2026, 9, 14), QTime(6, 0, 0));
    reporter.check(WinEase::Common::nextDailyFire(QTime(7, 0), earlyMorning)
                       == QDateTime(QDate(2026, 9, 14), QTime(7, 0)),
                   QStringLiteral("P2-07 当天还没到的时刻 → 排在当天（不是无脑明天）"));

    reporter.check(WinEase::Common::nextDailyFire(QTime(23, 30), lateNight)
                       == QDateTime(QDate(2026, 9, 15), QTime(23, 30)),
                   QStringLiteral("P2-07 设定的时刻正好等于当前时刻 → **严格**排到下一次"
                                  "（用户 09:01 设「每天 09:00」不该立刻弹提醒）"));

    const QDateTime monthEnd(QDate(2026, 9, 30), QTime(23, 0, 0));
    reporter.check(WinEase::Common::nextDailyFire(QTime(1, 0), monthEnd)
                       == QDateTime(QDate(2026, 10, 1), QTime(1, 0)),
                   QStringLiteral("P2-07 跨月正确（9/30 23:00 排「每天 01:00」→ 10/1）"));

    // ---- ② 纯函数：休眠恢复后的补触发 ----
    ScheduleTask missedTask;
    missedTask.id = QStringLiteral("unit_missed");
    missedTask.action = QStringLiteral("remind");
    missedTask.repeatDaily = true;
    missedTask.dailyTime = QTime(9, 0);
    missedTask.nextFireAt = now.addSecs(-3 * 3600); // 睡了三小时

    ScheduleTask futureTask = missedTask;
    futureTask.id = QStringLiteral("unit_future");
    futureTask.nextFireAt = now.addSecs(3600);

    ScheduleTask disabledTask = missedTask;
    disabledTask.id = QStringLiteral("unit_disabled");
    disabledTask.enabled = false;

    const QList<ScheduleTask> due =
        WinEase::Common::dueTasks({ missedTask, futureTask, disabledTask }, now);
    reporter.check(due.size() == 1 && due.first().id == QStringLiteral("unit_missed"),
                   QStringLiteral("P2-07 休眠三小时唤醒：**错过的任务被捞出来**（不静默丢弃）、"
                                  "未来的与已停用的不掺和"),
                   QStringLiteral("捞出 %1 条").arg(due.size()));
    reporter.check(!due.isEmpty() && due.first().missed,
                   QStringLiteral("P2-07 补触发的任务被如实标记（通知里要写「补触发」）"));

    const ScheduleTask advancedDaily = WinEase::Common::advanceSchedule(missedTask, now);
    reporter.check(advancedDaily.enabled && advancedDaily.nextFireAt > now
                       && advancedDaily.nextFireAt == WinEase::Common::nextDailyFire(QTime(9, 0), now),
                   QStringLiteral("P2-07 每天重复的任务触发后**排到下一次**（不会丢，也不会连着触发）"),
                   advancedDaily.nextFireAt.toString(Qt::ISODate));

    ScheduleTask onceTask = missedTask;
    onceTask.repeatDaily = false;
    reporter.check(!WinEase::Common::advanceSchedule(onceTask, now).enabled,
                   QStringLiteral("P2-07 一次性任务触发后自动停用"));

    // ---- 面板与插件 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kSchedulerId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-07 插件已从插件目录加载（monitor.scheduler）"));
    if (plugin == nullptr) {
        return;
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kSchedulerPanelName,
                   QStringLiteral("P2-07 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kSchedulerStatusLabel);
    auto *countdownLabel = childNamed<QLabel>(settings, kSchedulerCountdownLabel);
    auto *table = childNamed<QTableWidget>(settings, kSchedulerTable);
    auto *actionCombo = childNamed<QComboBox>(settings, kSchedulerActionCombo);
    auto *timeEdit = childNamed<QTimeEdit>(settings, kSchedulerTimeEdit);
    auto *repeatCheck = childNamed<QCheckBox>(settings, kSchedulerRepeatCheck);
    auto *titleEdit = childNamed<QLineEdit>(settings, kSchedulerTitleEdit);
    auto *addButton = childNamed<QPushButton>(settings, kSchedulerAddButton);
    auto *removeButton = childNamed<QPushButton>(settings, kSchedulerRemoveButton);
    auto *toggleButton = childNamed<QPushButton>(settings, kSchedulerToggleButton);
    auto *cancelButton = childNamed<QPushButton>(settings, kSchedulerCancelButton);

    reporter.check(statusLabel != nullptr && countdownLabel != nullptr && table != nullptr
                       && actionCombo != nullptr && timeEdit != nullptr && repeatCheck != nullptr
                       && titleEdit != nullptr && addButton != nullptr && removeButton != nullptr
                       && toggleButton != nullptr && cancelButton != nullptr,
                   QStringLiteral("P2-07 面板控件按对象名全部找到"));
    if (statusLabel == nullptr || countdownLabel == nullptr || table == nullptr
        || actionCombo == nullptr || timeEdit == nullptr || addButton == nullptr
        || removeButton == nullptr || cancelButton == nullptr) {
        delete settings;
        return;
    }

    const auto cleanup = [&]() {
        manager.setPluginEnabled(kSchedulerId, false);
        delete settings;
    };

    // ---- ③ 端到端：启用后立刻补触发（等价于"刚开机/刚唤醒"）----
    MicMuteGuard micGuard;
    reporter.check(micGuard.valid(),
                   QStringLiteral("P2-07 前置：能读到麦克风当前静音状态"
                                  "（否则「定时静音」那半段无意义）"),
                   micGuard.error());
    if (!micGuard.valid()) {
        delete settings;
        return;
    }

    const int notificationsBefore = services.notificationCount();
    services.elevation()->setAvailable(true);
    services.elevation()->clearCalls();

    reporter.check(manager.setPluginEnabled(kSchedulerId, true),
                   QStringLiteral("P2-07 插件启用成功"), plugin->lastError());
    settle(500);

    reporter.check(services.notificationCount() > notificationsBefore,
                   QStringLiteral("P2-07 「错过」的提醒任务在启用瞬间就被补触发（弹了通知）"),
                   QStringLiteral("通知数 %1 → %2")
                       .arg(notificationsBefore)
                       .arg(services.notificationCount()));
    reporter.check(services.warnings().join(QLatin1Char('\n')).contains(QStringLiteral("补触发")),
                   QStringLiteral("P2-07 通知里如实标注了「补触发」（不是假装正好到点）"),
                   services.warnings().isEmpty() ? QStringLiteral("(没有通知)")
                                                 : services.warnings().last());

    bool muted = false;
    QString micError;
    const bool micReadable = MicMuteGuard::readMuted(&muted, &micError);
    reporter.check(micReadable && muted,
                   QStringLiteral("P2-07 定时静音任务**真的改了系统麦克风**（读回确认）"),
                   micError.isEmpty() ? QStringLiteral("读回 = 静音") : micError);
    if (micReadable && muted == micGuard.originalMuted()) {
        reporter.info(QStringLiteral("P2-07 说明：原状态本来就是静音，"
                                     "「真的改了」这条因此退化成了「读回一致」"));
    }

    reporter.check(table->rowCount() == kPresetTaskCount,
                   QStringLiteral("P2-07 任务表按配置渲染出 %1 行").arg(kPresetTaskCount),
                   QStringLiteral("实际 %1 行").arg(table->rowCount()));

    // ---- ④ 电源类任务：先进倒计时，**不立刻关机** ----
    reporter.check(countdownLabel->text().contains(QStringLiteral("秒后执行关机")),
                   QStringLiteral("P2-07 定时关机任务到点后进入可取消的倒计时（不是立刻黑屏）"),
                   shorten(countdownLabel->text()));
    reporter.check(cancelButton->isEnabled(),
                   QStringLiteral("P2-07 倒计时期间「取消倒计时」可用"));
    reporter.check(services.elevation()->callCount(QStringLiteral("powerAction")) == 0,
                   QStringLiteral("P2-07 倒计时期间电源请求**没有**发出去"),
                   QStringLiteral("桩收到 %1 次").arg(services.elevation()->callCount()));

    // 取消第一条（下一 tick 里第二条会到点，正好接力验证"到点真的执行"）
    cancelButton->click();
    settle(250);
    reporter.check(countdownLabel->text().contains(QStringLiteral("没有进行中")),
                   QStringLiteral("P2-07 点「取消倒计时」→ 面板状态如实回到「没有进行中」"),
                   shorten(countdownLabel->text()));

    const bool dispatched = waitFor(
        [&services] { return services.elevation()->callCount(QStringLiteral("powerAction")) > 0; },
        12000);
    reporter.check(dispatched,
                   QStringLiteral("P2-07 ★ 第二条电源任务的倒计时走完后，请求真的发出去了"),
                   shorten(countdownLabel->text()));
    reporter.check(services.elevation()
                           ->lastArguments()
                           .value(QStringLiteral("action"))
                           .toString()
                       == QStringLiteral("shutdown"),
                   QStringLiteral("P2-07 发出的请求是 shutdown（动作来自任务本身）"),
                   services.elevation()->lastArguments().value(QStringLiteral("action")).toString());

    // ---- ⑤ 触发之后任务被推进：一次性停用、每天排到下一次 ----
    const QList<ScheduleTask> persisted = WinEase::Common::tasksFromList(
        services.configValue(kSchedulerId, QStringLiteral("tasks"), QStringList()).toStringList());
    reporter.check(persisted.size() == kPresetTaskCount,
                   QStringLiteral("P2-07 触发不会把任务弄丢（配置里还是 %1 条）")
                       .arg(kPresetTaskCount),
                   QStringLiteral("实际 %1 条").arg(persisted.size()));

    const auto taskById = [&persisted](const QString &id) {
        for (const ScheduleTask &task : persisted) {
            if (task.id == id) {
                return task;
            }
        }
        return ScheduleTask();
    };

    const ScheduleTask remindAfter = taskById(QStringLiteral("seed_remind"));
    reporter.check(remindAfter.enabled && remindAfter.nextFireAt > now,
                   QStringLiteral("P2-07 每天重复的提醒任务被排到下一次（不丢也不连发）"),
                   remindAfter.nextFireAt.toString(Qt::ISODate));
    reporter.check(!taskById(QStringLiteral("seed_mute")).enabled,
                   QStringLiteral("P2-07 一次性的静音任务触发后自动停用"));
    reporter.check(!taskById(QStringLiteral("seed_power_later")).enabled,
                   QStringLiteral("P2-07 一次性的电源任务触发后自动停用（不会半夜又关一次机）"));

    // ---- ⑥ 面板添加/删除任务（用户真实路径）----
    actionCombo->setCurrentIndex(actionCombo->findData(QStringLiteral("remind")));
    timeEdit->setTime(QTime(7, 30));
    repeatCheck->setChecked(true);
    titleEdit->setText(QStringLiteral("自检：喝水"));
    addButton->click();
    settle(250);
    reporter.check(table->rowCount() == kPresetTaskCount + 1,
                   QStringLiteral("P2-07 面板「添加任务」→ 表格多一行"),
                   QStringLiteral("实际 %1 行").arg(table->rowCount()));

    const QList<ScheduleTask> afterAdd = WinEase::Common::tasksFromList(
        services.configValue(kSchedulerId, QStringLiteral("tasks"), QStringList()).toStringList());
    reporter.check(afterAdd.size() == kPresetTaskCount + 1,
                   QStringLiteral("P2-07 新任务同时落到配置里（配置就是任务列表的唯一真相）"),
                   QStringLiteral("配置里 %1 条").arg(afterAdd.size()));

    // 提醒任务没填内容时应当被拒绝（一条空提醒等于没有提醒）
    actionCombo->setCurrentIndex(actionCombo->findData(QStringLiteral("remind")));
    titleEdit->setText(QString());
    addButton->click();
    settle(200);
    reporter.check(table->rowCount() == kPresetTaskCount + 1,
                   QStringLiteral("P2-07 提醒内容为空时拒绝添加（不产生「空提醒」）"),
                   shorten(statusLabel->text()));

    table->setCurrentCell(table->rowCount() - 1, 0);
    removeButton->click();
    settle(250);
    reporter.check(table->rowCount() == kPresetTaskCount,
                   QStringLiteral("P2-07 面板「删除所选」→ 表格与配置同时少一条"),
                   QStringLiteral("实际 %1 行").arg(table->rowCount()));

    // ---- ⑦ 停用：倒计时与定时器一起收干净 ----
    reporter.check(manager.setPluginEnabled(kSchedulerId, false),
                   QStringLiteral("P2-07 插件停用成功"));
    settle(300);
    reporter.check(services.trayBadgeText(kSchedulerId).isEmpty(),
                   QStringLiteral("P2-07 停用后托盘徽标被清掉"));
    const int callsAfterDisable = services.elevation()->callCount(QStringLiteral("powerAction"));
    settle(kPresetCountdown * 1000 + 1500);
    reporter.check(services.elevation()->callCount(QStringLiteral("powerAction")) == callsAfterDisable,
                   QStringLiteral("P2-07 停用后不再有新的电源请求（定时器真的停了）"),
                   QStringLiteral("停用前后都是 %1 次").arg(callsAfterDisable));

    cleanup();

    // ---- ⑧ 收尾：麦克风状态还原并确认（**先还原、再断言**）----
    reporter.check(micGuard.restore(),
                   QStringLiteral("P2-07 收尾：把麦克风静音状态写回用户原值（系统接受了这次写入）"));
    settle(250);
    bool finalMuted = !micGuard.originalMuted();
    QString finalError;
    const bool finalReadable = MicMuteGuard::readMuted(&finalMuted, &finalError);
    reporter.check(finalReadable && finalMuted == micGuard.originalMuted(),
                   QStringLiteral("P2-07 收尾：麦克风静音状态已还原为用户原值"),
                   QStringLiteral("原值 %1 / 现在 %2")
                       .arg(micGuard.originalMuted() ? QStringLiteral("静音") : QStringLiteral("未静音"),
                            finalMuted ? QStringLiteral("静音") : QStringLiteral("未静音")));
}

} // namespace

int runPowerGroupTests(Reporter &reporter,
                       WinEase::PluginManager &manager,
                       StubServices &services)
{
    const int failuresAtStart = reporter.failures();

    reporter.info(QStringLiteral("说明：本组**不会真的关机/锁屏**——电源请求打到记录型提权桩上；"
                                 "唯一的真实改动是 P2-07 的定时静音（用例结束还原麦克风）"));

    runPowerPanelCase(reporter, manager, services);
    runSchedulerCase(reporter, manager, services);

    reporter.info(QStringLiteral("系统与媒体组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
