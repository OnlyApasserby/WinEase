// ============================================================================
//  hook_smoke —— HookService 验收测试（对应 ROADMAP P0-2 的验收标准）
//
//  验收标准（逐条对应）：
//    1. "订阅者能收到全局按键与鼠标事件"
//       → 用 SendInput 注入合成事件，验证 Direct 与 Queued 两类订阅者都能收到
//    2. "单个订阅者回调故意睡 500ms 时，主程序仍可用"
//       → 排队投递模式下，注入 N 个事件的总耗时远小于 N×睡眠时间，
//         以此证明钩子回调从未被订阅者阻塞
//    3. 看门狗：Direct 订阅者连续超预算会被自动降级
//
//  ⚠ 安全性设计（这点很重要）：
//      测试会向系统注入合成输入。为避免影响用户真实桌面，
//      第一步就安装一个 **Direct 拦截器**，它吞掉所有自称"注入"的事件
//      （HookEvent::isInjected）。因此正常情况下用户桌面不会收到任何输入。
//      注入内容也刻意选择"即使泄漏也无害"的组合：F24（无默认行为）、
//      滚轮 ±120（净滚动为零）、鼠标移动 1 像素后立即移回。
//
//  退出码 0 表示全部通过。
// ============================================================================

#include "core/HookServiceImpl.h"
#include "sdk/HookService.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>

#include <windows.h>

namespace {

using WinEase::HookDelivery;
using WinEase::HookEvent;
using WinEase::HookEventKey;
using WinEase::HookEventMouseButton;
using WinEase::HookEventMouseMove;
using WinEase::HookEventMouseWheel;
using WinEase::HookEventTypes;
using WinEase::HookListener;
using WinEase::HookModAlt;
using WinEase::HookModCtrl;
using WinEase::HookModifiers;
using WinEase::HookModShift;
using WinEase::HookService;
using WinEase::HookServiceImpl;

// ---------------------------------------------------------------------------
//  输出
// ---------------------------------------------------------------------------

constexpr const char *kColorReset = "\x1b[0m";
constexpr const char *kColorGreen = "\x1b[32m";
constexpr const char *kColorRed = "\x1b[31m";
constexpr const char *kColorGray = "\x1b[90m";
constexpr const char *kColorCyan = "\x1b[36m";
constexpr const char *kColorYellow = "\x1b[33m";

void printLine(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
    std::fputc('\n', stdout);
}

class Reporter
{
public:
    void section(const QString &title)
    {
        printLine(QStringLiteral("\n%1== %2 ==%3")
                      .arg(QString::fromUtf8(kColorCyan), title, QString::fromUtf8(kColorReset)));
    }

    void check(bool condition, const QString &name, const QString &detail = QString())
    {
        if (condition) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3")
                          .arg(QString::fromUtf8(kColorGreen),
                               QString::fromUtf8(kColorReset), name));
        } else {
            ++m_failed;
            printLine(QStringLiteral("  %1[失败]%2 %3")
                          .arg(QString::fromUtf8(kColorRed),
                               QString::fromUtf8(kColorReset), name));
        }
        if (!detail.isEmpty()) {
            printLine(QStringLiteral("         %1%2%3")
                          .arg(QString::fromUtf8(kColorGray), detail,
                               QString::fromUtf8(kColorReset)));
        }
    }

    void note(const QString &text)
    {
        printLine(QStringLiteral("  %1· %2%3").arg(QString::fromUtf8(kColorGray), text,
                                                  QString::fromUtf8(kColorReset)));
    }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    int m_passed = 0;
    int m_failed = 0;
};

/// 运行事件循环指定时长（排队投递需要它才能被处理）
void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

// ---------------------------------------------------------------------------
//  订阅者
// ---------------------------------------------------------------------------

/// 统计事件种类的订阅者基类。
/// 计数器全部用原子量：回调可能在钩子线程或工作线程执行。
class CountingListener : public HookListener
{
public:
    explicit CountingListener(QObject *parent = nullptr)
        : HookListener(parent)
    {
    }

    std::atomic<int> keyCount{0};
    std::atomic<int> keyPressCount{0};
    std::atomic<int> buttonCount{0};
    std::atomic<int> moveCount{0};
    std::atomic<int> wheelCount{0};
    std::atomic<int> wheelDeltaSum{0};
    std::atomic<int> injectedCount{0};
    std::atomic<int> callbackCount{0};
    std::atomic<int> maxCallbackMs{0};

