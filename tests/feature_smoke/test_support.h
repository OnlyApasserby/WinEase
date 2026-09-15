#pragma once

// ============================================================================
//  feature_smoke / test_support.h —— P1 功能插件端到端自检的公共脚手架
//
//  与 plugin_smoke 的区别（有意为之）：
//    * plugin_smoke 把插件源码编进测试，验的是**插件契约与异常边界**
//    * 本程序用 QPluginLoader 加载 build/bin/plugins 下**真实的插件 DLL**，
//      验的是"用户真正装上的那个插件"，并且回到 Win32 侧读**真实窗口状态**
//
//  目标窗口为什么放在**另一个进程**：
//    平台层的 isManageableWindow() 会排除"本进程的窗口"（WinEase 自己的界面、
//    悬浮层都不该被当作操作目标）。若测试窗口与插件同进程，正确的产品行为
//    反而会让测试无法进行 —— 而且那样测的根本不是真实场景。
//    因此探针窗口跑在一个独立进程里（本 exe 的 --probe-window 子模式）。
// ============================================================================

#include "app/core/AdminHelper.h"
#include "app/core/PluginManager.h"
#include "app/core/SettingsManager.h"
#include "sdk/ElevationService.h" // 记录型提权桩（P2-06/P2-07 要验电源请求链路）
#include "sdk/HookService.h"
#include "sdk/IFeaturePlugin.h"
#include "sdk/OverlayHost.h" // 悬浮层组用例需要真实的宿主（不是桩）
#include "sdk/PluginServices.h"
#include "win32/CoreAudio.h"
#include "win32/WindowTarget.h"
#include "win32/WindowUtils.h"

#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QProcess>
#include <QRect>
#include <QString>
#include <QVariant>

#include <functional>

class QWidget;

namespace FeatureSmoke {

/// 与其它自检程序完全一致的输出风格（std::printf + UTF-8）
class Reporter
{
public:
    explicit Reporter(const QString &suiteName);

    void check(bool ok, const QString &what, const QString &detail = QString());
    void info(const QString &text);

    int total() const { return m_total; }
    int failures() const { return m_failures; }
    QString suiteName() const { return m_suite; }

private:
    QString m_suite;
    int m_total = 0;
    int m_failures = 0;
};

/// 记录型提权服务桩：**只记录、不执行**。
///
/// 为什么需要它：P2-06/P2-07 必须验"倒计时到点之后真的把电源请求发出去了、参数对"，
/// 但自检**绝对不能真的关机**。这个桩让整条链路（确认 → 倒计时 → 参数组装 → 请求）
/// 全部真实跑一遍，只在最后一步之前停住。
/// 它同时是"提权助手不可用"场景的开关：`setAvailable(false)` 时插件必须**如实报错**，
/// 而不是假装成功（fail-closed）。
class RecordingElevationService : public WinEase::ElevationService
{
public:
    struct Call {
        QString operation;
        QVariantMap arguments;
    };

    WinEase::ElevationResult execute(const QString &operation, const QVariantMap &arguments) override;
    bool isHelperAvailable() override { return m_available; }
    QString statusText() override;

    void setAvailable(bool available) { m_available = available; }
    bool available() const { return m_available; }

    /// 预设某个操作成功时返回的 data（自检用它伪造"助手侧的结果"，
    /// 例如"历史备份列表"——那些备份文件只有真助手才看得到）
    void setResultData(const QString &operation, const QVariantMap &data)
    {
        m_resultData.insert(operation, data);
    }
    void clearResultData() { m_resultData.clear(); }

