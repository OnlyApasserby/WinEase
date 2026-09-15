#pragma once

// ============================================================================
//  HookService.h —— 全局输入钩子服务的插件契约
//
//  为什么必须由主程序统一安装钩子（而不是各插件自己装）：
//    1. WH_KEYBOARD_LL / WH_MOUSE_LL 是**进程级全局**钩子，每个插件各装一次会
//       导致同一事件被重复回调多次，且回调顺序不可控
//    2. 钩子回调运行在安装钩子的线程上，**任何一个回调卡顿都会拖慢全局鼠标/键盘**
//       （Windows 实测超时会静默摘除钩子）
//    3. 因此必须有一个宿主集中管理：统一安装、统一限时、统一降级
//
//  分层：
//    * 本文件（SDK）= 契约：事件结构 + 订阅者基类 + 服务抽象接口
//    * 主程序 app/core/HookServiceImpl = 实现：专用钩子线程 + Win32 钩子 + 看门狗
//    * 插件通过 PluginServices::hookService() 取得接口
//
//  ---------------------------------------------------------------------------
//  插件用法（订阅者必须是独立的 QObject，不能与 IFeaturePlugin 混在一个类里）：
//
//      class MyListener : public WinEase::HookListener
//      {
//      public:
//          explicit MyListener(QObject *parent = nullptr) : HookListener(parent) {}
//      protected:
//          bool onHookEvent(const WinEase::HookEvent &event) override
//          {
//              if (event.type == WinEase::HookEventKey && event.key.pressed) { ... }
//              return false;   // 返回 true 表示吞掉事件（仅 Direct 模式有效）
//          }
//      };
//
//      // onEnable():
//      m_listener = new MyListener(this);
//      services()->hookService()->subscribe(m_listener,
//                                          WinEase::HookEventKey | WinEase::HookEventMouseWheel);
//      // onDisable():
//      m_listener->deleteLater();   // HookListener 析构时会自动退订
//  ---------------------------------------------------------------------------
//
//  ⚠ 两种投递模式的取舍（这是本服务最重要的设计决策）：
//
//    HookDelivery::Queued（默认，观察者）
//        事件被 postEvent 到订阅者所在线程后立即返回。**永远不会拖慢输入**。
//        代价：无法拦截事件（事件早已交给系统）。
//        → 需要"看到"事件的功能一律用它：硬件监控、焦点高亮、手势轨迹。
//
//    HookDelivery::Direct（拦截者）
//        在钩子线程上同步调用，可以返回 true 吞掉事件，配合 sendKey/sendMouse*
//        就能实现"拦截并改写"。**必须极快返回**（预算见 callbackBudgetMs()）。
//        超预算的订阅者会被自动降级为 Queued 并记录日志。
//        → 只给真正需要拦截的功能用：按键映射、文本扩展、滚轮增强、鼠标手势。
// ============================================================================

#include <QFlags>
#include <QObject>
#include <QPoint>
#include <QString>

#include <QtGlobal>

namespace WinEase {

class HookService;

// ============================================================================
//  修饰键
// ============================================================================

enum HookModifier {
    HookModNone = 0x0,
    HookModCtrl = 0x1,
    HookModShift = 0x2,
    HookModAlt = 0x4,
    HookModWin = 0x8,

    /// 全部修饰键（用于"恰好匹配"判定）
    HookModAll = HookModCtrl | HookModShift | HookModAlt | HookModWin,
};
Q_DECLARE_FLAGS(HookModifiers, HookModifier)

// ============================================================================
//  事件类型
// ============================================================================

enum HookEventType {
    HookEventNone = 0x0,
    HookEventKey = 0x1,          ///< 键盘按下 / 抬起
    HookEventMouseButton = 0x2,  ///< 鼠标按键按下 / 抬起
    HookEventMouseMove = 0x4,    ///< 鼠标移动（高频，注意性能）
    HookEventMouseWheel = 0x8,   ///< 滚轮（含横向）