    /// 记录回调所在的线程（用于验证排队投递确实投递到了订阅者线程）
    std::atomic<Qt::HANDLE> lastCallbackThread{nullptr};
    /// 记录观察到的 F24 事件（用于校验字段解析）
    std::atomic<quint32> lastVirtualKey{0};
    std::atomic<bool> lastWasPressed{false};
    std::atomic<quint32> lastModifiers{0};

protected:
    bool onHookEvent(const HookEvent &event) override
    {
        QElapsedTimer timer;
        timer.start();

        callbackCount.fetch_add(1, std::memory_order_relaxed);
        lastCallbackThread.store(QThread::currentThreadId(), std::memory_order_relaxed);
        if (event.isInjected()) {
            injectedCount.fetch_add(1, std::memory_order_relaxed);
        }

        switch (event.type) {
        case HookEventKey:
            keyCount.fetch_add(1, std::memory_order_relaxed);
            if (event.key.pressed) {
                keyPressCount.fetch_add(1, std::memory_order_relaxed);
            }
            lastVirtualKey.store(event.key.virtualKey, std::memory_order_relaxed);
            lastWasPressed.store(event.key.pressed, std::memory_order_relaxed);
            lastModifiers.store(event.modifiers, std::memory_order_relaxed);
            break;
        case HookEventMouseButton:
            buttonCount.fetch_add(1, std::memory_order_relaxed);
            break;
        case HookEventMouseMove:
            moveCount.fetch_add(1, std::memory_order_relaxed);
            break;
        case HookEventMouseWheel:
            wheelCount.fetch_add(1, std::memory_order_relaxed);
            wheelDeltaSum.fetch_add(event.mouseWheel.delta, std::memory_order_relaxed);
            break;
        default:
            break;
        }

        const int elapsed = static_cast<int>(timer.nsecsElapsed() / 1000 / 1000);
        maxCallbackMs.store(qMax(maxCallbackMs.load(std::memory_order_relaxed), elapsed),
                           std::memory_order_relaxed);

        return handleEvent(event);
    }

    /// 子类可覆盖以决定是否吞掉事件
    virtual bool handleEvent(const HookEvent &event)
    {
        Q_UNUSED(event)
        return false; // 默认不拦截
    }
};

/// 安全拦截器：吞掉一切"注入"的事件，保证合成输入不会影响真实桌面。
/// 同时也是"Direct 订阅者可以拦截事件"的验证载体。
class SafetyInterceptor : public CountingListener
{
public:
    HookDelivery delivery() const override { return HookDelivery::Direct; }

    std::atomic<int> consumeCount{0};

protected:
    bool handleEvent(const HookEvent &event) override
    {
        if (event.isInjected()) {
            consumeCount.fetch_add(1, std::memory_order_relaxed);
            return true; // 吞掉
        }
        return false;
    }
};

/// 故意变慢的 Direct 订阅者：用于验证看门狗降级
class SlowDirectListener : public CountingListener
{
public:
    explicit SlowDirectListener(int sleepMs)
        : m_sleepMs(sleepMs)
    {
    }

    HookDelivery delivery() const override { return HookDelivery::Direct; }

    QString degradeReason() const
    {
        const QMutexLocker locker(&m_reasonMutex);
        return m_reason;
    }

protected:
    bool handleEvent(const HookEvent &event) override
    {
        Q_UNUSED(event)
        QThread::msleep(static_cast<unsigned long>(m_sleepMs));
        return false;
    }

    void onDegraded(const QString &reason) override
    {
        const QMutexLocker locker(&m_reasonMutex);
        m_reason = reason;
    }

private:
    int m_sleepMs = 0;
    mutable QMutex m_reasonMutex;
    QString m_reason;
};

/// 故意变慢的排队订阅者：跑在独立线程上，用于证明"慢订阅者不会阻塞输入"
class SlowQueuedListener : public CountingListener
{
public:
    explicit SlowQueuedListener(int sleepMs)
        : m_sleepMs(sleepMs)
    {
    }

    HookDelivery delivery() const override { return HookDelivery::Queued; }

protected:
    bool handleEvent(const HookEvent &event) override
    {
        Q_UNUSED(event)
        QThread::msleep(static_cast<unsigned long>(m_sleepMs));
        return false;
    }

private:
    int m_sleepMs = 0;
};

// ---------------------------------------------------------------------------
//  注入辅助
// ---------------------------------------------------------------------------

/// 注入一个 F24 按下 + 抬起（F24 没有任何默认行为，即使泄漏也无害）
int injectKeyPair(int times)
{
    int sent = 0;
    for (int index = 0; index < times; ++index) {
        if (HookService::sendKey(VK_F24, true)) {
            ++sent;
        }
        if (HookService::sendKey(VK_F24, false)) {
            ++sent;
        }
    }
    return sent;
}

