#include "core/HookServiceImpl.h"

#include "win32/RegistryUtils.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QSemaphore>
#include <QThread>
#include <QVariant>

#include <windows.h>

namespace WinEase {

namespace {

/// 发给钩子线程的自定义消息
constexpr UINT kMsgReinstallHooks = WM_APP + 0x51;
constexpr UINT kMsgQuitHookThread = WM_APP + 0x52;

/// 无注册表配置时的系统钩子超时默认值（Win8 及以后为 1000ms，更早为 300ms）
constexpr int kDefaultSystemTimeoutMs = 1000;

/// 派发快照容量上限（订阅者数量远超此值时超出部分被忽略）
constexpr int kMaxDispatchTargets = 32;

/// Direct 回调连续超预算多少次后降级为排队投递
constexpr int kMaxCallbackOverruns = 3;

/// 每个订阅者允许积压的鼠标移动事件上限。
/// 鼠标移动是唯一的高频事件（可达 1000Hz），订阅者长时间不处理时不设上限会持续吃内存。
constexpr int kMaxPendingMouseMoves = 256;

/// 时间换算（统一用微秒做中间单位，避免浮点误差）
constexpr qint64 kNanosecondsPerMicrosecond = 1000;
constexpr qint64 kMicrosecondsPerMillisecond = 1000;

/// 当前服务实例。
///
/// 低层钩子回调是自由函数，必须通过它找回服务对象。本服务按设计是进程内单例：
/// 全局钩子重复安装会互相干扰（同一事件被回调多次、回调顺序不可控），
/// 因此该约束是刻意的，而不是实现偷懒。
HookServiceImpl *g_service = nullptr;

/// 把 Win32 低层键盘消息解析为 HookEvent
bool parseKeyboardEvent(WPARAM wParam, LPARAM lParam, HookEvent *eventOut)
{
    const auto *data = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
    if (data == nullptr) {
        return false;
    }

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp) {
        return false;
    }

    HookEvent event;
    event.type = HookEventKey;
    event.modifiers = static_cast<quint32>(HookService::currentModifiers());
    event.key.virtualKey = data->vkCode;
    event.key.scanCode = data->scanCode;
    event.key.pressed = isDown;
    event.key.isExtended = (data->flags & LLKHF_EXTENDED) != 0;
    event.key.isSystem = (wParam == WM_SYSKEYDOWN || wParam == WM_SYSKEYUP);
    event.key.isInjected = (data->flags & LLKHF_INJECTED) != 0;
    event.key.modifiers = static_cast<HookModifiers>(event.modifiers);
    event.key.timeStamp = data->time;

    // 键盘事件本身不带坐标，但很多功能需要"按键发生时的鼠标位置"
    POINT cursor{};
    if (::GetCursorPos(&cursor) != FALSE) {
        event.cursorPos = QPoint(cursor.x, cursor.y);
    }

