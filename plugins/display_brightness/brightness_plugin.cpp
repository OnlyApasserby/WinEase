#include "brightness_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/ComApartment.h"
#include "win32/WindowUtils.h"

#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

using WinEase::Win32::BrightnessChannel;
using WinEase::Win32::GammaSnapshot;

/// 色温面板范围：6500K = "不调色"档（直接还原原始 ramp）
constexpr int kMinKelvin = 2500;
constexpr int kNeutralKelvin = 6500;

/// 亮度防抖：拖一次滑块会发几十次 valueChanged，硬件写入慢且没必要那么频繁
constexpr int kBrightnessDebounceMs = 150;
/// 护眼时段轮询：一分钟一次足够（时段边界最细就是分钟）
constexpr int kScheduleTickMs = 60 * 1000;
/// 亮度真值轮询：用户用 Fn 键 / 系统托盘 / 显示器按钮改亮度时，面板上的数字要能跟上
constexpr int kPollIntervalMs = 5 * 1000;
/// 快捷键调亮度的步长（%）
constexpr int kBrightnessStep = 10;

int clampKelvin(int kelvin)
{
    return std::clamp(kelvin, kMinKelvin, kNeutralKelvin);
}

QTime parseTime(const QString &text, const QTime &fallback)
{
    const QTime parsed = QTime::fromString(text, QStringLiteral("HH:mm"));
    return parsed.isValid() ? parsed : fallback;
}

QString timeText(const QTime &time)
{
    return time.isValid() ? time.toString(QStringLiteral("HH:mm")) : QStringLiteral("--:--");
}

/// 人话化的色温描述（6500K 是"不调色"，别让用户以为自己在设一个滤镜强度）
QString kelvinText(int kelvin)
{
    return kelvin >= kNeutralKelvin ? QStringLiteral("6500K（不调色）")
                                    : QStringLiteral("%1K").arg(kelvin);
}

} // namespace

BrightnessPlugin::BrightnessPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString BrightnessPlugin::id() const
{
    return QStringLiteral("display.brightness");
}

QString BrightnessPlugin::name() const
{
    return QStringLiteral("亮度色温护眼");
}

QString BrightnessPlugin::description() const
{
    return QStringLiteral("外接屏走 DDC/CI、内置屏走 WMI 调亮度；色温与按时间自动切换的护眼模式");
}

QIcon BrightnessPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::DisplayAssist);
}

WinEase::FeatureCategory BrightnessPlugin::category() const
{
    return WinEase::FeatureCategory::DisplayAssist;
}

QStringList BrightnessPlugin::tags() const
{
    return { QStringLiteral("亮度"), QStringLiteral("色温"), QStringLiteral("护眼"),
             QStringLiteral("夜间"), QStringLiteral("gamma"), QStringLiteral("liangdu"),
             QStringLiteral("huyan") };
}

bool BrightnessPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence BrightnessPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+B"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool BrightnessPlugin::initialize()
{
    // 配置只在 initialize() 读一次，之后改动走面板控件（踩坑 #26）
    if (WinEase::PluginServices *svc = services()) {
        m_kelvin = clampKelvin(svc->configValue(id(), QStringLiteral("kelvin"), kNeutralKelvin).toInt());
        m_eyeCareEnabled = svc->configValue(id(), QStringLiteral("eyeCare"), false).toBool();
        m_dayKelvin = clampKelvin(
            svc->configValue(id(), QStringLiteral("dayKelvin"), kNeutralKelvin).toInt());
        m_nightKelvin = clampKelvin(
            svc->configValue(id(), QStringLiteral("nightKelvin"), 3400).toInt());
        m_nightStart = parseTime(svc->configValue(id(), QStringLiteral("nightStart"),
                                                  QStringLiteral("21:00")).toString(),
                                 QTime(21, 0));
        m_nightEnd = parseTime(svc->configValue(id(), QStringLiteral("nightEnd"),
                                                QStringLiteral("07:00")).toString(),
                               QTime(7, 0));
    }

    if (m_brightnessTimer == nullptr) {
        m_brightnessTimer = new QTimer(this);
        m_brightnessTimer->setSingleShot(true);
        m_brightnessTimer->setInterval(kBrightnessDebounceMs);
        connect(m_brightnessTimer, &QTimer::timeout, this, [this] {
            applyBrightness(m_brightnessPercent);
        });
    }

    if (m_scheduleTimer == nullptr) {
        m_scheduleTimer = new QTimer(this);
        m_scheduleTimer->setInterval(kScheduleTickMs);
        connect(m_scheduleTimer, &QTimer::timeout, this, [this] {
            // 一分钟一次：既判断时段是否跨过边界，也把"被系统/别的软件重置掉的 gamma"纠正回来
            if (isEnabled() && m_eyeCareEnabled) {
                applyColorTemperature();
            }
        });
    }

    if (m_pollTimer == nullptr) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(kPollIntervalMs);
        connect(m_pollTimer, &QTimer::timeout, this, [this] { pollBrightness(); });
    }

    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("亮度色温已就绪（色温 %1，护眼 %2）")
                   .arg(kelvinText(m_kelvin))
                   .arg(m_eyeCareEnabled ? QStringLiteral("开") : QStringLiteral("关")));
    return true;
}

