#include "scheduler_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/CoreAudio.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

using WinEase::Common::PowerAction;
using WinEase::Common::ScheduleTask;

/// 每秒检查一次：秒级精度够用，而且"到点"这件事晚一秒用户看得出来
constexpr int kTickIntervalMs = 1000;
/// 交给系统后的兜底反悔窗口（用户已经在插件里倒计时过）
constexpr int kSystemTimeoutSeconds = 5;

/// 表格列的固定顺序（自检按列号断言）
constexpr int kColumnTime = 0;
constexpr int kColumnAction = 1;
constexpr int kColumnTitle = 2;
constexpr int kColumnState = 3;

} // namespace

SchedulerPlugin::SchedulerPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString SchedulerPlugin::id() const
{
    return QStringLiteral("monitor.scheduler");
}

QString SchedulerPlugin::name() const
{
    return QStringLiteral("定时任务");
}

QString SchedulerPlugin::description() const
{
    return QStringLiteral("定时提醒 / 静音 / 关机（到点仍可取消）；休眠唤醒后错过的任务会补触发");
}

QIcon SchedulerPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::SystemMonitor);
}

WinEase::FeatureCategory SchedulerPlugin::category() const
{
    return WinEase::FeatureCategory::SystemMonitor;
}

QStringList SchedulerPlugin::tags() const
{
    return { QStringLiteral("定时"), QStringLiteral("提醒"), QStringLiteral("计划"),
             QStringLiteral("定时关机"), QStringLiteral("dingshi"), QStringLiteral("tixing") };
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool SchedulerPlugin::initialize()
{
    // 配置只在 initialize() 读一次，之后改动走面板控件（踩坑 #26）
    if (WinEase::PluginServices *svc = services()) {
        m_countdownSeconds =
            qBound(0, svc->configValue(id(), QStringLiteral("countdownSeconds"), 60).toInt(), 600);
        m_tasks = WinEase::Common::tasksFromList(
            svc->configValue(id(), QStringLiteral("tasks"), QStringList()).toStringList());
    }

    if (m_tick == nullptr) {
        m_tick = new QTimer(this);
        m_tick->setInterval(kTickIntervalMs);
        connect(m_tick, &QTimer::timeout, this, [this] { tick(); });
    }

    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("定时任务已就绪：%1 条（启用 %2 条，电源操作倒计时 %3 秒）")
                   .arg(m_tasks.size())
                   .arg(std::count_if(m_tasks.cbegin(), m_tasks.cend(),
                                      [](const ScheduleTask &task) { return task.enabled; }))
                   .arg(m_countdownSeconds));
    return true;
}

void SchedulerPlugin::shutdown()
{
    if (m_countdown.isActive()) {
        cancelCountdown(QStringLiteral("插件已卸载，倒计时被取消"));
    }
    if (m_tick != nullptr) {
        m_tick->stop();
    }
}

bool SchedulerPlugin::canEnable(QString *reason) const
{
    // 没有硬前提：提醒是纯本地的，静音/电源动作缺条件时逐条如实报错即可
    Q_UNUSED(reason)
    return true;
}