    *eventOut = event;
    return true;
}

/// 把 Win32 低层鼠标消息解析为 HookEvent
bool parseMouseEvent(WPARAM wParam, LPARAM lParam, HookEvent *eventOut)
{
    const auto *data = reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
    if (data == nullptr) {
        return false;
    }

    const QPoint position(data->pt.x, data->pt.y);
    const bool isInjected = (data->flags & LLMHF_INJECTED) != 0;

    HookEvent event;
    event.modifiers = static_cast<quint32>(HookService::currentModifiers());
    event.cursorPos = position;

    switch (wParam) {
    case WM_MOUSEMOVE:
        event.type = HookEventMouseMove;
        event.mouseMove.isInjected = isInjected;
        event.mouseMove.screenPos = position;
        event.mouseMove.timeStamp = data->time;
        break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        event.type = HookEventMouseButton;

        HookMouseButton button = HookMouseButton::Left;
        switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            button = HookMouseButton::Left;
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            button = HookMouseButton::Right;
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            button = HookMouseButton::Middle;
            break;
        default:
            // 侧键：mouseData 高字 1 = X1（后退），2 = X2（前进）
            button = (HIWORD(data->mouseData) == XBUTTON1) ? HookMouseButton::X1
                                                           : HookMouseButton::X2;
            break;
        }

        const bool pressed = (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN
                              || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN);
        event.mouseButton.button = button;
        event.mouseButton.pressed = pressed;
        event.mouseButton.isInjected = isInjected;
        event.mouseButton.modifiers = static_cast<HookModifiers>(event.modifiers);
        event.mouseButton.screenPos = position;
        event.mouseButton.timeStamp = data->time;
        break;
    }

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        event.type = HookEventMouseWheel;
        event.mouseWheel.delta = static_cast<short>(HIWORD(data->mouseData));
        event.mouseWheel.horizontal = (wParam == WM_MOUSEHWHEEL);
        event.mouseWheel.isInjected = isInjected;
        event.mouseWheel.modifiers = static_cast<HookModifiers>(event.modifiers);
        event.mouseWheel.screenPos = position;
        event.mouseWheel.timeStamp = data->time;
        break;

    default:
        return false; // 不关心其它鼠标消息
    }

    *eventOut = event;
    return true;
}

/// 低层键盘钩子过程（运行在钩子线程）
LRESULT CALLBACK lowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    // code < 0 时必须直接转交，不允许做任何处理
    if (code < 0 || g_service == nullptr) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    HookEvent event;
    if (!parseKeyboardEvent(wParam, lParam, &event)) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (g_service->dispatchNativeEvent(event)) {
        return 1; // 吞掉：不再传递给系统
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

/// 低层鼠标钩子过程（运行在钩子线程）
LRESULT CALLBACK lowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code < 0 || g_service == nullptr) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    HookEvent event;
    if (!parseMouseEvent(wParam, lParam, &event)) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (g_service->dispatchNativeEvent(event)) {
        return 1;
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace

// ============================================================================
//  钩子线程
//
//  刻意**不声明 Q_OBJECT**：它不需要信号槽，只需要一个带消息循环的线程。
//  这样可以避免为 .cpp 内的 QObject 子类生成 moc 带来的一堆构建配置问题。
// ============================================================================

class HookWorkerThread : public QThread
{
public:
    explicit HookWorkerThread(HookServiceImpl *owner)
        : QThread()
        , m_owner(owner)
    {
        setObjectName(QStringLiteral("WinEaseHookThread"));
    }

    ~HookWorkerThread() override
    {
        requestStop();
        wait(3000);
    }

    HookWorkerThread(const HookWorkerThread &) = delete;
    HookWorkerThread &operator=(const HookWorkerThread &) = delete;

    /// 等待钩子安装流程结束（成功或失败）
    bool waitReady(int timeoutMs) { return m_ready.tryAcquire(1, timeoutMs); }

    bool hooksInstalled() const { return m_keyboardHook != nullptr || m_mouseHook != nullptr; }

    QString installError() const { return m_installError; }

    void requestStop()
    {
        if (m_threadId != 0) {
            ::PostThreadMessageW(m_threadId, kMsgQuitHookThread, 0, 0);
        }
    }

    void requestReinstall()
    {
        if (m_threadId != 0) {
            ::PostThreadMessageW(m_threadId, kMsgReinstallHooks, 0, 0);
        }
    }

protected:
    void run() override
    {
        m_threadId = ::GetCurrentThreadId();

        // 必须先触发一次消息队列的创建，否则其它线程的 PostThreadMessage 会失败
        // （对"还没有消息队列的线程"调用 PostThreadMessage 返回 ERROR_INVALID_THREAD_ID）
        MSG message{};
        ::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        const bool installed = installHooks();
        m_owner->onHooksInstalled(installed, m_installError);
        m_ready.release();

        if (!installed) {
            m_threadId = 0;
            return;
        }

        // 线程消息循环：低层钩子的回调依赖它才能被投递
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == kMsgQuitHookThread) {
                break;
            }
            if (message.message == kMsgReinstallHooks) {
                uninstallHooks();
                installHooks();
                continue;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        uninstallHooks();
        m_threadId = 0;
    }

private:
    bool installHooks()
    {
        g_service = m_owner;

        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        m_keyboardHook = ::SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc, instance, 0);
        m_mouseHook = ::SetWindowsHookExW(WH_MOUSE_LL, lowLevelMouseProc, instance, 0);

        if (m_keyboardHook == nullptr || m_mouseHook == nullptr) {
            m_installError = QStringLiteral("安装全局钩子失败（错误码 %1）：键盘钩子 %2，鼠标钩子 %3")
                                 .arg(::GetLastError())
                                 .arg(m_keyboardHook != nullptr ? QStringLiteral("成功")
                                                                : QStringLiteral("失败"))
                                 .arg(m_mouseHook != nullptr ? QStringLiteral("成功")
                                                             : QStringLiteral("失败"));
            uninstallHooks();
            return false;
        }

        m_installError.clear();
        return true;
    }

    void uninstallHooks()
    {
        if (m_keyboardHook != nullptr) {
            ::UnhookWindowsHookEx(m_keyboardHook);
            m_keyboardHook = nullptr;
        }
        if (m_mouseHook != nullptr) {
            ::UnhookWindowsHookEx(m_mouseHook);
            m_mouseHook = nullptr;
        }
        g_service = nullptr;
    }

    HookServiceImpl *m_owner = nullptr;
    QSemaphore m_ready;
    DWORD m_threadId = 0;
    HHOOK m_keyboardHook = nullptr;
    HHOOK m_mouseHook = nullptr;
    QString m_installError;
};