void BrightnessPlugin::shutdown()
{
    stopScheduleTimer();
    if (m_brightnessTimer != nullptr) {
        m_brightnessTimer->stop();
    }
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
}

bool BrightnessPlugin::canEnable(QString *reason) const
{
    if (!WinEase::Win32::isThreadComReady()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("当前线程未初始化 COM，无法访问显示器控制通路");
        }
        return false;
    }

    // 只要**至少有一条**可用通路就允许启用：亮度（DDC/CI 或 WMI）与 gamma 色温，
    // 任意一块屏上任意一条成立即可 —— 笔记本外接屏、远程桌面等场景下不能一概拒绝
    const QList<WinEase::Win32::MonitorInfo> all = WinEase::Win32::monitors();
    for (int index = 0; index < all.size(); ++index) {
        if (WinEase::Win32::isGammaSupported(index)) {
            return true;
        }
        if (WinEase::Win32::resolveBrightnessChannel(index).supported) {
            return true;
        }
    }

    if (reason != nullptr) {
        *reason = QStringLiteral("本机既没有可写亮度的显示器，也不支持 gamma 色温调节");
    }
    return false;
}

bool BrightnessPlugin::onEnable()
{
    rescanChannels();

    // 色温是叠在系统上的滤镜：先记下原始 ramp 当基准，之后每次调节都基于它重算
    m_gammaBaselines.clear();
    m_appliedRamps.clear();
    m_appliedKelvin = -1;
    const int monitorCount = WinEase::Win32::monitors().size();
    for (int index = 0; index < monitorCount; ++index) {
        m_gammaBaselines.append(WinEase::Win32::captureGamma(index));
    }

    // 应用当前应有的色温（6500K 时等于不动；护眼开启时按当前时段）
    applyColorTemperature();

    // ⚠ 亮度**不主动写**：它是硬件状态，启用一项功能不该去动用户的屏幕亮度。
    //   面板上显示的是 rescanChannels() 读回来的真值。

    if (m_eyeCareEnabled) {
        startScheduleTimer();
    }

    Q_EMIT statusMessage(QStringLiteral("已就绪：%1 · %2")
                             .arg(channelText(), scheduleText()));

    // ⚠ 时序：onEnable() 期间宿主的"已启用"状态还没落定，此刻 isEnabled() 仍是 false，
    //    直接刷面板会把控件留在"禁用"上（用户看到能按但按不动）。
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void BrightnessPlugin::onDisable()
{
    stopScheduleTimer();
    if (m_brightnessTimer != nullptr) {
        m_brightnessTimer->stop();
    }

    // 停用 = 完整还原：色温是叠加在系统上的滤镜，必须逐元素还原到启用前的 ramp
    int restored = 0;
    QStringList failures;
    for (const GammaSnapshot &baseline : m_gammaBaselines) {
        if (!baseline.valid) {
            continue;
        }
        QString error;
        if (WinEase::Win32::restoreGamma(baseline, &error)) {
            ++restored;
        } else {
            failures << QStringLiteral("显示器 %1：%2").arg(baseline.monitorIndex).arg(error);
        }
    }
    m_gammaBaselines.clear();
    m_appliedRamps.clear();
    m_appliedKelvin = -1;
    m_lastApplyFailed = false;

    // ⚠ 亮度**有意不还原**（与 media.mic_mute 的 fail-closed 例外同类）：
    //   屏幕现在多亮就是用户眼前的样子，停用一项功能时把它改回去是**新的副作用**。
    //   色温必须还原、亮度必须不还原 —— 这两条都被自检钉住了。
    if (failures.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("已停用：色温已还原（%1 块屏），亮度保持现状")
                                 .arg(restored));
    } else {
        Q_EMIT statusMessage(QStringLiteral("已停用：色温还原失败（%1）").arg(failures.join(QStringLiteral("；"))));
    }

    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

void BrightnessPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return; // <id>::default 在加载时就注册了，停用期间也可能被分发到
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("default") || action == QLatin1String("toggle")) {
        setEyeCareEnabled(!m_eyeCareEnabled, true);
        return;
    }
    if (action == QLatin1String("brighter")) {
        applyBrightness(m_brightnessPercent + kBrightnessStep);
        return;
    }
    if (action == QLatin1String("darker")) {
        applyBrightness(m_brightnessPercent - kBrightnessStep);
    }
}

