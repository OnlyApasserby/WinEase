#pragma once

// ============================================================================
//  scheduler_plugin.h —— P2-07 定时任务（monitor.scheduler）
//
//  三类动作：**提醒**（托盘气泡）/ **定时静音**（麦克风）/ **定时电源操作**
//  （关机/重启/休眠…，复用 P2-06 那套"倒计时可取消"）。
//
//  ---------------------------------------------------------------------------
//  两条关键设计（决定了"跨零点与休眠恢复不丢任务"能不能成立）：
//
//   1. **时间决策是纯函数**（`plugins/common/ScheduleEngine.*`）：
//      任务存的是**绝对触发时刻** `nextFireAt`，"每天 09:00" 只是排定它的规则。
//      调度器每秒只做一件事：`now >= nextFireAt` 就触发 —— 于是系统睡了三个小时
//      醒来，错过的那条任务照样会被捞出来执行（并如实标注"补触发"）。
//      副作用是"跨零点/跨月/休眠"全都能用固定 now 当场断言，不用等一整天。
//
//   2. **电源类任务也要经过倒计时**：定时关机不是"到点就黑屏"，而是到点后开始
//      60 秒倒数（托盘显示数字、面板可取消）。用户半夜被"定时关机"打断时，
//      至少还有一次反悔的机会。
//
//  有意裁剪：**不注册到任务计划程序（schtasks.exe）**。理由：真注册进去之后，
//  卸载插件/停用功能都会留下一条系统级计划任务 —— 而本项目所有功能的契约是
//  "停用即完整还原"（架构约定 9），做不到干净还原的功能不如不做；
//  用户要开机即生效的场景，把 WinEase 放进启动项更符合这个产品的模型。
// ============================================================================

#include "PowerControl.h"
#include "ScheduleEngine.h"
#include "sdk/IFeaturePlugin.h"

#include <QDateTime>
#include <QList>
#include <QPointer>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimeEdit;
class QTimer;
class QWidget;

class SchedulerPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "scheduler_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit SchedulerPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;

private:
    /// 每秒一次：先推进电源倒计时，再把"该触发的任务"捞出来执行
    void tick();
    void fireTask(const WinEase::Common::ScheduleTask &task);

    /// 开始电源倒计时。
    /// @return false 表示**这条任务没被处理**（已有倒计时在走）—— 调用方必须**不推进**它，
    ///         让它保持"待触发"，等当前倒计时结束后下一拍再试（绝不静默丢掉用户的任务）
    bool startPowerCountdown(const WinEase::Common::ScheduleTask &task);
    void executePower(WinEase::Common::PowerAction action);
    void cancelCountdown(const QString &reason);

    /// 定时静音：改麦克风并**回读确认**（与 P2-10 同一条纪律）
    bool applyMicrophoneMute(bool muted, QString *messageOut);

    // ---------------- 任务列表 ----------------
    void persistTasks();
    void advanceTask(const QString &taskId, const QDateTime &now);
    int indexOfTask(const QString &taskId) const;

    // ---------------- 面板动作 ----------------
    void addTaskFromPanel();
    void removeSelectedTask();
    void toggleSelectedTask();

    void refreshPanel();
    void updateBadge();
    QString statusText() const;
    QString nextFireText() const;
    QString countdownText() const;

    QList<WinEase::Common::ScheduleTask> m_tasks;
    WinEase::Common::PowerCountdown m_countdown;
    QTimer *m_tick = nullptr;

    int m_countdownSeconds = 60;
    QString m_lastEvent;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_countdownLabel;
    QPointer<QTableWidget> m_table;
    QPointer<QComboBox> m_actionCombo;
    QPointer<QTimeEdit> m_timeEdit;
    QPointer<QCheckBox> m_repeatCheck;
    QPointer<QLineEdit> m_titleEdit;
    QPointer<QPushButton> m_addButton;
    QPointer<QPushButton> m_removeButton;
    QPointer<QPushButton> m_toggleButton;
    QPointer<QPushButton> m_cancelButton;
};
