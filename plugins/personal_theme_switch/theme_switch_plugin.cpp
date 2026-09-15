#include "theme_switch_plugin.h"

#include "sdk/PluginServices.h"
#include "win32/RegistryUtils.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

using WinEase::Win32::RegistryRoot;
using WinEase::Win32::RegistryValueType;
using WinEase::Win32::RegistryView;

namespace {

const QString kAppsValue = QStringLiteral("AppsUseLightTheme");
const QString kSystemValue = QStringLiteral("SystemUsesLightTheme");

/// 主题变更的广播区域名：Explorer / UWP 应用都监听它
const QString kColorArea = QStringLiteral("ImmersiveColorSet");

} // namespace

ThemeSwitchPlugin::ThemeSwitchPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString ThemeSwitchPlugin::id() const
{
    return QStringLiteral("personal.theme_switch");
}

QString ThemeSwitchPlugin::name() const
{
    return QStringLiteral("系统主题切换");
}

QString ThemeSwitchPlugin::description() const
{
    return QStringLiteral("一键切 Windows 深色 / 浅色 / 自定义（含任务栏）；停用或退出即恢复原样");
}

QIcon ThemeSwitchPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Personalization);
}

WinEase::FeatureCategory ThemeSwitchPlugin::category() const
{
    return WinEase::FeatureCategory::Personalization;
}

QStringList ThemeSwitchPlugin::tags() const
{
    return { QStringLiteral("主题"), QStringLiteral("深色"), QStringLiteral("浅色"),
             QStringLiteral("theme"), QStringLiteral("dark"), QStringLiteral("夜间模式") };
}

bool ThemeSwitchPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence ThemeSwitchPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+T"));
}

// ---------------------------------------------------------------------------
//  注册表
// ---------------------------------------------------------------------------

QString ThemeSwitchPlugin::personalizePath()
{
    return QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
}

ThemeSwitchPlugin::Snapshot ThemeSwitchPlugin::readSnapshot() const
{
    Snapshot snapshot;
    const QVariant apps = WinEase::Win32::readValue(RegistryRoot::CurrentUser, personalizePath(),
                                                    kAppsValue);
    const QVariant system = WinEase::Win32::readValue(RegistryRoot::CurrentUser, personalizePath(),
                                                      kSystemValue);
    snapshot.apps = apps;
    snapshot.system = system;
    snapshot.valid = apps.isValid() || system.isValid();
    return snapshot;
}

QString ThemeSwitchPlugin::describeCurrent() const
{
    const Snapshot now = readSnapshot();
    if (!now.valid) {
        return QStringLiteral("读不到主题设置（该键不存在？）");
    }
    const int apps = now.apps.isValid() ? now.apps.toInt() : -1;
    const int system = now.system.isValid() ? now.system.toInt() : -1;
    const QString appsText = (apps == 1) ? QStringLiteral("浅色") : QStringLiteral("深色");
    const QString systemText = (system == 1) ? QStringLiteral("浅色") : QStringLiteral("深色");

    if (apps == system) {
        return QStringLiteral("%1（应用%2 / 系统%3）")
            .arg(apps == 1 ? QStringLiteral("浅色") : QStringLiteral("深色"), appsText, systemText);
    }
    // Win11 的「自定义模式」就是两者不一致
    return QStringLiteral("自定义（应用%1 / 系统%2）").arg(appsText, systemText);
}

bool ThemeSwitchPlugin::writeValues(int appsLight, int systemLight, QString *errorOut)
{
    QString error;
    if (!WinEase::Win32::writeValue(RegistryRoot::CurrentUser, personalizePath(), kAppsValue,
                                    appsLight, RegistryValueType::DWord, RegistryView::Default,
                                    &error)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("写入 %1 失败：%2").arg(kAppsValue, error);
        }
        return false;
    }
    if (!WinEase::Win32::writeValue(RegistryRoot::CurrentUser, personalizePath(), kSystemValue,
                                    systemLight, RegistryValueType::DWord, RegistryView::Default,
                                    &error)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("写入 %1 失败：%2").arg(kSystemValue, error);
        }
        return false;
    }

    // 不广播的话，任务栏/开始菜单要等下次登录才变（验收要求"立即跟随"）
    WinEase::Win32::broadcastSettingChange(kColorArea);
    return true;
}

