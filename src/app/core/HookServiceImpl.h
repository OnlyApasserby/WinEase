#pragma once

// ============================================================================
//  HookServiceImpl.h —— 全局输入钩子服务的实现（Windows）
//
//  架构（对应 ROADMAP P0-2）：
//
//      ┌─ 钩子线程（HookWorkerThread，带有自己的消息循环）──────────┐
//      │   SetWindowsHookEx(WH_KEYBOARD_LL / WH_MOUSE_LL)          │
//      │   系统回调 → 解析为 HookEvent → dispatchNativeEvent()      │
//      │        ├─ Direct  订阅者：本线程同步调用（有看门狗限时）    │
//      │        └─ Queued  订阅者：postEvent 到对方线程后立刻返回    │
//      └───────────────────────────────────────────────────────────┘
//                  ▲ 订阅/退订（加锁）            │ 排队投递
//      ┌─ 主线程 ───┴──────────────────────────────┴──────────────┐
//      │  AppContext 持有本对象；插件通过 PluginServices 取得接口   │
//      └──────────────────────────────────────────────────────────┘
//
//  三个关键决策及理由：
//
//   1. **钩子装在专用线程而不是主线程**
//      低层钩子的回调运行在"安装它的那个线程"上。若装在主线程，
//      任何一次 UI 卡顿都会连带拖慢全局输入，且回调里也不能安全地做耗时操作。
//      放到专用线程后，主线程的 UI 与输入路径彻底解耦。
//
//   2. **默认投递模式是"排队"，不是"同步"**
//      排队投递让钩子回调以微秒级返回，**输入永远不会被订阅者拖慢**；
//      代价是无法拦截事件。需要拦截的功能必须显式声明 HookDelivery::Direct，
//      并接受看门狗的限时约束。
//
//   3. **看门狗 + 自动降级**
//      Direct 回调超过 callbackBudgetMs() 连续若干次即被永久降级为排队投递。
//      超过系统 LowLevelHooksTimeout 时认为"钩子已被 Windows 静默摘除"，
//      主动重装（Windows 不提供任何摘除通知，这是唯一可行的检测手段）。
//
//  单例约束：全局钩子重复安装会互相干扰，因此本进程内只允许一个实例
//  （由 AppContext 持有，符合该约束）。
// ============================================================================

#include "sdk/HookService.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QPointer>
#include <QString>

#include <atomic>

namespace WinEase {

class HookWorkerThread;

class HookServiceImpl : public QObject, public HookService
{
    Q_OBJECT

public:
    explicit HookServiceImpl(QObject *parent = nullptr);
    ~HookServiceImpl() override;

    HookServiceImpl(const HookServiceImpl &) = delete;
    HookServiceImpl &operator=(const HookServiceImpl &) = delete;

    /// 启动服务（创建钩子线程并安装钩子）
    bool start();

    /// 停止服务并卸载钩子；可重复调用
    void stop();

    // ---------------- HookService ----------------
    bool isRunning() const override;
    QString lastError() const override;
    bool restart() override;
    int callbackBudgetMs() const override;
    bool subscribe(HookListener *listener, HookEventTypes types) override;
    void unsubscribe(HookListener *listener) override;
    int subscriberCount() const override;
    Stats stats() const override;
    void notifyQueuedEventProcessed(HookListener *listener) override;

    // ---------------- 内部入口（插件不应调用） ----------------

    /// 低层钩子过程解析出事件后调用；返回 true 表示吞掉该事件。
    /// 声明为 public 是因为调用方是 .cpp 内匿名命名空间的钩子过程，
    /// 无法通过 friend 声明成为友元。
    bool dispatchNativeEvent(const HookEvent &event);

Q_SIGNALS:
    /// 服务停止（钩子无法安装或已卸载）
    void serviceStopped(const QString &reason);
    /// 某个订阅者被降级为排队投递
    void subscriberDegraded(const QString &reason);

private:
    friend class HookWorkerThread;

    /// 由钩子线程在安装/卸载成功或失败时调用
    void onHooksInstalled(bool installed, const QString &error);

    /// 请求钩子线程重装钩子（怀疑已被系统摘除）
    void requestReinstall(const QString &reason);

    /// 记录一次 Direct 回调超时；达到阈值则降级
    void noteCallbackOverrun(HookListener *listener, int elapsedMs, int budgetMs);

    /// 通知全部订阅者服务已停止
    void notifyAllStopped(const QString &reason);

    void setLastError(const QString &error);

    struct Subscription {
        QPointer<HookListener> listener;
        HookEventTypes types;
        HookDelivery delivery;
        int overruns = 0;
        bool degraded = false;
    };

    /// 排队投递的积压量（仅鼠标移动需要限制，它是唯一的高频事件）
    int pendingMouseMoves(HookListener *listener) const;

    HookWorkerThread *m_thread = nullptr;

    mutable QMutex m_mutex;
    QList<Subscription> m_subscriptions;
    QHash<HookListener *, int> m_pendingMouseMoves;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_reinstallPending{false};

    int m_budgetMs = 50;
    std::atomic<int> m_systemTimeoutMs{1000};
    std::atomic<int> m_reinstallCount{0};
    std::atomic<int> m_degradedCount{0};

    // 统计计数：钩子线程写，其它线程读
    std::atomic<quint64> m_keyEvents{0};
    std::atomic<quint64> m_mouseButtonEvents{0};
    std::atomic<quint64> m_mouseMoveEvents{0};
    std::atomic<quint64> m_mouseWheelEvents{0};
    std::atomic<quint64> m_consumedEvents{0};
    std::atomic<quint64> m_droppedEvents{0};
    std::atomic<qint64> m_maxCallbackUs{0};

    mutable QMutex m_errorMutex;
    QString m_lastError;
};

} // namespace WinEase