// ============================================================================
//  HookServiceImpl
// ============================================================================

HookServiceImpl::HookServiceImpl(QObject *parent)
    : QObject(parent)
{
}

HookServiceImpl::~HookServiceImpl()
{
    stop();

    // 服务即将消失：断开所有订阅者的反向指针，
    // 否则它们析构时会调用本对象的 unsubscribe（悬空访问）
    QList<HookListener *> listeners;
    {
        const QMutexLocker locker(&m_mutex);
        for (const Subscription &subscription : m_subscriptions) {
            if (subscription.listener.data() != nullptr) {
                listeners.append(subscription.listener.data());
            }
        }
        m_subscriptions.clear();
        m_pendingMouseMoves.clear();
    }
    for (HookListener *listener : std::as_const(listeners)) {
        listener->setOwningService(nullptr);
    }
}

bool HookServiceImpl::start()
{
    if (m_running.load()) {
        return true;
    }
    if (m_thread != nullptr) {
        stop();
    }

    // 系统钩子超时：超过它 Windows 会**静默摘除**钩子且不提供任何通知。
    // 读取注册表配置，把看门狗的硬阈值设成与实际系统配置一致。
    const QVariant configured = Win32::readValue(Win32::RegistryRoot::CurrentUser,
                                                 QStringLiteral("Control Panel\\Desktop"),
                                                 QStringLiteral("LowLevelHooksTimeout"));
    int timeoutMs = kDefaultSystemTimeoutMs;
    if (configured.isValid()) {
        const int milliseconds = configured.toInt();
        if (milliseconds > 0) {
            timeoutMs = milliseconds;
        }
    }
    m_systemTimeoutMs.store(timeoutMs);

    auto *thread = new HookWorkerThread(this);
    m_thread = thread;
    thread->start();

    if (!thread->waitReady(5000)) {
        setLastError(QStringLiteral("钩子线程启动超时"));
        stop();
        return false;
    }

    if (!thread->hooksInstalled()) {
        setLastError(thread->installError());
        stop();
        return false;
    }

    m_running.store(true);
    setLastError(QString());
    return true;
}

