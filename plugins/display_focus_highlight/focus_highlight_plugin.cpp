#include "focus_highlight_plugin.h"

#include "sdk/OverlayHost.h"
#include "sdk/PluginServices.h"
#include "win32/WindowTarget.h"
#include "win32/WindowUtils.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <climits>

namespace {

/// 30fps：足够顺滑；每帧只重画圆环那一小块，不会把整屏层重绘成卡顿源
constexpr int kTickIntervalMs = 33;

/// 状态文本里"高亮位置"的最小上报间隔：30fps 全报会在卡片上刷出噪声
constexpr int kReportIntervalMs = 200;

} // namespace

FocusHighlightPlugin::FocusHighlightPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
    m_tickTimer.setInterval(kTickIntervalMs);
    connect(&m_tickTimer, &QTimer::timeout, this, [this] { tick(); });
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString FocusHighlightPlugin::id() const
{
    return QStringLiteral("display.focus_highlight");
}

QString FocusHighlightPlugin::name() const
{
    return QStringLiteral("焦点高亮");
}

QString FocusHighlightPlugin::description() const
{
    return QStringLiteral("跟着鼠标的聚光灯：整屏压暗，只亮出光标周围一圈，讲解录屏时一目了然");
}

QIcon FocusHighlightPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::DisplayAssist);
}

WinEase::FeatureCategory FocusHighlightPlugin::category() const
{
    return WinEase::FeatureCategory::DisplayAssist;
}

QStringList FocusHighlightPlugin::tags() const
{
    return { QStringLiteral("高亮"), QStringLiteral("聚光灯"), QStringLiteral("spotlight"),
             QStringLiteral("录屏"), QStringLiteral("讲解"), QStringLiteral("focus") };
}

bool FocusHighlightPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence FocusHighlightPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+H"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool FocusHighlightPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_style.radius = qBound(20, svc->configValue(id(), QStringLiteral("radius"), 90).toInt(), 600);
        m_style.veilAlpha =
            qBound(0, svc->configValue(id(), QStringLiteral("veilAlpha"), 110).toInt(), 220);
        m_style.ringOnly = svc->configValue(id(), QStringLiteral("ringOnly"), false).toBool();
        m_style.breathing = svc->configValue(id(), QStringLiteral("breathing"), true).toBool();

        const QString colorName =
            svc->configValue(id(), QStringLiteral("ringColor"), QStringLiteral("#FFC107")).toString();
        const QColor color = QColor::fromString(colorName);
        if (color.isValid()) {
            m_style.ringColor = color;
        }
    }
    return true;
}

void FocusHighlightPlugin::shutdown()
{
    hideSpotlight();
}

bool FocusHighlightPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    if (svc->overlayHost() == nullptr) {
        setLastError(QStringLiteral("悬浮层宿主不可用（安全模式下不提供）"));
        return false;
    }

    if (!svc->registerHotkey(id(), QStringLiteral("style"),
                             QKeySequence(QStringLiteral("Ctrl+Shift+Alt+H")),
                             QStringLiteral("WinEase：焦点高亮切换样式（聚光灯 / 只画圆环）"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「切换样式」快捷键注册失败（可能被占用）"));
    }

    Q_EMIT statusMessage(QStringLiteral("就绪：%1 开关聚光灯")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void FocusHighlightPlugin::onDisable()
{
    m_tickTimer.stop();
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("style"));
    }
    destroyOverlays();
    m_active = false;
    Q_EMIT statusMessage(QStringLiteral("已停用焦点高亮"));
}

void FocusHighlightPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("style")) {
        toggleStyle();
    } else {
        toggleSpotlight(); // "default"
    }
}

// ---------------------------------------------------------------------------
//  悬浮层
// ---------------------------------------------------------------------------

