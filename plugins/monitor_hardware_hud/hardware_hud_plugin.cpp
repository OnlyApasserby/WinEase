#include "hardware_hud_plugin.h"

#include "hud_overlay.h"
#include "sdk/FeatureCategory.h"
#include "sdk/OverlayHost.h"
#include "sdk/PluginServices.h"
#include "win32/WindowUtils.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace WinEase::FeaturePlugins {

namespace {

// ---------------- 配置键（落在 [Plugins/monitor.hardware_hud] 段）----------------
const QString kKeyShowCpu = QStringLiteral("showCpu");
const QString kKeyShowMemory = QStringLiteral("showMemory");
const QString kKeyShowNetwork = QStringLiteral("showNetwork");
const QString kKeyShowGpu = QStringLiteral("showGpu");
const QString kKeyShowTemperature = QStringLiteral("showTemperature");
const QString kKeyIntervalMs = QStringLiteral("intervalMs");
const QString kKeyInteractive = QStringLiteral("interactive");
const QString kKeyAnchorX = QStringLiteral("anchorX");
const QString kKeyAnchorY = QStringLiteral("anchorY");

// ---------------- 采样间隔护栏 ----------------
// 下限 500ms：SystemInfo 头注释的要求（CPU/网络采样是"两次计数之差"，太快既无意义也费电）
// 上限 10s：再慢就失去"监控"的意义了
constexpr int kMinIntervalMs = 500;
constexpr int kMaxIntervalMs = 10000;
constexpr int kIntervalStepMs = 100;

/// 开启后多久补采一次：PDH 的首次采集只建基准，0.6s 后再采一次就能立刻看到真实读数
constexpr int kWarmUpDelayMs = 600;

/// 默认位置到屏幕右/上边缘的留白
constexpr int kDefaultMarginLogical = 24;

// ---------------- 设置面板控件 objectName（自检按名取控件）----------------
const char *const kObjCpuCheck = "hudCpuCheck";
const char *const kObjMemoryCheck = "hudMemoryCheck";
const char *const kObjNetworkCheck = "hudNetworkCheck";
const char *const kObjGpuCheck = "hudGpuCheck";
const char *const kObjTemperatureCheck = "hudTemperatureCheck";
const char *const kObjIntervalSpin = "hudIntervalSpin";
const char *const kObjInteractiveCheck = "hudInteractiveCheck";
const char *const kObjResetButton = "hudResetPositionButton";
const char *const kObjDetailLabel = "hudDetailLabel";
const char *const kObjUnavailableLabel = "hudUnavailableLabel";

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

HardwareHudPlugin::HardwareHudPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

HardwareHudPlugin::~HardwareHudPlugin() = default;

// ============================================================================
//  元信息
// ============================================================================

QString HardwareHudPlugin::id() const
{
    return QStringLiteral("monitor.hardware_hud");
}

QString HardwareHudPlugin::name() const
{
    return QStringLiteral("硬件监控悬浮窗");
}

QString HardwareHudPlugin::description() const
{
    return QStringLiteral("桌面上常驻的小面板：CPU / 内存 / 网速 / GPU / 磁盘温度一眼可见");
}

QString HardwareHudPlugin::detailedDescription() const
{
    return QStringLiteral(
        "在桌面上放一个小面板，实时显示本机的硬件读数，随时瞟一眼就知道机器在干什么。\n"
        "\n"
        "本批显示 5 项指标（全部零依赖、零提权）：\n"
        "· CPU 利用率　PDH 计数器，与任务管理器同源\n"
        "· 内存占用　　已用 / 总量 + 百分比\n"
        "· 网速　　　　下行 / 上行实时速率\n"
        "· GPU 利用率　PDH 的 GPU Engine，与任务管理器同源\n"
        "· 磁盘温度　　IOCTL_STORAGE_QUERY_PROPERTY，读不到时如实说明原因\n"
        "\n"
        "已知限制：CPU / 主板温度需要 PawnIO 驱动 + 用户态 PawnIOLib.dll，"
        "本机缺 PawnIOLib.dll，因此“CPU 温度”一行显示“需要 PawnIOLib.dll（本机缺失）”"
        "而不是 0°C 或留空——该能力属于后续里程碑。\n"
        "\n"
        "用法：\n"
        "· 主快捷键开关悬浮窗；面板位置会被记住，下次开启回到原处\n"
        "· 面板默认“点击穿透”，完全不挡下层窗口的点击；"
        "切到“可拖拽”后即可用鼠标把面板拖到顺手的位置（靠近屏幕边缘会自动吸附）\n"
        "· 可逐项开关指标、可调刷新间隔（最快 500ms）\n"
        "· 读不到的指标会在对应行用琥珀色写出原因，不会伪装成 0");
}

QIcon HardwareHudPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::SystemMonitor);
}