bool SchedulerPlugin::onEnable()
{
    // 启用瞬间先补一次：机器可能刚开机，昨天错过的那条任务要立刻被认领
    const QDateTime now = QDateTime::currentDateTime();
    const QList<ScheduleTask> due = WinEase::Common::dueTasks(m_tasks, now);
    for (const ScheduleTask &task : due) {
        fireTask(task);
    }

    if (m_tick != nullptr) {
        m_tick->start();
    }

    m_lastEvent = nextFireText();
    Q_EMIT statusMessage(m_lastEvent);

    // ⚠ 时序（踩坑 #51）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void SchedulerPlugin::onDisable()
{
    if (m_countdown.isActive()) {
        cancelCountdown(QStringLiteral("功能已停用，倒计时被取消"));
    }
    if (m_tick != nullptr) {
        m_tick->stop();
    }
    updateBadge();
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  调度
// ---------------------------------------------------------------------------

void SchedulerPlugin::tick()
{
    const QDateTime now = QDateTime::currentDateTime();

    // ① 先推进电源倒计时（到点就把请求交给提权助手）
    if (m_countdown.isActive()) {
        if (m_countdown.tick(now)) {
            const PowerAction action = m_countdown.action();
            updateBadge();
            executePower(action);
        } else {
            updateBadge();
            refreshPanel();
        }
    }

    // ② 再认领"该触发的任务"（含休眠/挂起期间错过的）
    //    ⚠ 遍历的是副本：fireTask() 会改 m_tasks（推进下次触发时刻）
    const QList<ScheduleTask> due = WinEase::Common::dueTasks(m_tasks, now);
    for (const ScheduleTask &task : due) {
        fireTask(task);
    }
}

void SchedulerPlugin::fireTask(const ScheduleTask &task)
{
    const QString prefix = task.missed ? QStringLiteral("【补触发】") : QString();
    const QString description = WinEase::Common::humanizeTask(task);
    WinEase::PluginServices *svc = services();
    QString detail;
    bool failed = false;
    /// 是否算"这条任务处理完了"——只有处理完才推进它的下一次触发时刻
    bool handled = true;

    if (task.action == QLatin1String("remind")) {
        const QString body = task.title.isEmpty() ? QStringLiteral("到时间了") : task.title;
        if (svc != nullptr) {
            svc->notify(QStringLiteral("定时提醒%1").arg(prefix), body);
        }
        detail = QStringLiteral("%1定时提醒：%2").arg(prefix, body);
        clearLastError();
    } else if (task.action == QLatin1String("mute") || task.action == QLatin1String("unmute")) {
        QString message;
        failed = !applyMicrophoneMute(task.action == QLatin1String("mute"), &message);
        detail = QStringLiteral("%1%2").arg(prefix, message);
        if (failed) {
            setLastError(message);
            if (svc != nullptr) {
                svc->notify(name(), message);
            }
        } else {
            clearLastError();
        }
    } else {
        // 电源动作：**不立刻执行**，先进可取消的倒计时
        handled = startPowerCountdown(task);
        detail = QStringLiteral("%1%2").arg(prefix, m_lastEvent);
        failed = !handled;
    }

    logMessage(failed ? WinEase::PluginLogLevel::Warning : WinEase::PluginLogLevel::Info,
               QStringLiteral("定时任务触发：%1 → %2").arg(description, detail));

    m_lastEvent = detail;
    Q_EMIT statusMessage(detail);

    if (handled) {
        advanceTask(task.id, QDateTime::currentDateTime());
    }
    // 没处理成（已有倒计时在走）就**不推进**：任务保持"待触发"，
    // 下一拍会再被 dueTasks() 捞出来 —— 宁可晚几分钟，也不能静默丢掉
    refreshPanel();
}

bool SchedulerPlugin::startPowerCountdown(const ScheduleTask &task)
{
    bool ok = false;
    const PowerAction action = WinEase::Common::powerActionFromKey(task.action, &ok);
    if (!ok) {
        m_lastEvent = QStringLiteral("未知的电源动作：%1").arg(task.action);
        return false;
    }

    if (m_countdown.isActive()) {
        // ⚠ 两条电源任务同时到点（例如"关机"和"重启"都排在 23:00）时，
        //   绝不能把正在走的倒计时**覆盖**掉 —— 用户看到的剩余秒数会莫名其妙地跳回去；
        //   也绝不能静默丢掉任务（那等于用户设的关机永远不会发生）。
        //   做法：如实说一句，并把任务留在"待触发"状态，等当前倒计时结束后下一拍再试。
        m_lastEvent = QStringLiteral("已有电源倒计时在进行中（%1 秒后执行%2），"
                                     "本任务稍后自动重试")
                          .arg(m_countdown.remainingSeconds(QDateTime::currentDateTime()))
                          .arg(WinEase::Common::powerActionText(m_countdown.action()));
        return false;
    }

    const QString text = WinEase::Common::powerActionText(action);
    const QDateTime now = QDateTime::currentDateTime();

    if (m_countdownSeconds <= 0) {
        executePower(action);
        return true; // 任务已被处理（立即执行）
    }

    m_countdown.begin(action, m_countdownSeconds, now);
    updateBadge();
    m_lastEvent = QStringLiteral("将在 %1 秒后执行%2（点「取消倒计时」可撤销）")
                      .arg(m_countdownSeconds)
                      .arg(text);
    return true;
}

void SchedulerPlugin::executePower(PowerAction action)
{
    WinEase::PluginServices *svc = services();
    // "落盘 → 执行 → 如实报结果"与 P2-06 共用同一份实现
    const WinEase::Common::PowerActionResult result =
        WinEase::Common::executePowerAction(svc, action, kSystemTimeoutSeconds);

    if (result.ok) {
        clearLastError();
        m_lastEvent = result.message;
        Q_EMIT statusMessage(m_lastEvent);
    } else {
        setLastError(result.error);
        m_lastEvent = QStringLiteral("「%1」执行失败：%2")
                          .arg(WinEase::Common::powerActionText(action), result.error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
        if (svc != nullptr) {
            svc->notify(name(), m_lastEvent);
        }
        Q_EMIT statusMessage(m_lastEvent);
    }

    updateBadge();
    refreshPanel();
}

void SchedulerPlugin::cancelCountdown(const QString &reason)
{
    if (!m_countdown.cancel()) {
        m_lastEvent = QStringLiteral("当前没有可取消的操作");
        refreshPanel();
        return;
    }
    updateBadge();
    m_lastEvent = QStringLiteral("已取消：%1")
                      .arg(reason.isEmpty() ? QStringLiteral("用户点了取消") : reason);
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

bool SchedulerPlugin::applyMicrophoneMute(bool muted, QString *messageOut)
{
    const auto fail = [messageOut](const QString &text) {
        if (messageOut != nullptr) {
            *messageOut = text;
        }
        return false;
    };

    QString error;
    WinEase::Win32::AudioEndpoint mic =
        WinEase::Win32::AudioEndpoint::defaultEndpoint(WinEase::Win32::AudioDirection::Capture, &error);
    if (!mic.isValid()) {
        return fail(QStringLiteral("没有可用的麦克风（%1）").arg(error));
    }

    if (!mic.setMuted(muted, &error)) {
        return fail(QStringLiteral("设置麦克风静音失败：%1").arg(error));
    }

    // 写硬件之后回读确认（与 P2-10 同一条纪律：不信自己刚写进去的参数）
    bool actual = false;
    if (!mic.muteState(&actual, &error)) {
        return fail(QStringLiteral("麦克风静音已写入但读回失败：%1").arg(error));
    }
    if (actual != muted) {
        return fail(QStringLiteral("麦克风静音未生效（目标 %1，读回 %2）")
                        .arg(muted ? QStringLiteral("静音") : QStringLiteral("取消静音"),
                             actual ? QStringLiteral("静音") : QStringLiteral("取消静音")));
    }

    if (messageOut != nullptr) {
        *messageOut = muted ? QStringLiteral("已静音麦克风") : QStringLiteral("已取消麦克风静音");
    }
    return true;
}

// ---------------------------------------------------------------------------
//  任务列表
// ---------------------------------------------------------------------------

int SchedulerPlugin::indexOfTask(const QString &taskId) const
{
    for (int index = 0; index < m_tasks.size(); ++index) {
        if (m_tasks.at(index).id == taskId) {
            return index;
        }
    }
    return -1;
}

void SchedulerPlugin::advanceTask(const QString &taskId, const QDateTime &now)
{
    const int index = indexOfTask(taskId);
    if (index < 0) {
        return;
    }
    m_tasks[index] = WinEase::Common::advanceSchedule(m_tasks.at(index), now);
    persistTasks();
}

void SchedulerPlugin::persistTasks()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("tasks"),
                            WinEase::Common::tasksToList(m_tasks));
        svc->syncConfig();
    }
}

// ---------------------------------------------------------------------------
//  面板
// ---------------------------------------------------------------------------

QWidget *SchedulerPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("schedulerPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("schedulerStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *countdownLabel = new QLabel(widget);
    countdownLabel->setObjectName(QStringLiteral("schedulerCountdownLabel"));
    countdownLabel->setWordWrap(true);
    layout->addWidget(countdownLabel);

    auto *table = new QTableWidget(0, 4, widget);
    table->setObjectName(QStringLiteral("schedulerTable"));
    table->setHorizontalHeaderLabels({ QStringLiteral("时间"), QStringLiteral("动作"),
                                       QStringLiteral("说明"), QStringLiteral("状态") });
    table->horizontalHeader()->setSectionResizeMode(kColumnTitle, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    auto *addRow = new QHBoxLayout();
    auto *actionCombo = new QComboBox(widget);
    actionCombo->setObjectName(QStringLiteral("schedulerActionCombo"));
    for (const QString &key : WinEase::Common::scheduleActionKeys()) {
        actionCombo->addItem(WinEase::Common::scheduleActionText(key), key);
    }
    addRow->addWidget(actionCombo);

    auto *timeEdit = new QTimeEdit(widget);
    timeEdit->setObjectName(QStringLiteral("schedulerTimeEdit"));
    timeEdit->setDisplayFormat(QStringLiteral("HH:mm"));
    timeEdit->setTime(QTime(9, 0));
    addRow->addWidget(timeEdit);

    auto *repeatCheck = new QCheckBox(QStringLiteral("每天"), widget);
    repeatCheck->setObjectName(QStringLiteral("schedulerRepeatCheck"));
    repeatCheck->setChecked(true);
    addRow->addWidget(repeatCheck);

    auto *titleEdit = new QLineEdit(widget);
    titleEdit->setObjectName(QStringLiteral("schedulerTitleEdit"));
    titleEdit->setPlaceholderText(QStringLiteral("提醒内容（提醒任务必填）"));
    addRow->addWidget(titleEdit, 1);

    auto *addButton = new QPushButton(QStringLiteral("添加任务"), widget);
    addButton->setObjectName(QStringLiteral("schedulerAddButton"));
    addRow->addWidget(addButton);
    layout->addLayout(addRow);

    auto *buttonRow = new QHBoxLayout();
    auto *toggleButton = new QPushButton(QStringLiteral("启用/停用所选"), widget);
    toggleButton->setObjectName(QStringLiteral("schedulerToggleButton"));
    buttonRow->addWidget(toggleButton);
    auto *removeButton = new QPushButton(QStringLiteral("删除所选"), widget);
    removeButton->setObjectName(QStringLiteral("schedulerRemoveButton"));
    buttonRow->addWidget(removeButton);
    auto *cancelButton = new QPushButton(QStringLiteral("取消倒计时"), widget);
    cancelButton->setObjectName(QStringLiteral("schedulerCancelButton"));
    buttonRow->addWidget(cancelButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *hint = new QLabel(
        QStringLiteral("任务存的是**绝对触发时刻**：机器休眠/挂起期间错过的任务，"
                       "唤醒后会被补触发（通知里标注「补触发」），不会静默丢掉。\n"
                       "电源类任务到点后**先进可取消的倒计时**（托盘显示秒数），"
                       "倒计时结束才把请求交给提权助手。\n"
                       "有意不注册到任务计划程序（schtasks）：那会留下系统级残留，"
                       "与本项目「停用即完整还原」的约定冲突。"),
        widget);
    hint->setObjectName(QStringLiteral("schedulerHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_countdownLabel = countdownLabel;
    m_table = table;
    m_actionCombo = actionCombo;
    m_timeEdit = timeEdit;
    m_repeatCheck = repeatCheck;
    m_titleEdit = titleEdit;
    m_addButton = addButton;
    m_removeButton = removeButton;
    m_toggleButton = toggleButton;
    m_cancelButton = cancelButton;

    connect(addButton, &QPushButton::clicked, widget, [this] { addTaskFromPanel(); });
    connect(removeButton, &QPushButton::clicked, widget, [this] { removeSelectedTask(); });
    connect(toggleButton, &QPushButton::clicked, widget, [this] { toggleSelectedTask(); });
    connect(cancelButton, &QPushButton::clicked, widget,
            [this] { cancelCountdown(QStringLiteral("用户点了「取消倒计时」")); });

    refreshPanel();
    return widget;
}

void SchedulerPlugin::addTaskFromPanel()
{
    if (m_panel.isNull() || m_actionCombo.isNull() || m_timeEdit.isNull()) {
        return;
    }

    const QString action = m_actionCombo->currentData().toString();
    const QString title = m_titleEdit.isNull() ? QString() : m_titleEdit->text().trimmed();
    if (action == QLatin1String("remind") && title.isEmpty()) {
        // 一条没有内容的提醒等于没有提醒 —— 直接拒绝，别让用户以为加上了
        m_lastEvent = QStringLiteral("提醒任务需要填写提醒内容");
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    const QDateTime now = QDateTime::currentDateTime();
    ScheduleTask task;
    task.id = WinEase::Common::makeTaskId(m_tasks, now);
    task.action = action;
    task.title = title;
    task.dailyTime = m_timeEdit->time();
    task.repeatDaily = m_repeatCheck.isNull() ? true : m_repeatCheck->isChecked();
    task.nextFireAt = WinEase::Common::nextDailyFire(task.dailyTime, now);
    task.enabled = true;

    if (!task.isValid()) {
        m_lastEvent = QStringLiteral("任务信息不完整，未添加");
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    m_tasks.append(task);
    persistTasks();
    m_lastEvent = QStringLiteral("已添加：%1").arg(WinEase::Common::humanizeTask(task));
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

void SchedulerPlugin::removeSelectedTask()
{
    if (m_table.isNull()) {
        return;
    }
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_tasks.size()) {
        m_lastEvent = QStringLiteral("请先选中一条任务");
        refreshPanel();
        return;
    }

    const QString description = WinEase::Common::humanizeTask(m_tasks.at(row));
    m_tasks.removeAt(row);
    persistTasks();
    m_lastEvent = QStringLiteral("已删除：%1").arg(description);
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

void SchedulerPlugin::toggleSelectedTask()
{
    if (m_table.isNull()) {
        return;
    }
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_tasks.size()) {
        m_lastEvent = QStringLiteral("请先选中一条任务");
        refreshPanel();
        return;
    }

    ScheduleTask &task = m_tasks[row];
    task.enabled = !task.enabled;
    // 重新启用时把触发时刻重新排到未来：否则"停用了三天再打开"会立刻补触发一堆
    if (task.enabled && task.nextFireAt < QDateTime::currentDateTime()) {
        task.nextFireAt = task.repeatDaily && task.dailyTime.isValid()
                              ? WinEase::Common::nextDailyFire(task.dailyTime,
                                                               QDateTime::currentDateTime())
                              : task.nextFireAt;
    }
    persistTasks();
    m_lastEvent = QStringLiteral("%1：%2")
                      .arg(task.enabled ? QStringLiteral("已启用") : QStringLiteral("已停用"),
                           WinEase::Common::humanizeTask(task));
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

QString SchedulerPlugin::nextFireText() const
{
    const QDateTime now = QDateTime::currentDateTime();
    QStringList upcoming;
    for (const WinEase::Common::ScheduleTask &task : m_tasks) {
        if (!task.enabled || !task.nextFireAt.isValid()) {
            continue;
        }
        const qint64 seconds = now.secsTo(task.nextFireAt);
        upcoming << QStringLiteral("%1（%2）")
                        .arg(WinEase::Common::humanizeTask(task),
                             seconds <= 0 ? QStringLiteral("即将触发")
                                          : WinEase::Common::humanizeDuration(seconds));
    }

    if (upcoming.isEmpty()) {
        return QStringLiteral("没有启用的任务");
    }
    return QStringLiteral("下一个任务：%1").arg(upcoming.join(QStringLiteral("；")));
}

QString SchedulerPlugin::countdownText() const
{
    return m_countdown.describe(QDateTime::currentDateTime());
}

QString SchedulerPlugin::statusText() const
{
    const int enabled = static_cast<int>(
        std::count_if(m_tasks.cbegin(), m_tasks.cend(),
                      [](const ScheduleTask &task) { return task.enabled; }));
    QString text = QStringLiteral("共 %1 条任务（启用 %2 条）· %3")
                       .arg(m_tasks.size())
                       .arg(enabled)
                       .arg(nextFireText());
    if (!m_lastEvent.isEmpty()) {
        text += QStringLiteral(" · 最近：%1").arg(m_lastEvent);
    }
    return text;
}

void SchedulerPlugin::updateBadge()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    if (!m_countdown.isActive()) {
        svc->setTrayBadge(id(), QString(), QString(), QString());
        return;
    }

    const int remaining = m_countdown.remainingSeconds(QDateTime::currentDateTime());
    svc->setTrayBadge(id(),
                      QString::number(remaining),
                      QStringLiteral("#e67e22"),
                      QStringLiteral("%1 秒后执行%2")
                          .arg(remaining)
                          .arg(WinEase::Common::powerActionText(m_countdown.action())));
}

void SchedulerPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    if (!m_statusLabel.isNull()) {
        m_statusLabel->setText(statusText());
    }
    if (!m_countdownLabel.isNull()) {
        m_countdownLabel->setText(m_countdown.isActive()
                                      ? countdownText()
                                      : QStringLiteral("当前没有进行中的电源倒计时"));
    }

    if (!m_table.isNull()) {
        m_table->setRowCount(m_tasks.size());
        const QDateTime now = QDateTime::currentDateTime();
        for (int row = 0; row < m_tasks.size(); ++row) {
            const ScheduleTask &task = m_tasks.at(row);
            const QString when = task.repeatDaily && task.dailyTime.isValid()
                                     ? task.dailyTime.toString(QStringLiteral("HH:mm"))
                                     : QStringLiteral("仅一次");
            const QString state = !task.enabled
                                      ? QStringLiteral("已停用")
                                      : (task.nextFireAt.isValid()
                                             ? QStringLiteral("下次 %1")
                                                   .arg(WinEase::Common::humanizeDuration(
                                                       qMax<qint64>(0, now.secsTo(task.nextFireAt))))
                                             : QStringLiteral("未排定"));

            const QStringList cells{ when,
                                     WinEase::Common::scheduleActionText(task.action),
                                     task.title.isEmpty() ? QStringLiteral("—") : task.title,
                                     state };
            for (int column = 0; column < cells.size(); ++column) {
                auto *item = m_table->item(row, column);
                if (item == nullptr) {
                    m_table->setItem(row, column, new QTableWidgetItem(cells.at(column)));
                } else {
                    item->setText(cells.at(column));
                }
            }
        }
        if (m_table->currentRow() < 0 && m_tasks.size() > 0) {
            m_table->setCurrentCell(0, 0);
        }
    }

    if (!m_addButton.isNull()) {
        m_addButton->setEnabled(isEnabled());
    }
    if (!m_removeButton.isNull()) {
        m_removeButton->setEnabled(isEnabled());
    }
    if (!m_toggleButton.isNull()) {
        m_toggleButton->setEnabled(isEnabled());
    }
    if (!m_cancelButton.isNull()) {
        m_cancelButton->setEnabled(isEnabled() && m_countdown.isActive());
    }
    if (!m_actionCombo.isNull()) {
        m_actionCombo->setEnabled(isEnabled());
    }
    if (!m_timeEdit.isNull()) {
        m_timeEdit->setEnabled(isEnabled());
    }
    if (!m_repeatCheck.isNull()) {
        m_repeatCheck->setEnabled(isEnabled());
    }
    if (!m_titleEdit.isNull()) {
        m_titleEdit->setEnabled(isEnabled());
    }
}