// ---------------------------------------------------------------------------
//  亮度
// ---------------------------------------------------------------------------

void BrightnessPlugin::rescanChannels()
{
    m_channels.clear();

    const QList<WinEase::Win32::MonitorInfo> all = WinEase::Win32::monitors();
    for (int index = 0; index < all.size(); ++index) {
        m_channels.append(WinEase::Win32::resolveBrightnessChannel(index));
    }

    // 面板显示的亮度是**真值**（第一条可用通路读回来的），不是我们记的值
    for (const BrightnessChannel &channel : m_channels) {
        if (channel.supported) {
            m_brightnessPercent = channel.percent;
            break;
        }
    }
}

void BrightnessPlugin::requestBrightness(double percent)
{
    m_brightnessPercent = std::clamp(percent, 0.0, 100.0);

    if (!isEnabled()) {
        refreshPanel();
        return;
    }
    if (m_brightnessTimer != nullptr) {
        m_brightnessTimer->start(); // 防抖：150ms 内多次改动只写一次硬件
    } else {
        applyBrightness(m_brightnessPercent);
    }
    refreshPanel();
}

void BrightnessPlugin::applyBrightness(double percent)
{
    if (m_channels.isEmpty()) {
        rescanChannels();
    }

    const double target = std::clamp(percent, 0.0, 100.0);
    int written = 0;
    QStringList applied;
    QStringList problems;

    for (const BrightnessChannel &channel : m_channels) {
        if (!channel.supported) {
            continue; // 外接屏不支持亮度是正常情况，不算失败
        }

        QString error;
        if (!WinEase::Win32::setBrightnessPercentOn(channel, target, &error)) {
            problems << QStringLiteral("%1：%2").arg(channel.target, error);
            continue;
        }

        // 写硬件之后**回读确认**（与麦克风静音同一条纪律：不信自己刚写进去的参数）
        double actual = 0.0;
        if (!WinEase::Win32::readBrightnessPercent(channel, &actual, &error)) {
            problems << QStringLiteral("%1：写入后读回失败（%2）").arg(channel.target, error);
            continue;
        }
        ++written;
        applied << QStringLiteral("%1 → %2%（%3）")
                       .arg(channel.target)
                       .arg(actual, 0, 'f', 0)
                       .arg(WinEase::Win32::brightnessBackendText(channel.backend));
        m_brightnessPercent = actual;
    }

    if (written == 0) {
        const QString message = problems.isEmpty()
                                    ? QStringLiteral("本机没有可调节亮度的显示器")
                                    : problems.join(QStringLiteral("；"));
        setLastError(message);
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("设置亮度失败：%1").arg(message));
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), message);
        }
    } else {
        clearLastError();
        Q_EMIT statusMessage(QStringLiteral("亮度 %1%").arg(m_brightnessPercent, 0, 'f', 0));
        if (!problems.isEmpty()) {
            logMessage(WinEase::PluginLogLevel::Warning,
                       QStringLiteral("部分显示器亮度未写入：%1").arg(problems.join(QStringLiteral("；"))));
        }
    }

    refreshPanel();
}

void BrightnessPlugin::pollBrightness()
{
    // 只读不写：用户用 Fn 键、系统托盘或显示器自己的按钮改了亮度，
    // 面板上的那个数字必须跟着变 —— 我们**不自己记一份**亮度状态
    for (const BrightnessChannel &channel : m_channels) {
        if (!channel.supported) {
            continue;
        }

        double actual = 0.0;
        if (WinEase::Win32::readBrightnessPercent(channel, &actual)) {
            if (std::abs(actual - m_brightnessPercent) >= 1.0) {
                m_brightnessPercent = actual;
                refreshPanel();
            }
        }
        return; // 面板上只显示一条亮度（第一条可用通路）
    }
}