void HookServiceImpl::stop()
{
    if (m_thread != nullptr) {
        m_thread->requestStop();
        m_thread->wait(3000);
        delete m_thread;
        m_thread = nullptr;
    }

    if (m_running.exchange(false)) {
        notifyAllStopped(QStringLiteral("钩子服务已停止"));
    }
}

void HookServiceImpl::onHooksInstalled(bool installed, const QString &error)
{
    if (!installed) {
        setLastError(error);
        Q_EMIT serviceStopped(error);
    }
}

bool HookServiceImpl::isRunning() const
{
    return m_running.load();
}

QString HookServiceImpl::lastError() const
{
    const QMutexLocker locker(&m_errorMutex);
    return m_lastError;
}

void HookServiceImpl::setLastError(const QString &error)
{
    const QMutexLocker locker(&m_errorMutex);
    m_lastError = error;
}

int HookServiceImpl::callbackBudgetMs() const
{
    return m_budgetMs;
}

bool HookServiceImpl::restart()
{
    stop();
    return start();
}

// ---------------------------------------------------------------------------
//  订阅
// ---------------------------------------------------------------------------

bool HookServiceImpl::subscribe(HookListener *listener, HookEventTypes types)
{
    if (listener == nullptr) {
        return false;
    }

    {
        const QMutexLocker locker(&m_mutex);
        listener->setOwningService(this);

        bool updated = false;
        for (Subscription &subscription : m_subscriptions) {
            if (subscription.listener.data() != listener) {
                continue;
            }
            // 重新订阅：重置投递模式与超时计数（给被降级的订阅者一次改正机会）
            subscription.types = types;
            subscription.delivery = listener->delivery();
            subscription.overruns = 0;
            subscription.degraded = false;
            updated = true;
            break;
        }

        if (!updated) {
            Subscription subscription;
            subscription.listener = listener;
            subscription.types = types;
            subscription.delivery = listener->delivery();
            m_subscriptions.append(subscription);
        }
    }

    // 服务未运行时订阅也会被记录，待 start() 后自动生效
    return m_running.load();
}

void HookServiceImpl::unsubscribe(HookListener *listener)
{
    if (listener == nullptr) {
        return;
    }

    const QMutexLocker locker(&m_mutex);
    for (int index = m_subscriptions.size() - 1; index >= 0; --index) {
        if (m_subscriptions.at(index).listener.data() == listener) {
            m_subscriptions.removeAt(index);
        }
    }
    m_pendingMouseMoves.remove(listener);
    listener->setOwningService(nullptr);
}

int HookServiceImpl::subscriberCount() const
{
    const QMutexLocker locker(&m_mutex);
    int count = 0;
    for (const Subscription &subscription : m_subscriptions) {
        if (subscription.listener.data() != nullptr) {
            ++count;
        }
    }
    return count;
}

int HookServiceImpl::pendingMouseMoves(HookListener *listener) const
{
    // ⚠ 必须在已持有 m_mutex 的情况下调用（QMutex 非递归，内部不能再加锁）
    return m_pendingMouseMoves.value(listener, 0);
}

void HookServiceImpl::notifyQueuedEventProcessed(HookListener *listener)
{
    if (listener == nullptr) {
        return;
    }

    const QMutexLocker locker(&m_mutex);
    const auto iterator = m_pendingMouseMoves.find(listener);
    if (iterator == m_pendingMouseMoves.end()) {
        return;
    }
    if (iterator.value() <= 1) {
        m_pendingMouseMoves.erase(iterator);
    } else {
        --iterator.value();
    }
}

// ---------------------------------------------------------------------------
//  事件派发（运行在钩子线程）
// ---------------------------------------------------------------------------

