#include "wheel_enhance_plugin.h"

#include "sdk/HookService.h"
#include "sdk/PluginServices.h"
#include "VolumeSteps.h"
#include "win32/CoreAudio.h"
#include "win32/WindowUtils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

namespace VolumeSteps = WinEase::FeaturePlugins::VolumeSteps;
namespace Win32 = WinEase::Win32;

namespace {

/// 状态提示的最小间隔（连续滚动时别把卡片刷爆），毫秒
constexpr int kStatusThrottleMs = 120;

/// 任务栏窗口类名：
///   Shell_TrayWnd          —— 主任务栏
///   Shell_SecondaryTrayWnd —— 副显示器上的任务栏
/// ⚠ 不包含通知区域弹出窗（NotifyIconOverflowWindow）等独立窗口：
///   它们不是"任务栏"，接管它们没有依据（见头文件"只吞自己该吞的"）。
bool isTaskbarClassName(const QString &className)
{
    return className == QLatin1String("Shell_TrayWnd")
        || className == QLatin1String("Shell_SecondaryTrayWnd");
}

} // namespace

// ============================================================================
//  监听器：Direct 投递（能吞事件），但**只做判断与投递**，不做任何慢操作
// ============================================================================

class WheelVolumeListener : public WinEase::HookListener
{
public:
    using Handler = std::function<bool(const WinEase::HookEvent &)>;

    explicit WheelVolumeListener(Handler handler, QObject *parent = nullptr)
        : WinEase::HookListener(parent)
        , m_handler(std::move(handler))
    {
    }

    /// Direct：任务栏上的滚轮要**吞掉**（否则任务栏/系统还会拿到它）
    WinEase::HookDelivery delivery() const override { return WinEase::HookDelivery::Direct; }

protected:
    bool onHookEvent(const WinEase::HookEvent &event) override
    {
        return m_handler != nullptr ? m_handler(event) : false;
    }

private:
    Handler m_handler;
};

// ============================================================================
//  插件本体
// ============================================================================

WheelEnhancePlugin::WheelEnhancePlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

QString WheelEnhancePlugin::id() const
{
    return QStringLiteral("input.wheel_enhance");
}

QString WheelEnhancePlugin::name() const
{
    return QStringLiteral("滚轮增强");
}

QString WheelEnhancePlugin::description() const
{
    return QStringLiteral("在任务栏上滚动即可调节音量（其余位置的滚轮照常放行）");
}

QIcon WheelEnhancePlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::InputEfficiency);
}

WinEase::FeatureCategory WheelEnhancePlugin::category() const
{
    return WinEase::FeatureCategory::InputEfficiency;
}

QStringList WheelEnhancePlugin::tags() const
{
    return { QStringLiteral("滚轮"), QStringLiteral("音量"), QStringLiteral("任务栏"),
             QStringLiteral("wheel"), QStringLiteral("volume") };
}

bool WheelEnhancePlugin::supportsHotkey() const
{
    // 没有快捷键：这个功能就是"滚轮"，给它配个快捷键反而没人按
    return false;
}

bool WheelEnhancePlugin::hasSettings() const
{
    return true;
}

// ---------------------------------------------------------------------------
//  配置
// ---------------------------------------------------------------------------

void WheelEnhancePlugin::loadFromConfig()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    m_stepPercent = qBound(1, svc->configValue(id(), QStringLiteral("stepPercent"), 5).toInt(), 100);

    const QString modifiers = svc->configValue(id(), QStringLiteral("requireModifiers"),
                                               QStringLiteral("none")).toString().toLower();
    static const QStringList known = { QStringLiteral("none"), QStringLiteral("ctrl"),
                                       QStringLiteral("alt"), QStringLiteral("shift"),
                                       QStringLiteral("ctrl+alt") };
    m_requireModifiers = known.contains(modifiers) ? modifiers : QStringLiteral("none");

    m_invert = svc->configValue(id(), QStringLiteral("invert"), false).toBool();
    m_showStatus = svc->configValue(id(), QStringLiteral("showStatus"), true).toBool();
}