WinEase::FeatureCategory HardwareHudPlugin::category() const
{
    return WinEase::FeatureCategory::SystemMonitor;
}

QStringList HardwareHudPlugin::tags() const
{
    return {QStringLiteral("硬件"),   QStringLiteral("监控"), QStringLiteral("温度"),
            QStringLiteral("CPU"),    QStringLiteral("内存"), QStringLiteral("网速"),
            QStringLiteral("GPU"),    QStringLiteral("悬浮窗"), QStringLiteral("HUD"),
            QStringLiteral("hardware"), QStringLiteral("monitor")};
}

// ============================================================================
//  能力标记
// ============================================================================

bool HardwareHudPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence HardwareHudPlugin::defaultHotkey() const
{
    // 主快捷键：开关悬浮窗（由宿主注册与分发）
    return QKeySequence(QStringLiteral("Ctrl+Alt+I"));
}

// ============================================================================
//  生命周期
// ============================================================================

bool HardwareHudPlugin::initialize()
{
    m_tickTimer.setTimerType(Qt::CoarseTimer); // 秒级刷新不需要精确定时器（省电）
    connect(&m_tickTimer, &QTimer::timeout, this, &HardwareHudPlugin::tick);
    return true;
}

void HardwareHudPlugin::shutdown()
{
    m_tickTimer.stop();
    m_running = false;
    destroyOverlay();
}

bool HardwareHudPlugin::canEnable(QString *reason) const
{
    const WinEase::PluginServices *svc = services();
    if (svc == nullptr || svc->overlayHost() == nullptr) {
        if (reason != nullptr) {
            *reason = QStringLiteral("悬浮层宿主不可用（安全模式下不提供）");
        }
        return false;
    }
    return true;
}

