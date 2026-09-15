#include "always_on_top_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/WindowUtils.h"

#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::WindowHandle;

/// HWND → QHash 的键（HWND 本质是句柄值，用整数存最省事）
quintptr handleKey(WindowHandle hwnd)
{
    return reinterpret_cast<quintptr>(hwnd);
}

} // namespace

AlwaysOnTopPlugin::AlwaysOnTopPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString AlwaysOnTopPlugin::id() const
{
    return QStringLiteral("window.always_on_top");
}

QString AlwaysOnTopPlugin::name() const
{
    return QStringLiteral("窗口置顶");
}

QString AlwaysOnTopPlugin::description() const
{
    return QStringLiteral("把窗口钉在最前面；可跟随鼠标，也可锁定某个窗口");
}

QIcon AlwaysOnTopPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::WindowManagement);
}

WinEase::FeatureCategory AlwaysOnTopPlugin::category() const
{
    return WinEase::FeatureCategory::WindowManagement;
}

QStringList AlwaysOnTopPlugin::tags() const
{
    return { QStringLiteral("置顶"), QStringLiteral("最前"), QStringLiteral("top"),
             QStringLiteral("zd") };
}

bool AlwaysOnTopPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence AlwaysOnTopPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+P"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool AlwaysOnTopPlugin::initialize()
{
    m_target.load(services(), id());
    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("窗口置顶已就绪"));
    return true;
}

void AlwaysOnTopPlugin::shutdown()
{
    // 退出前必须把窗口恢复原样，否则会留下"用户找不到原因"的置顶窗口
    restoreAll();
}

bool AlwaysOnTopPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    // 主快捷键（默认 Ctrl+Alt+P）由主程序按 <id>::default 统一注册，
    // 这里只注册附加动作；注册失败不阻断启用（用户可在快捷键设置里改键）
    const bool lockOk = m_target.registerLockHotkey(QKeySequence(QStringLiteral("Ctrl+Shift+Alt+P")));
    const bool restoreOk = svc->registerHotkey(id(), QStringLiteral("unpin_all"),
                                               QKeySequence(QStringLiteral("Ctrl+Shift+Alt+U")),
                                               QStringLiteral("WinEase：取消全部窗口置顶"));
    if (!lockOk || !restoreOk) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("部分辅助快捷键注册失败（多半是被其它程序占用）"));
    }

    reportStatus();
    return true;
}

void AlwaysOnTopPlugin::onDisable()
{
    m_target.unregisterLockHotkey();
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("unpin_all"));
    }
    restoreAll();
    Q_EMIT statusMessage(QStringLiteral("已停用"));
}

void AlwaysOnTopPlugin::onHotkey(const QString &hotkeyId)
{
    // <id>::default 在插件**加载时**就注册好了，因此停用期间也可能被分发到，
    // 这里必须先挡住（否则用户以为"关了还在生效"）
    if (!isEnabled()) {
        return;
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);

    // 锁定/解锁由共用实现处理（四个窗口插件语义一致）
    QString lockStatus;
    if (m_target.handleLockAction(action, &lockStatus)) {
        Q_EMIT statusMessage(lockStatus);
        return;
    }

    if (action == QLatin1String("unpin_all")) {
        restoreAll();
        reportStatus();
    } else {
        toggleTargetTopMost();
    }
}

// ---------------------------------------------------------------------------
//  功能实现
// ---------------------------------------------------------------------------

