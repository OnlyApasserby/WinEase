#pragma once

// ============================================================================
//  PluginManager.h —— 插件加载与生命周期管理
//
//  职责：
//    1. 扫描插件目录（默认 <程序目录>/plugins），用 QPluginLoader 加载全部 .dll
//    2. 通过 qobject_cast<IFeaturePlugin*> 识别插件（依赖插件声明 Q_INTERFACES）
//    3. 调用 initializePlugin() 注入宿主服务，并按配置恢复启用状态
//    4. 为界面/托盘提供按分类查询、启用/停用等能力
//    5. 退出时统一 shutdown 并卸载
//
//  插件来源有两种：
//    * 动态库插件：plugins/ 目录下的独立 .dll
//    * 静态注册插件：编译进主程序的插件类，通过 registerStaticPlugin() 注册
//
//  ---------------------------------------------------------------------------
//  崩溃隔离（P0-5）
//
//  **每一次调用进插件的路径都包着 Guard::invoke()（SEH + C++ 异常边界），
//  一旦插件崩溃就立刻隔离**，规则如下：
//
//    * 加载期先做一次"元信息探针"（守卫调用全部元信息 getter），
//      并把结果缓存进 PluginMeta —— 崩溃后不再调用插件的任何方法（对象可能已损坏），
//      因此后续的错误上报、卡片展示全部使用缓存值；
//    * 生命周期调用（initialize/setEnabled/shutdown/createSettingsWidget/onHotkey）
//      崩溃 → 该插件进入隔离名单：状态对外表现为"已失败"，不再被任何路径调用，
//      配置里强制停用，并通过 pluginCrashed 通知界面在卡片上显示原因；
//    * 已隔离插件的 DLL **不卸载**（不调用 shutdown()、不析构 QPluginLoader）：
//      在已损坏的对象上跑析构函数属于二次崩溃，
//      代价是该 DLL 常驻到进程退出（有意为之，见 PluginGuard.h 约束 3）。
//
//  已知限制：元信息在探针之后不再重复校验。若插件在元信息 getter 里做副作用/随机崩溃，
//  仍可能崩在界面刷新路径上（探针只能覆盖"确定性崩溃"这一最常见情况）。
// ============================================================================

#include "core/PluginGuard.h"
#include "sdk/FeatureCategory.h"
#include "sdk/IFeaturePlugin.h"

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

class QPluginLoader;
class QWidget;

namespace WinEase {

class PluginServices;

class PluginManager : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager() override;

    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;

    // ---------------- 加载 ----------------

    /// 注入宿主服务（必须在 loadPlugins 之前调用）
    void setServices(PluginServices *services);

    /// 扫描目录并加载全部插件，返回成功加载数量
    int loadPlugins(const QString &pluginDirectory);

    /// 静态注册（编译进主程序的插件），用于内置功能
    bool registerStaticPlugin(IFeaturePlugin *plugin);

    /// 卸载全部插件（退出时调用，可重复调用）
    void unloadAll();

    /// 最近一次加载过程中的错误信息："插件文件/库 -> 原因"
    QStringList loadErrors() const;

    // ---------------- 查询 ----------------

    QList<IFeaturePlugin *> plugins() const;
    QList<IFeaturePlugin *> plugins(FeatureCategory category) const;
    IFeaturePlugin *plugin(const QString &pluginId) const;
    /// 按功能名（name()）查找插件；未命中时依次尝试 id 与名称模糊匹配，均忽略大小写
    IFeaturePlugin *pluginByName(const QString &name) const;
    int pluginCount() const;
    int pluginCount(FeatureCategory category) const;
    /// 分类 -> 插件数量（FeatureCategory::Unknown 表示全部）
    QHash<int, int> categoryCounts() const;

    // ---------------- 启用状态 ----------------

    bool setPluginEnabled(const QString &pluginId, bool enabled);
    bool togglePlugin(const QString &pluginId);
    bool isPluginEnabled(const QString &pluginId) const;
    /// 批量启用/停用（跳过需要管理员权限但当前未提权的插件）
    int setAllEnabled(bool enabled);

    /// 按配置恢复各插件的启用状态（加载完成后调用一次）
    void restoreSavedStates();
    /// 把当前启用状态写回配置
    void saveStates() const;

    /// 是否存在"需要管理员权限但当前未提权"的插件
    bool hasElevationBlockedPlugins() const;
    QStringList elevationBlockedPluginNames() const;

    // ---------------- 崩溃隔离（P0-5）----------------

    /// 该插件是否已因崩溃被隔离（隔离后主程序不再调用它）
    bool isPluginFailed(const QString &pluginId) const;

