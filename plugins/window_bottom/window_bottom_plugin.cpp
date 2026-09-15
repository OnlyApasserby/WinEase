#include "window_bottom_plugin.h"

#include "sdk/PluginServices.h"

#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::WindowHandle;

/// SetWinEventHook 的回调是 C 函数指针，没有 user data 参数，
/// 因此用文件级指针回指插件实例。每个插件是独立 DLL，各自持有一份，
/// 不会与其它插件互相干扰。
WindowBottomPlugin *s_instance = nullptr;

quintptr handleKey(WindowHandle hwnd)
{
    return reinterpret_cast<quintptr>(hwnd);
}

bool sameWindow(WindowHandle hwnd, quint32 processId, const QString &className)
{
    return WinEase::Win32::isValidWindow(hwnd)
           && WinEase::Win32::windowProcessId(hwnd) == processId
           && WinEase::Win32::windowClassName(hwnd) == className;
}

} // namespace

WindowBottomPlugin::WindowBottomPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

WindowBottomPlugin::~WindowBottomPlugin()
{
    // 插件被卸载（崩溃隔离之外的正常路径）时也要把系统状态还原干净
    releaseAll();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString WindowBottomPlugin::id() const
{
    return QStringLiteral("window.bottom");
}

QString WindowBottomPlugin::name() const
{
    return QStringLiteral("窗口置底");
}

QString WindowBottomPlugin::description() const
{
    return QStringLiteral("把窗口钉在其它窗口下面，像动态壁纸一样不被打扰");
}

QIcon WindowBottomPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::WindowManagement);
}

WinEase::FeatureCategory WindowBottomPlugin::category() const
{
    return WinEase::FeatureCategory::WindowManagement;
}

QStringList WindowBottomPlugin::tags() const
{
    return { QStringLiteral("置底"), QStringLiteral("钉底"), QStringLiteral("动态壁纸"),
             QStringLiteral("bottom"), QStringLiteral("zd") };
}

bool WindowBottomPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence WindowBottomPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+B"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool WindowBottomPlugin::initialize()
{
    m_target.load(services(), id());
    return true;
}

void WindowBottomPlugin::shutdown()
{
    releaseAll();
}

bool WindowBottomPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    if (!m_target.registerLockHotkey(QKeySequence(QStringLiteral("Ctrl+Shift+Alt+K")))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「锁定目标窗口」的快捷键注册失败（可能被占用）"));
    }
    if (!svc->registerHotkey(id(), QStringLiteral("release"),
                             QKeySequence(QStringLiteral("Ctrl+Shift+Alt+B")),
                             QStringLiteral("WinEase：解除全部窗口置底"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("解除置底的快捷键注册失败（可能被占用）"));
    }

    Q_EMIT statusMessage(QStringLiteral("按 %1 把窗口钉到底部")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void WindowBottomPlugin::onDisable()
{
    m_target.unregisterLockHotkey();
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("release"));
    }
    releaseAll();
    Q_EMIT statusMessage(QStringLiteral("已停用，窗口恢复原样"));
}

void WindowBottomPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);

    QString lockStatus;
    if (m_target.handleLockAction(action, &lockStatus)) {
        Q_EMIT statusMessage(lockStatus);
        return;
    }

    if (action == QLatin1String("release")) {
        releaseAll();
        return;
    }
    pinTargetToBottom();
}

// ---------------------------------------------------------------------------
//  功能实现
// ---------------------------------------------------------------------------

bool WindowBottomPlugin::pinTargetToBottom()
{
    const WindowHandle target = m_target.resolveTarget();
    if (target == nullptr) {
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), QStringLiteral("没有找到可操作的窗口"));
        }
        return false;
    }

    const quintptr key = handleKey(target);
    if (!m_pinned.contains(key)) {
        BottomRecord record;
        // 记录原始扩展样式：置底要加 WS_EX_NOACTIVATE，还原时必须只回滚这一位
        record.exStyle = WinEase::Win32::captureExStyle(target);
        record.processId = WinEase::Win32::windowProcessId(target);
        record.className = WinEase::Win32::windowClassName(target);
        m_pinned.insert(key, record);
    }

    // 不抢焦点：否则用户点一下窗口就把它激活回最前，"置底"立刻失效
    const bool noActivate = WinEase::Win32::setNoActivate(target, true);
    const bool bottomed = WinEase::Win32::setBottom(target);
    if (!bottomed) {
        setLastError(QStringLiteral("置底失败：%1").arg(WinEase::Win32::windowTitle(target)));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        return false;
    }

    ensureHookInstalled();
    if (!noActivate) {
        // 少数窗口不接受 NOACTIVATE（例如已经是最顶层工具窗），只是体验打折，不算失败
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("该窗口不支持“点击不激活”，点击后可能仍会跳到最前"));
    }

    Q_EMIT statusMessage(QStringLiteral("已钉到底部：%1（共 %2 个）")
                             .arg(WinEase::FeaturePlugins::WindowTargetState::describe(target))
                             .arg(m_pinned.size()));
    return true;
}