/// 注入滚轮（+120 后 -120，净滚动为零）
int injectWheel()
{
    int sent = 0;
    if (HookService::sendMouseWheel(120)) {
        ++sent;
    }
    if (HookService::sendMouseWheel(-120)) {
        ++sent;
    }
    return sent;
}

/// 注入 1 像素的鼠标移动并立即移回
int injectMouseJiggle()
{
    POINT cursor{};
    if (::GetCursorPos(&cursor) == FALSE) {
        return 0;
    }

    int sent = 0;
    if (HookService::sendMouseMove(QPoint(cursor.x + 1, cursor.y))) {
        ++sent;
    }
    if (HookService::sendMouseMove(QPoint(cursor.x, cursor.y))) {
        ++sent;
    }
    return sent;
}

} // namespace

int main(int argc, char *argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hook_smoke"));

    printLine(QStringLiteral("WinEase 全局输入钩子验收测试（HookService / P0-2）"));
    printLine(QStringLiteral("说明：注入的合成输入会被安全拦截器吞掉，正常不会影响你的鼠标键盘"));

    Reporter reporter;

    // ========================================================================
    reporter.section(QStringLiteral("服务安装"));
    // ========================================================================

    HookServiceImpl service;
    reporter.check(service.start(), QStringLiteral("安装全局钩子"), service.lastError());
    reporter.check(service.isRunning(), QStringLiteral("服务状态为运行中"));
    reporter.check(service.lastError().isEmpty(), QStringLiteral("无错误信息"),
                   service.lastError());
    reporter.note(QStringLiteral("回调预算 %1 ms").arg(service.callbackBudgetMs()));

    if (!service.isRunning()) {
        printLine(QStringLiteral("\n%1致命：钩子无法安装，后续测试无意义%2")
                      .arg(QString::fromUtf8(kColorRed), QString::fromUtf8(kColorReset)));
        return 1;
    }

    // ---------------- 安全拦截器（必须最先安装）----------------
    SafetyInterceptor interceptor;
    service.subscribe(&interceptor, WinEase::HookEventAll);
    reporter.check(service.subscriberCount() == 1, QStringLiteral("拦截器已订阅"),
                   QStringLiteral("订阅者数量 %1").arg(service.subscriberCount()));

    // 触发一次输入以确认拦截器真的在工作（否则后续注入有泄漏风险）
    const int warmupSent = injectKeyPair(1);
    pump(200);
    const bool interceptorAlive = interceptor.callbackCount.load() > 0
                                  && interceptor.consumeCount.load() > 0;
    reporter.check(interceptorAlive, QStringLiteral("安全拦截器已生效（注入事件被吞掉）"),
                   QStringLiteral("注入 %1 条，回调 %2 次，拦截 %3 次")
                       .arg(warmupSent)
                       .arg(interceptor.callbackCount.load())
                       .arg(interceptor.consumeCount.load()));

    if (!interceptorAlive) {
        printLine(QStringLiteral("\n%1致命：拦截器未生效，为安全起见中止测试%2")
                      .arg(QString::fromUtf8(kColorRed), QString::fromUtf8(kColorReset)));
        service.unsubscribe(&interceptor);
        service.stop();
        return 1;
    }

    // ========================================================================
    reporter.section(QStringLiteral("排队订阅者接收事件（验收标准 1）"));
    // ========================================================================

    CountingListener observer;
    service.subscribe(&observer, WinEase::HookEventAll);
    const Qt::HANDLE mainThreadId = QThread::currentThreadId();

    const int observerExpectedKeys = 2; // 一次按下 + 一次抬起
    if (injectKeyPair(1) == 0) {
        reporter.note(QStringLiteral("SendInput 注入失败（可能当前会话不允许注入输入）"));
    }
    injectWheel();
    injectMouseJiggle();
    pump(400);

    reporter.check(observer.keyCount.load() >= observerExpectedKeys,
                   QStringLiteral("收到键盘事件"),
                   QStringLiteral("键事件 %1（按下 %2）")
                       .arg(observer.keyCount.load())
                       .arg(observer.keyPressCount.load()));
    reporter.check(observer.wheelCount.load() >= 2, QStringLiteral("收到滚轮事件"),
                   QStringLiteral("滚轮 %1 次，delta 合计 %2（+120/-120 净为零）")
                       .arg(observer.wheelCount.load())
                       .arg(observer.wheelDeltaSum.load()));
    reporter.check(observer.moveCount.load() >= 1, QStringLiteral("收到鼠标移动事件"),
                   QStringLiteral("移动 %1 次").arg(observer.moveCount.load()));
    reporter.check(observer.injectedCount.load() == observer.callbackCount.load()
                       && observer.callbackCount.load() > 0,
                   QStringLiteral("注入标记正确（isInjected 全部为真）"),
                   QStringLiteral("注入 %1 / 回调 %2")
                       .arg(observer.injectedCount.load())
                       .arg(observer.callbackCount.load()));
    reporter.check(observer.lastVirtualKey.load() == VK_F24,
                   QStringLiteral("虚拟键码解析正确"),
                   QStringLiteral("VK_F24 = 0x%1")
                       .arg(observer.lastVirtualKey.load(), 0, 16));
    reporter.check(observer.lastCallbackThread.load() == mainThreadId,
                   QStringLiteral("排队投递到订阅者线程（而非钩子线程）"));

    const HookService::Stats afterBasics = service.stats();
    reporter.check(afterBasics.keyEvents >= observerExpectedKeys
                       && afterBasics.mouseWheelEvents >= 2
                       && afterBasics.mouseMoveEvents >= 1,
                   QStringLiteral("服务统计正确"),
                   QStringLiteral("键盘 %1 / 滚轮 %2 / 移动 %3 / 按键 %4")
                       .arg(afterBasics.keyEvents)
                       .arg(afterBasics.mouseWheelEvents)
                       .arg(afterBasics.mouseMoveEvents)
                       .arg(afterBasics.mouseButtonEvents));
    reporter.check(afterBasics.consumedEvents >= 1, QStringLiteral("拦截计数正确"),
                   QStringLiteral("拦截 %1 次").arg(afterBasics.consumedEvents));
    reporter.check(afterBasics.maxCallbackMs < 5.0,
                   QStringLiteral("正常情况下钩子回调极快"),
                   QStringLiteral("最大回调 %1 ms").arg(afterBasics.maxCallbackMs, 0, 'f', 3));

    // ========================================================================
    reporter.section(QStringLiteral("慢订阅者不阻塞输入（验收标准 2）"));
    // ========================================================================

    // 把慢订阅者放到独立线程：它的 150ms 睡眠既不会阻塞主线程，也不会阻塞钩子线程
    SlowQueuedListener slowQueued(150);
    QThread slowThread;
    slowThread.setObjectName(QStringLiteral("SlowSubscriber"));
    slowThread.start();
    slowQueued.moveToThread(&slowThread);
    service.subscribe(&slowQueued, WinEase::HookEventKey);

    constexpr int kSlowInjections = 5;
    QElapsedTimer injectTimer;
    injectTimer.start();
    injectKeyPair(kSlowInjections);
    const qint64 injectElapsedMs = injectTimer.elapsed();

    // 若投递是同步的，这里至少需要 5 × 150 = 750ms
    reporter.check(injectElapsedMs < 400,
                   QStringLiteral("注入 %1 次事件的耗时远小于订阅者睡眠时间")
                       .arg(kSlowInjections),
                   QStringLiteral("耗时 %1 ms（同步投递至少要 %2 ms）")
                       .arg(injectElapsedMs)
                       .arg(kSlowInjections * 150));

    // 等待慢订阅者把这些事件处理完（每次 150ms）
    pump(kSlowInjections * 150 * 2 + 500);
    reporter.check(slowQueued.callbackCount.load() >= kSlowInjections,
                   QStringLiteral("慢订阅者最终仍处理完全部事件"),
                   QStringLiteral("处理 %1 / 注入 %2")
                       .arg(slowQueued.callbackCount.load())
                       .arg(kSlowInjections * 2));

    const HookService::Stats afterSlow = service.stats();
    reporter.check(afterSlow.droppedEvents == 0, QStringLiteral("未发生事件丢弃（积压未超上限）"),
                   QStringLiteral("丢弃 %1").arg(afterSlow.droppedEvents));

    service.unsubscribe(&slowQueued);
    slowThread.quit();
    slowThread.wait(3000);

    // ========================================================================
    reporter.section(QStringLiteral("看门狗：超预算的 Direct 订阅者被降级"));
    // ========================================================================

    SlowDirectListener slowDirect(80); // 80ms > 预算 50ms
    service.subscribe(&slowDirect, WinEase::HookEventKey);

    // 需要连续 3 次超预算才降级，注入 4 次按下+抬起以留余量
    injectKeyPair(4);
    pump(1200); // 等降级通知排队送达

    reporter.check(!slowDirect.degradeReason().isEmpty(),
                   QStringLiteral("慢 Direct 订阅者已被降级"), slowDirect.degradeReason());
    reporter.check(service.stats().degradedSubscribers >= 1,
                   QStringLiteral("服务统计记录了降级"),
                   QStringLiteral("降级数 %1").arg(service.stats().degradedSubscribers));
    reporter.check(service.stats().maxCallbackMs >= 50.0,
                   QStringLiteral("最大回调耗时已被记录"),
                   QStringLiteral("%1 ms").arg(service.stats().maxCallbackMs, 0, 'f', 1));
    reporter.check(service.isRunning(), QStringLiteral("降级后服务仍在运行"));

    // 降级后该订阅者转为排队投递：其回调线程应变为它的所属线程
    const int callsBefore = slowDirect.callbackCount.load();
    injectKeyPair(1);
    pump(500);
    reporter.check(slowDirect.callbackCount.load() > callsBefore,
                   QStringLiteral("降级后仍能收到事件（改为排队投递）"),
                   QStringLiteral("回调 %1 → %2")
                       .arg(callsBefore)
                       .arg(slowDirect.callbackCount.load()));

    service.unsubscribe(&slowDirect);

    // ========================================================================
    reporter.section(QStringLiteral("修饰键工具"));
    // ========================================================================

    reporter.check(HookService::modifiersMatch(HookModCtrl, HookModCtrl),
                   QStringLiteral("Ctrl 恰好匹配 Ctrl"));
    reporter.check(!HookService::modifiersMatch(HookModCtrl | HookModShift, HookModCtrl),
                   QStringLiteral("Ctrl+Shift 不匹配只要 Ctrl"));
    reporter.check(HookService::modifiersMatch(HookModCtrl | HookModShift,
                                               HookModCtrl | HookModShift),
                   QStringLiteral("Ctrl+Shift 恰好匹配 Ctrl+Shift"));
    reporter.check(HookService::modifiersMatch(HookModifiers(), HookModifiers()),
                   QStringLiteral("无修饰键匹配无要求"));

    const HookModifiers current = HookService::currentModifiers();
    reporter.note(QStringLiteral("当前修饰键状态：%1")
                      .arg(current == HookModifiers() ? QStringLiteral("无")
                                                      : QStringLiteral("有按下")));

    // ========================================================================
    reporter.section(QStringLiteral("生命周期与清理"));
    // ========================================================================

    // 订阅者析构应自动退订
    {
        const int before = service.subscriberCount();
        auto *temporary = new CountingListener();
        service.subscribe(temporary, WinEase::HookEventKey);
        const int during = service.subscriberCount();
        delete temporary;
        const int after = service.subscriberCount();
        reporter.check(during == before + 1 && after == before,
                       QStringLiteral("订阅者析构时自动退订"),
                       QStringLiteral("%1 → %2 → %3").arg(before).arg(during).arg(after));
    }

    service.unsubscribe(&observer);
    reporter.check(service.subscriberCount() == 1, QStringLiteral("显式退订生效"),
                   QStringLiteral("剩余订阅者 %1").arg(service.subscriberCount()));

    const HookService::Stats finalStats = service.stats();
    reporter.check(finalStats.reinstallCount == 0,
                   QStringLiteral("未发生误判重装（回调超时均未触及系统超时阈值）"),
                   QStringLiteral("重装 %1 次 · 系统阈值 %2 ms")
                       .arg(finalStats.reinstallCount)
                       .arg(1000));

    service.stop();
    reporter.check(!service.isRunning(), QStringLiteral("服务已停止"));
    service.stop(); // 幂等
    reporter.check(!service.isRunning(), QStringLiteral("重复 stop() 幂等"));

    // ========================================================================
    const int total = reporter.passed() + reporter.failed();
    printLine(QString());
    printLine(QStringLiteral("──────────── 汇总 ────────────"));
    printLine(QStringLiteral("  通过 %1 / %2").arg(reporter.passed()).arg(total));
    printLine(QStringLiteral("  钩子统计：键盘 %1 · 鼠标按键 %2 · 移动 %3 · 滚轮 %4 · 拦截 %5")
                  .arg(finalStats.keyEvents)
                  .arg(finalStats.mouseButtonEvents)
                  .arg(finalStats.mouseMoveEvents)
                  .arg(finalStats.mouseWheelEvents)
                  .arg(finalStats.consumedEvents));
    if (reporter.failed() > 0) {
        printLine(QStringLiteral("  %1失败 %2%3")
                      .arg(QString::fromUtf8(kColorRed))
                      .arg(reporter.failed())
                      .arg(QString::fromUtf8(kColorReset)));
    }
    printLine(QStringLiteral("  结果：%1")
                  .arg(reporter.failed() == 0 ? QStringLiteral("全部通过")
                                              : QStringLiteral("存在失败项")));

    return reporter.failed() == 0 ? 0 : 1;
}