bool HookServiceImpl::dispatchNativeEvent(const HookEvent &event)
{
    // ---------------- 计数 ----------------
    switch (event.type) {
    case HookEventKey:
        m_keyEvents.fetch_add(1, std::memory_order_relaxed);
        break;
    case HookEventMouseButton:
        m_mouseButtonEvents.fetch_add(1, std::memory_order_relaxed);
        break;
    case HookEventMouseMove:
        m_mouseMoveEvents.fetch_add(1, std::memory_order_relaxed);
        break;
    case HookEventMouseWheel:
        m_mouseWheelEvents.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }

    // ---------------- 构建派发快照 ----------------
    //
    // 锁内只做"挑选订阅者"这件极快的事，把指针抄进栈上数组，**解锁后**才调用插件代码。
    // 这样插件回调再慢也不会阻塞主线程的 subscribe/unsubscribe，
    // 同时热路径（鼠标移动）没有任何堆分配。
    HookListener *directTargets[kMaxDispatchTargets] = {};
    HookListener *queuedTargets[kMaxDispatchTargets] = {};
    int directCount = 0;
    int queuedCount = 0;

    {
        const QMutexLocker locker(&m_mutex);
        const bool isMouseMove = (event.type == HookEventMouseMove);

        for (const Subscription &subscription : m_subscriptions) {
            HookListener *listener = subscription.listener.data();
            if (listener == nullptr || !subscription.types.testFlag(event.type)) {
                continue;
            }

            if (subscription.delivery == HookDelivery::Direct) {
                if (directCount < kMaxDispatchTargets) {
                    directTargets[directCount++] = listener;
                }
                continue;
            }

            // 排队投递只对高频的鼠标移动做积压保护
            if (isMouseMove) {
                const int pending = pendingMouseMoves(listener);
                if (pending >= kMaxPendingMouseMoves) {
                    m_droppedEvents.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                m_pendingMouseMoves[listener] = pending + 1;
            }

            if (queuedCount < kMaxDispatchTargets) {
                queuedTargets[queuedCount++] = listener;
            }
        }
    }

    // ---------------- Direct：同步调用 + 看门狗 ----------------
    bool consumed = false;
    const qint64 systemTimeoutUs =
        static_cast<qint64>(m_systemTimeoutMs.load()) * kMicrosecondsPerMillisecond;
    const qint64 budgetUs = static_cast<qint64>(m_budgetMs) * kMicrosecondsPerMillisecond;

    for (int index = 0; index < directCount; ++index) {
        HookListener *listener = directTargets[index];

        QElapsedTimer timer;
        timer.start();
        const bool handled = listener->dispatchHookEvent(event);
        const qint64 elapsedUs = timer.nsecsElapsed() / kNanosecondsPerMicrosecond;

        m_maxCallbackUs.store(qMax(m_maxCallbackUs.load(std::memory_order_relaxed), elapsedUs),
                              std::memory_order_relaxed);

        if (handled) {
            consumed = true;
        }

        if (elapsedUs > systemTimeoutUs) {
            // 超过系统钩子超时：Windows 极可能已把钩子摘掉，主动重装
            requestReinstall(QStringLiteral("事件回调耗时 %1ms，超过系统钩子超时 %2ms")
                                 .arg(elapsedUs / kMicrosecondsPerMillisecond)
                                 .arg(m_systemTimeoutMs.load()));
        }
        if (elapsedUs > budgetUs) {
            noteCallbackOverrun(listener,
                                static_cast<int>(elapsedUs / kMicrosecondsPerMillisecond),
                                m_budgetMs);
        }
    }

    if (consumed) {
        m_consumedEvents.fetch_add(1, std::memory_order_relaxed);
    }

    // ---------------- Queued：投递到订阅者线程 ----------------
    //
    // 用 lambda 形式的 QueuedConnection，而不是自定义 QEvent 子类：
    //   * 不需要跨模块统一事件类型 id（QEvent::registerEventType 在每个模块返回不同值，
    //     主程序创建的 QEvent 插件根本认不出来——这是个很容易踩的跨 DLL 陷阱）
    //   * 不需要 qRegisterMetaType
    //   * 接收者析构时 Qt 会自动撤销未执行的调用，不会悬空访问
    for (int index = 0; index < queuedCount; ++index) {
        HookListener *listener = queuedTargets[index];
        QMetaObject::invokeMethod(
            listener,
            [listener, event]() {
                listener->dispatchHookEvent(event);
                listener->reportQueuedProcessed();
            },
            Qt::QueuedConnection);
    }

    return consumed;
}

void HookServiceImpl::noteCallbackOverrun(HookListener *listener, int elapsedMs, int budgetMs)
{
    QString reason;
    {
        const QMutexLocker locker(&m_mutex);
        for (Subscription &subscription : m_subscriptions) {
            if (subscription.listener.data() != listener) {
                continue;
            }
            if (subscription.degraded) {
                return; // 已降级，不再重复处理
            }
            ++subscription.overruns;
            if (subscription.overruns >= kMaxCallbackOverruns) {
                subscription.degraded = true;
                subscription.delivery = HookDelivery::Queued;
                m_degradedCount.fetch_add(1, std::memory_order_relaxed);
                reason = QStringLiteral("同步回调耗时 %1ms（预算 %2ms）已连续 %3 次超时，"
                                        "为保证全局输入不被阻塞，已自动降级为排队投递")
                             .arg(elapsedMs)
                             .arg(budgetMs)
                             .arg(subscription.overruns);
            }
            break;
        }
    }

    if (reason.isEmpty()) {
        return;
    }

    // 信号会排队到接收者线程；对 listener 虚函数的调用同样要搬回它自己的线程
    Q_EMIT subscriberDegraded(reason);
    QMetaObject::invokeMethod(
        listener,
        [listener, reason]() { listener->notifyDegraded(reason); },
        Qt::QueuedConnection);
}

void HookServiceImpl::notifyAllStopped(const QString &reason)
{
    QList<HookListener *> listeners;
    {
        const QMutexLocker locker(&m_mutex);
        listeners.reserve(m_subscriptions.size());
        for (const Subscription &subscription : m_subscriptions) {
            if (subscription.listener.data() != nullptr) {
                listeners.append(subscription.listener.data());
            }
        }
    }

    Q_EMIT serviceStopped(reason);
    for (HookListener *listener : std::as_const(listeners)) {
        QMetaObject::invokeMethod(
            listener,
            [listener, reason]() { listener->notifyServiceStopped(reason); },
            Qt::QueuedConnection);
    }
}

void HookServiceImpl::requestReinstall(const QString &reason)
{
    // 只允许一个重装请求在途，避免回调持续超时时反复重装
    if (m_reinstallPending.exchange(true)) {
        return;
    }

    m_reinstallCount.fetch_add(1, std::memory_order_relaxed);
    setLastError(reason);

    if (m_thread != nullptr) {
        m_thread->requestReinstall();
    }

    // 下一轮事件循环放开闸门（重装请求已投递给钩子线程）
    QMetaObject::invokeMethod(
        this,
        [this]() { m_reinstallPending.store(false); },
        Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
//  统计
// ---------------------------------------------------------------------------

HookService::Stats HookServiceImpl::stats() const
{
    Stats result;
    result.keyEvents = m_keyEvents.load();
    result.mouseButtonEvents = m_mouseButtonEvents.load();
    result.mouseMoveEvents = m_mouseMoveEvents.load();
    result.mouseWheelEvents = m_mouseWheelEvents.load();
    result.consumedEvents = m_consumedEvents.load();
    result.droppedEvents = m_droppedEvents.load();
    result.maxCallbackMs =
        static_cast<double>(m_maxCallbackUs.load()) / static_cast<double>(kMicrosecondsPerMillisecond);
    result.degradedSubscribers = m_degradedCount.load();
    result.reinstallCount = m_reinstallCount.load();
    return result;
}

} // namespace WinEase
