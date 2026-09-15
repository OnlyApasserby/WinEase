#include "magnifier_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/ComApartment.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

using WinEase::Win32::Magnifier;
using WinEase::Win32::MagnifierShape;

/// 跟随周期 30ms（≈33fps）。每个周期只有 3 次轻调用，放大合成在系统侧完成，
/// 所以这个频率既不撕裂也不吃 CPU；再快的收益肉眼看不出来。
constexpr int kFollowIntervalMs = 30;

/// 倍率/尺寸的默认值与面板范围（平台层还会各自夹一次，双重保险）
constexpr double kDefaultFactor = 2.5;
constexpr int kDefaultSizePx = 240;
constexpr double kMinFactorUi = 1.2;
constexpr double kMaxFactorUi = 8.0;
constexpr int kMinSizeUi = 120;
constexpr int kMaxSizeUi = 600;

} // namespace

MagnifierPlugin::MagnifierPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString MagnifierPlugin::id() const
{
    return QStringLiteral("display.magnifier");
}

QString MagnifierPlugin::name() const
{
    return QStringLiteral("放大镜增强");
}

QString MagnifierPlugin::description() const
{
    return QStringLiteral("跟随光标的原生放大窗：倍率可调，支持矩形/圆角/圆形遮罩");
}

QIcon MagnifierPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::DisplayAssist);
}

WinEase::FeatureCategory MagnifierPlugin::category() const
{
    return WinEase::FeatureCategory::DisplayAssist;
}

QStringList MagnifierPlugin::tags() const
{
    return { QStringLiteral("放大镜"), QStringLiteral("放大"), QStringLiteral("magnifier"),
             QStringLiteral("fz"), QStringLiteral("细节") };
}

bool MagnifierPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence MagnifierPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+Z"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool MagnifierPlugin::initialize()
{
    // 配置只在 initialize() 读一次，之后改配置走面板控件
    if (WinEase::PluginServices *svc = services()) {
        m_factor = svc->configValue(id(), QStringLiteral("factor"), kDefaultFactor).toDouble();
        m_sizePx = svc->configValue(id(), QStringLiteral("sizePx"), kDefaultSizePx).toInt();
        m_shape = WinEase::Win32::magnifierShapeFromKey(
            svc->configValue(id(), QStringLiteral("shape"), QStringLiteral("rounded"))
                .toString());
    }

    if (m_followTimer == nullptr) {
        m_followTimer = new QTimer(this);
        m_followTimer->setInterval(kFollowIntervalMs);
        connect(m_followTimer, &QTimer::timeout, this, [this] { followTick(); });
    }

    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("放大镜已就绪（默认 ×%1，%2 px，%3）")
                   .arg(m_factor)
                   .arg(m_sizePx)
                   .arg(WinEase::Win32::magnifierShapeKey(m_shape)));
    return true;
}

void MagnifierPlugin::shutdown()
{
    stopMagnifier(QString());
    if (m_followTimer != nullptr) {
        m_followTimer->stop();
    }
}

bool MagnifierPlugin::canEnable(QString *reason) const
{
    if (!WinEase::Win32::isThreadComReady()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("当前线程未初始化 COM，无法使用 Magnification API");
        }
        return false;
    }

    // 真正的探测就是初始化一次：API 不可用（进程非 DPI 感知 / 合成被关）会直接失败
    QString error;
    if (!Magnifier::apiAvailable(&error)) {
        if (reason != nullptr) {
            *reason = error;
        }
        return false;
    }
    return true;
}

bool MagnifierPlugin::onEnable()
{
    // 启用只是"待命"：不立刻弹窗（用户可能正在设置面板里点来点去）。
    // 真正显示由快捷键或面板按钮触发 —— 这也是"功能启用"与"放大中"分开的原因。
    Q_EMIT statusMessage(QStringLiteral("放大镜已待命：按 %1 或面板按钮开始放大")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));

    // ⚠ 时序：onEnable() 执行期间宿主的"已启用"状态还没落定，此刻 isEnabled() 仍是 false，
    //    直接刷面板会把按钮留在"禁用"上（用户会看到一个按不动的按钮）。
    //    所以排到事件循环下一轮再刷。
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void MagnifierPlugin::onDisable()
{
    // 停用必须把窗口收干净：这是"可完整还原"落到停用上的那条约定
    stopMagnifier(QString());
    // 同理（见 onEnable）：面板按钮的可用状态要等宿主把状态改完再刷
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
}

void MagnifierPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return; // <id>::default 在加载时就注册了，停用期间也会被分发到
    }

    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("default") || action == QLatin1String("toggle")) {
        toggleMagnifier();
        return;
    }
    if (action == QLatin1String("zoom_in")) {
        applyFactor(m_factor * 1.25, true);
        return;
    }
    if (action == QLatin1String("zoom_out")) {
        applyFactor(m_factor / 1.25, true);
    }
}