bool HardwareHudPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        Q_EMIT statusMessage(QStringLiteral("开启失败：%1").arg(lastError()));
        return false;
    }

    loadOptions();

    WinEase::OverlayHost *host = svc->overlayHost();
    if (host == nullptr) {
        setLastError(QStringLiteral("悬浮层宿主不可用（安全模式下不提供）"));
        Q_EMIT statusMessage(QStringLiteral("开启失败：%1").arg(lastError()));
        return false;
    }

    // 先采一次样：这次只为建立 PDH 基准，同时把面板的"行数"定下来
    // （行数决定窗口高度，必须在定位之前确定）
    const Hud::HudSnapshot initial = Hud::buildSnapshot(collectInput(), m_options);

    if (!ensureOverlay()) {
        Q_EMIT statusMessage(QStringLiteral("开启失败：%1").arg(lastError()));
        return false;
    }

    m_overlay->setSnapshot(initial);

    if (!m_anchorKnown) {
        m_anchor = defaultAnchor();
        m_anchorKnown = true;
        if (svc != nullptr) {
            svc->setConfigValue(id(), kKeyAnchorX, m_anchor.x());
            svc->setConfigValue(id(), kKeyAnchorY, m_anchor.y());
        }
    }
    // 允许吸附：配置里的位置可能来自已拔掉的显示器，这一步会把它拉回可见区域
    m_overlay->setAnchor(m_anchor, true);

    if (!m_overlay->isOverlayVisible()) {
        m_overlay->show();
    }

    if (!svc->registerHotkey(id(), QStringLiteral("interactive"),
                             QKeySequence(QStringLiteral("Ctrl+Shift+Alt+I")),
                             QStringLiteral("WinEase：硬件监控悬浮窗 穿透 / 可拖拽 切换"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("快捷键 Ctrl+Shift+Alt+I 注册失败（可能被别的程序占用）"));
    }
    if (!svc->registerHotkey(id(), QStringLiteral("reset"),
                             QKeySequence(QStringLiteral("Ctrl+Alt+Shift+I")),
                             QStringLiteral("WinEase：硬件监控悬浮窗 复位到默认位置"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("快捷键 Ctrl+Alt+Shift+I 注册失败（可能被别的程序占用）"));
    }

    m_running = true;
    m_lastSummary.clear();
    m_tickTimer.setInterval(qBound(kMinIntervalMs, m_intervalMs, kMaxIntervalMs));
    m_tickTimer.start();

    // 首次刷新排到事件循环下一轮：在 onEnable() 里同步读 isEnabled() 拿到的是旧值（踩坑 #51/#57）
    // 0.6s 后的补采则让 PDH 有"两次采集"可用，面板不会长时间停在“首次采样中…”
    QTimer::singleShot(kWarmUpDelayMs, this, [this] {
        if (m_running) {
            tick();
        }
    });

    Q_EMIT statusMessage(m_interactive
                             ? QStringLiteral("硬件监控悬浮窗已开启（可拖拽中：拖动面板移动，靠近边缘自动吸附）")
                             : QStringLiteral("硬件监控悬浮窗已开启（点击穿透；Ctrl+Shift+Alt+I 切换为可拖拽）"));
    return true;
}

void HardwareHudPlugin::onDisable()
{
    m_tickTimer.stop();
    m_running = false;

    WinEase::PluginServices *svc = services();
    if (svc != nullptr) {
        svc->unregisterHotkey(id(), QStringLiteral("interactive"));
        svc->unregisterHotkey(id(), QStringLiteral("reset"));
    }

    // 悬浮层所有权归宿主：只能"请求回收"，绝不能自己 delete（OverlayHost.h 的约定）
    destroyOverlay();
    m_lastSummary.clear();

    if (QPointer<QLabel> label = m_detailLabel) {
        label->setText(QStringLiteral("未开启"));
    }

    Q_EMIT statusMessage(QStringLiteral("硬件监控悬浮窗已关闭"));
}

void HardwareHudPlugin::onHotkey(const QString &hotkeyId)
{
    // ⚠ 宿主分发进来的是**完整快捷键 id**（"monitor.hardware_hud::interactive"），
    //   不是动作名。直接拿它比 "interactive" 会永远比不中 —— 第一版就是这么错的：
    //   自检里 dispatchAction() 照样返回 true，面板却毫无反应，
    //   要不是有一条"复位后位置真的变了"的断言，这个 bug 会一路带到用户手上
    //   （Ctrl+Shift+Alt+I 按下去没反应）。与 AlwaysOnTopPlugin 等既有插件保持同一写法
    //   （踩坑 #73）。
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("interactive")) {
        setInteractive(!m_interactive);
        return;
    }
    if (action == QLatin1String("reset")) {
        resetAnchor();
        return;
    }
}

// ============================================================================
//  采样
// ============================================================================