void AlwaysOnTopPlugin::toggleTargetTopMost()
{
    const WindowHandle target = m_target.resolveTarget();
    if (target == nullptr) {
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), QStringLiteral("没有找到可操作的窗口"));
        }
        return;
    }

    const quintptr key = handleKey(target);
    const bool currentlyTopMost = WinEase::Win32::isTopMost(target);
    const bool wantTopMost = !currentlyTopMost;

    if (wantTopMost && !m_pinned.contains(key)) {
        // 记录**改动前**的状态：有些窗口本来就置顶（例如播放器小窗），
        // 还原时必须回到"原本就是置顶"，而不是一律取消置顶
        PinRecord record;
        record.wasTopMost = currentlyTopMost;
        record.processId = WinEase::Win32::windowProcessId(target);
        record.className = WinEase::Win32::windowClassName(target);
        m_pinned.insert(key, record);
    }

    if (!WinEase::Win32::setTopMost(target, wantTopMost)) {
        m_pinned.remove(key);
        setLastError(QStringLiteral("置顶操作失败：%1").arg(WinEase::Win32::windowTitle(target)));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        return;
    }

    if (!wantTopMost) {
        // 已取消置顶 → 不再需要还原
        m_pinned.remove(key);
    }

    // 状态文本要带上具体窗口：卡片上只说"已置顶 1 个"用户不知道是哪个
    Q_EMIT statusMessage(QStringLiteral("%1：%2（共 %3 个）")
                             .arg(wantTopMost ? QStringLiteral("已置顶") : QStringLiteral("已取消置顶"),
                                  WinEase::FeaturePlugins::WindowTargetState::describe(target))
                             .arg(m_pinned.size()));
}

void AlwaysOnTopPlugin::restoreAll()
{
    if (m_pinned.isEmpty()) {
        return;
    }

    int restored = 0;
    for (auto it = m_pinned.cbegin(); it != m_pinned.cend(); ++it) {
        const WindowHandle hwnd = reinterpret_cast<WindowHandle>(it.key());
        if (!WinEase::Win32::isValidWindow(hwnd)) {
            continue; // 窗口已关闭
        }
        // 句柄可能已被系统回收给别的窗口：确认进程与窗口类仍然一致才敢动它
        const PinRecord &record = it.value();
        if (WinEase::Win32::windowProcessId(hwnd) != record.processId
            || WinEase::Win32::windowClassName(hwnd) != record.className) {
            continue;
        }
        WinEase::Win32::setTopMost(hwnd, record.wasTopMost);
        ++restored;
    }

    m_pinned.clear();
    if (restored > 0) {
        logMessage(WinEase::PluginLogLevel::Info,
                   QStringLiteral("已还原 %1 个窗口的置顶状态").arg(restored));
    }
}

void AlwaysOnTopPlugin::reportStatus()
{
    if (m_pinned.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("当前没有置顶窗口"));
        return;
    }
    Q_EMIT statusMessage(QStringLiteral("已置顶 %1 个窗口").arg(m_pinned.size()));
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *AlwaysOnTopPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    layout->addWidget(m_target.createGroup(widget));

    auto *pinnedLabel = new QLabel(widget);
    pinnedLabel->setWordWrap(true);
    layout->addWidget(pinnedLabel);

    auto *restoreButton = new QPushButton(QStringLiteral("取消全部置顶"), widget);
    layout->addWidget(restoreButton, 0, Qt::AlignLeft);

    auto *hint = new QLabel(QStringLiteral("快捷键：切换置顶 %1 · 锁定目标 %2 · 取消全部 %3。\n"
                                          "功能停用或程序退出时会自动把窗口恢复原状。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText),
                                     QStringLiteral("Ctrl+Shift+Alt+P"),
                                     QStringLiteral("Ctrl+Shift+Alt+U")),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refreshList = [this, pinnedLabel] {
        if (m_pinned.isEmpty()) {
            pinnedLabel->setText(QStringLiteral("当前没有被本功能置顶的窗口。"));
            return;
        }
        QStringList lines;
        lines.reserve(m_pinned.size());
        for (auto it = m_pinned.cbegin(); it != m_pinned.cend(); ++it) {
            lines.append(WinEase::FeaturePlugins::WindowTargetState::describe(
                reinterpret_cast<WindowHandle>(it.key())));
        }
        pinnedLabel->setText(QStringLiteral("已置顶 %1 个窗口：\n· %2")
                                 .arg(lines.size())
                                 .arg(lines.join(QStringLiteral("\n· "))));
    };
    refreshList();

    connect(restoreButton, &QPushButton::clicked, widget, [this, refreshList] {
        restoreAll();
        refreshList();
    });

    return widget;
}
