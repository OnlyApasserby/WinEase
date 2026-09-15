#include "power_panel_plugin.h"

#include "sdk/ElevationService.h"
#include "sdk/PluginServices.h"
#include "win32/PowerUtils.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Common::PowerAction;

/// 倒计时每秒一次：托盘上的数字是秒，慢一拍用户就看得出来
constexpr int kTickIntervalMs = 1000;
/// 交给助手时给系统留的反悔窗口（秒）——用户已经在插件里倒计时过了，这里只做兜底
constexpr int kSystemTimeoutSeconds = 5;

QString buttonObjectName(PowerAction action)
{
    return QStringLiteral("powerBtn_%1").arg(WinEase::Common::powerActionKey(action));
}

} // namespace

PowerPanelPlugin::PowerPanelPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString PowerPanelPlugin::id() const
{
    return QStringLiteral("monitor.power_panel");
}

QString PowerPanelPlugin::name() const
{
    return QStringLiteral("快速关机");
}

QString PowerPanelPlugin::description() const
{
    return QStringLiteral("锁定/注销/睡眠/休眠/重启/关机：可取消倒计时，关机前先保存插件状态");
}

QIcon PowerPanelPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::SystemMonitor);
}

WinEase::FeatureCategory PowerPanelPlugin::category() const
{
    return WinEase::FeatureCategory::SystemMonitor;
}

QStringList PowerPanelPlugin::tags() const
{
    return { QStringLiteral("关机"), QStringLiteral("重启"), QStringLiteral("休眠"),
             QStringLiteral("睡眠"), QStringLiteral("锁定"), QStringLiteral("注销"),
             QStringLiteral("guangji"), QStringLiteral("chongqi") };
}

