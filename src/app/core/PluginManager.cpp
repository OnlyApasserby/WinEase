#include "core/PluginManager.h"

#include "core/AdminHelper.h"
#include "core/GlobalHotkeyManager.h"
#include "core/Logging.h"
#include "core/SettingsManager.h"
#include "sdk/PluginServices.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QPluginLoader>

namespace WinEase {

namespace {

/// 崩溃插件（动态库）的 QPluginLoader 停放区。
///
/// 为什么"故意泄漏"：隔离插件不能再被触碰 —— `~QPluginLoader` 会 unload 动态库、
/// 进而调用插件的析构函数，而对象很可能已处于损坏状态，等于再崩一次。
/// 代价是该 DLL 常驻到进程退出（几 MB），换来的是"插件崩溃后主程序一定能正常退出"。
std::vector<std::unique_ptr<QPluginLoader>> &quarantinedLoaders()
{
    static auto *store = new std::vector<std::unique_ptr<QPluginLoader>>(); // 有意泄漏
    return *store;
}

/// 崩溃插件（静态注册）的停放区：同上，不做 delete
std::vector<IFeaturePlugin *> &quarantinedPlugins()
{
    static auto *store = new std::vector<IFeaturePlugin *>(); // 有意泄漏
    return *store;
}

/// 取插件的 Qt 元对象类名（用于崩溃时标识"是谁崩了"）。
/// ⚠ 崩溃后连 metaObject() 都可能不可用，因此这里再包一层边界，失败则给出占位文本。
QString safeClassName(IFeaturePlugin *plugin)
{
    QString label = QStringLiteral("<插件对象已损坏，无法取类名>");
    Guard::CrashReport report;
    Guard::invoke([&] { label = QString::fromLatin1(plugin->metaObject()->className()); }, &report);
    if (report.crashed()) {
        label = QStringLiteral("<插件对象已损坏，无法取类名>");
    }
    return label;
}

} // namespace

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
{
}

PluginManager::~PluginManager()
{
    unloadAll();
}

void PluginManager::setServices(PluginServices *services)
{
    m_services = services;
}

QString PluginManager::hotkeyIdPrefix(const QString &pluginId)
{
    return pluginId + QStringLiteral("::");
}

// ---------------------------------------------------------------------------
//  崩溃隔离（P0-5）
// ---------------------------------------------------------------------------

bool PluginManager::isQuarantined(const IFeaturePlugin *plugin) const
{
    return m_failures.contains(plugin);
}

QString PluginManager::cachedId(const IFeaturePlugin *plugin) const
{
    const auto it = m_meta.constFind(plugin);
    return it == m_meta.constEnd() ? QString() : it->id;
}

const PluginManager::PluginMeta *PluginManager::metaOf(const IFeaturePlugin *plugin) const
{
    const auto it = m_meta.constFind(plugin);
    return it == m_meta.constEnd() ? nullptr : &it.value();
}

bool PluginManager::isPluginFailed(const QString &pluginId) const
{
    return m_failedIds.contains(pluginId);
}

QString PluginManager::idOf(const IFeaturePlugin *plugin) const
{
    return cachedId(plugin);
}

QString PluginManager::nameOf(const IFeaturePlugin *plugin) const
{
    const PluginMeta *meta = metaOf(plugin);
    return meta == nullptr ? QString() : meta->name;
}

FeatureCategory PluginManager::categoryOf(const IFeaturePlugin *plugin) const
{
    const PluginMeta *meta = metaOf(plugin);
    return meta == nullptr ? FeatureCategory::Unknown : meta->category;
}

QString PluginManager::descriptionOf(const IFeaturePlugin *plugin) const
{
    const PluginMeta *meta = metaOf(plugin);
    return meta == nullptr ? QString() : meta->description;
}

QString PluginManager::versionOf(const IFeaturePlugin *plugin) const
{
    const PluginMeta *meta = metaOf(plugin);
    return meta == nullptr ? QString() : meta->version;
}

QString PluginManager::authorOf(const IFeaturePlugin *plugin) const
{
    const PluginMeta *meta = metaOf(plugin);
    return meta == nullptr ? QString() : meta->author;
}

QString PluginManager::detailedDescriptionOf(const IFeaturePlugin *plugin) const
{
    const PluginMeta *meta = metaOf(plugin);
    return meta == nullptr ? QString() : meta->detailedDescription;
}

QString PluginManager::pluginFailureReason(const QString &pluginId) const
{
    if (pluginId.isEmpty()) {
        return QString();
    }
    // 用缓存 id 反查：加载期就崩溃的插件不在 m_pluginsById 里，
    // 但其失败原因同样应该可查（界面/日志都会用到）
    for (auto it = m_failures.constBegin(); it != m_failures.constEnd(); ++it) {
        if (cachedId(it.key()) == pluginId) {
            return it.value();
        }
    }
    return QString();
}

QStringList PluginManager::failedPluginIds() const
{
    return m_failedIds;
}

QString PluginManager::quarantine(IFeaturePlugin *plugin,
                                  const QString &where,
                                  const Guard::CrashReport &report)
{
    const QString pluginId = cachedId(plugin);
    const QString label = pluginId.isEmpty() ? safeClassName(plugin) : pluginId;

    const QString reason = QStringLiteral("功能在 %1 中崩溃（%2），已被隔离，重新启动程序后才能再次启用")
                               .arg(where, report.description);

    m_failures.insert(plugin, reason);
    // m_failedIds 只记录"曾成功进入活动列表后才崩溃"的插件：
    // 加载期就崩掉的插件没有卡片可标记，其失败原因走 loadErrors()
    if (!pluginId.isEmpty() && m_pluginsById.contains(pluginId) && !m_failedIds.contains(pluginId)) {
        m_failedIds.append(pluginId);
        // 配置里强制停用：下次启动也不会自动拉起一个"已崩溃"的功能
        SettingsManager::instance().setPluginEnabled(pluginId, false);
    }

    qCCritical(lcPlugin).noquote() << QStringLiteral("插件已隔离 [%1] %2").arg(label, reason);

    // ⚠ 不调用插件对象的任何方法（setState/setLastError 都是插件对象上的调用），
    //   失败状态由主程序自己持有并经下面的信号告知界面
    Q_EMIT pluginError(label, reason);
    Q_EMIT pluginCrashed(label, reason);
    return reason;
}

// ---------------------------------------------------------------------------
//  加载
// ---------------------------------------------------------------------------

int PluginManager::loadPlugins(const QString &pluginDirectory)
{
    const QDir dir(pluginDirectory);
    if (!dir.exists()) {
        qCWarning(lcPlugin) << "插件目录不存在:" << pluginDirectory;
        Q_EMIT loadFinished(m_plugins.size(), 0);
        return 0;
    }

    const QStringList filters{ QStringLiteral("*.dll") };
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Name);

