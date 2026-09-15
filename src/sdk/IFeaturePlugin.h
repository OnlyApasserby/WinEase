#pragma once

// ============================================================================
//  IFeaturePlugin.h —— 功能插件抽象基类
//
//  这是 WinEase 插件体系的唯一契约。每一个功能（例如"窗口置顶"、"剪贴板历史"）
//  都实现为一个独立动态库（.dll），继承本类并通过 Q_PLUGIN_METADATA 导出。
//
//  ---------------------------------------------------------------------------
//  插件实现模板（四个必须点：继承 IFeaturePlugin + Q_OBJECT + Q_PLUGIN_METADATA
//  + Q_INTERFACES）：
//
//      class MyPlugin : public WinEase::IFeaturePlugin   // 不要再继承 QObject！
//      {
//          Q_OBJECT
//          Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "my_plugin.json")
//          Q_INTERFACES(WinEase::IFeaturePlugin)
//      public:
//          QString id() const override { return QStringLiteral("window.pin"); }
//          ...
//      };
//
//  因为 IFeaturePlugin 已经继承 QObject，插件类只继承接口即可；若像 Qt 官方示例
//  那样写成 "public QObject, public IFeaturePlugin"，就会出现二义基类，导致
//  connect() 调用不明确、moc 生成的 qt_metacast 转换歧义等编译错误。
//
//  注意：Q_INTERFACES 是 qobject_cast<IFeaturePlugin*> 能跨 DLL 生效的前提，
//  缺少它主程序将无法识别插件。
//  ---------------------------------------------------------------------------
//
//  生命周期（由主程序 PluginManager 驱动）：
//
//      QPluginLoader::instance()
//            │
//            ▼
//      initializePlugin(services)   ──► 读取配置、创建内部资源
//            │
//            ▼
//      setEnabled(true)  ──► onEnable()   功能真正开始工作
//      setEnabled(false) ──► onDisable()
//            │
//            ▼
//      shutdownPlugin()     ──► 保存状态、释放资源
// ============================================================================

#include "sdk/FeatureCategory.h"
#include "sdk/WinEaseVersion.h"

#include <QIcon>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>

class QWidget;

namespace WinEase {

class PluginServices;
/// 日志级别（完整定义见 sdk/PluginServices.h，此处仅前置声明供接口使用）
enum class PluginLogLevel;

/// 插件运行状态
enum class PluginState {
    Unloaded = 0, ///< 尚未加载 / 已卸载
    Loaded,       ///< 已初始化，处于待命状态
    Running,      ///< 功能已启用，正在运行
    Failed        ///< 初始化或启用失败
};

// ============================================================================
//  IFeaturePlugin
// ============================================================================
class IFeaturePlugin : public QObject
{
    Q_OBJECT

public:
    explicit IFeaturePlugin(QObject *parent = nullptr);
    ~IFeaturePlugin() override;

    IFeaturePlugin(const IFeaturePlugin &) = delete;
    IFeaturePlugin &operator=(const IFeaturePlugin &) = delete;

    // ========================================================================
    //  一、元信息（子类必须实现）
    // ========================================================================

    /// 全局唯一标识，建议 "<分类>.<功能>"，例如 "window.pin"。
    /// 该值同时作为配置文件 section 名与快捷键前缀，发布后不可更改。
    virtual QString id() const = 0;

    /// 功能中文名（卡片标题）
    virtual QString name() const = 0;

    /// 一句话功能描述（卡片副标题）
    virtual QString description() const = 0;

    /// 卡片 / 列表图标（建议 32x32 以上，可为资源图标）
    virtual QIcon icon() const = 0;

    /// 所属分类，决定在主界面左侧导航栏中的位置
    virtual FeatureCategory category() const = 0;

    // ---------------- 元信息（可选覆盖）----------------
    /// 插件版本号
    virtual QString version() const;
    /// 作者
    virtual QString author() const;
    /// 帮助页「关于插件」里的**完整说明**（卡片上放不下的东西：能力边界、已知限制、
    /// "为什么只做成这样"这类）。支持多段（用 `\n`）；返回空串表示没有额外说明。
    /// ⚠ 这一份是**给用户看的**：写清"做不到什么、为什么"，不要只写宣传语。
    virtual QString detailedDescription() const;
    /// 搜索关键词补充（中文别名 / 拼音首字母），参与搜索框匹配
    virtual QStringList tags() const;