Hud::MetricInput HardwareHudPlugin::collectInput()
{
    Hud::MetricInput input;

    const WinEase::Win32::CpuSampler::Sample cpu = m_cpu.sample();
    input.cpuValid = cpu.valid;
    input.cpuPercent = cpu.overallPercent;
    input.cpuError = cpu.error;

    const WinEase::Win32::MemoryInfo memory = WinEase::Win32::memoryInfo();
    input.memoryValid = memory.valid;
    input.memoryPercent = memory.usagePercent;
    input.memoryUsedBytes = memory.usedBytes;
    input.memoryTotalBytes = memory.totalBytes;

    const WinEase::Win32::NetworkSampler::Sample network = m_network.sample();
    input.networkValid = network.valid;
    input.rxBytesPerSecond = network.rxBytesPerSecond;
    input.txBytesPerSecond = network.txBytesPerSecond;
    input.networkTotalRxBytes = network.totalRxBytes;
    input.networkTotalTxBytes = network.totalTxBytes;
    input.networkError = network.error;

    const WinEase::Win32::GpuSampler::Sample gpu = m_gpu.sample();
    input.gpuValid = gpu.valid;
    input.gpuPercent = gpu.utilizationPercent;
    input.gpuError = gpu.error;

    const QList<WinEase::Win32::DiskTemperature> disks = WinEase::Win32::diskTemperatures();
    input.disks.reserve(disks.size());
    for (const WinEase::Win32::DiskTemperature &disk : disks) {
        Hud::MetricInput::DiskReading reading;
        reading.model = disk.model;
        reading.celsius = disk.celsius;
        reading.valid = disk.valid;
        reading.error = disk.error;
        input.disks.append(reading);
    }

    // CPU / 主板温度：本批不接 PawnIO（PawnIOLib.dll 本机缺失，签名未定，不凭猜写代码）。
    // 这里如实报不可用 —— 第二里程碑把它替换成真实读数即可，其余链路无需改动
    input.cpuTempValid = false;
    input.cpuTempError = Hud::cpuTempUnavailableText();

    return input;
}

void HardwareHudPlugin::tick()
{
    if (!m_running) {
        return;
    }

    // 面板可能被宿主回收（显示器热插拔等），缺了就重建一次；
    // ensureOverlay() 只在"本插件确实没有悬浮层"时才创建，不会反复 new
    if (m_overlay.isNull()) {
        if (!ensureOverlay()) {
            return;
        }
    }

    const Hud::HudSnapshot snapshot = Hud::buildSnapshot(collectInput(), m_options);
    m_overlay->setSnapshot(snapshot);

    const QString summary = summaryText(snapshot);
    if (summary != m_lastSummary) {
        m_lastSummary = summary;
        Q_EMIT statusMessage(summary);
    }
    if (QPointer<QLabel> label = m_detailLabel) {
        label->setText(summary);
    }
}

QString HardwareHudPlugin::summaryText(const Hud::HudSnapshot &snapshot) const
{
    QStringList parts;
    parts.reserve(snapshot.metrics.size());
    for (const Hud::MetricReading &reading : snapshot.metrics) {
        parts.append(QStringLiteral("%1 %2").arg(reading.label, reading.valueText));
    }
    if (parts.isEmpty()) {
        return QStringLiteral("未选择任何指标");
    }
    return parts.join(QStringLiteral(" · "));
}

void HardwareHudPlugin::pushSnapshot()
{
    if (m_overlay.isNull()) {
        return;
    }
    m_overlay->setSnapshot(Hud::buildSnapshot(collectInput(), m_options));
}

// ============================================================================
//  悬浮层
// ============================================================================

bool HardwareHudPlugin::ensureOverlay()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host == nullptr) {
        setLastError(QStringLiteral("悬浮层宿主不可用（安全模式下不提供）"));
        return false;
    }

    const QList<WinEase::OverlayWindow *> existing = host->overlaysOfOwner(id());
    if (!existing.isEmpty()) {
        auto *typed = dynamic_cast<HudOverlay *>(existing.first());
        if (typed != nullptr) {
            m_overlay = typed;
            return true;
        }
        // 类型不对（理论上不该发生）：请求宿主回收后重建，避免拿到别人的层
        host->closeOverlaysOfOwner(id());
    }

    auto *overlay = new HudOverlay();
    overlay->setNoActivate(true);
    overlay->setClickThrough(!m_interactive);
    overlay->setCursor(m_interactive ? Qt::SizeAllCursor : Qt::ArrowCursor);

    // 位置定案后由面板上报 → 插件持久化（拖动结束/吸附完成各报一次）
    connect(overlay, &HudOverlay::anchorCommitted, this, [this](const QPoint &physicalTopLeft) {
        m_anchor = physicalTopLeft;
        m_anchorKnown = true;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), kKeyAnchorX, m_anchor.x());
            svc->setConfigValue(id(), kKeyAnchorY, m_anchor.y());
        }
    });

    if (!host->adoptOverlay(id(), overlay)) {
        // 托管失败说明宿主拒绝了它 —— 这时所有权还在我们手上，必须自己收拾
        overlay->closeOverlay();
        setLastError(QStringLiteral("创建硬件监控面板失败"));
        return false;
    }

    m_overlay = overlay;
    return true;
}