void WindowBottomPlugin::releaseAll()
{
    if (m_pinned.isEmpty()) {
        removeHookIfIdle();
        return;
    }

    int restored = 0;
    for (auto it = m_pinned.cbegin(); it != m_pinned.cend(); ++it) {
        const WindowHandle hwnd = reinterpret_cast<WindowHandle>(it.key());
        const BottomRecord &record = it.value();
        if (!sameWindow(hwnd, record.processId, record.className)) {
            continue;
        }
        WinEase::Win32::restoreExStyle(hwnd, record.exStyle);
        ++restored;
    }
    m_pinned.clear();
    removeHookIfIdle();

    if (restored > 0) {
        Q_EMIT statusMessage(QStringLiteral("已解除 %1 个窗口的置底").arg(restored));
    }
}

// ---------------------------------------------------------------------------
//  z 序维持
//
//  为什么需要钩子：
//      SetWindowPos(HWND_BOTTOM) 只是一次性动作。别的程序（甚至该窗口自己）
//      可能把它重新抬起来，用户就会觉得"置底偶尔失效"。监听"前台窗口变化"
//      这个最常引发 z 序变化的时机，把钉底窗口重新压回去即可。
// ---------------------------------------------------------------------------

void WindowBottomPlugin::ensureHookInstalled()
{
    if (m_hook != nullptr) {
        return;
    }
    s_instance = this;
    m_hook = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
                               EVENT_SYSTEM_FOREGROUND,
                               nullptr,
                               &WindowBottomPlugin::foregroundEventProc,
                               0,
                               0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (m_hook == nullptr) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("前台窗口监听安装失败，置底可能被别的程序破坏"));
    }
}

void WindowBottomPlugin::removeHookIfIdle()
{
    if (m_hook == nullptr || !m_pinned.isEmpty()) {
        return;
    }
    ::UnhookWinEvent(m_hook);
    m_hook = nullptr;
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void CALLBACK WindowBottomPlugin::foregroundEventProc(HWINEVENTHOOK hook,
                                                      DWORD event,
                                                      HWND hwnd,
                                                      LONG idObject,
                                                      LONG idChild,
                                                      DWORD eventThread,
                                                      DWORD eventTime)
{
    Q_UNUSED(hook);
    Q_UNUSED(event);
    Q_UNUSED(eventThread);
    Q_UNUSED(eventTime);

    // 只关心"整个窗口"的前台切换，忽略菜单/控件级别的通知
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) {
        return;
    }
    if (s_instance != nullptr) {
        s_instance->pushPinnedWindowsDown();
    }
}

void WindowBottomPlugin::pushPinnedWindowsDown()
{
    if (m_pushing || m_pinned.isEmpty()) {
        return;
    }
    m_pushing = true; // 回调里也会改 z 序，防重入
    for (auto it = m_pinned.cbegin(); it != m_pinned.cend(); ++it) {
        const WindowHandle hwnd = reinterpret_cast<WindowHandle>(it.key());
        const BottomRecord &record = it.value();
        if (!sameWindow(hwnd, record.processId, record.className)) {
            continue;
        }
        // 不做"是否还在最底"的判断：桌面窗口也是同级兄弟窗口，
        // GetWindow(GW_HWNDNEXT) 未必为空，靠它判断很容易误判。
        // 重新压一次的成本极低（同一个位置 SetWindowPos 不产生视觉变化）。
        WinEase::Win32::setBottom(hwnd);
    }
    m_pushing = false;
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *WindowBottomPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    layout->addWidget(m_target.createGroup(widget));

    auto *listLabel = new QLabel(widget);
    listLabel->setWordWrap(true);
    layout->addWidget(listLabel);

    auto *releaseButton = new QPushButton(QStringLiteral("解除全部置底"), widget);
    layout->addWidget(releaseButton, 0, Qt::AlignLeft);

    auto *hint = new QLabel(QStringLiteral("快捷键：钉到底部 %1 · 解除全部 %2\n"
                                          "被钉底的窗口会加上「不抢焦点」，点击它不会跳到最前 —— "
                                          "这是故意的，用来当画中画/动态壁纸。停用功能时自动恢复。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText),
                                     QStringLiteral("Ctrl+Shift+Alt+B")),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refreshList = [this, listLabel] {
        if (m_pinned.isEmpty()) {
            listLabel->setText(QStringLiteral("当前没有置于底部的窗口。"));
            return;
        }
        QStringList lines;
        lines.reserve(m_pinned.size());
        for (auto it = m_pinned.cbegin(); it != m_pinned.cend(); ++it) {
            lines.append(WinEase::FeaturePlugins::WindowTargetState::describe(
                reinterpret_cast<WindowHandle>(it.key())));
        }
        listLabel->setText(QStringLiteral("已置底 %1 个窗口：\n· %2")
                               .arg(lines.size())
                               .arg(lines.join(QStringLiteral("\n· "))));
    };
    refreshList();

    connect(releaseButton, &QPushButton::clicked, widget, [this, refreshList] {
        releaseAll();
        refreshList();
    });

    return widget;
}
