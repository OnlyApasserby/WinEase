#pragma once

// ============================================================================
//  PowerControl.h —— 电源动作 + "确认 → 倒计时 → 执行"状态机
//
//  给谁用：P2-06 电源面板（`monitor.power_panel`）与 P2-07 定时任务
//  （`monitor.scheduler`）—— 两边都要"倒计时可取消 + 到点执行"，各写一份必然分叉。
//
//  ⚠ 本文件**不含 Q_OBJECT**（plugins/common 的约定）：定时器由插件提供，
//     插件每秒调一次 `tick()` 即可，共享的是语义而不是事件循环。
//
//  为什么要倒计时而不是"点一下就关机"：
//      关机/重启/注销是**不可撤销地打断用户当前工作**的操作。产品的做法是
//      "先给用户一段可撤销的时间窗"（路线图 P2-06 必备项），窗口内托盘倒数、
//      面板给出「取消」，窗口结束才真正把请求交给提权助手。
//
//  为什么要"取消"独立于助手：见 `src/win32/PowerUtils.h` 的说明 ——
//      撤销通道不能依赖另一个进程还活着。
// ============================================================================

#include <QDateTime>
#include <QList>
#include <QString>

namespace WinEase {
class PluginServices;
}

namespace WinEase::Common {

/// 电源动作。**键值必须与提权助手白名单一致**（`src/helper/HelperOps.cpp` 的 `opPowerAction`）
enum class PowerAction {
    Lock = 0,   ///< 锁定会话（普通权限即可，不经过助手）
    Logoff,     ///< 注销当前用户
    Sleep,      ///< 睡眠
    Hibernate,  ///< 休眠
    Reboot,     ///< 重启
    Shutdown,   ///< 关机
};

/// 助手/配置用的稳定键（"lock"/"logoff"/"sleep"/"hibernate"/"reboot"/"shutdown"）
QString powerActionKey(PowerAction action);
/// 界面上给用户看的字（"锁定"/"注销"/…）
QString powerActionText(PowerAction action);
PowerAction powerActionFromKey(const QString &key, bool *ok = nullptr);
/// 界面按这个顺序排列动作（从轻到重）
QList<PowerAction> allPowerActions();

/// 是否"会打断当前工作"（决定二次确认的措辞与默认是否要倒计时）
bool powerActionInterruptsWork(PowerAction action);

/// 该动作是否需要提权助手（只有"锁定"不需要：`LockWorkStation` 普通权限即可）
bool powerActionNeedsElevation(PowerAction action);

// ============================================================================
//  倒计时状态机
// ============================================================================
class PowerCountdown
{
public:
    PowerCountdown() = default;

    /// 开始倒计时（`seconds <= 0` 表示"下一次 tick 立刻到点"）
    void begin(PowerAction action, int seconds, const QDateTime &now);

    /// 推进：到点返回 true（调用方据此执行动作），未到点返回 false
    bool tick(const QDateTime &now);

    /// 取消。**不在倒计时中时返回 false** —— 调用方必须如实报"当前没有可取消的操作"，
    /// 不能假装取消成功（那会让用户以为关机被撤销了）
    bool cancel();

    bool isActive() const { return m_active; }
    PowerAction action() const { return m_action; }
    QDateTime deadline() const { return m_deadline; }

    /// 剩余秒数（不在倒计时中返回 0；已到点返回 0）
    int remainingSeconds(const QDateTime &now) const;

    /// 倒计时进度文本（"3 秒后执行关机"）
    QString describe(const QDateTime &now) const;

private:
    bool m_active = false;
    PowerAction m_action = PowerAction::Shutdown;
    QDateTime m_deadline;
};

// ============================================================================
//  真正的执行（P2-06 面板与 P2-07 调度器共用同一条链路）
//
//  顺序即纪律，不能换：
//    ① `syncConfig()` —— **先把状态落盘**。用户点关机是不给程序留退路的，
//       "已启用哪些插件"必须先写进 config.ini（路线图 P2-06 的必备项）；
//    ② 锁定走本地（`LockWorkStation` 普通权限即可，不必等助手）；
//    ③ 其余动作走提权助手拿 `SE_SHUTDOWN_NAME`；**拿不到就如实报错**，
//       绝不假装已经关机（用户以为关了然后走开，比直接报错糟糕得多）。
// ============================================================================

struct PowerActionResult {
    bool ok = false;
    QString message; ///< 成功时给用户看的一句话
    QString error;   ///< 失败时的中文原因（面向用户）
};

/// @param systemTimeoutSeconds 交给系统后的反悔窗口（用户已在插件里倒计时过，这里只兜底）
PowerActionResult executePowerAction(WinEase::PluginServices *services,
                                     PowerAction action,
                                     int systemTimeoutSeconds = 5);

} // namespace WinEase::Common