    int loaded = 0;
    for (const QFileInfo &entry : entries) {
        auto loader = std::make_unique<QPluginLoader>(entry.absoluteFilePath());

        // 先读取元数据用于日志，避免加载失败时定位困难
        const QJsonObject meta = loader->metaData().value(QStringLiteral("MetaData")).toObject();
        const QString metaName = meta.value(QStringLiteral("name")).toString(entry.completeBaseName());

        QObject *instance = loader->instance();
        if (!instance) {
            const QString reason = loader->errorString();
            m_loadErrors.append(QStringLiteral("%1 -> %2").arg(metaName, reason));
            qCWarning(lcPlugin) << "插件加载失败:" << entry.fileName() << reason;
            continue;
        }

        // Q_INTERFACES 生效时 qobject_cast 才能跨 DLL 成功；否则退化为 RTTI 转换
        auto *plugin = qobject_cast<IFeaturePlugin *>(instance);
        if (!plugin) {
            plugin = dynamic_cast<IFeaturePlugin *>(instance);
            if (plugin) {
                qCWarning(lcPlugin) << "插件" << entry.fileName()
                                    << "未声明 Q_INTERFACES，已通过 RTTI 加载，建议补全。";
            }
        }
        if (!plugin) {
            m_loadErrors.append(QStringLiteral("%1 -> 不是 WinEase 功能插件（缺少 Q_INTERFACES）").arg(metaName));
            qCWarning(lcPlugin) << "非 WinEase 插件，已跳过:" << entry.fileName();
            loader->unload();
            continue;
        }

        if (!initializePlugin(plugin)) {
            // ⚠ 这里不能再用 plugin->id()：崩溃插件不允许再被调用，一律用缓存的 id
            const QString failedId = cachedId(plugin);
            const QString label = failedId.isEmpty() ? metaName : failedId;
            if (isQuarantined(plugin)) {
                // 崩溃原因同时要进"加载失败报告"，否则用户只看到一句"初始化失败"
                const QString reason = pluginFailureReason(failedId);
                if (!reason.isEmpty()) {
                    m_loadErrors.append(QStringLiteral("%1 -> %2").arg(label, reason));
                }
                quarantinedLoaders().push_back(std::move(loader)); // 不卸载，避免二次崩溃
            } else {
                m_loadErrors.append(QStringLiteral("%1 -> 初始化失败").arg(label));
                loader->unload();
            }
            continue;
        }

        const PluginMeta *info = metaOf(plugin);
        ++loaded;
        qCInfo(lcPlugin) << "已加载插件:" << (info ? info->id : QString())
                         << (info ? info->name : QString()) << entry.fileName();
        m_loaders.push_back(std::move(loader));
    }