    /// 常用组合
    HookEventAllMouse = HookEventMouseButton | HookEventMouseMove | HookEventMouseWheel,
    HookEventAll = HookEventKey | HookEventAllMouse,
};
Q_DECLARE_FLAGS(HookEventTypes, HookEventType)

// ============================================================================
//  事件数据
//
//  说明：为了让插件不必包含 windows.h，这里把 Win32 的原始字段平铺出来。
//        HookEvent 一次携带全部种类的事件字段（约 100 字节，栈上传递，无堆分配），
//        插件按 event.type 读取对应字段即可。
// ============================================================================

/// 鼠标按键
enum class HookMouseButton {
    Left,
    Right,
    Middle,
    X1, ///< 侧键 1（后退）
    X2  ///< 侧键 2（前进）
};

struct HookKeyEvent {
    quint32 virtualKey = 0;    ///< Win32 虚拟键码（VK_*）
    quint32 scanCode = 0;
    bool pressed = false;      ///< true = 按下，false = 抬起
    bool isExtended = false;   ///< 扩展键（右 Ctrl、右 Alt、方向键、小键盘回车等）
    bool isSystem = false;     ///< 由 Alt 组合触发的系统按键消息
    bool isInjected = false;   ///< 由 SendInput 注入（用于避免自身回灌成死循环）
    HookModifiers modifiers = HookModNone;
    quint32 timeStamp = 0;     ///< 系统毫秒时间戳
};

struct HookMouseButtonEvent {
    HookMouseButton button = HookMouseButton::Left;
    bool pressed = true;
    bool isInjected = false;
    HookModifiers modifiers = HookModNone;
    QPoint screenPos;          ///< 物理像素、屏幕坐标
    quint32 timeStamp = 0;
};

struct HookMouseMoveEvent {
    bool isInjected = false;
    QPoint screenPos;
    quint32 timeStamp = 0;
};

struct HookMouseWheelEvent {
    int delta = 0;             ///< 120 的整数倍；正数 = 向上 / 向右
    bool horizontal = false;   ///< true = 横向滚轮
    bool isInjected = false;
    HookModifiers modifiers = HookModNone;
    QPoint screenPos;          ///< 滚轮发生位置（按位置分发必须用它，如"任务栏滚轮调音量"）
    quint32 timeStamp = 0;
};

/// 统一的输入事件载体
struct HookEvent {
    HookEventType type = HookEventNone;
    quint32 modifiers = 0; ///< 与 HookModifiers 相同编码，便于直接做位运算
    /// 事件发生瞬间的鼠标位置（物理像素）。键盘事件也会填充它，
    /// 供"按住某键 + 鼠标位置"类的功能使用。
    QPoint cursorPos;

    HookKeyEvent key;
    HookMouseButtonEvent mouseButton;
    HookMouseMoveEvent mouseMove;
    HookMouseWheelEvent mouseWheel;

    /// 该事件是否由本程序（或其它程序）注入
    bool isInjected() const
    {
        switch (type) {
        case HookEventKey:
            return key.isInjected;
        case HookEventMouseButton:
            return mouseButton.isInjected;
        case HookEventMouseMove:
            return mouseMove.isInjected;
        case HookEventMouseWheel:
            return mouseWheel.isInjected;
        default:
            break;
        }
        return false;
    }

    /// 事件发生时的屏幕坐标（键盘事件返回当时的鼠标位置）
    QPoint screenPos() const
    {
        switch (type) {
        case HookEventKey:
            return cursorPos;
        case HookEventMouseButton:
            return mouseButton.screenPos;
        case HookEventMouseMove:
            return mouseMove.screenPos;
        case HookEventMouseWheel:
            return mouseWheel.screenPos;
        default:
            break;
        }
        return cursorPos;
    }
};

// ============================================================================
//  投递模式
// ============================================================================

enum class HookDelivery {
    Queued, ///< 投递到订阅者线程（默认，绝不阻塞输入，不能拦截）
    Direct  ///< 在钩子线程同步调用（可拦截，必须极快返回）
};

// ============================================================================
//  HookListener —— 订阅者基类
//
//  ⚠ 必须是 QObject 的子类（排队投递需要线程归属），且**不要**与 IFeaturePlugin
//    写在同一个类里——两者都继承 QObject，会形成二义基类。
//    正确做法：插件持有 listener 成员，而不是让插件类同时是 listener。
// ============================================================================

class HookListener : public QObject
{
    Q_OBJECT

public:
    explicit HookListener(QObject *parent = nullptr);
    ~HookListener() override;

    HookListener(const HookListener &) = delete;
    HookListener &operator=(const HookListener &) = delete;

    /// 本订阅者期望的投递模式。默认 Queued（安全优先）。
    /// 只有确实需要"吞掉/改写事件"时才改为 Direct。
    virtual HookDelivery delivery() const { return HookDelivery::Queued; }

    // ---------------- 以下三个方法由 HookService 调用，插件不要自行调用 ----------------

    /// 派发事件（内部会转发到 onHookEvent；排队模式下由事件循环调用）
    bool dispatchHookEvent(const HookEvent &event);

    /// 通知：钩子服务已停止（例如无法安装钩子）
    void notifyServiceStopped(const QString &reason);

    /// 通知：本订阅者因回调超时被降级为排队投递
    void notifyDegraded(const QString &reason);

