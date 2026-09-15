#include "snap_layout_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/WindowUtils.h"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Win32::WindowHandle;
using Zone = SnapLayoutPlugin::Zone;

/// 区域与人话名称
QString zoneName(Zone zone)
{
    switch (zone) {
    case Zone::Left:
        return QStringLiteral("左半");
    case Zone::Right:
        return QStringLiteral("右半");
    case Zone::Top:
        return QStringLiteral("上半");
    case Zone::Bottom:
        return QStringLiteral("下半");
    case Zone::TopLeft:
        return QStringLiteral("左上");
    case Zone::TopRight:
        return QStringLiteral("右上");
    case Zone::BottomLeft:
        return QStringLiteral("左下");
    case Zone::BottomRight:
        return QStringLiteral("右下");
    case Zone::Center:
        return QStringLiteral("居中");
    case Zone::LeftThird:
        return QStringLiteral("左三分之一");
    case Zone::RightThird:
        return QStringLiteral("右三分之一");
    }
    return QString();
}

/// 区域 ↔ 配置/快捷键动作名（稳定标识，改动会让用户自定义键位失效）
QString zoneAction(Zone zone)
{
    switch (zone) {
    case Zone::Left:
        return QStringLiteral("left");
    case Zone::Right:
        return QStringLiteral("right");
    case Zone::Top:
        return QStringLiteral("top");
    case Zone::Bottom:
        return QStringLiteral("bottom");
    case Zone::TopLeft:
        return QStringLiteral("topleft");
    case Zone::TopRight:
        return QStringLiteral("topright");
    case Zone::BottomLeft:
        return QStringLiteral("bottomleft");
    case Zone::BottomRight:
        return QStringLiteral("bottomright");
    case Zone::Center:
        return QStringLiteral("center");
    case Zone::LeftThird:
        return QStringLiteral("left_third");
    case Zone::RightThird:
        return QStringLiteral("right_third");
    }
    return QString();
}

Zone zoneFromAction(const QString &action)
{
    static const Zone kAll[] = { Zone::Left,      Zone::Right,       Zone::Top,
                                 Zone::Bottom,    Zone::TopLeft,     Zone::TopRight,
                                 Zone::BottomLeft, Zone::BottomRight, Zone::Center,
                                 Zone::LeftThird, Zone::RightThird };
    for (Zone zone : kAll) {
        if (zoneAction(zone) == action) {
            return zone;
        }
    }
    return Zone::Left; // 未知动作按"左半"处理（旧配置里可能残留别的名字）
}

/// 区域在指定工作区内的目标矩形（物理像素）。
///
/// ⚠ 切分规则：第二块用"剩余宽度"而不是 width/2，
///    否则 2561 宽的显示器上左右两半会差 1px（露缝）。
QRect zoneRect(const QRect &area, Zone zone)
{
    const int halfW = area.width() / 2;
    const int halfH = area.height() / 2;
    const int restW = area.width() - halfW;
    const int restH = area.height() - halfH;
    const int thirdW = area.width() / 3;

    switch (zone) {
    case Zone::Left:
        return QRect(area.left(), area.top(), halfW, area.height());
    case Zone::Right:
        return QRect(area.left() + halfW, area.top(), restW, area.height());
    case Zone::Top:
        return QRect(area.left(), area.top(), area.width(), halfH);
    case Zone::Bottom:
        return QRect(area.left(), area.top() + halfH, area.width(), restH);
    case Zone::TopLeft:
        return QRect(area.left(), area.top(), halfW, halfH);
    case Zone::TopRight:
        return QRect(area.left() + halfW, area.top(), restW, halfH);
    case Zone::BottomLeft:
        return QRect(area.left(), area.top() + halfH, halfW, restH);
    case Zone::BottomRight:
        return QRect(area.left() + halfW, area.top() + halfH, restW, restH);
    case Zone::LeftThird:
        return QRect(area.left(), area.top(), thirdW, area.height());
    case Zone::RightThird:
        return QRect(area.left() + area.width() - thirdW, area.top(), thirdW, area.height());
    case Zone::Center:
        break; // 居中保留窗口原尺寸，由 centerWindowOnMonitor() 处理
    }
    return area;
}