void BrightnessPlugin::syncPollTimer()
{
    const bool hasChannel = std::any_of(m_channels.cbegin(), m_channels.cend(),
                                        [](const BrightnessChannel &channel) {
                                            return channel.supported;
                                        });
    const bool shouldPoll = isEnabled() && !m_panel.isNull() && hasChannel;

    if (m_pollTimer == nullptr) {
        return;
    }
    if (shouldPoll && !m_pollTimer->isActive()) {
        m_pollTimer->start();
    } else if (!shouldPoll && m_pollTimer->isActive()) {
        m_pollTimer->stop();
    }
}

// ---------------------------------------------------------------------------
//  色温 / 护眼
// ---------------------------------------------------------------------------

bool BrightnessPlugin::isNightTime(const QTime &now) const
{
    if (!m_nightStart.isValid() || !m_nightEnd.isValid() || m_nightStart == m_nightEnd) {
        return false; // 起止相同 = 不启用夜间时段
    }
    if (m_nightStart < m_nightEnd) {
        return now >= m_nightStart && now < m_nightEnd;
    }
    // 跨午夜（例如 21:00 – 次日 07:00）：落在任一侧都算夜间
    return now >= m_nightStart || now < m_nightEnd;
}

QTime BrightnessPlugin::nextSwitchTime(const QTime &now) const
{
    if (!m_nightStart.isValid() || !m_nightEnd.isValid() || m_nightStart == m_nightEnd) {
        return QTime();
    }
    return isNightTime(now) ? m_nightEnd : m_nightStart;
}

int BrightnessPlugin::scheduledKelvin() const
{
    if (!m_eyeCareEnabled) {
        return m_kelvin;
    }
    return isNightTime(QTime::currentTime()) ? m_nightKelvin : m_dayKelvin;
}

bool BrightnessPlugin::colorTemperatureSettled() const
{
    if (m_appliedKelvin != scheduledKelvin()) {
        return false;
    }
    if (m_appliedRamps.isEmpty()) {
        return false;
    }
    // 我们写下去的那份 ramp 还在不在盘上？锁屏、切换显示模式、别的调色软件都会把它重置 ——
    // 只在自己缓存里说"已经应用过了"是不算数的
    for (const GammaSnapshot &appliedRamp : m_appliedRamps) {
        if (!appliedRamp.valid) {
            return false;
        }
        const GammaSnapshot current = WinEase::Win32::captureGamma(appliedRamp.monitorIndex);
        if (!current.valid || current.ramp != appliedRamp.ramp) {
            return false;
        }
    }
    return true;
}

void BrightnessPlugin::applyColorTemperature()
{
    if (m_gammaBaselines.isEmpty()) {
        // 面板可能在没有启用过的状态下被打开（例如宿主预览设置面板）
        const int monitorCount = WinEase::Win32::monitors().size();
        for (int index = 0; index < monitorCount; ++index) {
            m_gammaBaselines.append(WinEase::Win32::captureGamma(index));
        }
    }

    if (colorTemperatureSettled()) {
        return; // 时段没跨边界、盘上还是我们写的那份 → 不重复写 gamma
    }

    const int kelvin = scheduledKelvin();
    int applied = 0;
    QStringList failures;
    m_appliedRamps.clear();

    for (const GammaSnapshot &baseline : m_gammaBaselines) {
        if (!baseline.valid) {
            continue;
        }

        QString error;
        // 6500K = "不调色"档：直接还原原始 ramp（比套一组近似 1.0 的增益干净）
        const bool ok = kelvin >= kNeutralKelvin
                            ? WinEase::Win32::restoreGamma(baseline, &error)
                            : WinEase::Win32::applyColorTemperature(baseline.monitorIndex,
                                                                    kelvin,
                                                                    baseline,
                                                                    &error);
        if (!ok) {
            failures << QStringLiteral("显示器 %1：%2").arg(baseline.monitorIndex).arg(error);
            continue;
        }

        ++applied;
        // 记下"写下去之后读回来的样子"，供 colorTemperatureSettled() 日后核对
        m_appliedRamps.append(WinEase::Win32::captureGamma(baseline.monitorIndex));
    }

    m_appliedKelvin = kelvin;

    if (!failures.isEmpty()) {
        // 状态没变就不重复刷屏（一分钟一次的轮询会重试，日志不能跟着刷），
        // 但错误必须让宿主看得见（卡片/日志里要有话说，不能只在本插件内部吞掉）
        const QString message = failures.join(QStringLiteral("；"));
        setLastError(QStringLiteral("色温 %1K 应用失败：%2").arg(kelvin).arg(message));
        if (!m_lastApplyFailed) {
            logMessage(WinEase::PluginLogLevel::Warning,
                       QStringLiteral("色温应用失败：%1").arg(message));
            if (WinEase::PluginServices *svc = services()) {
                svc->notify(name(), QStringLiteral("色温应用失败：%1").arg(message));
            }
            m_lastApplyFailed = true;
        }
    } else {
        clearLastError();
        m_lastApplyFailed = false;
    }

    refreshPanel();
}