    /// 由 HookService 在订阅/退订时维护，用于析构时自动退订
    void setOwningService(HookService *service);

    /// 排队事件处理完毕后上报（内部使用，用于积压统计）
    void reportQueuedProcessed();

protected:
    /// 事件回调。**必须快速返回**（Direct 模式下预算见 HookService::callbackBudgetMs()）。
    /// @return 仅在 HookDelivery::Direct 下有意义：true 表示吞掉该事件；
    ///         Queued 模式下返回值被忽略（事件已经交给系统了）。
    virtual bool onHookEvent(const HookEvent &event) = 0;

    /// 钩子服务停止时回调（可选覆盖）
    virtual void onServiceStopped(const QString &reason);

    /// 本订阅者被降级时回调（可选覆盖）
    virtual void onDegraded(const QString &reason);

private:
    HookService *m_service = nullptr;
};

// ============================================================================
//  HookService —— 服务抽象接口
// ============================================================================

class HookService
{
public:
    HookService() = default;
    virtual ~HookService() = default;

    HookService(const HookService &) = delete;
    HookService &operator=(const HookService &) = delete;

    // ---------------- 生命周期 ----------------

    /// 钩子是否已安装并正在工作
    virtual bool isRunning() const = 0;

    /// 最近一次错误（空表示正常）
    virtual QString lastError() const = 0;

    /// 重装钩子（诊断与恢复用）
    virtual bool restart() = 0;

    /// 回调时间预算（毫秒）。Direct 订阅者连续超预算会被降级。
    virtual int callbackBudgetMs() const = 0;

    // ---------------- 订阅 ----------------

    /// 订阅事件。listener 所有权仍归调用方，必须比服务活得短。
    /// 重复订阅同一对象会更新其订阅类型。
    /// @return 服务未运行时返回 false（但订阅会被记录，服务恢复后自动生效）
    virtual bool subscribe(HookListener *listener, HookEventTypes types) = 0;

    /// 退订（HookListener 析构时也会自动调用，通常无需手工调用）
    virtual void unsubscribe(HookListener *listener) = 0;

    /// 当前订阅者数量（诊断用）
    virtual int subscriberCount() const = 0;

    // ---------------- 统计 ----------------

    struct Stats {
        quint64 keyEvents = 0;
        quint64 mouseButtonEvents = 0;
        quint64 mouseMoveEvents = 0;
        quint64 mouseWheelEvents = 0;
        quint64 consumedEvents = 0;      ///< 被 Direct 订阅者拦截的事件数
        quint64 droppedEvents = 0;       ///< 因订阅者积压被丢弃的排队事件数
        double maxCallbackMs = 0.0;      ///< 钩子回调内的最大耗时
        int degradedSubscribers = 0;     ///< 被降级的订阅者数量
        int reinstallCount = 0;          ///< 因怀疑钩子被摘除而重装的次数
    };

    virtual Stats stats() const = 0;

    // ---------------- 排队投递的积压统计（由 HookListener 回调） ----------------

    /// 由 HookListener 在处理完一个排队事件后调用，用于统计积压量。
    /// 插件无需关心；只有自定义 HookListener 实现才可能碰到。
    virtual void notifyQueuedEventProcessed(HookListener *listener) = 0;

    // ---------------- 注入（"拦截并改写"的后半段） ----------------
    //
    //  典型用法（拦截并改写）：
    //      Direct 订阅者返回 true 吞掉原事件，然后调用 sendKey() 补发一个新事件。
    //      补发的事件会带 isInjected=true，订阅者据此避免自我回灌形成死循环。

    static bool sendKey(quint32 virtualKey, bool pressed, bool extended = false);
    static bool sendMouseButton(HookMouseButton button, bool pressed);
    static bool sendMouseWheel(int delta, bool horizontal = false);
    static bool sendMouseMove(const QPoint &screenPos);

    // ---------------- 修饰键工具 ----------------

    /// 当前全局修饰键状态
    static HookModifiers currentModifiers();

    /// 判断实际修饰键是否**恰好**满足要求：
    /// required 中的键必须按下，required 之外的修饰键必须未按下。
    /// 例如 required = HookModCtrl 时，Ctrl+Shift+滚轮 不匹配。
    static bool modifiersMatch(HookModifiers actual, HookModifiers required);
};

} // namespace WinEase

Q_DECLARE_OPERATORS_FOR_FLAGS(WinEase::HookEventTypes)
Q_DECLARE_OPERATORS_FOR_FLAGS(WinEase::HookModifiers)

Q_DECLARE_METATYPE(WinEase::HookEvent)
Q_DECLARE_METATYPE(WinEase::HookModifiers)
