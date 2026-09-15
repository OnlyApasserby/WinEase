#include "core/AppContext.h"

#include "core/AdminHelper.h"
#include "core/ElevationClient.h"
#include "core/GlobalHotkeyManager.h"
#include "core/HookServiceImpl.h"
#include "core/Logging.h"
#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "sdk/OverlayHost.h" // 悬浮层宿主已移入 SDK（插件 DLL 也要调用它）

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace WinEase {

namespace {
/// 插件目录：优先使用主程序同级的 plugins/，便于绿色版部署
QString resolvePluginDirectory()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString local = QDir(appDir).filePath(QStringLiteral("plugins"));
    if (QFileInfo::exists(local)) {
        return local;
    }
    return local; // 不存在也返回，由 PluginManager 记录日志
}
} // namespace

AppContext::AppContext(QObject *parent)
    : QObject(parent)
    , m_pluginManager(std::make_unique<PluginManager>(this))
    , m_hotkeyManager(std::make_unique<GlobalHotkeyManager>(this))
    , m_hookService(std::make_unique<HookServiceImpl>())
    , m_overlayHost(std::make_unique<OverlayHost>())
    , m_elevation(std::make_unique<ElevationClient>(this))
{
    // 钩子服务的信号在主线程消费（服务内部会把跨线程通知排队过来）
    connect(m_hookService.get(), &HookServiceImpl::serviceStopped, this,
            [](const QString &reason) { qCWarning(lcApp) << "全局钩子服务停止:" << reason; });
    connect(m_hookService.get(), &HookServiceImpl::subscriberDegraded, this,
            [](const QString &reason) { qCWarning(lcApp) << "订阅者被降级:" << reason; });

    // 悬浮层是置顶透明窗口：插件一旦被停用就必须收回，否则会一直挡在用户桌面上。
    // 插件自己的 onDisable() 通常已经关了，这里是**兜底**（插件崩溃/漏写时也安全）
    connect(m_pluginManager.get(), &PluginManager::pluginStateChanged, this,
            [this](IFeaturePlugin *plugin, bool enabled) {
                if (enabled || plugin == nullptr || !m_overlayHost) {
                    return;
                }
                // ⚠ 用缓存 id 而非 plugin->id()：插件可能已崩溃被隔离，不允许再调用它（P0-5）
                const QString ownerId = m_pluginManager->idOf(plugin);
                const int closed = m_overlayHost->closeOverlaysOfOwner(ownerId);
                if (closed > 0) {
                    qCInfo(lcApp) << "已回收插件" << ownerId << "残留的悬浮层:" << closed;
                }
            });
}

AppContext::~AppContext()
{
    shutdown();
}

// ---------------------------------------------------------------------------
//  初始化 / 收尾
// ---------------------------------------------------------------------------