    const QList<Call> &calls() const { return m_calls; }
    /// 某类操作被调用了几次（operation 为空表示全部）
    int callCount(const QString &operation = QString()) const;
    QStringList operations() const;
    /// 最近一次调用的参数（没调用过返回空 map）
    QVariantMap lastArguments() const;
    /// **某类操作**最近一次的参数。
    /// ⚠ 比 lastArguments() 可靠得多：一次业务动作往往会带出一串调用
    ///   （例如 hosts 保存成功后会紧接着去刷新"历史备份列表"），
    ///   直接看"最后一次调用"会读到那条列表请求，参数自然对不上
    QVariantMap lastArgumentsOf(const QString &operation) const;
    void clearCalls() { m_calls.clear(); }

private:
    /// 默认"不可用"：自检必须**显式**打开才走真实链路，
    /// 这样"忘了打开"会表现成失败，而不是悄悄绕过了断言
    bool m_available = false;
    QList<Call> m_calls;
    QHash<QString, QVariantMap> m_resultData;
};

/// 宿主服务桩：只实现插件真正用到的那几项。
/// 与 plugin_smoke 的桩保持同一形态，便于对照。
class StubServices : public WinEase::PluginServices
{
public:
    QVariant configValue(const QString &pluginId,
                         const QString &key,
                         const QVariant &defaultValue) const override;
    void setConfigValue(const QString &pluginId, const QString &key, const QVariant &value) override;
    void syncConfig() override {}
    void log(const QString &pluginId, WinEase::PluginLogLevel level, const QString &message) override;
    void notify(const QString &title, const QString &body) override;
    void setTrayBadge(const QString &pluginId,
                      const QString &text,
                      const QString &colorHex,
                      const QString &tooltip) override;
    bool registerHotkey(const QString &pluginId,
                        const QString &action,
                        const QKeySequence &sequence,
                        const QString &description) override;
    void unregisterHotkey(const QString &pluginId, const QString &action) override;

    /// 钩子服务是**真的**（输入组要用它吞掉任务栏上的真实滚轮事件）：
    /// 桩里没有钩子，P2-05 就只能测"函数返回值"，测不到"事件有没有被拦下来"。
    /// 实例由 main() 持有并 start()（原型的服务生命周期与主程序一致）
    void setHookService(WinEase::HookService *service) { m_hookService = service; }
    WinEase::HookService *hookService() const override { return m_hookService; }
    /// 悬浮层宿主是**真的**（P1-07/P1-08 要真的建出置顶窗口）；
    /// 其余服务仍是桩。注意它必须是 `mutable`：接口签名是 const
    WinEase::OverlayHost *overlayHost() const override { return &m_overlayHost; }
    /// 提权服务是**记录型桩**：P2-06/P2-07 要在不真关机的前提下断言请求参数
    WinEase::ElevationService *elevationService() const override
    {
        return m_elevationEnabled ? &m_elevation : nullptr;
    }
    /// 自检直接操作它（开关可用性、读调用记录）
    RecordingElevationService *elevation() const { return &m_elevation; }
    /// 连"通道"都不给（模拟宿主没提供提权服务）：用来验插件是否**诚实禁用**而不是假装能写
    void setElevationEnabled(bool enabled) { m_elevationEnabled = enabled; }

    QString appVersion() const override { return QStringLiteral("feature_smoke"); }
    bool isElevated() const override { return WinEase::Admin::isProcessElevated(); }
    /// 配置路径交给真实设置管理器（main 里已把 APPDATA 重定向到临时目录），
    /// 这样插件"从配置目录推导出的数据目录"落在沙盒里，而不是 %APPDATA% 真身
    QString configFilePath() const override
    {
        return WinEase::SettingsManager::instance().configFilePath();
    }
    QString logDirectory() const override { return QString(); }
    QWidget *mainWindow() const override { return nullptr; }

    /// 预设配置（例如把 targetMode 提前设成 locked_window）
    void preset(const QString &pluginId, const QString &key, const QVariant &value);
    bool hasHotkey(const QString &hotkeyId) const { return m_hotkeys.contains(hotkeyId); }
    QStringList registeredHotkeys() const { return m_hotkeys.keys(); }
    int logCount() const { return m_logCount; }
    QStringList warnings() const { return m_warnings; }

    /// 某个插件现在"挂在托盘上"的徽标文字（按最后一次调用算：空文字 = 已清除）。
    /// 托盘状态是插件**对外可见**的副作用，自检靠它断言"状态真的报出去了"。
    QString trayBadgeText(const QString &pluginId) const { return m_badges.value(pluginId); }
    int trayBadgeCalls() const { return m_badgeCalls; }
    /// 弹过几个气泡（"可选气泡提示"这类开关只能靠计数验证"有没有弹"）
    int notificationCount() const { return m_notifications; }
    /// 最近一次徽标调用的颜色（用来断言静音/非静音用了不同颜色）
    QString lastTrayBadgeColor() const { return m_lastBadgeColor; }
    QString lastTrayBadgeTooltip() const { return m_lastBadgeTooltip; }

private:
    QHash<QString, QVariant> m_config;
    QHash<QString, QString> m_hotkeys;
    QStringList m_warnings;
    int m_logCount = 0;
    /// 托盘徽标：记录"每个插件当前挂着什么"（空/不存在 = 没挂）
    QHash<QString, QString> m_badges;
    int m_badgeCalls = 0;
    int m_notifications = 0;
    QString m_lastBadgeColor;
    QString m_lastBadgeTooltip;
    WinEase::HookService *m_hookService = nullptr;
    mutable WinEase::OverlayHost m_overlayHost;
    mutable RecordingElevationService m_elevation;
    bool m_elevationEnabled = true;
};

/// 独立进程里的"目标窗口"探针
class ProbeWindow
{
public:
    ~ProbeWindow();