void FocusHighlightPlugin::ensureOverlays()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr || !host->overlaysOfOwner(id()).isEmpty()) {
        return;
    }

    const QList<WinEase::Win32::MonitorInfo> all = WinEase::Win32::monitors();
    for (int index = 0; index < all.size(); ++index) {
        if (!all.at(index).valid) {
            continue;
        }
        auto *overlay = new SpotlightOverlay();
        overlay->setStyle(m_style);
        // 验收项"不干扰被点击窗口"就落在这两行：始终穿透 + 不抢焦点
        overlay->setClickThrough(true);
        overlay->setNoActivate(true);
        if (!overlay->coverMonitor(index)) {
            delete overlay; // 还没交给宿主
            continue;
        }
        host->adoptOverlay(id(), overlay); // 交给宿主托管（崩溃/停用时兜底回收）
        overlay->show();
    }
}

void FocusHighlightPlugin::destroyOverlays()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    if (WinEase::OverlayHost *host = svc->overlayHost()) {
        host->closeOverlaysOfOwner(id());
    }
}

void FocusHighlightPlugin::applyStyle()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    if (WinEase::OverlayHost *host = svc->overlayHost()) {
        for (WinEase::OverlayWindow *overlay : host->overlaysOfOwner(id())) {
            if (auto *spotlight = dynamic_cast<SpotlightOverlay *>(overlay)) {
                spotlight->setStyle(m_style);
            }
        }
    }
}

void FocusHighlightPlugin::persistStyle()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("radius"), m_style.radius);
        svc->setConfigValue(id(), QStringLiteral("veilAlpha"), m_style.veilAlpha);
        svc->setConfigValue(id(), QStringLiteral("ringColor"),
                            m_style.ringColor.name(QColor::HexRgb));
        svc->setConfigValue(id(), QStringLiteral("ringOnly"), m_style.ringOnly);
        svc->setConfigValue(id(), QStringLiteral("breathing"), m_style.breathing);
    }
}

// ---------------------------------------------------------------------------
//  功能
// ---------------------------------------------------------------------------

bool FocusHighlightPlugin::showSpotlight()
{
    ensureOverlays();
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr || host->overlaysOfOwner(id()).isEmpty()) {
        setLastError(QStringLiteral("创建聚光灯悬浮层失败"));
        Q_EMIT statusMessage(QStringLiteral("开启失败：%1").arg(lastError()));
        return false;
    }

    m_active = true;
    m_phase = 0.0;
    m_lastReported = QPoint(INT_MIN, INT_MIN); // 强制下一帧报一次位置
    tick();
    m_tickTimer.start();

    Q_EMIT statusMessage(QStringLiteral("聚光灯已开启（半径 %1 px · %2）")
                             .arg(m_style.radius)
                             .arg(m_style.ringOnly ? QStringLiteral("只画圆环")
                                                   : QStringLiteral("整屏压暗")));
    return true;
}

bool FocusHighlightPlugin::hideSpotlight()
{
    m_tickTimer.stop();
    m_active = false;
    destroyOverlays();
    Q_EMIT statusMessage(QStringLiteral("聚光灯已关闭"));
    return true;
}

bool FocusHighlightPlugin::toggleSpotlight()
{
    return m_active ? hideSpotlight() : showSpotlight();
}

bool FocusHighlightPlugin::toggleStyle()
{
    m_style.ringOnly = !m_style.ringOnly;
    persistStyle();
    applyStyle();
    Q_EMIT statusMessage(QStringLiteral("样式已切换为%1")
                             .arg(m_style.ringOnly ? QStringLiteral("只画圆环")
                                                   : QStringLiteral("聚光灯（整屏压暗）")));
    return true;
}