WinEase::HookModifiers WheelEnhancePlugin::requiredModifiers() const
{
    if (m_requireModifiers == QLatin1String("ctrl")) {
        return WinEase::HookModCtrl;
    }
    if (m_requireModifiers == QLatin1String("alt")) {
        return WinEase::HookModAlt;
    }
    if (m_requireModifiers == QLatin1String("shift")) {
        return WinEase::HookModShift;
    }
    if (m_requireModifiers == QLatin1String("ctrl+alt")) {
        return WinEase::HookModCtrl | WinEase::HookModAlt;
    }
    return WinEase::HookModNone;
}

bool WheelEnhancePlugin::initialize()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法初始化"));
        return false;
    }
    loadFromConfig();
    return true;
}

void WheelEnhancePlugin::shutdown()
{
    onDisable();
}

bool WheelEnhancePlugin::canEnable(QString *reason) const
{
    if (services() == nullptr) {
        if (reason != nullptr) {
            *reason = QStringLiteral("宿主服务未注入");
        }
        return false;
    }
    if (services()->hookService() == nullptr) {
        // 如实拒绝：没有钩子服务就没法知道滚轮发生在哪，硬启用等于什么都干不了
        if (reason != nullptr) {
            *reason = QStringLiteral("输入钩子服务不可用（主程序可能以安全模式启动）");
        }
        return false;
    }
    return true;
}

bool WheelEnhancePlugin::onEnable()
{
    WinEase::HookService *hooks = services()->hookService();
    if (hooks == nullptr) {
        setLastError(QStringLiteral("输入钩子服务不可用"));
        return false;
    }

    m_accumulator = 0;
    m_appliedNotches = 0;
    m_lastStatusText.clear();
    m_statusThrottle.invalidate();

    m_listener = new WheelVolumeListener(
        [this](const WinEase::HookEvent &event) { return handleWheelOnHookThread(event); }, this);

    if (!hooks->subscribe(m_listener, WinEase::HookEventMouseWheel)) {
        // 订阅会被记录、服务恢复后自动生效 → 不算失败，但必须说清楚
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("钩子服务当前未运行，订阅已登记（服务恢复后自动生效）"));
    }

    reportStatus(QStringLiteral("已启用：在任务栏上滚动调音量（每格 %1 个百分点）")
                     .arg(m_stepPercent),
                 true);
    return true;
}

void WheelEnhancePlugin::onDisable()
{
    if (m_listener != nullptr) {
        if (WinEase::PluginServices *svc = services()) {
            if (WinEase::HookService *hooks = svc->hookService()) {
                hooks->unsubscribe(m_listener); // 先退订，杜绝"停用后还吞事件"
            }
        }
        m_listener->deleteLater();
        m_listener = nullptr;
    }
    reportStatus(QStringLiteral("已停用：滚轮恢复原样"), true);
}

// ---------------------------------------------------------------------------
//  钩子线程：只判位置 + 投递（必须极快）
// ---------------------------------------------------------------------------

bool WheelEnhancePlugin::handleWheelOnHookThread(const WinEase::HookEvent &event)
{
    if (m_listener == nullptr || !isEnabled()) {
        return false;
    }
    if (event.type != WinEase::HookEventMouseWheel || event.mouseWheel.horizontal) {
        return false; // 横向滚轮不接管
    }

    // 修饰键不符合配置 → 放行（默认要求"不按任何修饰键"，按住 Ctrl 时就不接管，
    // 这样需要原行为的场合有路可走）
    if (!WinEase::HookService::modifiersMatch(event.mouseWheel.modifiers, requiredModifiers())) {
        return false;
    }

    // 位置判定：光标下最上层窗口是不是任务栏
    // ⚠ 这是本回调里唯一的两三句 Win32 调用，都在微秒级
    const QPoint pos = event.mouseWheel.screenPos;
    const Win32::WindowHandle hwnd = Win32::topLevelWindowAt(pos);
    if (hwnd == nullptr || !isTaskbarClassName(Win32::windowClassName(hwnd))) {
        return false; // 不在任务栏 → 不插手
    }

    const int delta = m_invert ? -event.mouseWheel.delta : event.mouseWheel.delta;
    const int notches = VolumeSteps::consumeNotches(m_accumulator, delta);

    if (notches != 0) {
        // 投递到主线程做 COM 调用：**lambda 里再读一次当前音量**，
        // 所以即使连续多格、或别的程序刚改过音量，也不会算到陈旧值上
        QMetaObject::invokeMethod(
            this, [this, notches] { applyNotches(notches); }, Qt::QueuedConnection);
    }
    return true; // 在我们的地盘上，事件归我们（任务栏不会收到它）
}