    /// 启动子进程并等待窗口就绪；失败时 errorOut 给出原因
    bool start(QString *errorOut);
    void stop();

    bool isReady() const { return m_handle != nullptr; }
    WinEase::Win32::WindowHandle handle() const { return m_handle; }
    /// 窗口中心（物理像素）——"跟随鼠标"模式需要把光标放到这里
    QPoint center() const;
    QRect rect() const;
    QRect visualRect() const;
    QString title() const;

    static QString titleText();

    /// 让子进程做两件事：
    ///   ① 把目标窗口用 SetWindowPos(HWND_TOP) 抬起来（模拟"别的程序把它抬起来"）
    ///   ② 把子进程的**辅助窗口**切到前台（模拟"用户切到别的窗口"）
    ///
    /// 为什么必须由子进程来触发前台变化：
    ///   WindowBottomPlugin 用 WINEVENT_SKIPOWNPROCESS 装钩子，**本进程**（装了钩子的
    ///   那个进程）产生的 z 序/前台变化会被自动过滤掉。所以只有"另一个进程"的动作
    ///   才能真正走到"置底维持"这条逻辑上；在测试进程自己身上做实验必然假通过。
    void requestRaise();

    /// 辅助窗口句柄（子进程里的第二个顶层窗口，用于制造前台切换）
    WinEase::Win32::WindowHandle helperHandle() const { return m_helper; }

    /// 子进程模式：创建目标窗口与辅助窗口并运行事件循环，直到哨兵文件被删除
    static int runChild(const QString &readyFile);

private:
    QProcess *m_process = nullptr;
    QString m_readyFile;
    QString m_raiseFile;
    WinEase::Win32::WindowHandle m_handle = nullptr;
    WinEase::Win32::WindowHandle m_helper = nullptr;
};

// ---------------------------------------------------------------------------
//  P3-02 文件锁定用例的脚手架
// ---------------------------------------------------------------------------

/// 独占锁定一个文件的**子进程**（"文件被占用删不掉"的真实占用源）。
///
/// 为什么必须是子进程：
///   自检要验的是"**结束占用进程之后**文件能不能删掉"，而被结束的那个进程
///   不能是自检自己 —— 插件拒绝结束 WinEase 自身（这条规则本身也要断言），
///   真把自检进程杀了后面也就没得测了。子进程持有一个 `dwShareMode = 0` 的句柄，
///   这正是"文件被占用"最朴素、也最真实的样子（连它自己再打开一次都会被拒）。
class FileLockProbe
{
public:
    ~FileLockProbe();

    /// 起子进程并等它真的把文件锁住；失败时 errorOut 给原因
    bool start(const QString &filePath, QString *errorOut = nullptr);
    void stop();

    /// 子进程是否还在运行
    bool isRunning() const;
    /// 等子进程退出（返回是否已退出）
    bool waitFinished(int timeoutMs);
    quint32 pid() const;