void FocusHighlightPlugin::tick()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr) {
        return;
    }

    const QPoint cursor = WinEase::Win32::cursorPosition();
    m_phase += kTickIntervalMs / 1000.0 * 0.6; // 约 0.6Hz 的呼吸
    if (m_phase > 1.0) {
        m_phase -= 1.0;
    }

    for (WinEase::OverlayWindow *overlay : host->overlaysOfOwner(id())) {
        if (auto *spotlight = dynamic_cast<SpotlightOverlay *>(overlay)) {
            spotlight->setFocusPoint(cursor);
            spotlight->setBreathingPhase(m_phase);
        }
    }

    // 位置只在"真的移动了 + 距上次上报够久"时报一次，避免 30fps 刷屏
    if (cursor != m_lastReported
        && (!m_reportTimer.isValid() || m_reportTimer.elapsed() >= kReportIntervalMs)) {
        m_lastReported = cursor;
        m_reportTimer.restart();
        Q_EMIT statusMessage(QStringLiteral("高亮位置 %1,%2").arg(cursor.x()).arg(cursor.y()));
    }
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *FocusHighlightPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *buttonRow = new QHBoxLayout();
    auto *toggleButton = new QPushButton(QStringLiteral("开启 / 关闭"), widget);
    auto *styleButton = new QPushButton(QStringLiteral("切换样式"), widget);
    buttonRow->addWidget(toggleButton);
    buttonRow->addWidget(styleButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *radiusRow = new QHBoxLayout();
    radiusRow->addWidget(new QLabel(QStringLiteral("亮区半径："), widget));
    auto *radiusBox = new QSpinBox(widget);
    radiusBox->setRange(20, 600);
    radiusBox->setSuffix(QStringLiteral(" px"));
    radiusBox->setValue(m_style.radius);
    radiusRow->addWidget(radiusBox);
    radiusRow->addStretch(1);
    layout->addLayout(radiusRow);

    auto *alphaRow = new QHBoxLayout();
    alphaRow->addWidget(new QLabel(QStringLiteral("压暗强度："), widget));
    auto *alphaBox = new QSpinBox(widget);
    alphaBox->setRange(0, 220);
    alphaBox->setValue(m_style.veilAlpha);
    alphaRow->addWidget(alphaBox);
    alphaRow->addStretch(1);
    layout->addLayout(alphaRow);

    auto *colorButton = new QPushButton(QStringLiteral("圆环颜色…"), widget);
    colorButton->setStyleSheet(QStringLiteral("border-left: 24px solid %1;")
                                   .arg(m_style.ringColor.name(QColor::HexRgb)));
    layout->addWidget(colorButton, 0, Qt::AlignLeft);

    auto *ringOnlyBox = new QCheckBox(QStringLiteral("只画圆环（不压暗整屏）"), widget);
    ringOnlyBox->setChecked(m_style.ringOnly);
    layout->addWidget(ringOnlyBox);

    auto *breathingBox = new QCheckBox(QStringLiteral("呼吸动画"), widget);
    breathingBox->setChecked(m_style.breathing);
    layout->addWidget(breathingBox);

    auto *hint = new QLabel(QStringLiteral("快捷键：%1 开关 · Ctrl+Shift+Alt+H 切换样式\n"
                                          "悬浮层始终点击穿透、不抢焦点 —— 高亮期间照常操作下层窗口。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    QObject::connect(toggleButton, &QPushButton::clicked, widget, [this] { toggleSpotlight(); });
    QObject::connect(styleButton, &QPushButton::clicked, widget, [this] { toggleStyle(); });
    QObject::connect(radiusBox, &QSpinBox::valueChanged, widget, [this](int value) {
        m_style.radius = value;
        persistStyle();
        applyStyle();
    });
    QObject::connect(alphaBox, &QSpinBox::valueChanged, widget, [this](int value) {
        m_style.veilAlpha = value;
        persistStyle();
        applyStyle();
    });
    QObject::connect(colorButton, &QPushButton::clicked, widget, [this, colorButton] {
        const QColor picked = QColorDialog::getColor(m_style.ringColor, nullptr,
                                                     QStringLiteral("圆环颜色"),
                                                     QColorDialog::ShowAlphaChannel);
        if (!picked.isValid()) {
            return;
        }
        m_style.ringColor = picked;
        persistStyle();
        applyStyle();
        colorButton->setStyleSheet(QStringLiteral("border-left: 24px solid %1;")
                                       .arg(picked.name(QColor::HexRgb)));
    });
    QObject::connect(ringOnlyBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_style.ringOnly = checked;
        persistStyle();
        applyStyle();
    });
    QObject::connect(breathingBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_style.breathing = checked;
        persistStyle();
        applyStyle();
    });

    return widget;
}
