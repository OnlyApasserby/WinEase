#include "window_opacity_plugin.h"

#include "sdk/PluginServices.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::WindowHandle;

constexpr qreal kMinOpacity = 0.10; ///< 再淡就点不到控件了
constexpr qreal kMaxOpacity = 1.00;

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

WindowOpacityPlugin::WindowOpacityPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString WindowOpacityPlugin::id() const
{
    return QStringLiteral("window.opacity");
}

QString WindowOpacityPlugin::name() const
{
    return QStringLiteral("窗口透明度");
}

QString WindowOpacityPlugin::description() const
{
    return QStringLiteral("把窗口调成半透明，方便参考着背后的内容继续操作");
}

QIcon WindowOpacityPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::WindowManagement);
}

WinEase::FeatureCategory WindowOpacityPlugin::category() const
{
    return WinEase::FeatureCategory::WindowManagement;
}

QStringList WindowOpacityPlugin::tags() const
{
    return { QStringLiteral("透明"), QStringLiteral("半透明"), QStringLiteral("opacity"),
             QStringLiteral("tm") };
}

bool WindowOpacityPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence WindowOpacityPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+Minus"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool WindowOpacityPlugin::initialize()
{
    m_target.load(services(), id());

    if (WinEase::PluginServices *svc = services()) {
        // 步长是用户偏好的持久化设置（用百分比存，便于用户直接改配置文件）
        const int percent = svc->configValue(id(), QStringLiteral("stepPercent"), 10).toInt();
        m_step = qBound(0.05, percent / 100.0, 0.50);
    }
    return true;
}

void WindowOpacityPlugin::shutdown()
{
    // 退出时必须还原，否则用户会看到一个"永远半透明"的窗口却找不到原因
    resetAll();
}

bool WindowOpacityPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    if (!m_target.registerLockHotkey(QKeySequence(QStringLiteral("Ctrl+Shift+Alt+T")))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「锁定目标窗口」的快捷键注册失败（可能被占用）"));
    }

    const bool up = svc->registerHotkey(id(), QStringLiteral("increase"),
                                        QKeySequence(QStringLiteral("Ctrl+Alt+Plus")),
                                        QStringLiteral("WinEase：提高窗口不透明度"));
    const bool reset = svc->registerHotkey(id(), QStringLiteral("reset"),
                                           QKeySequence(QStringLiteral("Ctrl+Alt+0")),
                                           QStringLiteral("WinEase：窗口恢复完全不透明"));
    if (!up || !reset) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("辅助快捷键注册失败（可能被占用），可在快捷键设置里改键"));
    }

    Q_EMIT statusMessage(QStringLiteral("当前步长 %1%").arg(qRound(m_step * 100)));
    return true;
}

void WindowOpacityPlugin::onDisable()
{
    m_target.unregisterLockHotkey();
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("increase"));
        svc->unregisterHotkey(id(), QStringLiteral("reset"));
    }
    resetAll();
    Q_EMIT statusMessage(QStringLiteral("已停用，窗口已恢复不透明"));
}

void WindowOpacityPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return; // 停用期间 <id>::default 仍可能被分发到
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);

    QString lockStatus;
    if (m_target.handleLockAction(action, &lockStatus)) {
        Q_EMIT statusMessage(lockStatus);
        return;
    }

    if (action == QLatin1String("increase")) {
        adjustOpacity(m_step);
    } else if (action == QLatin1String("reset")) {
        resetAll();
    } else {
        adjustOpacity(-m_step); // "default" = 变淡一档
    }
}

// ---------------------------------------------------------------------------
//  功能实现
// ---------------------------------------------------------------------------