    /// 子进程模式：独占打开 filePath 并一直持有，直到 readyFile 被删除
    static int runChild(const QString &filePath, const QString &readyFile);

private:
    QProcess *m_process = nullptr;
    QString m_readyFile;
};

// ---------------------------------------------------------------------------
//  P3-09 音量混合器用例的脚手架（**能自己造出真实的音频会话**）
// ---------------------------------------------------------------------------

/// 真的在播放音频的**子进程**：最小 WASAPI 渲染客户端，持续灌静音数据。
///
/// 为什么非要有它：P3-09 的验收标准是"对**同时播放的应用**分别调节互不影响"，
/// 而这台机器上未必有 3 个正在出声的应用（安静时只有一份"系统声音"会话）。
/// 拿"系统声音"当被测对象只能测到三分之一的产品行为，而且**永远测不出**
/// 最要命的那条 —— "按应用调音量必须覆盖该进程的**全部**会话"。
///
/// 于是让它扮演"应用"：本 exe 自己就是最真实的播放器，起的每个子进程都会
/// 在系统里注册**一份真实音频会话**（进程名都是 feature_smoke.exe）。
/// 两个子进程 → 同一进程名的两份会话 → 正好覆盖"一个应用多份会话"这条
/// 最容易写错的路径；父进程则回到 Win32 侧逐份读真实音量来断言。
///
/// 注意：子进程灌的是**全零**数据，听不见声音，但它是一份货真价实的活动音频流，
/// 对 Core Audio 来说与放歌没有任何区别（会话照样 Active、音量照样可控）。
class AudioSessionProbe
{
public:
    ~AudioSessionProbe();

    /// 起子进程并等它把音频会话真的建出来。
    /// @param volume 子进程给**自己会话**设的初值（自检靠"两份不同"证明读的是真实会话，
    ///               而不是把"自己写进去的值"读回来了）
    bool start(float volume, QString *errorOut = nullptr);
    void stop();

    bool isRunning() const;
    /// 等子进程退出（返回是否已退出）
    bool waitFinished(int timeoutMs);
    quint32 pid() const;

    /// 子进程模式：建一份真实音频会话并持有到 readyFile 被删除
    static int runChild(const QString &readyFile, float volume);

private:
    QProcess *m_process = nullptr;
    QString m_readyFile;
};

// ---------------------------------------------------------------------------
//  P3-11 媒体控制面板用例的脚手架（**能自己造出真实的媒体会话**）
// ---------------------------------------------------------------------------

/// 自己给自己注册一份**真实媒体会话**的子进程（SMTC 发布方）。
///
/// 为什么非要有它：P3-11 的验收是"能控制 Chrome / Spotify / 系统播放器"，
/// 而自检机器上通常**一个播放器都没开着** —— 直接在真机上跑只能测到
/// "没有会话时面板怎么显示"这一半（那一半也要测，但不足以证明控制链路通）。
///
/// 做法：子进程 `SetCurrentProcessExplicitAppUserModelID` 给自己一个 AUMID，
/// 建一个真实窗口，再用桌面互操作接口
/// `ISystemMediaTransportControlsInterop::GetForWindow` 取到 SMTC 控制器，
/// 设好曲目/艺术家并声明"正在播放"。这样系统里就多出一份**货真价实的会话**，
/// 父进程用 `GlobalSystemMediaTransportControlsSessionManager` 应当能枚举到它、
/// 读它的曲目、并且真的把它暂停 / 跳转（回读真实播放状态断言）。
///
/// 副作用（已写进 `ENV-SETUP.md` §6）：用例期间系统媒体浮出控件里会短暂多出
/// 一条"WinEase 自检曲目"，会话**不发声**（它没有音频流），子进程退出即消失。
class MediaSessionProbe
{
public:
    ~MediaSessionProbe();

    /// 起子进程并等它把会话真的注册出来（就绪文件出现 = 已经设好曲目）
    bool start(QString *errorOut = nullptr);
    void stop();

    bool isRunning() const;
    /// 等子进程退出（返回是否已退出）
    bool waitFinished(int timeoutMs);
    quint32 pid() const;

    /// 子进程自报的发布结果（"ready" / "failed"）
    QString state() const;
    /// 子进程自报的失败原因（state == "ready" 时为空）
    QString error() const;
    /// 子进程自己在系统里找到的**自己的**会话定位串（"-" = 没找到）
    QString sessionId() const;
    /// 子进程自己枚举到的会话条数
    int sessionCount() const;
    /// 子进程自报"自己就是系统当前会话"
    bool selfIsCurrent() const;
    /// 子进程收到过的媒体键（"Play" / "Pause" / "Next" ...），按收到顺序
    QStringList receivedButtons() const;
    /// 子进程收到的"跳转位置"请求（毫秒，按收到顺序）
    QList<qint64> requestedPositions() const;

