#include "sdk/IFeaturePlugin.h"

#include "sdk/PluginServices.h"

namespace WinEase {

IFeaturePlugin::IFeaturePlugin(QObject *parent)
    : QObject(parent)
{
}

IFeaturePlugin::~IFeaturePlugin() = default;

// ---------------------------------------------------------------------------
//  可选元信息默认实现
// ---------------------------------------------------------------------------

QString IFeaturePlugin::version() const
{
    return QStringLiteral("1.0.0");
}

QString IFeaturePlugin::author() const
{
    return QString();
}

QString IFeaturePlugin::detailedDescription() const
{
    // 默认"没有额外说明"（帮助页只显示一行 description）
    return {};
}

QStringList IFeaturePlugin::tags() const
{
    return {};
}

bool IFeaturePlugin::requiresAdmin() const
{
    return false;
}

bool IFeaturePlugin::supportsHotkey() const
{
    return false;
}

QKeySequence IFeaturePlugin::defaultHotkey() const
{
    return {};
}

bool IFeaturePlugin::hasSettings() const
{
    return true;
}

QWidget *IFeaturePlugin::createSettingsWidget(QWidget *parent)
{
    Q_UNUSED(parent)
    return nullptr;
}

// ---------------------------------------------------------------------------
//  宿主服务
// ---------------------------------------------------------------------------

void IFeaturePlugin::setServices(PluginServices *services)
{
    m_services = services;
}

PluginServices *IFeaturePlugin::services() const
{
    return m_services;
}

// ---------------------------------------------------------------------------
//  生命周期（由 PluginManager 调用）
// ---------------------------------------------------------------------------

bool IFeaturePlugin::initializePlugin(PluginServices *services)
{
    setServices(services);

    if (m_initialized) {
        return true;
    }

    clearLastError();

    if (!initialize()) {
        if (m_lastError.isEmpty()) {
            setLastError(tr("插件初始化失败：%1").arg(id()));
        }
        setState(PluginState::Failed);
        Q_EMIT errorOccurred(m_lastError);
        return false;
    }

    m_initialized = true;
    setState(PluginState::Loaded);
    return true;
}

void IFeaturePlugin::shutdownPlugin()
{
    if (!m_initialized) {
        return;
    }

    // 先停用，再释放资源
    setEnabled(false);

    shutdown();

    m_initialized = false;
    setState(PluginState::Unloaded);
}

bool IFeaturePlugin::isInitialized() const
{
    return m_initialized;
}

// ---------------------------------------------------------------------------
//  启用 / 停用
// ---------------------------------------------------------------------------

bool IFeaturePlugin::isEnabled() const
{
    return m_enabled;
}

bool IFeaturePlugin::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return true;
    }

    if (enabled) {
        if (!m_initialized) {
            setLastError(tr("插件尚未初始化，无法启用：%1").arg(id()));
            Q_EMIT errorOccurred(m_lastError);
            return false;
        }

        QString reason;
        if (!canEnable(&reason)) {
            setLastError(reason.isEmpty() ? tr("当前环境不满足启用条件") : reason);
            Q_EMIT errorOccurred(m_lastError);
            return false;
        }

        clearLastError();
        if (!onEnable()) {
            if (m_lastError.isEmpty()) {
                setLastError(tr("启用失败：%1").arg(id()));
            }
            setState(PluginState::Loaded);
            Q_EMIT errorOccurred(m_lastError);
            return false;
        }

        m_enabled = true;
        setState(PluginState::Running);
    } else {
        onDisable();
        m_enabled = false;
        setState(PluginState::Loaded);
    }

    Q_EMIT enabledChanged(m_enabled);
    return true;
}

void IFeaturePlugin::dispatchHotkey(const QString &hotkeyId)
{
    onHotkey(hotkeyId);
}

// ---------------------------------------------------------------------------
//  状态
// ---------------------------------------------------------------------------

PluginState IFeaturePlugin::state() const
{
    return m_state;
}

void IFeaturePlugin::setState(PluginState state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(m_state);
}

QString IFeaturePlugin::lastError() const
{
    return m_lastError;
}

// ---------------------------------------------------------------------------
//  可覆盖的钩子
// ---------------------------------------------------------------------------

bool IFeaturePlugin::onEnable()
{
    return true;
}

void IFeaturePlugin::onDisable()
{
}

bool IFeaturePlugin::canEnable(QString *reason) const
{
    Q_UNUSED(reason)
    return true;
}

void IFeaturePlugin::onHotkey(const QString &hotkeyId)
{
    Q_EMIT hotkeyTriggered(hotkeyId);
}

// ---------------------------------------------------------------------------
//  辅助
// ---------------------------------------------------------------------------

void IFeaturePlugin::setLastError(const QString &error)
{
    m_lastError = error;
}

void IFeaturePlugin::clearLastError()
{
    m_lastError.clear();
}

void IFeaturePlugin::logMessage(PluginLogLevel level, const QString &message)
{
    if (m_services) {
        m_services->log(id(), level, message);
    }
}

} // namespace WinEase