bool AppContext::initialize()
{
    if (m_initialized) {
        return true;
    }

    SettingsManager &settings = SettingsManager::instance();

    // 1. 配置
    if (!settings.initialize()) {
        qCWarning(lcApp) << "配置初始化失败，将使用默认值继续运行";
    }

    // 2. 日志（必须在配置之后，因为日志目录来自配置）
    Logging::install(settings.logDirectory());
    Logging::pruneOldLogs();

    // 3. 日志级别（模板接口直接返回 int）
    const int levelValue = settings.value(QStringLiteral("General/logLevel"), 0);
    Logging::setMinimumLevel(Logging::toQtMsgType(levelValue));

    // 4. 宿主服务注入
    m_pluginManager->setServices(this);

    // 4.5 全局输入钩子服务（必须在插件被启用之前启动，插件的 onEnable 里会订阅）
    //     失败不阻断启动：没有输入钩子时依赖它的功能会自动降级，其余功能不受影响
    if (m_hookService->start()) {
        const HookService::Stats initial = m_hookService->stats();
        qCInfo(lcApp) << "全局输入钩子已安装"
                      << "(回调预算" << m_hookService->callbackBudgetMs() << "ms)"
                      << "(初始统计: 键" << initial.keyEvents << ")";
    } else {
        qCWarning(lcApp) << "全局输入钩子安装失败:" << m_hookService->lastError()
                         << "—— 依赖输入钩子的功能将不可用";
    }

    // 5. 主程序自身的全局快捷键（显示主窗口）
    m_hotkeyManager->registerHotkey(appHotkeyId(QStringLiteral("showMainWindow")),
                                    QKeySequence(QStringLiteral("Ctrl+Alt+W")),
                                    tr("WinEase：显示/隐藏主界面"),
                                    QStringLiteral("app"));
    m_hotkeyManager->registerHotkey(appHotkeyId(QStringLiteral("toggleAll")),
                                    QKeySequence(),
                                    tr("WinEase：一键启用/停用全部功能"),
                                    QStringLiteral("app"));

    connect(m_hotkeyManager.get(), &GlobalHotkeyManager::hotkeyTriggered, this,
            [this](const QString &hotkeyId) {
                if (hotkeyId == appHotkeyId(QStringLiteral("showMainWindow"))) {
                    Q_EMIT activationRequested();
                    return;
                }
                if (hotkeyId == appHotkeyId(QStringLiteral("toggleAll"))) {
                    // 只要还有启用中的功能就全部停用，否则全部启用。
                    // ⚠ 用管理器的安全查询：已崩溃隔离的功能视为停用且不会被调用
                    bool anyEnabled = false;
                    for (IFeaturePlugin *plugin : m_pluginManager->plugins()) {
                        if (m_pluginManager->isPluginEnabled(m_pluginManager->idOf(plugin))) {
                            anyEnabled = true;
                            break;
                        }
                    }
                    const int changed = m_pluginManager->setAllEnabled(!anyEnabled);
                    qCInfo(lcApp) << QStringLiteral("快捷键切换全部功能：%1 个被%2")
                                         .arg(changed)
                                         .arg(anyEnabled ? QStringLiteral("停用") : QStringLiteral("启用"));
                    return;
                }

                // 插件快捷键：交给管理器分发（内部带 SEH/C++ 异常边界，插件崩在回调里即被隔离）
                if (!m_pluginManager->dispatchHotkey(hotkeyId)) {
                    qCWarning(lcApp) << "快捷键没有对应功能，或目标功能已被隔离:" << hotkeyId;
                }
            });

    qCInfo(lcApp) << "应用上下文初始化完成, 版本" << QCoreApplication::applicationVersion()
                  << "权限:" << Admin::elevationDescription()
                  << "提权助手:" << m_elevation->statusText();
    m_initialized = true;
    return true;
}

int AppContext::loadPlugins()
{
    const QString directory = resolvePluginDirectory();
    qCInfo(lcApp) << "插件目录:" << directory;

    const int loaded = m_pluginManager->loadPlugins(directory);

    const QStringList errors = m_pluginManager->loadErrors();
    for (const QString &error : errors) {
        qCWarning(lcPlugin) << "插件加载问题:" << error;
    }

    // 主程序快捷键配置载入
    m_hotkeyManager->loadFromSettings();

    return loaded;
}

void AppContext::restorePluginStates()
{
    m_pluginManager->restoreSavedStates();
}