    // ========================================================================
    //  二、能力标记（主程序据此做界面提示与权限校验）
    // ========================================================================

    /// 是否需要管理员权限；为 true 时主程序会在启动阶段提示用户
    virtual bool requiresAdmin() const;
    /// 是否支持全局快捷键
    virtual bool supportsHotkey() const;
    /// 默认全局快捷键（支持快捷键时有效）
    virtual QKeySequence defaultHotkey() const;
    /// 是否提供设置面板（settingsWidget() 返回 nullptr 时返回 false）
    virtual bool hasSettings() const;

    // ========================================================================
    //  三、生命周期
    // ========================================================================

    /// 初始化：读取配置、创建内部对象。返回 false 视为加载失败。
    /// 注意：本函数会被 initializePlugin() 调用，不要在内部再次触发启停。
    virtual bool initialize() = 0;

    /// 关闭：保存状态、释放资源。必须保证可被重复调用。
    virtual void shutdown() = 0;

    /// 设置面板；无设置项时返回 nullptr。返回值所有权归调用方（父窗口）。
    virtual QWidget *createSettingsWidget(QWidget *parent = nullptr);

    // ========================================================================
    //  四、运行状态（由主程序驱动，插件一般只读）
    // ========================================================================

    /// 主程序注入宿主服务（在 initialize() 之前调用）
    void setServices(PluginServices *services);
    PluginServices *services() const;

    /// 受保护的两段式入口：状态校验 + 调用 initialize()/shutdown()
    bool initializePlugin(PluginServices *services);
    void shutdownPlugin();

    bool isInitialized() const;

    bool isEnabled() const;
    /// 切换启用状态；成功返回 true，失败时状态保持不变
    bool setEnabled(bool enabled);

    /// 宿主分发全局快捷键的公开入口（由 PluginManager 调用，内部转发到受保护的 onHotkey()）。
    /// ⚠ 主程序必须**隔着 SEH/C++ 异常边界**调用本函数（否则插件崩在 onHotkey 里会带走主程序）
    void dispatchHotkey(const QString &hotkeyId);

    PluginState state() const;
    void setState(PluginState state);

    /// 最近一次错误信息（空表示无错误）
    QString lastError() const;

Q_SIGNALS:
    /// 启用状态发生变化
    void enabledChanged(bool enabled);
    /// 运行状态发生变化
    void stateChanged(WinEase::PluginState state);
    /// 插件向卡片推送的简短状态文本，例如"已置顶 3 个窗口"
    void statusMessage(const QString &message);
    /// 发生错误（主程序会记入日志并在界面上体现）
    void errorOccurred(const QString &message);
    /// 请求主程序弹出系统通知
    void notificationRequested(const QString &title, const QString &body);
    /// 全局快捷键被触发（hotkeyId 为 "<pluginId>::<action>"）
    void hotkeyTriggered(const QString &hotkeyId);

protected:
    /// 真正的启用逻辑：返回 false 表示启用失败（状态会回滚）
    virtual bool onEnable();
    /// 真正的停用逻辑
    virtual void onDisable();

    /// 启用前置校验，例如依赖缺失、系统版本过低。reason 用于回写错误信息。
    virtual bool canEnable(QString *reason) const;

    /// 全局快捷键回调入口，默认转发到 hotkeyTriggered 信号
    virtual void onHotkey(const QString &hotkeyId);

    void setLastError(const QString &error);
    void clearLastError();

    /// 便捷方法：通过宿主服务写日志
    void logMessage(PluginLogLevel level, const QString &message);

private:
    PluginServices *m_services = nullptr;
    PluginState m_state = PluginState::Unloaded;
    bool m_enabled = false;
    bool m_initialized = false;
    QString m_lastError;
};

} // namespace WinEase

Q_DECLARE_METATYPE(WinEase::PluginState)

// ---------------------------------------------------------------------------
//  插件接口注册
//
//  IID 必须与插件侧 Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid ...) 完全一致，
//  它是 qobject_cast<IFeaturePlugin *>() 能够跨 DLL 边界识别插件的依据。
//  缺少本行时，主程序只能退回 RTTI（dynamic_cast）识别插件，且在 moc 阶段会报
//  "Undefined interface"。
// ---------------------------------------------------------------------------
Q_DECLARE_INTERFACE(WinEase::IFeaturePlugin, WinEase_IFeaturePlugin_iid)