    // 以下"缓存读"接口在插件崩溃后仍然安全（不调用插件对象）：
    // 界面刷新/过滤路径应当用它们替代 plugin->id() / name() / category() / ...
    QString idOf(const IFeaturePlugin *plugin) const;
    QString nameOf(const IFeaturePlugin *plugin) const;
    FeatureCategory categoryOf(const IFeaturePlugin *plugin) const;
    /// 一句话描述（卡片副标题 / 帮助页）
    QString descriptionOf(const IFeaturePlugin *plugin) const;
    /// 版本号
    QString versionOf(const IFeaturePlugin *plugin) const;
    /// 作者
    QString authorOf(const IFeaturePlugin *plugin) const;
    /// 帮助页「关于插件」用的**完整说明**（`IFeaturePlugin::detailedDescription()`）；
    /// 插件没写时为空串
    QString detailedDescriptionOf(const IFeaturePlugin *plugin) const;
    /// 崩溃/隔离原因（中文，可直接展示）；未崩溃时为空
    QString pluginFailureReason(const QString &pluginId) const;
    /// 已隔离插件的 id 列表
    QStringList failedPluginIds() const;

    /// 创建插件的设置面板（带异常边界）。插件未提供或无此插件时返回 nullptr；
    /// 崩溃时返回 nullptr 并隔离该插件
    QWidget *createSettingsWidget(const QString &pluginId, QWidget *parent = nullptr);

    /// 分发全局快捷键（hotkeyId 形如 "<pluginId>::<action>"）。
    /// 返回 true 表示已交给插件处理；"app::" 前缀由主程序自行处理，本函数返回 false
    bool dispatchHotkey(const QString &hotkeyId);

Q_SIGNALS:
    void pluginLoaded(WinEase::IFeaturePlugin *plugin);
    void pluginStateChanged(WinEase::IFeaturePlugin *plugin, bool enabled);
    void pluginStatusMessage(const QString &pluginId, const QString &message);
    void pluginError(const QString &pluginId, const QString &message);
    /// 插件崩溃并被隔离（reason 为中文原因，界面应把卡片标记为失败）
    void pluginCrashed(const QString &pluginId, const QString &reason);
    void loadFinished(int totalCount, int failureCount);

private:
    /// 加载期元信息快照：探针（带异常边界）读一次后缓存，
    /// 插件崩溃后主程序不再调用插件，一切展示都取这里的值
    struct PluginMeta {
        QString id;
        QString name;
        QString description;
        /// 帮助页「关于插件」用的完整说明（卡片上放不下的能力边界等）
        QString detailedDescription;
        QString version;
        QString author;
        QStringList tags;
        QKeySequence defaultHotkey;
        FeatureCategory category = FeatureCategory::Unknown;
        bool requiresAdmin = false;
        bool supportsHotkey = false;
        bool hasSettings = false;
    };

    void connectPluginSignals(IFeaturePlugin *plugin);
    bool initializePlugin(IFeaturePlugin *plugin);
    static QString hotkeyIdPrefix(const QString &pluginId);

    /// 元信息探针：守卫调用全部元信息 getter，成功时写入 metaOut
    bool probeMetadata(IFeaturePlugin *plugin, PluginMeta *metaOut, QString *errorOut);
    /// 记录崩溃并隔离插件，返回统一的中文原因文本
    QString quarantine(IFeaturePlugin *plugin, const QString &where, const Guard::CrashReport &report);
    /// 是否已被隔离
    bool isQuarantined(const IFeaturePlugin *plugin) const;
    /// 缓存 id（不调用插件；插件崩溃后仍可用）
    QString cachedId(const IFeaturePlugin *plugin) const;
    /// 缓存元信息
    const PluginMeta *metaOf(const IFeaturePlugin *plugin) const;

    PluginServices *m_services = nullptr;

    QVector<IFeaturePlugin *> m_plugins;            ///< 保持加载顺序
    QHash<QString, IFeaturePlugin *> m_pluginsById; ///< id -> 插件
    /// QPluginLoader 不可拷贝，且 QList 要求元素可拷贝，故使用 std::vector 持有
    std::vector<std::unique_ptr<QPluginLoader>> m_loaders;

    QStringList m_loadErrors;
    QStringList m_ownPlugins; ///< 静态注册的插件 id，析构时负责释放

    QHash<const IFeaturePlugin *, PluginMeta> m_meta;    ///< 元信息缓存
    QHash<const IFeaturePlugin *, QString> m_failures;   ///< 崩溃原因（隔离标志）
    QStringList m_failedIds;                             ///< 已隔离插件 id（保持顺序）
};

} // namespace WinEase