void BrightnessPlugin::setKelvin(int kelvin, bool persist)
{
    m_kelvin = clampKelvin(kelvin);

    if (persist) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("kelvin"), m_kelvin);
            svc->syncConfig();
        }
    }

    syncKelvinControls();
    if (isEnabled() && !m_eyeCareEnabled) {
        applyColorTemperature();
    }
    refreshPanel();
}

void BrightnessPlugin::setEyeCareEnabled(bool enabled, bool persist)
{
    m_eyeCareEnabled = enabled;

    if (persist) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("eyeCare"), m_eyeCareEnabled);
            svc->syncConfig();
        }
    }

    if (enabled) {
        startScheduleTimer();
    } else {
        stopScheduleTimer();
    }

    // 开关一变就立刻按新规则重算色温（用户改设置要马上看到效果，不能等下一个轮询周期）
    if (isEnabled()) {
        applyColorTemperature();
    }

    Q_EMIT statusMessage(enabled ? QStringLiteral("护眼模式已开启：%1").arg(scheduleText())
                                 : QStringLiteral("护眼模式已关闭，色温回到 %1")
                                       .arg(kelvinText(m_kelvin)));
    refreshPanel();
}

void BrightnessPlugin::startScheduleTimer()
{
    if (m_scheduleTimer != nullptr && !m_scheduleTimer->isActive()) {
        m_scheduleTimer->start();
    }
}