bool WindowOpacityPlugin::adjustOpacity(qreal delta)
{
    const WindowHandle target = m_target.resolveTarget();
    if (target == nullptr) {
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), QStringLiteral("没有找到可操作的窗口"));
        }
        return false;
    }

    const quintptr key = handleKey(target);
    if (!m_adjusted.contains(key)) {
        // 第一次调整该窗口：先留一份原始状态用于还原
        OpacityRecord record;
        record.snapshot = WinEase::Win32::captureOpacityState(target);
        record.processId = WinEase::Win32::windowProcessId(target);
        record.className = WinEase::Win32::windowClassName(target);
        m_adjusted.insert(key, record);
    }

    const qreal current = WinEase::Win32::opacity(target);
    const qreal wanted = qBound(kMinOpacity, current + delta, kMaxOpacity);

    if (!WinEase::Win32::setOpacity(target, wanted)) {
        setLastError(QStringLiteral("设置透明度失败：%1（该窗口可能不支持分层）")
                         .arg(WinEase::Win32::windowTitle(target)));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        Q_EMIT statusMessage(QStringLiteral("设置失败"));
        return false;
    }

    const int percent = qRound(WinEase::Win32::opacity(target) * 100);
    Q_EMIT statusMessage(QStringLiteral("透明度 %1%：%2")
                             .arg(percent)
                             .arg(WinEase::FeaturePlugins::WindowTargetState::describe(target)));
    return true;
}

void WindowOpacityPlugin::resetAll()
{
    if (m_adjusted.isEmpty()) {
        return;
    }

    int restored = 0;
    for (auto it = m_adjusted.cbegin(); it != m_adjusted.cend(); ++it) {
        const WindowHandle hwnd = reinterpret_cast<WindowHandle>(it.key());
        const OpacityRecord &record = it.value();
        // 句柄可能已被系统回收给别的窗口 → 先验身份再动手
        if (!sameWindow(hwnd, record.processId, record.className)) {
            continue;
        }
        if (WinEase::Win32::restoreOpacityState(hwnd, record.snapshot)) {
            ++restored;
        }
    }

    m_adjusted.clear();
    if (restored > 0) {
        logMessage(WinEase::PluginLogLevel::Info,
                   QStringLiteral("已还原 %1 个窗口的透明度").arg(restored));
    }
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *WindowOpacityPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    layout->addWidget(m_target.createGroup(widget));

    auto *stepRow = new QHBoxLayout();
    stepRow->addWidget(new QLabel(QStringLiteral("每次调整幅度："), widget));
    auto *stepBox = new QSpinBox(widget);
    stepBox->setRange(5, 50);
    stepBox->setSingleStep(5);
    stepBox->setSuffix(QStringLiteral(" %"));
    stepBox->setValue(qRound(m_step * 100));
    stepRow->addWidget(stepBox);
    stepRow->addStretch(1);
    layout->addLayout(stepRow);

    auto *listLabel = new QLabel(widget);
    listLabel->setWordWrap(true);
    layout->addWidget(listLabel);

    auto *resetButton = new QPushButton(QStringLiteral("全部恢复不透明"), widget);
    layout->addWidget(resetButton, 0, Qt::AlignLeft);

    auto *hint = new QLabel(QStringLiteral("快捷键：变淡 %1 · 变回 %2 · 恢复不透明 %3\n"
                                          "下限 10% —— 再低就看不见也点不到控件了。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText),
                                     QStringLiteral("Ctrl+Alt+Plus"),
                                     QStringLiteral("Ctrl+Alt+0")),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refreshList = [this, listLabel] {
        if (m_adjusted.isEmpty()) {
            listLabel->setText(QStringLiteral("当前没有被调整过透明度的窗口。"));
            return;
        }
        QStringList lines;
        lines.reserve(m_adjusted.size());
        for (auto it = m_adjusted.cbegin(); it != m_adjusted.cend(); ++it) {
            const WindowHandle hwnd = reinterpret_cast<WindowHandle>(it.key());
            lines.append(QStringLiteral("%1（%2%）")
                             .arg(WinEase::FeaturePlugins::WindowTargetState::describe(hwnd))
                             .arg(qRound(WinEase::Win32::opacity(hwnd) * 100)));
        }
        listLabel->setText(QStringLiteral("已调整 %1 个窗口：\n· %2")
                               .arg(lines.size())
                               .arg(lines.join(QStringLiteral("\n· "))));
    };
    refreshList();

    connect(stepBox, &QSpinBox::valueChanged, widget, [this](int value) {
        m_step = qBound(0.05, value / 100.0, 0.50);
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("stepPercent"), value);
        }
        Q_EMIT statusMessage(QStringLiteral("当前步长 %1%").arg(qRound(m_step * 100)));
    });
    connect(resetButton, &QPushButton::clicked, widget, [this, refreshList] {
        resetAll();
        refreshList();
    });

    return widget;
}
