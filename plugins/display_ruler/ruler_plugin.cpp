#include "ruler_plugin.h"

#include "sdk/OverlayHost.h"
#include "sdk/PluginServices.h"
#include "win32/ScreenCapture.h" // virtualDesktopRect()
#include "win32/WindowTarget.h"
#include "win32/WindowUtils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {

/// 标尺跟随光标的采样间隔：60ms ≈ 16fps，肉眼连续但不会让整屏层一直重绘
constexpr int kFollowIntervalMs = 60;

// 单位键名（配置持久化用）。注意用 QString 而不是 const char*：
// Qt 6 去掉了 QLatin1String(const char*) 构造函数，拿 const char* 去比较会编译不过
const QString kUnitPx = QStringLiteral("px");
const QString kUnitCm = QStringLiteral("cm");
const QString kUnitIn = QStringLiteral("in");

} // namespace

RulerPlugin::RulerPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
    m_followTimer.setInterval(kFollowIntervalMs);
    connect(&m_followTimer, &QTimer::timeout, this, [this] { followCursor(); });
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString RulerPlugin::id() const
{
    return QStringLiteral("display.ruler");
}

QString RulerPlugin::name() const
{
    return QStringLiteral("屏幕标尺");
}

QString RulerPlugin::description() const
{
    return QStringLiteral("全屏透明标尺：落一个点，光标移到哪就量到哪（支持多条、可切单位）");
}

QIcon RulerPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::DisplayAssist);
}

WinEase::FeatureCategory RulerPlugin::category() const
{
    return WinEase::FeatureCategory::DisplayAssist;
}

QStringList RulerPlugin::tags() const
{
    return { QStringLiteral("标尺"), QStringLiteral("测距"), QStringLiteral("测量"),
             QStringLiteral("ruler"), QStringLiteral("像素"), QStringLiteral("尺寸") };
}

bool RulerPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence RulerPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+R"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool RulerPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_unit = svc->configValue(id(), QStringLiteral("unit"), QStringLiteral("px")).toString();
        m_interactive = svc->configValue(id(), QStringLiteral("interactive"), false).toBool();
    }
    if (m_unit != kUnitCm && m_unit != kUnitIn) {
        m_unit = kUnitPx;
    }
    return true;
}

void RulerPlugin::shutdown()
{
    destroyOverlays();
}

bool RulerPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    if (svc->overlayHost() == nullptr) {
        // 没有 OverlayKit 就没有标尺可言 —— 如实失败，别让用户对着一个假开关发呆
        setLastError(QStringLiteral("悬浮层宿主不可用（安全模式下不提供）"));
        return false;
    }

    const bool unitOk = svc->registerHotkey(id(), QStringLiteral("unit"),
                                            QKeySequence(QStringLiteral("Ctrl+Shift+Alt+R")),
                                            QStringLiteral("WinEase：标尺切换单位"));
    if (!unitOk) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「切换单位」快捷键注册失败（可能被占用）"));
    }

    Q_EMIT statusMessage(QStringLiteral("就绪：%1 显示或隐藏标尺")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void RulerPlugin::onDisable()
{
    m_followTimer.stop();
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("unit"));
    }
    destroyOverlays();
    m_visible = false;
    Q_EMIT statusMessage(QStringLiteral("已停用标尺"));
}

void RulerPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("unit")) {
        cycleUnit();
    } else if (action == QLatin1String("clear")) {
        clearRulers();
    } else if (action == QLatin1String("setStart")) {
        dropStartPoint();
    } else if (action == QLatin1String("pin")) {
        pinActiveRuler();
    } else if (action == QLatin1String("interactive")) {
        toggleInteractive();
    } else {
        toggleRuler(); // "default"
    }
}

// ---------------------------------------------------------------------------
//  悬浮层
// ---------------------------------------------------------------------------

QRect RulerPlugin::monitorRect() const
{
    // 标尺铺满"光标所在显示器"：多屏时用户通常在自己看的那块屏上量
    const WinEase::Win32::MonitorInfo monitor =
        WinEase::Win32::monitorForPoint(WinEase::Win32::cursorPosition());
    return monitor.valid ? monitor.geometry : WinEase::Win32::virtualDesktopRect();
}