/// 附加动作的默认键位
QKeySequence zoneHotkey(Zone zone)
{
    switch (zone) {
    case Zone::Right:
        return QKeySequence(QStringLiteral("Ctrl+Alt+Right"));
    case Zone::Top:
        return QKeySequence(QStringLiteral("Ctrl+Alt+Up"));
    case Zone::Bottom:
        return QKeySequence(QStringLiteral("Ctrl+Alt+Down"));
    case Zone::Center:
        return QKeySequence(QStringLiteral("Ctrl+Alt+C"));
    case Zone::TopLeft:
        return QKeySequence(QStringLiteral("Ctrl+Alt+1"));
    case Zone::TopRight:
        return QKeySequence(QStringLiteral("Ctrl+Alt+2"));
    case Zone::BottomLeft:
        return QKeySequence(QStringLiteral("Ctrl+Alt+3"));
    case Zone::BottomRight:
        return QKeySequence(QStringLiteral("Ctrl+Alt+4"));
    case Zone::LeftThird:
        return QKeySequence(QStringLiteral("Ctrl+Shift+Alt+Left"));
    case Zone::RightThird:
        return QKeySequence(QStringLiteral("Ctrl+Shift+Alt+Right"));
    case Zone::Left:
        break; // 左半是主快捷键，由主程序统一注册
    }
    return QKeySequence();
}

bool isPrimaryZone(Zone zone)
{
    return zone == Zone::Left;
}

} // namespace

SnapLayoutPlugin::SnapLayoutPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString SnapLayoutPlugin::id() const
{
    return QStringLiteral("window.snap_layout");
}

QString SnapLayoutPlugin::name() const
{
    return QStringLiteral("窗口快速分屏");
}

QString SnapLayoutPlugin::description() const
{
    return QStringLiteral("一个快捷键把窗口摆到左半 / 右半 / 四象限 / 居中");
}

QIcon SnapLayoutPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::WindowManagement);
}

WinEase::FeatureCategory SnapLayoutPlugin::category() const
{
    return WinEase::FeatureCategory::WindowManagement;
}

QStringList SnapLayoutPlugin::tags() const
{
    return { QStringLiteral("分屏"), QStringLiteral("吸附"), QStringLiteral("对齐"),
             QStringLiteral("snap"), QStringLiteral("fp") };
}

bool SnapLayoutPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence SnapLayoutPlugin::defaultHotkey() const
{
    // 主快捷键（<id>::default）对应"左半"，与其它分屏工具的习惯一致
    return QKeySequence(QStringLiteral("Ctrl+Alt+Left"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool SnapLayoutPlugin::initialize()
{
    m_target.load(services(), id());
    return true;
}

void SnapLayoutPlugin::shutdown()
{
    // 分屏只是"移动窗口"，用户随时可以再改，因此这里不需要还原任何系统状态。
    // （空实现是刻意的：接口要求 override，但本插件的副作用天然是"用户可再次操作"的）
}

bool SnapLayoutPlugin::onEnable()
{
    if (!m_target.registerLockHotkey(QKeySequence(QStringLiteral("Ctrl+Shift+Alt+L")))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「锁定目标窗口」的快捷键注册失败（可能被占用）"));
    }
    registerZoneHotkeys();
    Q_EMIT statusMessage(QStringLiteral("先激活（或把鼠标移到）窗口，再按快捷键"));
    return true;
}

void SnapLayoutPlugin::onDisable()
{
    m_target.unregisterLockHotkey();
    if (WinEase::PluginServices *svc = services()) {
        static const Zone kAll[] = { Zone::Right,      Zone::Top,        Zone::Bottom,
                                     Zone::Center,     Zone::TopLeft,    Zone::TopRight,
                                     Zone::BottomLeft, Zone::BottomRight, Zone::LeftThird,
                                     Zone::RightThird };
        for (Zone zone : kAll) {
            svc->unregisterHotkey(id(), zoneAction(zone));
        }
    }
    Q_EMIT statusMessage(QStringLiteral("已停用"));
}

void SnapLayoutPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return; // <id>::default 在插件加载时就注册了，停用期间必须挡住
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);

    QString lockStatus;
    if (m_target.handleLockAction(action, &lockStatus)) {
        Q_EMIT statusMessage(lockStatus);
        return;
    }

    // "default" 是主程序注册的主动作 → 左半
    snapTo(action == QLatin1String("default") ? Zone::Left : zoneFromAction(action));
}

void SnapLayoutPlugin::registerZoneHotkeys()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    static const Zone kAll[] = { Zone::Right,      Zone::Top,        Zone::Bottom,
                                 Zone::Center,     Zone::TopLeft,    Zone::TopRight,
                                 Zone::BottomLeft, Zone::BottomRight, Zone::LeftThird,
                                 Zone::RightThird };
    int failed = 0;
    for (Zone zone : kAll) {
        if (isPrimaryZone(zone)) {
            continue;
        }
        const QString action = zoneAction(zone);
        if (!svc->registerHotkey(id(), action, zoneHotkey(zone),
                                 QStringLiteral("WinEase：窗口%1").arg(zoneName(zone)))) {
            ++failed;
        }
    }
    if (failed > 0) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("%1 个分屏快捷键注册失败（可能被其它程序占用），"
                                  "可在「快捷键设置」里改用别的组合")
                       .arg(failed));
    }
}