// ---------------------------------------------------------------------------
//  放大镜的启停与跟随
// ---------------------------------------------------------------------------

bool MagnifierPlugin::startMagnifier()
{
    if (m_magnifier.isValid()) {
        return true;
    }

    QString error;
    if (!m_magnifier.create(m_sizePx, m_factor, m_shape, &error)) {
        setLastError(error);
        logMessage(WinEase::PluginLogLevel::Warning, error);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), error);
        }
        refreshPanel();
        return false;
    }

    m_followFailures = 0;
    if (m_followTimer != nullptr) {
        m_followTimer->start();
    }

    Q_EMIT statusMessage(QStringLiteral("放大镜已开启 ×%1").arg(m_factor));
    refreshPanel();
    return true;
}

void MagnifierPlugin::stopMagnifier(const QString &reason)
{
    if (m_followTimer != nullptr) {
        m_followTimer->stop();
    }
    const bool wasRunning = m_magnifier.isValid();
    m_magnifier.destroy();
    m_followFailures = 0;

    if (wasRunning) {
        Q_EMIT statusMessage(reason.isEmpty() ? QStringLiteral("放大镜已关闭") : reason);
    }
    refreshPanel();
}

void MagnifierPlugin::toggleMagnifier()
{
    if (m_magnifier.isValid()) {
        stopMagnifier(QString());
    } else {
        startMagnifier();
    }
}

void MagnifierPlugin::followTick()
{
    QString error;
    if (m_magnifier.followCursor(&error)) {
        m_followFailures = 0;
        return;
    }

    // 跟随失败（窗口被系统销毁 / 源区域被判非法）：不要静默重试到天荒地老，
    // 连续 3 次就如实停下并说清原因
    ++m_followFailures;
    if (m_followFailures < 3) {
        return;
    }

    setLastError(error);
    logMessage(WinEase::PluginLogLevel::Warning,
               QStringLiteral("放大镜跟随失败，已停止：%1").arg(error));
    if (WinEase::PluginServices *svc = services()) {
        svc->notify(name(), QStringLiteral("放大镜跟随失败，已停止：%1").arg(error));
    }
    stopMagnifier(QStringLiteral("放大镜已停止：%1").arg(error));
}

// ---------------------------------------------------------------------------
//  设置
// ---------------------------------------------------------------------------

void MagnifierPlugin::applyFactor(double factor, bool persist)
{
    m_factor = qBound(kMinFactorUi, factor, kMaxFactorUi);

    QString error;
    if (m_magnifier.isValid() && !m_magnifier.setFactor(m_factor, &error)) {
        logMessage(WinEase::PluginLogLevel::Warning, error);
    }
    if (persist) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("factor"), m_factor);
            svc->syncConfig();
        }
    }

    if (!m_factorSpin.isNull() && !qFuzzyCompare(m_factorSpin->value(), m_factor)) {
        // 快捷键改的倍率要同步回控件（否则面板上的数字会和实际不一致）
        QSignalBlocker blocker(m_factorSpin.data());
        m_factorSpin->setValue(m_factor);
    }
    refreshPanel();
}

void MagnifierPlugin::applySize(int sizePx, bool persist)
{
    m_sizePx = qBound(kMinSizeUi, sizePx, kMaxSizeUi);

    QString error;
    if (m_magnifier.isValid() && !m_magnifier.setSizePx(m_sizePx, &error)) {
        logMessage(WinEase::PluginLogLevel::Warning, error);
    }
    if (persist) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("sizePx"), m_sizePx);
            svc->syncConfig();
        }
    }

    if (!m_sizeSpin.isNull() && m_sizeSpin->value() != m_sizePx) {
        QSignalBlocker blocker(m_sizeSpin.data());
        m_sizeSpin->setValue(m_sizePx);
    }
    refreshPanel();
}