bool RulerPlugin::ensureOverlays()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr) {
        setLastError(QStringLiteral("悬浮层宿主不可用"));
        return false;
    }

    // 已经建过就复用（本函数被"显示/落点/切单位/切模式"反复调用）
    if (!host->overlaysOfOwner(id()).isEmpty()) {
        return true;
    }

    // 每显示器一个悬浮层：跨不同 DPI 的显示器上，用单个窗口时另一台必然被 DWM 缩放而模糊。
    // 自己 new 子类实例，再**交给宿主托管**（adoptOverlay）：这样插件崩溃/被停用时
    // 主程序能按 ownerId 兜底回收，不会在用户桌面上留下一层点不掉的透明窗口。
    const QList<WinEase::Win32::MonitorInfo> all = WinEase::Win32::monitors();
    int created = 0;
    for (int index = 0; index < all.size(); ++index) {
        if (!all.at(index).valid) {
            continue;
        }
        auto *overlay = new RulerOverlay();
        overlay->setNoActivate(true);
        // 可拖拽 = 关闭穿透；此时"能点到"的只有真正画了不透明内容的区域
        overlay->setClickThrough(!m_interactive);
        if (!overlay->coverMonitor(index)) {
            delete overlay; // 尚未交给宿主，此时还归本插件所有
            continue;
        }
        host->adoptOverlay(id(), overlay);
        overlay->show();
        ++created;
    }

    if (created == 0) {
        setLastError(QStringLiteral("创建悬浮层失败（没有可用显示器？）"));
        return false;
    }
    return true;
}

void RulerPlugin::destroyOverlays()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr) {
        return;
    }
    // 所有权在宿主，插件只能"请求回收"
    host->closeOverlaysOfOwner(id());
}

void RulerPlugin::refreshOverlays()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr) {
        return;
    }
    for (WinEase::OverlayWindow *overlay : host->overlaysOfOwner(id())) {
        if (auto *ruler = dynamic_cast<RulerOverlay *>(overlay)) {
            ruler->setUnit(unitName(), pixelsPerUnit());
            ruler->setInteractive(m_interactive);
            ruler->setActiveIndex(m_activeIndex);
            ruler->setRulers(m_rulers);
        }
    }
}

// ---------------------------------------------------------------------------
//  功能
// ---------------------------------------------------------------------------

bool RulerPlugin::toggleRuler()
{
    if (m_visible) {
        destroyOverlays();
        m_visible = false;
        m_followTimer.stop();
        emitStatus(QStringLiteral("标尺已隐藏"));
        return true;
    }

    const QPoint cursor = WinEase::Win32::cursorPosition();
    if (m_rulers.isEmpty()) {
        // 第一条标尺：起点与终点都落在当前光标处，等用户移动鼠标
        m_rulers.append(RulerOverlay::Ruler{ cursor, cursor });
        m_activeIndex = 0;
    }
    if (!ensureOverlays()) {
        emitStatus(QStringLiteral("显示标尺失败：%1").arg(lastError()));
        return false;
    }

    m_visible = true;
    m_lastLengthText.clear();
    refreshOverlays();
    m_followTimer.start();
    emitStatus(QStringLiteral("标尺已显示，移动鼠标开始测量（%1 · %2）")
                   .arg(unitName(),
                        m_interactive ? QStringLiteral("可拖拽") : QStringLiteral("点击穿透")));
    return true;
}

bool RulerPlugin::dropStartPoint()
{
    const QPoint cursor = WinEase::Win32::cursorPosition();

    if (!m_visible) {
        if (!toggleRuler()) {
            return false;
        }
    }

    // 活动标尺还是一条零长的新标尺 → 直接给它定起点；否则新建一条
    if (m_activeIndex >= 0 && m_activeIndex < m_rulers.size()) {
        if (m_rulers.at(m_activeIndex).length() < 1.0) {
            m_rulers[m_activeIndex] = RulerOverlay::Ruler{ cursor, cursor };
        } else {
            m_rulers.append(RulerOverlay::Ruler{ cursor, cursor });
            m_activeIndex = m_rulers.size() - 1;
        }
    } else {
        m_rulers.append(RulerOverlay::Ruler{ cursor, cursor });
        m_activeIndex = m_rulers.size() - 1;
    }

    m_lastLengthText.clear();
    refreshOverlays();
    emitStatus(QStringLiteral("起点已设为 %1,%2").arg(cursor.x()).arg(cursor.y()));
    return true;
}