void BrightnessPlugin::stopScheduleTimer()
{
    if (m_scheduleTimer != nullptr) {
        m_scheduleTimer->stop();
    }
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *BrightnessPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("brightnessPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("briStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *channelLabel = new QLabel(widget);
    channelLabel->setObjectName(QStringLiteral("briChannelLabel"));
    channelLabel->setWordWrap(true);
    layout->addWidget(channelLabel);

    // ---- 亮度 ----
    auto *brightnessRow = new QHBoxLayout();
    brightnessRow->addWidget(new QLabel(QStringLiteral("亮度："), widget));
    auto *brightnessSlider = new QSlider(Qt::Horizontal, widget);
    brightnessSlider->setObjectName(QStringLiteral("briBrightnessSlider"));
    brightnessSlider->setRange(0, 100);
    brightnessSlider->setValue(qRound(m_brightnessPercent));
    brightnessRow->addWidget(brightnessSlider, 1);
    auto *brightnessSpin = new QSpinBox(widget);
    brightnessSpin->setObjectName(QStringLiteral("briBrightnessSpin"));
    brightnessSpin->setRange(0, 100);
    brightnessSpin->setSuffix(QStringLiteral(" %"));
    brightnessSpin->setValue(qRound(m_brightnessPercent));
    brightnessRow->addWidget(brightnessSpin);
    layout->addLayout(brightnessRow);

    // ---- 色温 ----
    auto *kelvinRow = new QHBoxLayout();
    kelvinRow->addWidget(new QLabel(QStringLiteral("色温："), widget));
    auto *previewLabel = new QLabel(widget);
    previewLabel->setObjectName(QStringLiteral("briColorPreview"));
    previewLabel->setFixedSize(56, 22);
    kelvinRow->addWidget(previewLabel);
    auto *kelvinSlider = new QSlider(Qt::Horizontal, widget);
    kelvinSlider->setObjectName(QStringLiteral("briKelvinSlider"));
    kelvinSlider->setRange(kMinKelvin, kNeutralKelvin);
    kelvinSlider->setSingleStep(100);
    kelvinSlider->setValue(m_kelvin);
    kelvinRow->addWidget(kelvinSlider, 1);
    auto *kelvinSpin = new QSpinBox(widget);
    kelvinSpin->setObjectName(QStringLiteral("briKelvinSpin"));
    kelvinSpin->setRange(kMinKelvin, kNeutralKelvin);
    kelvinSpin->setSingleStep(100);
    kelvinSpin->setSuffix(QStringLiteral(" K"));
    kelvinSpin->setValue(m_kelvin);
    kelvinRow->addWidget(kelvinSpin);
    layout->addLayout(kelvinRow);

    // ---- 护眼模式 ----
    auto *eyeCareCheck = new QCheckBox(QStringLiteral("护眼模式（按时间自动切换色温）"), widget);
    eyeCareCheck->setObjectName(QStringLiteral("briEyeCareCheck"));
    eyeCareCheck->setChecked(m_eyeCareEnabled);
    layout->addWidget(eyeCareCheck);

    auto *dayRow = new QHBoxLayout();
    dayRow->addWidget(new QLabel(QStringLiteral("日间色温："), widget));
    auto *dayKelvinSpin = new QSpinBox(widget);
    dayKelvinSpin->setObjectName(QStringLiteral("briDayKelvinSpin"));
    dayKelvinSpin->setRange(kMinKelvin, kNeutralKelvin);
    dayKelvinSpin->setSingleStep(100);
    dayKelvinSpin->setSuffix(QStringLiteral(" K"));
    dayKelvinSpin->setValue(m_dayKelvin);
    dayRow->addWidget(dayKelvinSpin);
    dayRow->addSpacing(12);
    dayRow->addWidget(new QLabel(QStringLiteral("夜间色温："), widget));
    auto *nightKelvinSpin = new QSpinBox(widget);
    nightKelvinSpin->setObjectName(QStringLiteral("briNightKelvinSpin"));
    nightKelvinSpin->setRange(kMinKelvin, kNeutralKelvin);
    nightKelvinSpin->setSingleStep(100);
    nightKelvinSpin->setSuffix(QStringLiteral(" K"));
    nightKelvinSpin->setValue(m_nightKelvin);
    dayRow->addWidget(nightKelvinSpin);
    dayRow->addStretch(1);
    layout->addLayout(dayRow);

    auto *rangeRow = new QHBoxLayout();
    rangeRow->addWidget(new QLabel(QStringLiteral("夜间时段："), widget));
    auto *nightStartEdit = new QTimeEdit(widget);
    nightStartEdit->setObjectName(QStringLiteral("briNightStartEdit"));
    nightStartEdit->setDisplayFormat(QStringLiteral("HH:mm"));
    nightStartEdit->setTime(m_nightStart);
    rangeRow->addWidget(nightStartEdit);
    rangeRow->addWidget(new QLabel(QStringLiteral("至"), widget));
    auto *nightEndEdit = new QTimeEdit(widget);
    nightEndEdit->setObjectName(QStringLiteral("briNightEndEdit"));
    nightEndEdit->setDisplayFormat(QStringLiteral("HH:mm"));
    nightEndEdit->setTime(m_nightEnd);
    rangeRow->addWidget(nightEndEdit);
    rangeRow->addStretch(1);
    layout->addLayout(rangeRow);

    auto *scheduleLabel = new QLabel(widget);
    scheduleLabel->setObjectName(QStringLiteral("briScheduleLabel"));
    scheduleLabel->setWordWrap(true);
    layout->addWidget(scheduleLabel);

    auto *hint = new QLabel(
        QStringLiteral("快捷键：%1 开关护眼模式（可在主界面「快捷键」里修改）。\n"
                       "亮度走显示器自己的通路：外接屏 DDC/CI、笔记本内置屏 WMI，"
                       "面板上的数字是**刚从系统读回来的真值**（用 Fn 键或系统托盘改过也会跟着变，"
                       "本插件不自己记一份）。\n"
                       "色温走显卡 gamma，6500K 即「不调色」；停用功能会把 gamma 逐元素还原成"
                       "启用前的样子，而亮度**保持现状**（那是你眼前的屏幕状态，不该被我们改回去）。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText)),
        widget);
    hint->setObjectName(QStringLiteral("briHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_channelLabel = channelLabel;
    m_scheduleLabel = scheduleLabel;
    m_previewLabel = previewLabel;
    m_brightnessSlider = brightnessSlider;
    m_brightnessSpin = brightnessSpin;
    m_kelvinSlider = kelvinSlider;
    m_kelvinSpin = kelvinSpin;
    m_eyeCareCheck = eyeCareCheck;
    m_dayKelvinSpin = dayKelvinSpin;
    m_nightKelvinSpin = nightKelvinSpin;
    m_nightStartEdit = nightStartEdit;
    m_nightEndEdit = nightEndEdit;

    // 滑块与数字框双向同步（程序性赋值要用 QSignalBlocker 挡住，否则来回弹）
    connect(brightnessSlider, &QSlider::valueChanged, widget, [this](int value) {
        if (!m_brightnessSpin.isNull()) {
            QSignalBlocker blocker(m_brightnessSpin.data());
            m_brightnessSpin->setValue(value);
        }
        requestBrightness(value);
    });
    connect(brightnessSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        if (!m_brightnessSlider.isNull()) {
            QSignalBlocker blocker(m_brightnessSlider.data());
            m_brightnessSlider->setValue(value);
        }
        requestBrightness(value);
    });
    connect(kelvinSlider, &QSlider::valueChanged, widget,
            [this](int value) { setKelvin(value, true); });
    connect(kelvinSpin, &QSpinBox::valueChanged, widget,
            [this](int value) { setKelvin(value, true); });

    connect(eyeCareCheck, &QCheckBox::toggled, widget,
            [this](bool checked) { setEyeCareEnabled(checked, true); });

    connect(dayKelvinSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_dayKelvin = clampKelvin(value);
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("dayKelvin"), m_dayKelvin);
            svc->syncConfig();
        }
        if (isEnabled() && m_eyeCareEnabled) {
            applyColorTemperature();
        }
        refreshPanel();
    });
    connect(nightKelvinSpin, &QSpinBox::valueChanged, widget, [this](int value) {
        m_nightKelvin = clampKelvin(value);
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("nightKelvin"), m_nightKelvin);
            svc->syncConfig();
        }
        if (isEnabled() && m_eyeCareEnabled) {
            applyColorTemperature();
        }
        refreshPanel();
    });
    // 改时段后**立即重算**：用户把夜间起点拖到当前时刻之前，就该马上看到屏幕变暖，
    // 而不是等下一分钟的轮询（这也是自检驱动"现在属于夜间"的那条真实用户路径）
    connect(nightStartEdit, &QTimeEdit::timeChanged, widget, [this](const QTime &time) {
        m_nightStart = time;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("nightStart"), timeText(m_nightStart));
            svc->syncConfig();
        }
        if (isEnabled() && m_eyeCareEnabled) {
            applyColorTemperature();
        }
        refreshPanel();
    });
    connect(nightEndEdit, &QTimeEdit::timeChanged, widget, [this](const QTime &time) {
        m_nightEnd = time;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("nightEnd"), timeText(m_nightEnd));
            svc->syncConfig();
        }
        if (isEnabled() && m_eyeCareEnabled) {
            applyColorTemperature();
        }
        refreshPanel();
    });

    syncKelvinControls();
    updatePreviewColor();
    refreshPanel();
    return widget;
}