void HardwareHudPlugin::destroyOverlay()
{
    WinEase::PluginServices *svc = services();
    WinEase::OverlayHost *host = (svc != nullptr) ? svc->overlayHost() : nullptr;
    if (host != nullptr) {
        host->closeOverlaysOfOwner(id());
    }
    m_overlay = nullptr;
}

// ============================================================================
//  位置 / 交互模式
// ============================================================================

void HardwareHudPlugin::loadOptions()
{
    WinEase::PluginServices *svc = services();

    m_options.cpu = svc->configValue(id(), kKeyShowCpu, true).toBool();
    m_options.memory = svc->configValue(id(), kKeyShowMemory, true).toBool();
    m_options.network = svc->configValue(id(), kKeyShowNetwork, true).toBool();
    m_options.gpu = svc->configValue(id(), kKeyShowGpu, true).toBool();
    m_options.temperature = svc->configValue(id(), kKeyShowTemperature, true).toBool();

    m_intervalMs = qBound(kMinIntervalMs,
                          svc->configValue(id(), kKeyIntervalMs, 1000).toInt(),
                          kMaxIntervalMs);
    m_interactive = svc->configValue(id(), kKeyInteractive, false).toBool();

    const QVariant anchorX = svc->configValue(id(), kKeyAnchorX);
    const QVariant anchorY = svc->configValue(id(), kKeyAnchorY);
    if (anchorX.isValid() && anchorY.isValid()) {
        m_anchor = QPoint(anchorX.toInt(), anchorY.toInt());
        m_anchorKnown = true;
    }

    if (QPointer<QSpinBox> spin = m_intervalSpin) {
        const QSignalBlocker blocker(spin); // 程序化回填控件必须挡信号，否则回调又改控件（踩坑 #28）
        spin->setValue(m_intervalMs);
    }
    if (QPointer<QCheckBox> check = m_interactiveCheck) {
        const QSignalBlocker blocker(check);
        check->setChecked(m_interactive);
    }
}

QPoint HardwareHudPlugin::defaultAnchor() const
{
    WinEase::Win32::MonitorInfo monitor = WinEase::Win32::primaryMonitor();
    QRect area = monitor.valid ? monitor.workArea : QRect();
    if (area.isEmpty() && monitor.valid) {
        area = monitor.geometry;
    }
    if (area.isEmpty()) {
        area = QRect(0, 0, 1920, 1080);
    }

    const qreal scale = (monitor.scaleFactor > 0.0) ? monitor.scaleFactor : 1.0;

    // 面板真实尺寸优先（避免估算误差把人放到屏幕外）；面板还没建时用合理估值兜底
    QSize panelLogical(300, 168);
    if (!m_overlay.isNull()) {
        panelLogical = m_overlay->panelLogicalSize();
    }

    const QSize panelPhysical(qRound(panelLogical.width() * scale),
                              qRound(panelLogical.height() * scale));
    const int margin = qRound(kDefaultMarginLogical * scale);

    return QPoint(area.right() + 1 - panelPhysical.width() - margin, area.top() + margin);
}

void HardwareHudPlugin::setInteractive(bool interactive)
{
    m_interactive = interactive;

    WinEase::PluginServices *svc = services();
    if (svc != nullptr) {
        svc->setConfigValue(id(), kKeyInteractive, interactive);
    }

    if (!m_overlay.isNull()) {
        m_overlay->setClickThrough(!interactive);
        m_overlay->setCursor(interactive ? Qt::SizeAllCursor : Qt::ArrowCursor);
    }

    if (QPointer<QCheckBox> check = m_interactiveCheck) {
        const QSignalBlocker blocker(check);
        check->setChecked(interactive);
    }

    Q_EMIT statusMessage(interactive
                             ? QStringLiteral("面板可拖拽：按住任意位置拖动，靠近屏幕边缘会自动吸附")
                             : QStringLiteral("面板已切换为点击穿透（不再挡住下层窗口的点击）"));
}