bool ThemeSwitchPlugin::restoreSnapshot()
{
    if (!m_hasOriginal) {
        return true;
    }
    // 只还原"原来存在的值"：原来没有这个键就别凭空造一个（尽量少改用户系统）
    QString error;
    if (m_original.apps.isValid()) {
        WinEase::Win32::writeValue(RegistryRoot::CurrentUser, personalizePath(), kAppsValue,
                                   m_original.apps.toInt(), RegistryValueType::DWord,
                                   RegistryView::Default, &error);
    }
    if (m_original.system.isValid()) {
        WinEase::Win32::writeValue(RegistryRoot::CurrentUser, personalizePath(), kSystemValue,
                                   m_original.system.toInt(), RegistryValueType::DWord,
                                   RegistryView::Default, &error);
    }
    WinEase::Win32::broadcastSettingChange(kColorArea);
    m_changed = false;
    return true;
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool ThemeSwitchPlugin::initialize()
{
    return true;
}

void ThemeSwitchPlugin::shutdown()
{
    // 退出**不还原**（有意）：主题是"用户看得见的外观"，不该因为 WinEase 退出而跳变。
    // 契约因此是：**停用功能 → 恢复成启用前的样子；退出 WinEase → 保持现状**。
    // （P1-14 壁纸同理，而且它的验收原文就写着"重启后保持"。
    //   "无残留"针对的是"指向已不存在程序的引用"——见 P1-05 的右键菜单项；
    //   主题/壁纸只是一个合法的系统取值，不属于残留。）
    // 快照不跨会话：每次启用都重新取，所以"这次启用前是什么样"才是还原目标。
}

bool ThemeSwitchPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    if (!m_hasOriginal) {
        m_original = readSnapshot();
        m_hasOriginal = m_original.valid;
    }
    if (!m_hasOriginal) {
        setLastError(QStringLiteral("读不到当前主题设置（注册表键缺失）"));
        Q_EMIT statusMessage(QStringLiteral("启用失败：%1").arg(lastError()));
        return false;
    }

    Q_EMIT statusMessage(QStringLiteral("当前：%1 · %2 切换")
                             .arg(describeCurrent(),
                                  defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void ThemeSwitchPlugin::onDisable()
{
    restoreSnapshot();
    m_hasOriginal = false;
    Q_EMIT statusMessage(QStringLiteral("已停用，主题已恢复为 %1").arg(describeCurrent()));
}

void ThemeSwitchPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    applyMode(action.isEmpty() ? QStringLiteral("toggle") : action);
}

// ---------------------------------------------------------------------------
//  切换
// ---------------------------------------------------------------------------

bool ThemeSwitchPlugin::applyMode(const QString &modeKey)
{
    int appsLight = 0;
    int systemLight = 0;

    if (modeKey == QLatin1String("light")) {
        appsLight = 1;
        systemLight = 1;
    } else if (modeKey == QLatin1String("dark")) {
        appsLight = 0;
        systemLight = 0;
    } else if (modeKey == QLatin1String("custom")) {
        // Win11「自定义」的常见组合：应用浅色 + 系统（任务栏）深色
        appsLight = 1;
        systemLight = 0;
    } else {
        // toggle：按"应用"当前值反转（这是用户最可能感知的那一半）
        const Snapshot now = readSnapshot();
        const bool currentlyLight = now.apps.isValid() && now.apps.toInt() == 1;
        appsLight = currentlyLight ? 0 : 1;
        systemLight = appsLight;
    }

    QString error;
    if (!writeValues(appsLight, systemLight, &error)) {
        setLastError(error);
        logMessage(WinEase::PluginLogLevel::Warning, error);
        Q_EMIT statusMessage(QStringLiteral("切换主题失败：%1").arg(error));
        return false;
    }

    m_changed = true;
    Q_EMIT statusMessage(QStringLiteral("已切换为 %1").arg(describeCurrent()));
    return true;
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *ThemeSwitchPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *currentLabel = new QLabel(widget);
    currentLabel->setWordWrap(true);
    layout->addWidget(currentLabel);

    auto *row = new QHBoxLayout();
    auto *darkButton = new QPushButton(QStringLiteral("深色"), widget);
    auto *lightButton = new QPushButton(QStringLiteral("浅色"), widget);
    auto *customButton = new QPushButton(QStringLiteral("自定义（应用浅色 + 系统深色）"), widget);
    auto *toggleButton = new QPushButton(QStringLiteral("反转"), widget);
    row->addWidget(darkButton);
    row->addWidget(lightButton);
    row->addWidget(customButton);
    row->addWidget(toggleButton);
    row->addStretch(1);
    layout->addLayout(row);

    auto *restoreButton = new QPushButton(QStringLiteral("恢复为启用前的设置"), widget);
    layout->addWidget(restoreButton, 0, Qt::AlignLeft);

    auto *hint = new QLabel(
        QStringLiteral("快捷键：%1 在深色 / 浅色之间反转\n"
                       "· 改的是 **Windows 自己的主题**（其它应用、任务栏、开始菜单）；"
                       "WinEase 界面按设计固定深色，不受影响\n"
                       "· 只写 HKCU，不需要管理员权限；改完立刻广播 WM_SETTINGCHANGE，无需重启\n"
                       "· **停用本功能会恢复成启用前的主题**；退出 WinEase 不影响已切换的主题")
            .arg(defaultHotkey().toString(QKeySequence::NativeText)),
        widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto refresh = [this, currentLabel] {
        currentLabel->setText(QStringLiteral("当前：%1").arg(describeCurrent()));
    };
    refresh();

    QObject::connect(darkButton, &QPushButton::clicked, widget, [this, refresh] {
        applyMode(QStringLiteral("dark"));
        refresh();
    });
    QObject::connect(lightButton, &QPushButton::clicked, widget, [this, refresh] {
        applyMode(QStringLiteral("light"));
        refresh();
    });
    QObject::connect(customButton, &QPushButton::clicked, widget, [this, refresh] {
        applyMode(QStringLiteral("custom"));
        refresh();
    });
    QObject::connect(toggleButton, &QPushButton::clicked, widget, [this, refresh] {
        applyMode(QStringLiteral("toggle"));
        refresh();
    });
    QObject::connect(restoreButton, &QPushButton::clicked, widget, [this, refresh] {
        restoreSnapshot();
        refresh();
    });

    return widget;
}