// ---------------------------------------------------------------------------
//  主线程：真正调音量
// ---------------------------------------------------------------------------

void WheelEnhancePlugin::applyNotches(int notches)
{
    if (notches == 0) {
        return;
    }

    // 端点按需创建并缓存；设备热插拔（插耳机）后旧端点会失效，这里自动重建
    if (!m_endpoint.isValid()) {
        QString error;
        m_endpoint = Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Render, &error);
        if (!m_endpoint.isValid()) {
            const QString text = QStringLiteral("取不到默认音频输出设备：%1").arg(error);
            m_lastStatusText = text;
            reportStatus(text, true);
            logMessage(WinEase::PluginLogLevel::Warning, text);
            return;
        }
        m_deviceName = m_endpoint.deviceName();
    }

    QString error;
    const float current = m_endpoint.volume(&error);
    if (current < 0.0f) {
        m_lastStatusText = QStringLiteral("读音量失败：%1").arg(error);
        m_endpoint = Win32::AudioEndpoint(); // 下次重建
        reportStatus(m_lastStatusText, true);
        return;
    }

    const float next = VolumeSteps::applyNotches(current, notches, static_cast<float>(m_stepPercent));
    if (qFuzzyCompare(next + 1.0f, current + 1.0f)) {
        // 已经顶到边界：不写设备（避免无谓的写入），但如实说明"到头了"
        m_lastStatusText = notches > 0
            ? QStringLiteral("音量已经是 100%（最大值）")
            : QStringLiteral("音量已经是 0%（最小值）");
        reportStatus(m_lastStatusText, true);
        return;
    }

    if (!m_endpoint.setVolume(next, &error)) {
        m_lastStatusText = QStringLiteral("设置音量失败：%1").arg(error);
        m_endpoint = Win32::AudioEndpoint();
        reportStatus(m_lastStatusText, true);
        return;
    }

    ++m_appliedNotches;
    const bool muted = m_endpoint.isMuted();
    m_lastStatusText = muted
        ? QStringLiteral("音量 %1（当前处于静音，滚轮不改静音状态）").arg(VolumeSteps::percentText(next))
        : QStringLiteral("音量 %1（%2）").arg(VolumeSteps::percentText(next), m_deviceName);
    reportStatus(m_lastStatusText);
}