void HardwareHudPlugin::resetAnchor()
{
    m_anchor = defaultAnchor();
    m_anchorKnown = true;

    WinEase::PluginServices *svc = services();
    if (svc != nullptr) {
        svc->setConfigValue(id(), kKeyAnchorX, m_anchor.x());
        svc->setConfigValue(id(), kKeyAnchorY, m_anchor.y());
    }

    if (!m_overlay.isNull()) {
        m_overlay->setAnchor(m_anchor, true);
    }

    Q_EMIT statusMessage(QStringLiteral("面板已复位到默认位置（主屏右上角）"));
}

// ============================================================================
//  设置面板
// ============================================================================

QWidget *HardwareHudPlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // ---------------- 显示指标 ----------------
    auto *metricsGroup = new QGroupBox(QStringLiteral("显示指标"), page);
    auto *metricsGrid = new QGridLayout(metricsGroup);
    metricsGrid->setContentsMargins(12, 12, 12, 12);
    metricsGrid->setHorizontalSpacing(16);
    metricsGrid->setVerticalSpacing(8);

    const auto makeCheck = [&](const QString &objectName, const QString &text, bool checked,
                               const QString &tooltip, int row, int column) {
        auto *check = new QCheckBox(text, metricsGroup);
        check->setObjectName(objectName);
        check->setChecked(checked);
        check->setToolTip(tooltip);
        metricsGrid->addWidget(check, row, column);
        return check;
    };

    QCheckBox *cpuCheck = makeCheck(QString::fromLatin1(kObjCpuCheck), QStringLiteral("CPU 利用率"),
                                    m_options.cpu,
                                    QStringLiteral("PDH 计数器，与任务管理器同源。首次采样只建立基准。"),
                                    0, 0);
    QCheckBox *memoryCheck = makeCheck(QString::fromLatin1(kObjMemoryCheck), QStringLiteral("内存占用"),
                                       m_options.memory,
                                       QStringLiteral("GlobalMemoryStatusEx：已用 / 总量 + 百分比。"),
                                       0, 1);
    QCheckBox *networkCheck = makeCheck(QString::fromLatin1(kObjNetworkCheck), QStringLiteral("网速"),
                                        m_options.network,
                                        QStringLiteral("实时上下行速率；已排除回环、隧道与虚拟适配器。"),
                                        0, 2);
    QCheckBox *gpuCheck = makeCheck(QString::fromLatin1(kObjGpuCheck), QStringLiteral("GPU 利用率"),
                                    m_options.gpu,
                                    QStringLiteral("PDH 的 GPU Engine。虚拟机 / 无独显 / 精简系统上可能整机不可用。"),
                                    1, 0);
    QCheckBox *temperatureCheck = makeCheck(
        QString::fromLatin1(kObjTemperatureCheck), QStringLiteral("温度"),
        m_options.temperature,
        QStringLiteral("磁盘温度（零依赖，可读）+ CPU 温度（需要 PawnIOLib.dll，本机缺失）。"),
        1, 1);

    layout->addWidget(metricsGroup);

    // ---------------- 刷新与交互 ----------------
    auto *behaviourGroup = new QGroupBox(QStringLiteral("刷新与交互"), page);
    auto *behaviourLayout = new QVBoxLayout(behaviourGroup);
    behaviourLayout->setContentsMargins(12, 12, 12, 12);
    behaviourLayout->setSpacing(8);

    auto *intervalRow = new QWidget(behaviourGroup);
    auto *intervalLayout = new QHBoxLayout(intervalRow);
    intervalLayout->setContentsMargins(0, 0, 0, 0);
    intervalLayout->setSpacing(8);
    auto *intervalLabel = new QLabel(QStringLiteral("刷新间隔"), intervalRow);
    auto *intervalSpin = new QSpinBox(intervalRow);
    intervalSpin->setObjectName(QString::fromLatin1(kObjIntervalSpin));
    intervalSpin->setRange(kMinIntervalMs, kMaxIntervalMs);
    intervalSpin->setSingleStep(kIntervalStepMs);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    intervalSpin->setValue(m_intervalMs);
    intervalSpin->setToolTip(QStringLiteral(
        "最快 %1 ms。CPU / 网速属于“两次计数之差”，间隔太短既测不准也费电。")
                                 .arg(kMinIntervalMs));
    intervalLayout->addWidget(intervalLabel);
    intervalLayout->addWidget(intervalSpin);
    intervalLayout->addStretch(1);
    behaviourLayout->addWidget(intervalRow);

    auto *interactiveCheck = new QCheckBox(
        QStringLiteral("可拖拽（临时关掉点击穿透，以便把面板拖到顺手的位置）"), behaviourGroup);
    interactiveCheck->setObjectName(QString::fromLatin1(kObjInteractiveCheck));
    interactiveCheck->setChecked(m_interactive);
    interactiveCheck->setToolTip(QStringLiteral(
        "默认整层点击穿透，完全不挡下层窗口。\n"
        "勾选后：面板本体可以按住拖动，靠近屏幕边缘会自动吸附；"
        "代价是落在面板上的点击会被面板吃掉。\n"
        "快捷键 Ctrl+Shift+Alt+I 同效，Ctrl+Alt+Shift+I 复位到默认位置。"));
    behaviourLayout->addWidget(interactiveCheck);

    auto *resetButton = new QPushButton(QStringLiteral("复位到默认位置（主屏右上角）"), behaviourGroup);
    resetButton->setObjectName(QString::fromLatin1(kObjResetButton));
    behaviourLayout->addWidget(resetButton);

    auto *detailLabel = new QLabel(QStringLiteral("未开启"), behaviourGroup);
    detailLabel->setObjectName(QString::fromLatin1(kObjDetailLabel));
    detailLabel->setWordWrap(true);
    detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    behaviourLayout->addWidget(detailLabel);

    layout->addWidget(behaviourGroup);

    // ---------------- 已知限制（如实说明，不藏）----------------
    auto *limitLabel = new QLabel(Hud::cpuTempTooltipText(), page);
    limitLabel->setObjectName(QString::fromLatin1(kObjUnavailableLabel));
    limitLabel->setWordWrap(true);
    limitLabel->setEnabled(false); // 说明性文字，弱化显示
    layout->addWidget(limitLabel);

    layout->addStretch(1);

    // ---------------- 信号 ----------------
    const auto bindOption = [this](QCheckBox *check, bool Hud::MetricOptions::*member,
                                   const QString &key) {
        connect(check, &QCheckBox::toggled, this, [this, member, key](bool checked) {
            m_options.*member = checked;
            if (WinEase::PluginServices *svc = services()) {
                svc->setConfigValue(id(), key, checked);
            }
            // 立即反映到面板上（不等下一次定时器）——勾选就能看到行数的变化
            pushSnapshot();
        });
    };
    bindOption(cpuCheck, &Hud::MetricOptions::cpu, kKeyShowCpu);
    bindOption(memoryCheck, &Hud::MetricOptions::memory, kKeyShowMemory);
    bindOption(networkCheck, &Hud::MetricOptions::network, kKeyShowNetwork);
    bindOption(gpuCheck, &Hud::MetricOptions::gpu, kKeyShowGpu);
    bindOption(temperatureCheck, &Hud::MetricOptions::temperature, kKeyShowTemperature);

    connect(intervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        m_intervalMs = qBound(kMinIntervalMs, value, kMaxIntervalMs);
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), kKeyIntervalMs, m_intervalMs);
        }
        if (m_running) {
            m_tickTimer.setInterval(m_intervalMs);
        }
    });

    connect(interactiveCheck, &QCheckBox::toggled, this, &HardwareHudPlugin::setInteractive);
    connect(resetButton, &QPushButton::clicked, this, &HardwareHudPlugin::resetAnchor);

    m_interactiveCheck = interactiveCheck;
    m_intervalSpin = intervalSpin;
    m_detailLabel = detailLabel;

    if (m_running) {
        detailLabel->setText(m_lastSummary.isEmpty() ? QStringLiteral("采样中…") : m_lastSummary);
    }

    return page;
}

} // namespace WinEase::FeaturePlugins