void AppContext::shutdown()
{
    if (!m_initialized) {
        return;
    }

    qCInfo(lcApp) << "WinEase 正在退出…";

    if (m_pluginManager) {
        m_pluginManager->saveStates();
        m_pluginManager->unloadAll();
    }

    // 悬浮层兜底回收：插件自认为停用后仍可能残留（崩溃/漏写 onDisable）
    if (m_overlayHost) {
        const int closed = m_overlayHost->closeAll();
        if (closed > 0) {
            qCInfo(lcApp) << "退出前回收悬浮层:" << closed;
        }
    }
    if (m_hotkeyManager) {
        m_hotkeyManager->saveToSettings();
        m_hotkeyManager->unregisterAll();
    }

    // 钩子服务必须在插件卸载**之后**停止：插件销毁其 HookListener 时会自动退订，
    // 反过来的顺序会让服务持有已经失效的订阅关系
    if (m_hookService) {
        const HookService::Stats finalStats = m_hookService->stats();
        qCInfo(lcApp) << "输入钩子统计: 键盘" << finalStats.keyEvents
                      << "鼠标按键" << finalStats.mouseButtonEvents
                      << "鼠标移动" << finalStats.mouseMoveEvents
                      << "滚轮" << finalStats.mouseWheelEvents
                      << "拦截" << finalStats.consumedEvents
                      << "丢弃" << finalStats.droppedEvents
                      << "最大回调" << finalStats.maxCallbackMs << "ms";
        m_hookService->stop();
    }

    SettingsManager::instance().sync();
    Logging::uninstall();

    m_initialized = false;
}

QString AppContext::appHotkeyId(const QString &action)
{
    return QStringLiteral("app::") + action;
}

// ---------------------------------------------------------------------------
//  PluginServices 实现
// ---------------------------------------------------------------------------

QVariant AppContext::configValue(const QString &pluginId,
                                 const QString &key,
                                 const QVariant &defaultValue) const
{
    return SettingsManager::instance().rawPluginValue(pluginId, key, defaultValue);
}

void AppContext::setConfigValue(const QString &pluginId, const QString &key, const QVariant &value)
{
    SettingsManager::instance().setRawPluginValue(pluginId, key, value);
}

void AppContext::syncConfig()
{
    SettingsManager::instance().sync();
}

void AppContext::log(const QString &pluginId, PluginLogLevel level, const QString &message)
{
    const QString text = QStringLiteral("[%1] %2").arg(pluginId, message);
    switch (level) {
    case PluginLogLevel::Debug:
        qCDebug(lcPlugin).noquote() << text;
        break;
    case PluginLogLevel::Info:
        qCInfo(lcPlugin).noquote() << text;
        break;
    case PluginLogLevel::Warning:
        qCWarning(lcPlugin).noquote() << text;
        break;
    case PluginLogLevel::Critical:
        qCCritical(lcPlugin).noquote() << text;
        break;
    }
}

void AppContext::notify(const QString &title, const QString &body)
{
    Q_EMIT notificationRequested(title, body);
}

void AppContext::setTrayBadge(const QString &pluginId,
                              const QString &text,
                              const QString &colorHex,
                              const QString &tooltip)
{
    if (pluginId.isEmpty()) {
        return;
    }
    Q_EMIT trayBadgeRequested(pluginId, text, colorHex, tooltip);
}

bool AppContext::registerHotkey(const QString &pluginId,
                                const QString &action,
                                const QKeySequence &sequence,
                                const QString &description)
{
    if (!m_hotkeyManager) {
        return false;
    }
    const QString hotkeyId = pluginId + QStringLiteral("::") + action;
    return m_hotkeyManager->registerHotkey(hotkeyId, sequence, description, pluginId);
}

void AppContext::unregisterHotkey(const QString &pluginId, const QString &action)
{
    if (!m_hotkeyManager) {
        return;
    }
    m_hotkeyManager->unregisterHotkey(pluginId + QStringLiteral("::") + action);
}

HookService *AppContext::hookService() const
{
    return m_hookService.get();
}

OverlayHost *AppContext::overlayHost() const
{
    return m_overlayHost.get();
}

ElevationService *AppContext::elevationService() const
{
    return m_elevation.get();
}

QString AppContext::appVersion() const
{
    return QCoreApplication::applicationVersion();
}bool AppContext::isElevated() const
{
    return Admin::isProcessElevated();
}

QString AppContext::configFilePath() const
{
    return SettingsManager::instance().configFilePath();
}

QString AppContext::logDirectory() const
{
    return SettingsManager::instance().logDirectory();
}

QWidget *AppContext::mainWindow() const
{
    return m_mainWindow.data();
}

} // namespace WinEase