void WheelEnhancePlugin::reportStatus(const QString &text, bool force)
{
    if (!m_showStatus && !force) {
        return;
    }
    // 节流：连续滚动时只更新"最新值"，避免卡片被刷爆（但最后一次一定报出去）
    if (!force && m_statusThrottle.isValid()
        && m_statusThrottle.elapsed() < kStatusThrottleMs) {
        return;
    }
    m_statusThrottle.restart();
    Q_EMIT statusMessage(text);
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *WheelEnhancePlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *box = new QGroupBox(QStringLiteral("任务栏滚轮"), widget);
    auto *form = new QFormLayout(box);

    auto *stepSpin = new QSpinBox(box);
    stepSpin->setObjectName(QStringLiteral("wheelStepSpin"));
    stepSpin->setRange(1, 100);
    stepSpin->setSuffix(QStringLiteral(" %"));
    stepSpin->setValue(m_stepPercent);
    form->addRow(QStringLiteral("每格的音量步长"), stepSpin);

    auto *modifierCombo = new QComboBox(box);
    modifierCombo->setObjectName(QStringLiteral("wheelModifierCombo"));
    modifierCombo->addItem(QStringLiteral("不按修饰键（推荐）"), QStringLiteral("none"));
    modifierCombo->addItem(QStringLiteral("按住 Ctrl"), QStringLiteral("ctrl"));
    modifierCombo->addItem(QStringLiteral("按住 Alt"), QStringLiteral("alt"));
    modifierCombo->addItem(QStringLiteral("按住 Shift"), QStringLiteral("shift"));
    modifierCombo->addItem(QStringLiteral("按住 Ctrl+Alt"), QStringLiteral("ctrl+alt"));
    {
        const int index = modifierCombo->findData(m_requireModifiers);
        modifierCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    form->addRow(QStringLiteral("生效需要的修饰键"), modifierCombo);

    auto *invertBox = new QCheckBox(QStringLiteral("方向反转（向上滚=减小音量）"), box);
    invertBox->setObjectName(QStringLiteral("wheelInvertCheck"));
    invertBox->setChecked(m_invert);
    form->addRow(QString(), invertBox);

    auto *statusBox = new QCheckBox(QStringLiteral("在卡片上显示当前音量"), box);
    statusBox->setObjectName(QStringLiteral("wheelStatusCheck"));
    statusBox->setChecked(m_showStatus);
    form->addRow(QString(), statusBox);

    layout->addWidget(box);

    auto *currentLabel = new QLabel(widget);
    currentLabel->setObjectName(QStringLiteral("wheelCurrentVolumeLabel"));
    auto *refreshButton = new QPushButton(QStringLiteral("刷新当前音量"), widget);
    refreshButton->setObjectName(QStringLiteral("wheelRefreshButton"));
    auto *refreshRow = new QHBoxLayout();
    refreshRow->addWidget(currentLabel, 1);
    refreshRow->addWidget(refreshButton);
    layout->addLayout(refreshRow);

    auto refreshCurrent = [this, currentLabel] {
        if (!m_endpoint.isValid()) {
            QString error;
            m_endpoint = Win32::AudioEndpoint::defaultEndpoint(Win32::AudioDirection::Render, &error);
            if (!m_endpoint.isValid()) {
                currentLabel->setText(QStringLiteral("当前音量：取不到默认设备（%1）").arg(error));
                return;
            }
            m_deviceName = m_endpoint.deviceName();
        }
        const float volume = m_endpoint.volume();
        currentLabel->setText(volume < 0.0f
                                  ? QStringLiteral("当前音量：读取失败")
                                  : QStringLiteral("当前音量：%1（%2）")
                                        .arg(VolumeSteps::percentText(volume), m_deviceName));
    };
    connect(refreshButton, &QPushButton::clicked, widget, [refreshCurrent](bool) { refreshCurrent(); });
    refreshCurrent();

    auto *hint = new QLabel(
        QStringLiteral("范围：只在光标位于任务栏（含副屏任务栏）上时生效；"
                       "其余位置的滚轮一律放行，不影响滚动页面。\n"
                       "用鼠标滚轮事件被本功能吞掉，因此任务栏不会收到它"
                       "（也就不会触发 Aero Peek 一类的任务栏自身行为）。\n"
                       "⚠ 任务栏设为「自动隐藏」时，任务栏缩到屏幕外，滚轮落在桌面上 → 不生效。"),
        widget);
    hint->setObjectName(QStringLiteral("wheelScopeHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    connect(stepSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_stepPercent = value;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("stepPercent"), value);
            svc->syncConfig();
        }
        reportStatus(QStringLiteral("每格步长已改为 %1 个百分点").arg(value), true);
    });

    connect(modifierCombo, &QComboBox::currentIndexChanged, widget, [this, modifierCombo](int) {
        m_requireModifiers = modifierCombo->currentData().toString();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("requireModifiers"), m_requireModifiers);
            svc->syncConfig();
        }
    });

    connect(invertBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_invert = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("invert"), checked);
            svc->syncConfig();
        }
    });

    connect(statusBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_showStatus = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("showStatus"), checked);
            svc->syncConfig();
        }
    });

    return widget;
}