bool RulerPlugin::pinActiveRuler()
{
    if (m_activeIndex < 0) {
        emitStatus(QStringLiteral("当前没有跟随中的标尺"));
        return false;
    }
    m_activeIndex = -1;
    refreshOverlays();
    emitStatus(QStringLiteral("已钉住当前标尺（共 %1 条）；下一次落点将新建一条")
                   .arg(m_rulers.size()));
    return true;
}

bool RulerPlugin::cycleUnit()
{
    if (m_unit == kUnitPx) {
        m_unit = kUnitCm;
    } else if (m_unit == kUnitCm) {
        m_unit = kUnitIn;
    } else {
        m_unit = kUnitPx;
    }
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("unit"), m_unit);
    }
    m_lastLengthText.clear();
    refreshOverlays();
    emitStatus(QStringLiteral("单位已切换为 %1").arg(unitName()));
    // 换了单位要立刻按新单位重报一次长度：否则卡片上那条长度要等到用户再动一下鼠标
    // 才会变 —— 用户会以为"切了单位没反应"（自检就是这么抓出来的）
    reportLength();
    return true;
}

bool RulerPlugin::toggleInteractive()
{
    m_interactive = !m_interactive;
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("interactive"), m_interactive);
    }
    if (WinEase::PluginServices *svc = services()) {
        if (WinEase::OverlayHost *host = svc->overlayHost()) {
            for (WinEase::OverlayWindow *overlay : host->overlaysOfOwner(id())) {
                overlay->setClickThrough(!m_interactive);
            }
        }
    }
    refreshOverlays();
    emitStatus(m_interactive
                   ? QStringLiteral("已切换为可拖拽（只有标尺自身区域会挡住点击，其余仍然穿透）")
                   : QStringLiteral("已切换为点击穿透（整层不接收鼠标）"));
    return true;
}

bool RulerPlugin::clearRulers()
{
    m_rulers.clear();
    m_activeIndex = -1;
    m_lastLengthText.clear();
    destroyOverlays();
    m_visible = false;
    m_followTimer.stop();
    emitStatus(QStringLiteral("已清除全部标尺"));
    return true;
}

void RulerPlugin::followCursor()
{
    if (!m_visible || m_activeIndex < 0 || m_activeIndex >= m_rulers.size()) {
        return;
    }
    const QPoint cursor = WinEase::Win32::cursorPosition();
    RulerOverlay::Ruler &active = m_rulers[m_activeIndex];
    if (active.end == cursor) {
        // 光标没动 → 不做任何重绘（整屏半透明窗口的重绘代价很高）
        return;
    }
    active.end = cursor;
    refreshOverlays();
    reportLength();
}

void RulerPlugin::reportLength()
{
    if (m_activeIndex < 0 || m_activeIndex >= m_rulers.size()) {
        return;
    }
    const RulerOverlay::Ruler &active = m_rulers.at(m_activeIndex);
    // 只在"显示出来的长度文本"变化时才刷状态，避免每秒十几条噪声
    const QString text = formatLength(active.length());
    if (text == m_lastLengthText) {
        return;
    }
    m_lastLengthText = text;
    emitStatus(QStringLiteral("标尺长度 %1（起点 %2,%3 → 终点 %4,%5）")
                   .arg(text)
                   .arg(active.start.x())
                   .arg(active.start.y())
                   .arg(active.end.x())
                   .arg(active.end.y()));
}

// ---------------------------------------------------------------------------
//  单位与状态
// ---------------------------------------------------------------------------

QString RulerPlugin::unitName() const
{
    if (m_unit == kUnitCm) {
        return QStringLiteral("cm");
    }
    if (m_unit == kUnitIn) {
        return QStringLiteral("in");
    }
    return QStringLiteral("px");
}

qreal RulerPlugin::pixelsPerUnit() const
{
    if (m_unit == kUnitPx) {
        return 1.0;
    }
    // 按系统 DPI 估算：1 英寸 = dpi 物理像素（显示器物理尺寸系统给不准，界面上如实标注）
    const QRect rect = monitorRect();
    const WinEase::Win32::MonitorInfo monitor = WinEase::Win32::monitorForPoint(rect.center());
    const int dpi = (monitor.valid && monitor.dpi > 0) ? monitor.dpi : 96;
    if (m_unit == kUnitIn) {
        return dpi;
    }
    return dpi / 2.54; // cm
}