    /// 曲目/艺术家/专辑的固定文案（父进程与子进程共用同一份）
    static QString titleText();
    static QString artistText();
    static QString albumText();
    /// 探针会话对外声明的时长（毫秒）——平台层"tick → 毫秒"的换算靠它验
    static qint64 durationMs();
    /// 探针封面的纯色（父进程回读封面后断言这个颜色，证明封面数据真的过来了）
    static QColor artworkColor();
    /// 子进程给自己设的 AUMID（父进程靠它把"自己造的那一份"从真实会话里认出来）
    static QString appUserModelId();

    /// 子进程模式：注册一份真实媒体会话并持有到 readyFile 被删除
    static int runChild(const QString &readyFile);

private:
    QProcess *m_process = nullptr;
    QString m_readyFile;
};

// ---------------------------------------------------------------------------
//  用例通用小工具（窗口组与工具组共用）
// ---------------------------------------------------------------------------

/// 捕获插件的 statusMessage 信号。
/// 状态文本是用户唯一的即时反馈（"已复制 #3366CC"就是产品行为本身），
/// 所以它单独成一条断言，而不是只看"函数有没有报错"。
///
/// ⚠ 时序：要抓**启用瞬间**发出的那条，必须在 setPluginEnabled() 之前 attach
struct StatusLog {
    QStringList messages;

    void attach(WinEase::IFeaturePlugin *plugin);
    QString last() const;
    bool contains(const QString &needle) const;
    void clear() { messages.clear(); }
};

/// 经宿主分发快捷键（等价于用户按下快捷键：pluginId::action）
bool dispatchAction(WinEase::PluginManager &manager, const QString &pluginId, const QString &action);

// ---------------------------------------------------------------------------
//  工具组（P1-B）用到的脚手架
// ---------------------------------------------------------------------------

/// 剪贴板里的纯文本（无内容时空串）
QString clipboardText();

/// 剪贴板守卫：工具组用例几乎都要写剪贴板，**结束后必须还原**。
/// 用户可能正拿剪贴板里的一段东西，被自检吃掉是不可接受的。
class ClipboardGuard
{
public:
    ClipboardGuard();
    ~ClipboardGuard();

    QString original() const { return m_original; }
    bool hadText() const { return m_hadText; }

    /// 用例写剪贴板统一走这里（与插件内部同一套"写入 + 读回校验"逻辑不同，
    /// 这里只关心"确实写进去了"）
    void set(const QString &text) const;

private:
    QString m_original;
    bool m_hadText = false;
};

/// 系统音量/静音守卫：P2-05 用例会**真的改系统音量**（这就是产品行为本身，
/// 躲不开）。用户的音量是他自己调出来的，被自检改掉不还原是不可接受的。
/// 前置：取不到默认音频端点时 valid() 为 false，用例应如实报"前置不成立"。
class AudioVolumeGuard
{
public:
    AudioVolumeGuard();
    ~AudioVolumeGuard();

    bool valid() const { return m_valid; }
    QString error() const { return m_error; }
    float originalVolume() const { return m_volume; }

private:
    bool m_valid = false;
    QString m_error;
    float m_volume = -1.0f;
    bool m_muted = false;
};

/// 纯色小窗口：取色用例的"标准色卡"，也是焦点高亮用例的"背景板"。
/// @param topMost true 时用 Qt::WindowStaysOnTopHint（保证不被别的窗口挡住，
///        否则取到的是遮挡物的颜色）；焦点高亮用例要把它放在**悬浮层下面**，
///        所以传 false（悬浮层是置顶的，非置顶窗口一定被它压住）
class SolidColorWindow
{
public:
    explicit SolidColorWindow(const QColor &color,
                              const QPoint &logicalPosition = QPoint(300, 260),
                              bool topMost = true);
    ~SolidColorWindow();

    WinEase::Win32::WindowHandle handle() const;
    /// 视觉边界（物理像素）
    QRect physicalRect() const;
    /// 中心点（物理像素）—— 取色与光标都应该落在这里
    QPoint physicalCenter() const;
    QColor color() const { return m_color; }

private:
    QWidget *m_widget = nullptr;
    QColor m_color;
};

/// 本机回环监听套接字：端口占用用例的"真实占用源"。
/// 直接用 winsock（不引入 Qt6Network）：绑到 127.0.0.1 的**随机端口**，
/// 然后用 GetExtendedTcpTable 去查它 —— 这样不用猜"哪个端口空着"。
class LoopbackListener
{
public:
    LoopbackListener();
    ~LoopbackListener();