void MagnifierPlugin::applyShape(WinEase::Win32::MagnifierShape shape, bool persist)
{
    m_shape = shape;

    QString error;
    if (m_magnifier.isValid() && !m_magnifier.setShape(m_shape, &error)) {
        logMessage(WinEase::PluginLogLevel::Warning, error);
    }
    if (persist) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("shape"),
                                WinEase::Win32::magnifierShapeKey(m_shape));
            svc->syncConfig();
        }
    }

    if (!m_shapeCombo.isNull()) {
        const int index = m_shapeCombo->findData(WinEase::Win32::magnifierShapeKey(m_shape));
        if (index >= 0 && index != m_shapeCombo->currentIndex()) {
            QSignalBlocker blocker(m_shapeCombo.data());
            m_shapeCombo->setCurrentIndex(index);
        }
    }
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *MagnifierPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("magnifierPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("magStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *toggleButton = new QPushButton(widget);
    toggleButton->setObjectName(QStringLiteral("magToggleButton"));
    layout->addWidget(toggleButton, 0, Qt::AlignLeft);

    auto *factorRow = new QHBoxLayout();
    factorRow->addWidget(new QLabel(QStringLiteral("倍率："), widget));
    auto *factorSpin = new QDoubleSpinBox(widget);
    factorSpin->setObjectName(QStringLiteral("magFactorSpin"));
    factorSpin->setRange(kMinFactorUi, kMaxFactorUi);
    factorSpin->setSingleStep(0.5);
    factorSpin->setDecimals(1);
    factorSpin->setSuffix(QStringLiteral(" ×"));
    factorSpin->setValue(m_factor);
    factorRow->addWidget(factorSpin);
    factorRow->addStretch(1);
    layout->addLayout(factorRow);

    auto *sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel(QStringLiteral("窗口边长："), widget));
    auto *sizeSpin = new QSpinBox(widget);
    sizeSpin->setObjectName(QStringLiteral("magSizeSpin"));
    sizeSpin->setRange(kMinSizeUi, kMaxSizeUi);
    sizeSpin->setSingleStep(20);
    sizeSpin->setSuffix(QStringLiteral(" px"));
    sizeSpin->setValue(m_sizePx);
    sizeRow->addWidget(sizeSpin);
    sizeRow->addStretch(1);
    layout->addLayout(sizeRow);

    auto *shapeRow = new QHBoxLayout();
    shapeRow->addWidget(new QLabel(QStringLiteral("遮罩形状："), widget));
    auto *shapeCombo = new QComboBox(widget);
    shapeCombo->setObjectName(QStringLiteral("magShapeCombo"));
    shapeCombo->addItem(QStringLiteral("矩形"), WinEase::Win32::magnifierShapeKey(MagnifierShape::Rectangle));
    shapeCombo->addItem(QStringLiteral("圆角"), WinEase::Win32::magnifierShapeKey(MagnifierShape::Rounded));
    shapeCombo->addItem(QStringLiteral("圆形"), WinEase::Win32::magnifierShapeKey(MagnifierShape::Circle));
    shapeCombo->setCurrentIndex(
        std::max(0, shapeCombo->findData(WinEase::Win32::magnifierShapeKey(m_shape))));
    shapeRow->addWidget(shapeCombo);
    shapeRow->addStretch(1);
    layout->addLayout(shapeRow);

    auto *hint = new QLabel(
        QStringLiteral("快捷键：%1 开关放大镜（可在主界面「快捷键」里修改）。\n"
                       "放大画面由系统合成器直接产生（Magnification API），"
                       "本程序每个刷新周期只做 3 次轻调用，所以跟随几乎不占 CPU，"
                       "也不存在自抓屏方案的撕裂。\n"
                       "放大窗放在光标右下方、靠边自动翻转，并且点击穿透"
                       "（鼠标能继续操作它下面的东西）。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText)),
        widget);
    hint->setObjectName(QStringLiteral("magHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_statusLabel = statusLabel;
    m_toggleButton = toggleButton;
    m_factorSpin = factorSpin;
    m_sizeSpin = sizeSpin;
    m_shapeCombo = shapeCombo;

    connect(toggleButton, &QPushButton::clicked, widget, [this] { toggleMagnifier(); });
    connect(factorSpin, &QDoubleSpinBox::valueChanged, widget,
            [this](double value) { applyFactor(value, true); });
    connect(sizeSpin, &QSpinBox::valueChanged, widget,
            [this](int value) { applySize(value, true); });
    connect(shapeCombo, &QComboBox::currentIndexChanged, widget, [this, shapeCombo](int) {
        applyShape(WinEase::Win32::magnifierShapeFromKey(shapeCombo->currentData().toString()), true);
    });

    refreshPanel();
    return widget;
}

QString MagnifierPlugin::statusText() const
{
    // 源区尺寸 = 窗口边长 / 倍率：这正是"倍率"在 Magnification API 里的含义，
    // 写出来用户才知道自己看到的到底是多大一块屏幕
    const int sourceSize = qMax(1, qRound(static_cast<double>(m_sizePx) / m_factor));
    return QStringLiteral("状态：%1 · 倍率 ×%2 · 窗口 %3 px · 放大源区 %4×%4 px · 遮罩：%5")
        .arg(m_magnifier.isValid() ? QStringLiteral("跟随鼠标中") : QStringLiteral("未开启"))
        .arg(m_factor)
        .arg(m_sizePx)
        .arg(sourceSize)
        .arg(m_shape == MagnifierShape::Rectangle
                 ? QStringLiteral("矩形")
                 : (m_shape == MagnifierShape::Circle ? QStringLiteral("圆形")
                                                      : QStringLiteral("圆角")));
}

void MagnifierPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }
    if (!m_statusLabel.isNull()) {
        m_statusLabel->setText(statusText());
    }
    if (!m_toggleButton.isNull()) {
        m_toggleButton->setText(m_magnifier.isValid() ? QStringLiteral("停止放大")
                                                      : QStringLiteral("开始放大"));
        m_toggleButton->setEnabled(isEnabled());
    }
}
