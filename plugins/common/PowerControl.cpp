#include "PowerControl.h"

#include "sdk/ElevationService.h"
#include "sdk/PluginServices.h"
#include "win32/PowerUtils.h"

namespace WinEase::Common {

QString powerActionKey(PowerAction action)
{
    switch (action) {
    case PowerAction::Lock:      return QStringLiteral("lock");
    case PowerAction::Logoff:    return QStringLiteral("logoff");
    case PowerAction::Sleep:     return QStringLiteral("sleep");
    case PowerAction::Hibernate: return QStringLiteral("hibernate");
    case PowerAction::Reboot:    return QStringLiteral("reboot");
    case PowerAction::Shutdown:  break;
    }
    return QStringLiteral("shutdown");
}

QString powerActionText(PowerAction action)
{
    switch (action) {
    case PowerAction::Lock:      return QStringLiteral("锁定");
    case PowerAction::Logoff:    return QStringLiteral("注销");
    case PowerAction::Sleep:     return QStringLiteral("睡眠");
    case PowerAction::Hibernate: return QStringLiteral("休眠");
    case PowerAction::Reboot:    return QStringLiteral("重启");
    case PowerAction::Shutdown:  break;
    }
    return QStringLiteral("关机");
}

PowerAction powerActionFromKey(const QString &key, bool *ok)
{
    if (ok != nullptr) {
        *ok = true;
    }
    if (key == QLatin1String("lock"))    { return PowerAction::Lock; }
    if (key == QLatin1String("logoff"))  { return PowerAction::Logoff; }
    if (key == QLatin1String("sleep"))   { return PowerAction::Sleep; }
    if (key == QLatin1String("hibernate")) { return PowerAction::Hibernate; }
    if (key == QLatin1String("reboot"))  { return PowerAction::Reboot; }
    if (key == QLatin1String("shutdown")) { return PowerAction::Shutdown; }

    if (ok != nullptr) {
        *ok = false;
    }
    return PowerAction::Shutdown;
}

QList<PowerAction> allPowerActions()
{
    return { PowerAction::Lock, PowerAction::Logoff, PowerAction::Sleep,
             PowerAction::Hibernate, PowerAction::Reboot, PowerAction::Shutdown };
}

bool powerActionInterruptsWork(PowerAction action)
{
    // 锁定会立刻把用户挡在登录界面外，但它不丢任何状态（解锁即可继续）→ 算"轻"
    return action != PowerAction::Lock;
}

bool powerActionNeedsElevation(PowerAction action)
{
    return action != PowerAction::Lock;
}

// ============================================================================
//  PowerCountdown
// ============================================================================

void PowerCountdown::begin(PowerAction action, int seconds, const QDateTime &now)
{
    m_action = action;
    m_active = true;
    m_deadline = now.addSecs(qMax(0, seconds));
}

bool PowerCountdown::tick(const QDateTime &now)
{
    if (!m_active) {
        return false;
    }
    if (now < m_deadline) {
        return false;
    }
    // 到点即失效：执行与否由调用方决定，但"倒计时"这件事结束了
    m_active = false;
    return true;
}

bool PowerCountdown::cancel()
{
    if (!m_active) {
        return false;
    }
    m_active = false;
    return true;
}

int PowerCountdown::remainingSeconds(const QDateTime &now) const
{
    if (!m_active || !m_deadline.isValid()) {
        return 0;
    }
    const qint64 seconds = now.secsTo(m_deadline);
    return seconds > 0 ? static_cast<int>(seconds) : 0;
}

QString PowerCountdown::describe(const QDateTime &now) const
{
    if (!m_active) {
        return QString();
    }
    return QStringLiteral("%1 秒后执行%2（可取消）")
        .arg(remainingSeconds(now))
        .arg(powerActionText(m_action));
}

// ============================================================================
//  执行
// ============================================================================

PowerActionResult executePowerAction(WinEase::PluginServices *services,
                                     PowerAction action,
                                     int systemTimeoutSeconds)
{
    PowerActionResult result;
    const QString text = powerActionText(action);

    // ① 先把状态落盘（用户点关机不给程序留退路，配置必须先写进去）
    if (services != nullptr) {
        services->syncConfig();
    }

    // ② 锁定：本地做（普通权限即可，也不依赖另一个进程）
    if (!powerActionNeedsElevation(action)) {
        QString error;
        if (WinEase::Win32::lockWorkstation(&error)) {
            result.ok = true;
            result.message = QStringLiteral("已锁定会话");
        } else {
            result.error = error;
        }
        return result;
    }

    // ③ 其余动作经提权助手拿 SE_SHUTDOWN_NAME；没有助手就如实报错（fail-closed）
    WinEase::ElevationService *elevation = services != nullptr ? services->elevationService() : nullptr;
    if (elevation == nullptr) {
        result.error = QStringLiteral("提权助手不可用，「%1」无法执行"
                                      "（请在主界面启用提权助手，或允许 UAC 授权）")
                           .arg(text);
        return result;
    }

    QVariantMap arguments;
    arguments.insert(QStringLiteral("action"), powerActionKey(action));
    arguments.insert(QStringLiteral("timeout"), qMax(0, systemTimeoutSeconds));
    arguments.insert(QStringLiteral("message"),
                     QStringLiteral("WinEase：%1（%2 秒后，可用 shutdown /a 取消）")
                         .arg(text)
                         .arg(qMax(0, systemTimeoutSeconds)));

    const WinEase::ElevationResult elevated =
        elevation->execute(QStringLiteral("powerAction"), arguments);
    if (!elevated.isSuccess()) {
        result.error = elevated.error;
        return result;
    }

    result.ok = true;
    result.message = QStringLiteral("已请求%1（系统 %2 秒后执行）").arg(text).arg(systemTimeoutSeconds);
    return result;
}

} // namespace WinEase::Common