QString RulerPlugin::formatLength(qreal physicalPixels) const
{
    const qreal value = physicalPixels / pixelsPerUnit();
    if (m_unit == kUnitPx) {
        return QStringLiteral("%1 px").arg(qRound(value));
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(unitName());
}

void RulerPlugin::emitStatus(const QString &text)
{
    Q_EMIT statusMessage(text);
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *RulerPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *buttonRow = new QHBoxLayout();
    auto *toggleButton = new QPushButton(QStringLiteral("显示 / 隐藏"), widget);
    auto *dropButton = new QPushButton(QStringLiteral("以鼠标位置落点"), widget);
    auto *pinButton = new QPushButton(QStringLiteral("钉住当前标尺"), widget);
    auto *clearButton = new QPushButton(QStringLiteral("清除全部"), widget);
    buttonRow->addWidget(toggleButton);
    buttonRow->addWidget(dropButton);
    buttonRow->addWidget(pinButton);
    buttonRow->addWidget(clearButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *unitRow = new QHBoxLayout();
    unitRow->addWidget(new QLabel(QStringLiteral("单位："), widget));
    auto *unitBox = new QComboBox(widget);
    unitBox->addItem(QStringLiteral("像素 px"), QStringLiteral("px"));
    unitBox->addItem(QStringLiteral("厘米 cm"), QStringLiteral("cm"));
    unitBox->addItem(QStringLiteral("英寸 in"), QStringLiteral("in"));
    unitBox->setCurrentIndex(qMax(0, unitBox->findData(m_unit)));
    unitRow->addWidget(unitBox);
    unitRow->addStretch(1);
    layout->addLayout(unitRow);

    auto *interactiveBox = new QCheckBox(
        QStringLiteral("可拖拽（关闭点击穿透；只有标尺自身区域会挡住点击）"), widget);
    interactiveBox->setChecked(m_interactive);
    layout->addWidget(interactiveBox);

    auto *listLabel = new QLabel(widget);
    listLabel->setWordWrap(true);
    layout->addWidget(listLabel);

    auto *hint = new QLabel(QStringLiteral("快捷键：%1 显示/隐藏 · Ctrl+Shift+Alt+R 切换单位\n"
                                          "cm / in 按系统 DPI 估算（显示器物理尺寸系统给不准）。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refreshList = [this, listLabel] {
        if (m_rulers.isEmpty()) {
            listLabel->setText(QStringLiteral("当前没有标尺。"));
            return;
        }
        QStringList lines;
        for (int i = 0; i < m_rulers.size(); ++i) {
            const RulerOverlay::Ruler &ruler = m_rulers.at(i);
            lines.append(QStringLiteral("%1%2：%3")
                             .arg(i == m_activeIndex ? QStringLiteral("● ") : QStringLiteral("  "))
                             .arg(i + 1)
                             .arg(formatLength(ruler.length())));
        }
        listLabel->setText(QStringLiteral("共 %1 条（● 表示正在跟随鼠标）：\n%2")
                               .arg(m_rulers.size())
                               .arg(lines.join(QLatin1Char('\n'))));
    };
    refreshList();

    QObject::connect(toggleButton, &QPushButton::clicked, widget, [this, refreshList] {
        toggleRuler();
        refreshList();
    });
    QObject::connect(dropButton, &QPushButton::clicked, widget, [this, refreshList] {
        dropStartPoint();
        refreshList();
    });
    QObject::connect(pinButton, &QPushButton::clicked, widget, [this, refreshList] {
        pinActiveRuler();
        refreshList();
    });
    QObject::connect(clearButton, &QPushButton::clicked, widget, [this, refreshList] {
        clearRulers();
        refreshList();
    });
    QObject::connect(unitBox, &QComboBox::currentIndexChanged, widget, [this, unitBox, refreshList](int) {
        m_unit = unitBox->currentData().toString();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("unit"), m_unit);
        }
        m_lastLengthText.clear();
        refreshOverlays();
        emitStatus(QStringLiteral("单位已切换为 %1").arg(unitName()));
        reportLength();
        refreshList();
    });
    QObject::connect(interactiveBox, &QCheckBox::toggled, widget, [this, refreshList](bool checked) {
        if (checked == m_interactive) {
            return;
        }
        toggleInteractive();
        refreshList();
    });

    return widget;
}