bool PowerPanelPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence PowerPanelPlugin::defaultHotkey() const
{
    // 刻意选一个不常用的组合：关机这类动作被误触发会直接打断工作，
    // 好在它后面还有一道可取消的倒计时
    return QKeySequence(QStringLiteral("Ctrl+Shift+Alt+Q"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool PowerPanelPlugin::initialize()
{
    // 配置只在 initialize() 读一次，之后改动走面板控件（踩坑 #26）
    if (WinEase::PluginServices *svc = services()) {
        m_countdownSeconds =
            svc->configValue(id(), QStringLiteral("countdownSeconds"), 60).toInt();
        m_confirm = svc->configValue(id(), QStringLiteral("confirm"), true).toBool();
    }
    m_countdownSeconds = qBound(0, m_countdownSeconds, 600);

    if (m_tick == nullptr) {
        m_tick = new QTimer(this);
        m_tick->setInterval(kTickIntervalMs);
        connect(m_tick, &QTimer::timeout, this, [this] { tickCountdown(); });
    }

    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("电源面板已就绪（倒计时 %1 秒，二次确认 %2）")
                   .arg(m_countdownSeconds)
                   .arg(m_confirm ? QStringLiteral("开") : QStringLiteral("关")));
    return true;
}

void PowerPanelPlugin::shutdown()
{
    if (m_countdown.isActive()) {
        cancelCountdown(QStringLiteral("插件已卸载，倒计时被取消"));
    }
    if (m_tick != nullptr) {
        m_tick->stop();
    }
}

bool PowerPanelPlugin::canEnable(QString *reason) const
{
    // 这个插件**没有**"环境不支持"的硬前提：锁定是本地就能做的，
    // 其余动作缺提权助手时由 executeAction() 逐次如实报错（而不是一刀切不让启用）
    Q_UNUSED(reason)
    return true;
}

bool PowerPanelPlugin::onEnable()
{
    m_lastEvent = QStringLiteral("已就绪：选择动作后会先倒计时，随时可取消");
    Q_EMIT statusMessage(m_lastEvent);

    // ⚠ 时序（踩坑 #51）：onEnable() 期间宿主的"已启用"状态还没落定，
    //    直接刷面板会把控件留在禁用上，所以再排一发。
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void PowerPanelPlugin::onDisable()
{
    // 停用即撤销：倒计时是"即将打断用户"的承诺，功能都关了就不能还留着它
    if (m_countdown.isActive()) {
        cancelCountdown(QStringLiteral("功能已停用，倒计时被取消"));
    }
    updateBadge();
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

void PowerPanelPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("default") || action == QLatin1String("shutdown")) {
        requestAction(PowerAction::Shutdown);
    }
}

// ---------------------------------------------------------------------------
//  请求 → 倒计时 → 执行
// ---------------------------------------------------------------------------

void PowerPanelPlugin::requestAction(PowerAction action)
{
    if (!isEnabled()) {
        return;
    }

    const QString text = WinEase::Common::powerActionText(action);

    if (m_confirm) {
        // 二次确认：动作本身是"不可撤销地打断当前工作"，值得多问一句
        const QString question = WinEase::Common::powerActionInterruptsWork(action)
                                     ? QStringLiteral("确定要立即执行「%1」吗？").arg(text)
                                     : QStringLiteral("确定要「%1」吗？（解锁后可以继续）").arg(text);
        const QMessageBox::StandardButton answer =
            QMessageBox::question(m_panel.isNull() ? nullptr : m_panel.data(),
                                  QStringLiteral("确认电源操作"),
                                  question,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            m_lastEvent = QStringLiteral("已取消：%1（用户没有确认）").arg(text);
            refreshPanel();
            return;
        }
    }

    if (m_countdownSeconds <= 0) {
        executeAction(action);
        return;
    }

    m_countdown.begin(action, m_countdownSeconds, QDateTime::currentDateTime());
    m_lastEvent = QStringLiteral("已开始倒计时");
    if (m_tick != nullptr) {
        m_tick->start();
    }
    updateBadge();

    Q_EMIT statusMessage(QStringLiteral("%1 将在 %2 秒后执行，点「取消」可撤销")
                             .arg(text)
                             .arg(m_countdownSeconds));
    refreshPanel();
}

void PowerPanelPlugin::executeAction(PowerAction action)
{
    const QString text = WinEase::Common::powerActionText(action);
    WinEase::PluginServices *svc = services();

    // "落盘 → 执行 → 如实报结果"这条链路与 P2-07 调度器共用同一份实现
    // （纪律只该写一次，见 PowerControl.h 里的说明）
    const WinEase::Common::PowerActionResult result =
        WinEase::Common::executePowerAction(svc, action, kSystemTimeoutSeconds);

    if (result.ok) {
        clearLastError();
        m_lastEvent = result.message;
        Q_EMIT statusMessage(m_lastEvent);
    } else {
        setLastError(result.error);
        m_lastEvent = QStringLiteral("「%1」执行失败：%2").arg(text, result.error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
        if (svc != nullptr) {
            svc->notify(name(), m_lastEvent);
        }
        Q_EMIT statusMessage(m_lastEvent);
    }
    refreshPanel();
}

void PowerPanelPlugin::tickCountdown()
{
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_countdown.isActive()) {
        if (m_tick != nullptr) {
            m_tick->stop();
        }
        return;
    }

    if (m_countdown.tick(now)) {
        const PowerAction action = m_countdown.action();
        if (m_tick != nullptr) {
            m_tick->stop();
        }
        updateBadge();
        executeAction(action);
        return;
    }

    updateBadge();
    refreshPanel();
}

void PowerPanelPlugin::cancelCountdown(const QString &reason)
{
    // ⚠ 取消要如实：不在倒计时中时 `cancel()` 返回 false，界面上的「取消」就该是禁用的，
    //    绝不能报"已取消"让用户以为关机被撤销了
    if (!m_countdown.cancel()) {
        m_lastEvent = QStringLiteral("当前没有可取消的操作");
        refreshPanel();
        return;
    }

    if (m_tick != nullptr) {
        m_tick->stop();
    }
    updateBadge();
    m_lastEvent = QStringLiteral("已取消：%1")
                      .arg(reason.isEmpty() ? QStringLiteral("用户点了取消") : reason);
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QString PowerPanelPlugin::countdownText() const
{
    return m_countdown.describe(QDateTime::currentDateTime());
}

QString PowerPanelPlugin::helperText() const
{
    WinEase::PluginServices *svc = services();
    WinEase::ElevationService *elevation = svc != nullptr ? svc->elevationService() : nullptr;

    const QString privilege =
        WinEase::Win32::hasShutdownPrivilege()
            ? QStringLiteral("本机令牌持有 SeShutdownPrivilege")
            : QStringLiteral("本机令牌**不**持有 SeShutdownPrivilege（关机类动作必须经助手）");

    if (elevation == nullptr) {
        return QStringLiteral("提权助手：不可用 —— 关机/重启/休眠会如实报错，"
                              "「锁定」不受影响。%1")
            .arg(privilege);
    }
    return QStringLiteral("提权助手：%1 · %2").arg(elevation->statusText(), privilege);
}

QString PowerPanelPlugin::statusText() const
{
    QString text = m_lastEvent;
    if (m_countdown.isActive()) {
        text = QStringLiteral("%1（%2）").arg(countdownText(), m_lastEvent);
    }
    return text.isEmpty() ? QStringLiteral("待命") : text;
}

void PowerPanelPlugin::updateBadge()
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
                      QStringLiteral("#e74c3c"),
                      QStringLiteral("%1 秒后执行%2")
                          .arg(remaining)
                          .arg(WinEase::Common::powerActionText(m_countdown.action())));
}

QWidget *PowerPanelPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("powerPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("powerStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *grid = new QGridLayout();
    int index = 0;
    for (const PowerAction action : WinEase::Common::allPowerActions()) {
        auto *button = new QPushButton(WinEase::Common::powerActionText(action), widget);
        button->setObjectName(buttonObjectName(action));
        connect(button, &QPushButton::clicked, widget, [this, action] { requestAction(action); });
        grid->addWidget(button, index / 3, index % 3);
        m_actionButtons.insert(static_cast<int>(action), button);
        ++index;
    }
    layout->addLayout(grid);

    auto *countdownRow = new QHBoxLayout();
    countdownRow->addWidget(new QLabel(QStringLiteral("倒计时："), widget));
    auto *countdownSpin = new QSpinBox(widget);
    countdownSpin->setObjectName(QStringLiteral("powerCountdownSpin"));
    countdownSpin->setRange(0, 600);
    countdownSpin->setSingleStep(5);
    countdownSpin->setSuffix(QStringLiteral(" 秒"));
    countdownSpin->setValue(m_countdownSeconds);
    connect(countdownSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_countdownSeconds = qBound(0, value, 600);
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("countdownSeconds"), m_countdownSeconds);
            svc->syncConfig();
        }
        refreshPanel();
    });
    countdownRow->addWidget(countdownSpin);
    countdownRow->addSpacing(12);
    auto *cancelButton = new QPushButton(QStringLiteral("取消倒计时"), widget);
    cancelButton->setObjectName(QStringLiteral("powerCancelButton"));
    connect(cancelButton, &QPushButton::clicked, widget,
            [this] { cancelCountdown(QStringLiteral("用户点了「取消倒计时」")); });
    countdownRow->addWidget(cancelButton);
    countdownRow->addStretch(1);
    layout->addLayout(countdownRow);

    auto *helperLabel = new QLabel(widget);
    helperLabel->setObjectName(QStringLiteral("powerHelperLabel"));
    helperLabel->setWordWrap(true);
    layout->addWidget(helperLabel);

    auto *hint = new QLabel(
        QStringLiteral("快捷键：%1 直接发起关机倒计时（可在主界面「快捷键」里修改）。\n"
                       "除「锁定」外的动作需要管理员权限（`SE_SHUTDOWN_NAME`），按项目决策统一交给"
                       "提权助手执行；助手不可用时本插件**如实报错**，不会假装已经关机。\n"
                       "「取消倒计时」走的是本地实现（不经过助手）—— 机器正在关闭的时候，"
                       "撤销通道不能依赖另一个进程还活着。倒计时结束时若系统已经受理，"
                       "还可以用「shutdown /a」再撤销一次。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText)),
        widget);
    hint->setObjectName(QStringLiteral("powerHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_helperLabel = helperLabel;
    m_hintLabel = hint;
    m_countdownSpin = countdownSpin;
    m_cancelButton = cancelButton;

    refreshPanel();
    return widget;
}

void PowerPanelPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    const bool running = isEnabled();
    const bool counting = m_countdown.isActive();

    if (!m_statusLabel.isNull()) {
        m_statusLabel->setText(statusText());
    }
    if (!m_helperLabel.isNull()) {
        m_helperLabel->setText(helperText());
    }

    // 倒计时期间不让再点动作按钮：否则会出现"两个倒计时抢一个屏"的混乱
    for (QPointer<QPushButton> &button : m_actionButtons) {
        if (!button.isNull()) {
            button->setEnabled(running && !counting);
        }
    }

    if (!m_countdownSpin.isNull()) {
        m_countdownSpin->setEnabled(running);
        if (m_countdownSpin->value() != m_countdownSeconds) {
            QSignalBlocker blocker(m_countdownSpin.data());
            m_countdownSpin->setValue(m_countdownSeconds);
        }
    }
    if (!m_cancelButton.isNull()) {
        // 「取消」只在真有东西可取消时才可用 —— 用户点不出"取消了但没发生什么"的困惑
        m_cancelButton->setEnabled(running && counting);
    }
}