    const int failureCount = static_cast<int>(m_loadErrors.size());
    qCInfo(lcPlugin) << QStringLiteral("插件扫描完成：成功 %1 个，失败 %2 个").arg(loaded).arg(failureCount);
    Q_EMIT loadFinished(static_cast<int>(m_plugins.size()), failureCount);
    return loaded;
}

bool PluginManager::registerStaticPlugin(IFeaturePlugin *plugin)
{
    if (!plugin) {
        return false;
    }
    if (!initializePlugin(plugin)) {
        const QString failedId = cachedId(plugin);
        if (isQuarantined(plugin)) {
            const QString reason = pluginFailureReason(failedId);
            if (!reason.isEmpty()) {
                m_loadErrors.append(QStringLiteral("%1 -> %2")
                                        .arg(failedId.isEmpty() ? safeClassName(plugin) : failedId, reason));
            }
            quarantinedPlugins().push_back(plugin); // 崩溃插件不析构（避免二次崩溃）
        } else {
            m_loadErrors.append(QStringLiteral("%1 -> 初始化失败（静态注册）")
                                    .arg(failedId.isEmpty() ? safeClassName(plugin) : failedId));
            delete plugin;
        }
        return false;
    }
    m_ownPlugins.append(cachedId(plugin));
    return true;
}

bool PluginManager::probeMetadata(IFeaturePlugin *plugin, PluginMeta *metaOut, QString *errorOut)
{
    PluginMeta meta;
    Guard::CrashReport report;

    const bool ok = Guard::invoke([&] {
        meta.id = plugin->id();
        meta.name = plugin->name();
        meta.description = plugin->description();
        meta.detailedDescription = plugin->detailedDescription();
        meta.version = plugin->version();
        meta.author = plugin->author();
        meta.tags = plugin->tags();
        meta.category = plugin->category();
        meta.requiresAdmin = plugin->requiresAdmin();
        meta.supportsHotkey = plugin->supportsHotkey();
        meta.defaultHotkey = plugin->defaultHotkey();
        meta.hasSettings = plugin->hasSettings();
        const QIcon icon = plugin->icon(); // 图标内部可能加载资源，一并探一次
        Q_UNUSED(icon)
    }, &report);

    if (!ok) {
        const QString reason = QStringLiteral("读取插件元信息时崩溃（%1）").arg(report.description);
        m_failures.insert(plugin, reason); // 登记隔离：此后不再调用该插件
        qCCritical(lcPlugin) << "插件元信息探针失败:" << safeClassName(plugin) << reason;
        m_loadErrors.append(QStringLiteral("%1 -> %2").arg(safeClassName(plugin), reason));
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return false;
    }

    if (meta.id.isEmpty()) {
        const QString reason = QStringLiteral("插件 id() 为空，已拒绝加载");
        qCWarning(lcPlugin) << reason << "类名:" << safeClassName(plugin);
        m_loadErrors.append(QStringLiteral("%1 -> %2").arg(safeClassName(plugin), reason));
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return false;
    }

    if (m_pluginsById.contains(meta.id)) {
        const QString reason = QStringLiteral("插件 id 冲突，已跳过：%1").arg(meta.id);
        qCWarning(lcPlugin) << reason;
        m_loadErrors.append(reason);
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return false;
    }

    m_meta.insert(plugin, meta);
    if (metaOut != nullptr) {
        *metaOut = meta;
    }
    return true;
}