    bool listenTcp();
    bool bindUdp();

    quint16 port() const { return m_port; }
    bool isValid() const { return m_socket != kInvalidSocket; }

private:
    static constexpr quintptr kInvalidSocket = ~quintptr(0);

    quintptr m_socket = kInvalidSocket;
    quint16 m_port = 0;
};

// ---------------------------------------------------------------------------
//  通用断言/等待辅助（跨进程状态变化需要给一点时间，不能立刻断言）
// ---------------------------------------------------------------------------

/// 轮询等待（顺带转事件循环，好让 WinEvent 钩子等投递型通知有机会跑起来）
bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 2000);

/// ★ "连续稳定"等待：判定条件不是"**某一次**采样命中"，而是
///    ① 在超时时间内达到目标；② **连续** `stableSamples` 次采样都保持目标；
///    ③ 每次采样之间都把 Qt 事件处理一遍。
///
/// 为什么必须这样（而不是一次命中就返回）：这些状态都是**别的进程**在改的
/// （前台切换、z 序维持、鼠标抢位），中间会经过瞬态；一次命中就在瞬态上做断言，
/// 表现出来就是"偶发假失败"和"偶发假通过"（P1-04 的前台切换就是典型）。
/// @param sample 采样函数（应当很轻：只读一个状态）
bool waitForStable(const std::function<bool()> &sample,
                   int timeoutMs = 1500,
                   int stableSamples = 3,
                   int sampleIntervalMs = 30);

/// 等"某个窗口成为前台"，判定同上（连续稳定）
bool waitForForeground(WinEase::Win32::WindowHandle expected,
                       int timeoutMs = 1500,
                       int stableSamples = 3,
                       int sampleIntervalMs = 30);

/// 等"当前解析目标"变成期望窗口，判定同上（连续稳定）。
/// 锁定语义的对照条件就靠它：光标移到别的窗口后，要**等到解析结果稳定地指向别人**
/// 才能证明"锁定"真的在起作用（一次命中可能只是回调还没跑完）。
bool waitForResolvedTarget(WinEase::Win32::WindowTargetMode mode,
                           WinEase::Win32::WindowHandle expected,
                           int timeoutMs = 1000,
                           int stableSamples = 3,
                           int sampleIntervalMs = 30);

/// 阶段之间显式把事件队列走空（排队投递的 WinEvent 回调需要机会跑）。
/// 用例里"改状态 → 立刻断言"的地方一律先过一下它。
void settleEvents(int ms = 60);

/// 把光标移到指定**物理**坐标，最多重试 5 次（真实桌面里鼠标可能正被外部移动）。
/// 返回是否确实停在了目标点；调用方应把返回值当作**前置条件**断言出来
bool moveCursorTo(const QPoint &physicalPoint);

/// 窗口是否带某个扩展样式（WS_EX_*）
bool hasExStyle(WinEase::Win32::WindowHandle hwnd, LONG_PTR style);

/// 从一段文本里取出某标记之后的第一个整数（"标尺长度 412 px" → 412）；取不到返回 INT_MIN
int firstNumberAfter(const QString &text, const QString &marker);

/// 造一批"壁纸用图片"（P1-14 自检的素材）：在 parentDir 下建 wallpapers/ 并写 3 张纯色 BMP。
/// 用 BMP 而不是 PNG：SPI 兜底路径在个别系统上对 png 不认，bmp 一定认。
/// @return 图片所在目录；失败返回空串
QString createWallpaperFixture(const QString &parentDir, QString *errorOut);

/// 两个矩形是否在容差内一致（非整数缩放会带来 ≤1px 的取整损耗）
bool rectsClose(const QRect &actual, const QRect &expected, int tolerance);

QString rectText(const QRect &rect);

/// 顶层窗口 z 序下标（0 = 最上），基于 EnumWindows 的枚举顺序
int zOrderIndexOf(WinEase::Win32::WindowHandle hwnd);

/// 窗口是否带 WS_EX_NOACTIVATE（"点击不激活"）
bool isNoActivate(WinEase::Win32::WindowHandle hwnd);

QString handleText(WinEase::Win32::WindowHandle hwnd);

} // namespace FeatureSmoke