void BrightnessPlugin::syncKelvinControls()
{
    if (!m_kelvinSlider.isNull() && m_kelvinSlider->value() != m_kelvin) {
        QSignalBlocker blocker(m_kelvinSlider.data());
        m_kelvinSlider->setValue(m_kelvin);
    }
    if (!m_kelvinSpin.isNull() && m_kelvinSpin->value() != m_kelvin) {
        QSignalBlocker blocker(m_kelvinSpin.data());
        m_kelvinSpin->setValue(m_kelvin);
    }
    updatePreviewColor();
}

void BrightnessPlugin::updatePreviewColor()
{
    if (m_previewLabel.isNull()) {
        return;
    }

    const int kelvin = scheduledKelvin();
    if (kelvin >= kNeutralKelvin) {
        m_previewLabel->setStyleSheet(
            QStringLiteral("background-color:#ffffff;border:1px solid #666;"));
        return;
    }

    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    WinEase::Win32::colorTemperatureGains(kelvin, &red, &green, &blue);
    // 预览色块 = 白色经过这组增益之后的样子（增益里最大通道恒为 1.0）
    const QColor color(static_cast<int>(red * 255.0 + 0.5),
                       static_cast<int>(green * 255.0 + 0.5),
                       static_cast<int>(blue * 255.0 + 0.5));
    m_previewLabel->setStyleSheet(QStringLiteral("background-color:%1;border:1px solid #666;")
                                      .arg(color.name()));
}