bool PluginManager::initializePlugin(IFeaturePlugin *plugin)
{
    if (plugin == nullptr) {
        return false;
    }

    PluginMeta meta;
    QString probeError;
    if (!probeMetadata(plugin, &meta, &probeError)) {
        return false;
    }

    if (meta.requiresAdmin && !Admin::isProcessElevated()) {
        qCWarning(lcPlugin) << "插件需要管理员权限，当前未提权:" << meta.id;
    }

    // ---- 生命周期边界：initialize() 崩溃 → 隔离，绝不让主程序跟着崩 ----
    Guard::CrashReport report;
    bool initialized = false;
    if (!Guard::invoke([&] { initialized = plugin->initializePlugin(m_services); }, &report)) {
        quarantine(plugin, QStringLiteral("initialize()"), report);
        return false;
    }
    if (!initialized) {
        const QString reason = plugin->lastError().isEmpty()
            ? QStringLiteral("插件初始化失败：%1").arg(meta.id)
            : plugin->lastError();
        m_loadErrors.append(QStringLiteral("%1 -> %2").arg(meta.id, reason));
        return false;
    }

    m_plugins.append(plugin);
    m_pluginsById.insert(meta.id, plugin);
    connectPluginSignals(plugin);

    // 若插件声明支持快捷键，则把它注册到快捷键管理器
    if (meta.supportsHotkey && m_services) {
        const QString savedText = SettingsManager::instance().hotkeyText(meta.id + QStringLiteral("::default"));
        const QKeySequence seq = savedText.isEmpty()
            ? meta.defaultHotkey
            : GlobalHotkeyManager::textToSequence(savedText);
        if (!seq.isEmpty()) {
            m_services->registerHotkey(meta.id, QStringLiteral("default"), seq,
                                       QStringLiteral("%1：%2").arg(meta.name, meta.description));
        }
    }

    Q_EMIT pluginLoaded(plugin);
    return true;
}

void PluginManager::connectPluginSignals(IFeaturePlugin *plugin)
{
    const QString pluginId = cachedId(plugin);

    connect(plugin, &IFeaturePlugin::stateChanged, this, [this, plugin](PluginState state) {
        Q_EMIT pluginStateChanged(plugin, state == PluginState::Running);
    });

    connect(plugin, &IFeaturePlugin::statusMessage, this, [this, pluginId](const QString &message) {
        Q_EMIT pluginStatusMessage(pluginId, message);
    });

    connect(plugin, &IFeaturePlugin::errorOccurred, this, [this, pluginId](const QString &message) {
        qCWarning(lcPlugin) << pluginId << message;
        Q_EMIT pluginError(pluginId, message);
    });

    connect(plugin, &IFeaturePlugin::enabledChanged, this, [this, plugin, pluginId](bool enabled) {
        SettingsManager::instance().setPluginEnabled(pluginId, enabled);
        Q_EMIT pluginStateChanged(plugin, enabled);
    });
}

void PluginManager::unloadAll()
{
    if (m_plugins.isEmpty() && m_loaders.empty()) {
        return;
    }

    for (IFeaturePlugin *plugin : std::as_const(m_plugins)) {
        if (isQuarantined(plugin)) {
            // 已崩溃的插件不再调用（其资源只能随进程退出回收）
            qCWarning(lcPlugin) << "跳过已隔离插件的 shutdown():" << cachedId(plugin);
            continue;
        }
        Guard::CrashReport report;
        if (!Guard::invoke([&] { plugin->shutdownPlugin(); }, &report)) {
            quarantine(plugin, QStringLiteral("shutdown()"), report);
        }
    }

    // 静态注册的插件由本类负责释放；动态库插件由 QPluginLoader 释放。
    // 已隔离的一律不释放：在损坏对象上跑析构函数属于二次崩溃
    for (const QString &pluginId : std::as_const(m_ownPlugins)) {
        if (IFeaturePlugin *plugin = m_pluginsById.value(pluginId, nullptr)) {
            if (isQuarantined(plugin)) {
                quarantinedPlugins().push_back(plugin);
                qCCritical(lcPlugin) << "已隔离的静态插件保持存活（不析构）:" << pluginId;
                continue;
            }
            delete plugin;
        }
    }
    m_ownPlugins.clear();

    m_pluginsById.clear();
    m_plugins.clear();

    for (auto &loader : m_loaders) {
        if (!loader) {
            continue;
        }
        bool quarantined = false;
        if (auto *instance = loader->isLoaded() ? loader->instance() : nullptr) {
            if (auto *plugin = qobject_cast<IFeaturePlugin *>(instance)) {
                quarantined = isQuarantined(plugin);
            }
        }
        if (quarantined) {
            qCCritical(lcPlugin) << "已隔离的插件 DLL 保持加载（不卸载，避免析构二次崩溃）:"
                                 << loader->fileName();
            quarantinedLoaders().push_back(std::move(loader));
            continue;
        }
        loader->unload();
    }
    m_loaders.clear();

    qCInfo(lcPlugin) << "全部插件已卸载";
}