// ---------------------------------------------------------------------------
//  功能实现
// ---------------------------------------------------------------------------

bool SnapLayoutPlugin::snapTo(Zone zone)
{
    const WindowHandle target = m_target.resolveTarget();
    if (target == nullptr) {
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), QStringLiteral("没有找到可操作的窗口"));
        }
        return false;
    }

    // 居中：保持窗口原尺寸，只挪位置
    if (zone == Zone::Center) {
        if (!WinEase::Win32::centerWindowOnMonitor(target)) {
            setLastError(QStringLiteral("居中失败：%1").arg(WinEase::Win32::windowTitle(target)));
            logMessage(WinEase::PluginLogLevel::Warning, lastError());
            Q_EMIT statusMessage(QStringLiteral("居中失败"));
            return false;
        }
        Q_EMIT statusMessage(QStringLiteral("已居中：%1").arg(
            WinEase::FeaturePlugins::WindowTargetState::describe(target)));
        return true;
    }

    const WinEase::Win32::MonitorInfo monitor = WinEase::Win32::monitorForWindow(target);
    if (!monitor.valid || monitor.workArea.isEmpty()) {
        setLastError(QStringLiteral("没有查到窗口所在显示器的可用区域"));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        Q_EMIT statusMessage(QStringLiteral("分屏失败"));
        return false;
    }

    const QRect visualTarget = zoneRect(monitor.workArea, zone);
    const QRect windowTarget = WinEase::Win32::windowRectForVisualRect(target, visualTarget);
    if (!WinEase::Win32::moveWindow(target, windowTarget)) {
        setLastError(QStringLiteral("移动窗口失败：%1").arg(WinEase::Win32::windowTitle(target)));
        logMessage(WinEase::PluginLogLevel::Warning, lastError());
        Q_EMIT statusMessage(QStringLiteral("分屏失败"));
        return false;
    }

    Q_EMIT statusMessage(QStringLiteral("已摆到%1：%2")
                             .arg(zoneName(zone),
                                  WinEase::FeaturePlugins::WindowTargetState::describe(target)));
    return true;
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *SnapLayoutPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    layout->addWidget(m_target.createGroup(widget));

    auto *grid = new QGridLayout();
    // 按钮既方便鼠标用户，也顺带展示有哪些区域可用（区域名的顺序与视觉位置一致）
    const struct {
        Zone zone;
        int row;
        int column;
    } kButtons[] = { { Zone::TopLeft, 0, 0 },     { Zone::Top, 0, 1 },        { Zone::TopRight, 0, 2 },
                     { Zone::Left, 1, 0 },        { Zone::Center, 1, 1 },     { Zone::Right, 1, 2 },
                     { Zone::BottomLeft, 2, 0 },  { Zone::Bottom, 2, 1 },     { Zone::BottomRight, 2, 2 },
                     { Zone::LeftThird, 3, 0 },   { Zone::RightThird, 3, 2 } };
    for (const auto &item : kButtons) {
        auto *button = new QPushButton(zoneName(item.zone), widget);
        connect(button, &QPushButton::clicked, widget, [this, item] { snapTo(item.zone); });
        grid->addWidget(button, item.row, item.column);
    }
    layout->addLayout(grid);

    auto *hint = new QLabel(QStringLiteral("快捷键：左半 %1 · 右半 %2 · 上半 %3 · 下半 %4 · 居中 %5\n"
                                          "四象限 %6/%7/%8/%9 · 左右三分之一 %10/%11\n"
                                          "区域按窗口所在显示器的工作区计算，多屏下不会跨屏。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::Right).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::Top).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::Bottom).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::Center).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::TopLeft).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::TopRight).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::BottomLeft).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::BottomRight).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::LeftThird).toString(QKeySequence::NativeText),
                                     zoneHotkey(Zone::RightThird).toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    return widget;
}