QString BrightnessPlugin::channelText() const
{
    if (m_channels.isEmpty()) {
        return QStringLiteral("亮度通路：正在探测…");
    }

    QStringList parts;
    for (const BrightnessChannel &channel : m_channels) {
        if (channel.supported) {
            parts << QStringLiteral("%1 → %2").arg(channel.target, channel.detail);
        } else {
            parts << QStringLiteral("%1 → 不可用（%2）").arg(channel.target, channel.detail);
        }
    }
    return QStringLiteral("亮度通路：%1").arg(parts.join(QStringLiteral("；")));
}

QString BrightnessPlugin::scheduleText() const
{
    if (!m_eyeCareEnabled) {
        return QStringLiteral("护眼模式：关闭 · 手动色温 %1").arg(kelvinText(m_kelvin));
    }

    const QTime now = QTime::currentTime();
    const bool night = isNightTime(now);
    const QTime next = nextSwitchTime(now);
    return QStringLiteral("护眼模式：开启 · 当前%1时段（%2）· 夜间 %3–%4 · 下次切换 %5")
        .arg(night ? QStringLiteral("夜间") : QStringLiteral("日间"))
        .arg(kelvinText(night ? m_nightKelvin : m_dayKelvin))
        .arg(timeText(m_nightStart))
        .arg(timeText(m_nightEnd))
        .arg(timeText(next));
}

QString BrightnessPlugin::statusText() const
{
    return QStringLiteral("亮度 %1% · 色温 %2 · gamma 基准 %3 块屏")
        .arg(m_brightnessPercent, 0, 'f', 0)
        .arg(m_appliedKelvin >= 0 ? kelvinText(m_appliedKelvin) : QStringLiteral("未应用"))
        .arg(m_gammaBaselines.size());
}

void BrightnessPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    const bool running = isEnabled();
    const bool hasBrightnessChannel =
        std::any_of(m_channels.cbegin(), m_channels.cend(),
                    [](const BrightnessChannel &channel) { return channel.supported; });

    if (!m_statusLabel.isNull()) {
        m_statusLabel->setText(statusText());
    }
    if (!m_channelLabel.isNull()) {
        m_channelLabel->setText(channelText());
    }
    if (!m_scheduleLabel.isNull()) {
        m_scheduleLabel->setText(scheduleText());
    }

    if (!m_brightnessSlider.isNull()) {
        m_brightnessSlider->setEnabled(running && hasBrightnessChannel);
        const int value = qRound(m_brightnessPercent);
        if (m_brightnessSlider->value() != value) {
            QSignalBlocker blocker(m_brightnessSlider.data());
            m_brightnessSlider->setValue(value);
        }
    }
    if (!m_brightnessSpin.isNull()) {
        m_brightnessSpin->setEnabled(running && hasBrightnessChannel);
        const int value = qRound(m_brightnessPercent);
        if (m_brightnessSpin->value() != value) {
            QSignalBlocker blocker(m_brightnessSpin.data());
            m_brightnessSpin->setValue(value);
        }
    }

    // 护眼模式开启时色温由时段接管，手动色温控件让位（避免"我拖了没反应"的困惑）
    const bool manualKelvin = running && !m_eyeCareEnabled;
    if (!m_kelvinSlider.isNull()) {
        m_kelvinSlider->setEnabled(manualKelvin);
    }
    if (!m_kelvinSpin.isNull()) {
        m_kelvinSpin->setEnabled(manualKelvin);
    }
    if (!m_eyeCareCheck.isNull()) {
        m_eyeCareCheck->setEnabled(running);
        if (m_eyeCareCheck->isChecked() != m_eyeCareEnabled) {
            QSignalBlocker blocker(m_eyeCareCheck.data());
            m_eyeCareCheck->setChecked(m_eyeCareEnabled);
        }
    }
    if (!m_dayKelvinSpin.isNull()) {
        m_dayKelvinSpin->setEnabled(running && m_eyeCareEnabled);
    }
    if (!m_nightKelvinSpin.isNull()) {
        m_nightKelvinSpin->setEnabled(running && m_eyeCareEnabled);
    }
    if (!m_nightStartEdit.isNull()) {
        m_nightStartEdit->setEnabled(running && m_eyeCareEnabled);
    }
    if (!m_nightEndEdit.isNull()) {
        m_nightEndEdit->setEnabled(running && m_eyeCareEnabled);
    }

    updatePreviewColor();
    syncPollTimer();
}