QStringList PluginManager::loadErrors() const
{
    return m_loadErrors;
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

QList<IFeaturePlugin *> PluginManager::plugins() const
{
    return m_plugins;
}

QList<IFeaturePlugin *> PluginManager::plugins(FeatureCategory category) const
{
    if (category == FeatureCategory::Unknown) {
        return m_plugins;
    }

    QList<IFeaturePlugin *> result;
    result.reserve(m_plugins.size());
    for (IFeaturePlugin *plugin : m_plugins) {
        // 走元信息缓存：崩溃插件的 category() 不能再调
        const PluginMeta *meta = metaOf(plugin);
        if (meta != nullptr && meta->category == category) {
            result.append(plugin);
        }
    }
    return result;
}

IFeaturePlugin *PluginManager::plugin(const QString &pluginId) const
{
    return m_pluginsById.value(pluginId, nullptr);
}

IFeaturePlugin *PluginManager::pluginByName(const QString &name) const
{
    const QString needle = name.trimmed();
    if (needle.isEmpty()) {
        return nullptr;
    }

    // 1) 精确匹配显示名
    for (IFeaturePlugin *plugin : m_plugins) {
        const PluginMeta *meta = metaOf(plugin);
        if (meta != nullptr && meta->name.compare(needle, Qt::CaseInsensitive) == 0) {
            return plugin;
        }
    }

    // 2) 精确匹配标识（兼容调用方直接传 id）
    if (IFeaturePlugin *byId = plugin(needle)) {
        return byId;
    }

    // 3) 模糊匹配显示名，便于按前缀快速定位
    for (IFeaturePlugin *plugin : m_plugins) {
        const PluginMeta *meta = metaOf(plugin);
        if (meta != nullptr && meta->name.contains(needle, Qt::CaseInsensitive)) {
            return plugin;
        }
    }

    return nullptr;
}

int PluginManager::pluginCount() const
{
    return m_plugins.size();
}

int PluginManager::pluginCount(FeatureCategory category) const
{
    if (category == FeatureCategory::Unknown) {
        return m_plugins.size();
    }
    int count = 0;
    for (IFeaturePlugin *plugin : m_plugins) {
        const PluginMeta *meta = metaOf(plugin);
        if (meta != nullptr && meta->category == category) {
            ++count;
        }
    }
    return count;
}

QHash<int, int> PluginManager::categoryCounts() const
{
    QHash<int, int> counts;
    for (FeatureCategory category : Category::all()) {
        counts.insert(static_cast<int>(category), 0);
    }

    for (IFeaturePlugin *plugin : m_plugins) {
        const PluginMeta *meta = metaOf(plugin);
        if (meta == nullptr) {
            continue;
        }
        const int key = static_cast<int>(meta->category);
        counts[key] = counts.value(key, 0) + 1;
    }
    return counts;
}

// ---------------------------------------------------------------------------
//  启用状态
// ---------------------------------------------------------------------------

bool PluginManager::setPluginEnabled(const QString &pluginId, bool enabled)
{
    IFeaturePlugin *target = plugin(pluginId);
    if (!target) {
        return false;
    }

    if (isQuarantined(target)) {
        const QString reason = pluginFailureReason(pluginId);
        qCWarning(lcPlugin) << "拒绝操作已隔离的功能:" << pluginId;
        Q_EMIT pluginError(pluginId, reason);
        return false;
    }

    const PluginMeta *meta = metaOf(target);
    if (enabled && meta != nullptr && meta->requiresAdmin && !Admin::isProcessElevated()) {
        const QString reason = tr("该功能需要管理员权限，请以管理员身份重新启动 WinEase。");
        qCWarning(lcPlugin) << pluginId << reason;
        Q_EMIT pluginError(pluginId, reason);
        return false;
    }

    // ---- 生命周期边界：onEnable()/onDisable() 崩溃 → 隔离 ----
    Guard::CrashReport report;
    bool ok = false;
    if (!Guard::invoke([&] { ok = target->setEnabled(enabled); }, &report)) {
        quarantine(target,
                   enabled ? QStringLiteral("启用流程 onEnable()") : QStringLiteral("停用流程 onDisable()"),
                   report);
        return false;
    }

    if (ok) {
        SettingsManager::instance().setPluginEnabled(pluginId, enabled);
    }
    return ok;
}

bool PluginManager::togglePlugin(const QString &pluginId)
{
    IFeaturePlugin *target = plugin(pluginId);
    if (!target) {
        return false;
    }
    if (isQuarantined(target)) {
        Q_EMIT pluginError(pluginId, pluginFailureReason(pluginId));
        return false;
    }
    return setPluginEnabled(pluginId, !target->isEnabled());
}

bool PluginManager::isPluginEnabled(const QString &pluginId) const
{
    IFeaturePlugin *target = plugin(pluginId);
    if (target == nullptr || isQuarantined(target)) {
        return false;
    }
    return target->isEnabled();
}

int PluginManager::setAllEnabled(bool enabled)
{
    int changed = 0;
    for (IFeaturePlugin *plugin : std::as_const(m_plugins)) {
        if (isQuarantined(plugin)) {
            continue; // 已隔离的功能不再尝试启停
        }
        if (setPluginEnabled(cachedId(plugin), enabled)) {
            ++changed;
        }
    }
    return changed;
}

void PluginManager::restoreSavedStates()
{
    const SettingsManager &settings = SettingsManager::instance();
    for (IFeaturePlugin *plugin : std::as_const(m_plugins)) {
        if (isQuarantined(plugin)) {
            continue;
        }
        // 默认不自动启用，避免首次运行时同时开启大量功能打扰用户
        const bool saved = settings.isPluginEnabled(cachedId(plugin), false);
        if (saved) {
            setPluginEnabled(cachedId(plugin), true);
        }
    }
}

void PluginManager::saveStates() const
{
    for (IFeaturePlugin *plugin : m_plugins) {
        const QString pluginId = cachedId(plugin);
        if (pluginId.isEmpty()) {
            continue;
        }
        if (isQuarantined(plugin)) {
            // 崩溃插件在配置里固定为停用：不触碰插件对象
            SettingsManager::instance().setPluginEnabled(pluginId, false);
            continue;
        }
        SettingsManager::instance().setPluginEnabled(pluginId, plugin->isEnabled());
    }
    SettingsManager::instance().sync();
}

bool PluginManager::hasElevationBlockedPlugins() const
{
    if (Admin::isProcessElevated()) {
        return false;
    }
    for (IFeaturePlugin *plugin : m_plugins) {
        const PluginMeta *meta = metaOf(plugin);
        if (meta != nullptr && meta->requiresAdmin) {
            return true;
        }
    }
    return false;
}

QStringList PluginManager::elevationBlockedPluginNames() const
{
    QStringList names;
    if (Admin::isProcessElevated()) {
        return names;
    }
    for (IFeaturePlugin *plugin : m_plugins) {
        const PluginMeta *meta = metaOf(plugin);
        if (meta != nullptr && meta->requiresAdmin) {
            names.append(meta->name);
        }
    }
    return names;
}

// ---------------------------------------------------------------------------
//  带异常边界的能力调用（P0-5）
// ---------------------------------------------------------------------------

QWidget *PluginManager::createSettingsWidget(const QString &pluginId, QWidget *parent)
{
    IFeaturePlugin *target = plugin(pluginId);
    if (target == nullptr) {
        return nullptr;
    }
    if (isQuarantined(target)) {
        Q_EMIT pluginError(pluginId, pluginFailureReason(pluginId));
        return nullptr;
    }
    const PluginMeta *meta = metaOf(target);
    if (meta != nullptr && !meta->hasSettings) {
        return nullptr;
    }

    QWidget *panel = nullptr;
    Guard::CrashReport report;
    if (!Guard::invoke([&] { panel = target->createSettingsWidget(parent); }, &report)) {
        quarantine(target, QStringLiteral("createSettingsWidget()"), report);
        return nullptr;
    }
    return panel;
}

bool PluginManager::dispatchHotkey(const QString &hotkeyId)
{
    const int separator = hotkeyId.indexOf(QStringLiteral("::"));
    if (separator <= 0) {
        return false;
    }

    const QString pluginId = hotkeyId.left(separator);
    IFeaturePlugin *target = plugin(pluginId);
    if (target == nullptr) {
        return false;
    }
    if (isQuarantined(target)) {
        Q_EMIT pluginError(pluginId, pluginFailureReason(pluginId));
        return false;
    }

    Guard::CrashReport report;
    if (!Guard::invoke([&] { target->dispatchHotkey(hotkeyId); }, &report)) {
        quarantine(target, QStringLiteral("快捷键回调 onHotkey()"), report);
        return false;
    }
    return true;
}

} // namespace WinEase
