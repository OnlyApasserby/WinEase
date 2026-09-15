// ============================================================================
//  plugin_smoke —— 插件异常边界与崩溃隔离验收（P0-5）
//
//  验收目标（对应 ROADMAP P0-5）：
//    * 插件在 initialize() 里抛 C++ 异常 / 触发访问违例 → 主程序照常运行
//    * 崩溃插件被自动隔离：对外表现为"已失败"、原因可展示、且**不再被调用**
//    * 其余插件完全不受影响（启用、快捷键分发、停用照常）
//    * 卸载阶段不会因为已崩溃插件而二次崩溃（进程能正常退出）
//
//  与 theme_smoke / elevation_smoke 同一思路：把**真实的 PluginManager 与
//  PluginGuard**（主程序源文件）编入本目标，测的是主程序真正走的代码路径。
//
//  运行方式：
//      .\build\bin\plugin_smoke.exe        # 退出码 0 表示全部通过
//
//  ⚠ 本自检会在"临时目录"里跑（把 APPDATA 指过去），不会污染真实配置。
// ============================================================================

#include "core/AdminHelper.h"
#include "core/PluginGuard.h"
#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "sdk/FeatureCategory.h"
#include "sdk/IFeaturePlugin.h"
#include "sdk/PluginServices.h"

#include <QApplication>
#include <QDir>
#include <QHash>
#include <QKeySequence>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <stdexcept>

using WinEase::FeatureCategory;
using WinEase::IFeaturePlugin;
using WinEase::PluginManager;
using WinEase::PluginServices;
using WinEase::PluginState;
using WinEase::SettingsManager;

// ⚠ 这里用命名空间别名而不是 using 声明：MSVC 不接受 `using WinEase::Guard;`
//   （C2873），别名则完全正常
namespace Guard = WinEase::Guard;

namespace {

// ---------------------------------------------------------------------------
//  崩溃注入用的全局计数与"危险指针"
//  （用文件作用域 volatile 指针，编译器无法把空指针写入优化掉）
// ---------------------------------------------------------------------------

volatile int *g_nullTarget = nullptr;
volatile int g_sink = 0;

int g_throwInitCalls = 0;      ///< initialize() 抛异常的次数
int g_avInitCalls = 0;         ///< initialize() 触发访问违例的次数
int g_enableCrashCalls = 0;    ///< onEnable() 崩溃的次数
int g_enableCrashShutdowns = 0;///< 崩溃插件的 shutdown() 被调用次数（应为 0）
int g_settingsCrashCalls = 0;  ///< createSettingsWidget() 崩溃的次数
int g_healthyEnableCalls = 0;  ///< 正常插件 onEnable 次数
int g_healthyDisableCalls = 0; ///< 正常插件 onDisable 次数
int g_healthyHotkeyCalls = 0;  ///< 正常插件 onHotkey 次数
int g_healthyShutdowns = 0;    ///< 正常插件 shutdown 次数

/// 让编译器无法推断：写空指针
void crashByAccessViolation()
{
    *g_nullTarget = 42;
}

// ---------------------------------------------------------------------------
//  测试报告器
// ---------------------------------------------------------------------------

class Reporter
{
public:
    void check(bool ok, const QString &name, const QString &detail = QString())
    {
        ++m_total;
        if (ok) {
            std::printf("[通过] %s\n", name.toUtf8().constData());
            return;
        }
        ++m_failures;
        std::printf("[失败] %s", name.toUtf8().constData());
        if (!detail.isEmpty()) {
            std::printf("  —— %s", detail.toUtf8().constData());
        }
        std::printf("\n");
    }

    void info(const QString &text)
    {
        std::printf("[信息] %s\n", text.toUtf8().constData());
    }

    int total() const { return m_total; }
    int failures() const { return m_failures; }

private:
    int m_total = 0;
    int m_failures = 0;
};

// ---------------------------------------------------------------------------
//  宿主服务桩：只记录调用，不真正注册全局快捷键 / 不弹通知
// ---------------------------------------------------------------------------

class StubServices : public PluginServices
{
public:
    QVariant configValue(const QString &, const QString &key, const QVariant &defaultValue) const override
    {
        return m_config.value(key, defaultValue);
    }

    void setConfigValue(const QString &, const QString &key, const QVariant &value) override
    {
        m_config.insert(key, value);
    }

    void syncConfig() override {}

    void log(const QString &pluginId, WinEase::PluginLogLevel, const QString &message) override
    {
        ++m_logCount;
        Q_UNUSED(pluginId)
        Q_UNUSED(message)
    }

    void notify(const QString &title, const QString &body) override
    {
        ++m_notifications;
        Q_UNUSED(title)
        Q_UNUSED(body)
    }

    void setTrayBadge(const QString &pluginId, const QString &text,
                      const QString &colorHex, const QString &tooltip) override
    {
        ++m_badgeCalls;
        Q_UNUSED(colorHex)
        Q_UNUSED(tooltip)
        // 空文字 = 清除（约定见 PluginServices::setTrayBadge）。
        // 隔离测试会用一个"停用时不收拾界面"的插件来证明**宿主那一侧**必须兜底，
        // 所以这里如实记录"插件请求了什么"，不做任何自作主张的清理。
        if (text.isEmpty()) {
            m_badges.remove(pluginId);
        } else {
            m_badges.insert(pluginId, text);
        }
    }

    bool registerHotkey(const QString &pluginId, const QString &action,
                        const QKeySequence &sequence, const QString &description) override
    {
        Q_UNUSED(description)
        m_hotkeys.insert(pluginId + QStringLiteral("::") + action, sequence);
        return true;
    }

    void unregisterHotkey(const QString &pluginId, const QString &action) override
    {
        m_hotkeys.remove(pluginId + QStringLiteral("::") + action);
    }

    WinEase::HookService *hookService() const override { return nullptr; }
    WinEase::OverlayHost *overlayHost() const override { return nullptr; }
    WinEase::ElevationService *elevationService() const override { return nullptr; }

    QString appVersion() const override { return QStringLiteral("smoke"); }
    bool isElevated() const override { return WinEase::Admin::isProcessElevated(); }
    QString configFilePath() const override { return SettingsManager::instance().configFilePath(); }
    QString logDirectory() const override { return QDir::tempPath(); }
    QWidget *mainWindow() const override { return nullptr; }

    bool hasHotkey(const QString &hotkeyId) const { return m_hotkeys.contains(hotkeyId); }
    int notificationCount() const { return m_notifications; }
    int trayBadgeCalls() const { return m_badgeCalls; }
    /// 某个插件当前挂着的徽标（空 = 没挂）
    QString trayBadgeText(const QString &pluginId) const { return m_badges.value(pluginId); }

private:
    QHash<QString, QVariant> m_config;
    QHash<QString, QKeySequence> m_hotkeys;
    QHash<QString, QString> m_badges;
    int m_logCount = 0;
    int m_notifications = 0;
    int m_badgeCalls = 0;
};

} // namespace

// ---------------------------------------------------------------------------
//  测试用插件
//
//  ⚠ 带 Q_OBJECT 的类不要放进匿名命名空间（moc 对匿名命名空间内的类支持不佳），
//    因此单独放在具名命名空间里
// ---------------------------------------------------------------------------

namespace SmokeTest {

/// 完全正常的插件：用来证明"别的插件崩了不影响我"
class HealthyPlugin : public IFeaturePlugin
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("dev.smoke.healthy"); }
    QString name() const override { return QStringLiteral("正常功能"); }
    QString description() const override { return QStringLiteral("崩溃隔离的对照项"); }
    QIcon icon() const override { return QIcon(); }
    FeatureCategory category() const override { return FeatureCategory::Development; }
    QStringList tags() const override { return { QStringLiteral("对照"), QStringLiteral("healthy") }; }
    bool supportsHotkey() const override { return true; }
    QKeySequence defaultHotkey() const override { return QKeySequence(QStringLiteral("Ctrl+Alt+F10")); }

protected:
    bool initialize() override { return true; }
    void shutdown() override { ++g_healthyShutdowns; }
    bool onEnable() override { ++g_healthyEnableCalls; return true; }
    void onDisable() override { ++g_healthyDisableCalls; }
    void onHotkey(const QString &hotkeyId) override
    {
        ++g_healthyHotkeyCalls;
        IFeaturePlugin::onHotkey(hotkeyId);
    }
};

/// initialize() 抛 C++ 异常
class ThrowOnInitPlugin : public IFeaturePlugin
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("dev.smoke.throwinit"); }
    QString name() const override { return QStringLiteral("初始化抛异常"); }
    QString description() const override { return QStringLiteral("异常边界测试"); }
    QIcon icon() const override { return QIcon(); }
    FeatureCategory category() const override { return FeatureCategory::Development; }

protected:
    bool initialize() override
    {
        ++g_throwInitCalls;
        throw std::runtime_error("初始化时故意抛出的 C++ 异常");
    }
    void shutdown() override {}
};

/// initialize() 触发访问违例
class AccessViolationOnInitPlugin : public IFeaturePlugin
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("dev.smoke.avinit"); }
    QString name() const override { return QStringLiteral("初始化访问违例"); }
    QString description() const override { return QStringLiteral("SEH 边界测试"); }
    QIcon icon() const override { return QIcon(); }
    FeatureCategory category() const override { return FeatureCategory::Development; }

protected:
    bool initialize() override
    {
        ++g_avInitCalls;
        crashByAccessViolation();
        return true;
    }
    void shutdown() override { ++g_enableCrashShutdowns; }
};

/// onEnable() 触发访问违例：插件能正常加载，崩在启用阶段（对应"卡片显示原因"场景）
class CrashOnEnablePlugin : public IFeaturePlugin
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("dev.smoke.enablecrash"); }
    QString name() const override { return QStringLiteral("启用时崩溃"); }
    QString description() const override { return QStringLiteral("崩溃隔离测试"); }
    QIcon icon() const override { return QIcon(); }
    FeatureCategory category() const override { return FeatureCategory::Development; }
    bool supportsHotkey() const override { return true; }
    QKeySequence defaultHotkey() const override { return QKeySequence(QStringLiteral("Ctrl+Alt+F11")); }

protected:
    bool initialize() override { return true; }
    void shutdown() override { ++g_enableCrashShutdowns; }
    bool onEnable() override
    {
        // 先计数再崩：这样"隔离后计数不再增长"才能证明确实没有再调用插件
        ++g_enableCrashCalls;
        crashByAccessViolation();
        return true;
    }
    void onHotkey(const QString &hotkeyId) override
    {
        ++g_enableCrashCalls;
        crashByAccessViolation();
        IFeaturePlugin::onHotkey(hotkeyId);
    }
};

/// createSettingsWidget() 触发访问违例：打开设置面板时崩溃
class CrashOnSettingsPlugin : public IFeaturePlugin
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("dev.smoke.settingscrash"); }
    QString name() const override { return QStringLiteral("设置面板崩溃"); }
    QString description() const override { return QStringLiteral("设置面板异常边界测试"); }
    QIcon icon() const override { return QIcon(); }
    FeatureCategory category() const override { return FeatureCategory::Development; }
    bool hasSettings() const override { return true; }

protected:
    bool initialize() override { return true; }
    void shutdown() override { ++g_enableCrashShutdowns; }
    QWidget *createSettingsWidget(QWidget *parent) override
    {
        Q_UNUSED(parent)
        ++g_settingsCrashCalls;
        crashByAccessViolation();
        return nullptr;
    }
};

} // namespace SmokeTest

using namespace SmokeTest; // 测试用插件（具名命名空间，避免 moc 对匿名命名空间的限制）

int main(int argc, char *argv[])
{
    // 用 QApplication 而不是 QCoreApplication：插件元信息探针会调用 icon()，
    // 而 SDK 的 Category::icon() 内部可能创建像素图（需要 GUI 应用实例）
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("plugin_smoke"));

    // ---- 把配置目录重定向到临时目录，避免污染真实 %APPDATA%/WinEase ----
    const QString tempAppData = QDir::temp().filePath(
        QStringLiteral("winease_plugin_smoke_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(tempAppData);
    qputenv("APPDATA", tempAppData.toLocal8Bit());
    SettingsManager::instance().initialize();

    Reporter reporter;
    std::printf("=== 插件异常边界与崩溃隔离验收（P0-5）===\n");

    // -----------------------------------------------------------------------
    //  0) 边界原语本身：C++ 异常 与 SEH 必须被正确区分
    // -----------------------------------------------------------------------
    {
        Guard::CrashReport report;
        const bool ok = Guard::invoke([] { throw std::runtime_error("boom"); }, &report);
        reporter.check(!ok && report.kind == Guard::CrashKind::CppException
                           && report.detail.contains(QStringLiteral("boom")),
                       QStringLiteral("异常边界：C++ 异常被识别为 CppException 并取到 what()"),
                       report.description);
    }
    {
        Guard::CrashReport report;
        const bool ok = Guard::invoke([] { crashByAccessViolation(); }, &report);
        reporter.check(!ok && report.kind == Guard::CrashKind::StructuredException
                           && report.code == 0xC0000005ul,
                       QStringLiteral("异常边界：访问违例被识别为 SEH 0xC0000005"),
                       QStringLiteral("code=0x%1").arg(report.code, 8, 16, QLatin1Char('0')));
    }
    {
        Guard::CrashReport report;
        const bool ok = Guard::invoke([] { g_sink = 1; }, &report);
        reporter.check(ok && !report.crashed(),
                       QStringLiteral("异常边界：正常调用返回 true 且无崩溃报告"));
    }

    // -----------------------------------------------------------------------
    //  1) 正常插件：注册、元信息缓存、快捷键登记
    // -----------------------------------------------------------------------
    StubServices services;
    PluginManager manager;
    manager.setServices(&services);

    QStringList crashedIds;
    QStringList crashReasons;
    QObject::connect(&manager, &PluginManager::pluginCrashed, &manager,
                     [&crashedIds, &crashReasons](const QString &pluginId, const QString &reason) {
                         crashedIds.append(pluginId);
                         crashReasons.append(reason);
                     });

    auto *healthy = new HealthyPlugin();
    reporter.check(manager.registerStaticPlugin(healthy),
                   QStringLiteral("正常插件注册成功"));
    reporter.check(manager.pluginCount() == 1,
                   QStringLiteral("活动插件数为 1"),
                   QStringLiteral("实际 %1").arg(manager.pluginCount()));
    reporter.check(manager.idOf(healthy) == QStringLiteral("dev.smoke.healthy")
                       && manager.nameOf(healthy) == QStringLiteral("正常功能")
                       && manager.categoryOf(healthy) == FeatureCategory::Development,
                   QStringLiteral("元信息缓存（id/名称/分类）可用"));
    reporter.check(services.hasHotkey(QStringLiteral("dev.smoke.healthy::default")),
                   QStringLiteral("声明支持快捷键的插件已被登记到快捷键管理器"));

    // -----------------------------------------------------------------------
    //  2) initialize() 抛 C++ 异常 → 拒绝加载，主程序存活
    // -----------------------------------------------------------------------
    auto *thrower = new ThrowOnInitPlugin();
    reporter.check(!manager.registerStaticPlugin(thrower),
                   QStringLiteral("initialize() 抛 C++ 异常的插件注册失败（主程序存活）"));
    reporter.check(manager.pluginCount() == 1,
                   QStringLiteral("异常插件未被计入活动插件"));
    reporter.check(manager.loadErrors().join(QLatin1Char('\n')).contains(QStringLiteral("C++ 异常")),
                   QStringLiteral("加载错误里给出了 C++ 异常原因"),
                   manager.loadErrors().join(QLatin1Char('\n')));

    // -----------------------------------------------------------------------
    //  3) initialize() 触发访问违例 → 拒绝加载，主程序存活
    // -----------------------------------------------------------------------
    auto *avInit = new AccessViolationOnInitPlugin();
    reporter.check(!manager.registerStaticPlugin(avInit),
                   QStringLiteral("initialize() 访问违例的插件注册失败（主程序存活）"));
    reporter.check(manager.loadErrors().join(QLatin1Char('\n')).contains(QStringLiteral("访问违例")),
                   QStringLiteral("加载错误里给出了访问违例原因"),
                   manager.loadErrors().join(QLatin1Char('\n')));

    // -----------------------------------------------------------------------
    //  4) onEnable() 崩溃 → 已加载插件被隔离并标记失败
    // -----------------------------------------------------------------------
    auto *enableCrash = new CrashOnEnablePlugin();
    reporter.check(manager.registerStaticPlugin(enableCrash),
                   QStringLiteral("onEnable 会崩的插件可以正常注册（崩溃发生在启用阶段）"));
    reporter.check(!manager.setPluginEnabled(QStringLiteral("dev.smoke.enablecrash"), true),
                   QStringLiteral("启用崩溃插件返回失败"));
    reporter.check(g_enableCrashCalls == 1,
                   QStringLiteral("崩溃插件的 onEnable 确实被调用过 1 次"),
                   QStringLiteral("实际 %1").arg(g_enableCrashCalls));
    reporter.check(manager.isPluginFailed(QStringLiteral("dev.smoke.enablecrash")),
                   QStringLiteral("崩溃插件被标记为已失败（可展示给用户）"));
    reporter.check(manager.pluginFailureReason(QStringLiteral("dev.smoke.enablecrash"))
                       .contains(QStringLiteral("onEnable")),
                   QStringLiteral("失败原因指出崩在哪一步"),
                   manager.pluginFailureReason(QStringLiteral("dev.smoke.enablecrash")));
    reporter.check(crashedIds.contains(QStringLiteral("dev.smoke.enablecrash"))
                       && !crashReasons.isEmpty(),
                   QStringLiteral("pluginCrashed 信号已发出（界面据此在卡片上显示原因）"),
                   crashReasons.join(QLatin1Char('|')));
    reporter.check(!manager.isPluginEnabled(QStringLiteral("dev.smoke.enablecrash")),
                   QStringLiteral("隔离插件对外表现为未启用"));

    // 隔离的两条硬约束：不再调用插件、不再允许启用
    const int callsBefore = g_enableCrashCalls;
    reporter.check(!manager.setPluginEnabled(QStringLiteral("dev.smoke.enablecrash"), true),
                   QStringLiteral("对已隔离插件的启用请求被拒绝"));
    reporter.check(!manager.dispatchHotkey(QStringLiteral("dev.smoke.enablecrash::default")),
                   QStringLiteral("对已隔离插件的快捷键分发被拒绝"));
    reporter.check(manager.createSettingsWidget(QStringLiteral("dev.smoke.enablecrash"), nullptr) == nullptr,
                   QStringLiteral("对已隔离插件的设置面板请求被拒绝"));
    reporter.check(g_enableCrashCalls == callsBefore,
                   QStringLiteral("隔离后插件未被再次调用（调用计数不再增长）"),
                   QStringLiteral("隔离前 %1，隔离后 %2").arg(callsBefore).arg(g_enableCrashCalls));

    // -----------------------------------------------------------------------
    //  5) createSettingsWidget() 崩溃 → 同样隔离
    // -----------------------------------------------------------------------
    auto *settingsCrash = new CrashOnSettingsPlugin();
    reporter.check(manager.registerStaticPlugin(settingsCrash),
                   QStringLiteral("设置面板会崩的插件可以正常注册"));
    reporter.check(manager.createSettingsWidget(QStringLiteral("dev.smoke.settingscrash"), nullptr) == nullptr,
                   QStringLiteral("设置面板构造崩溃时返回 nullptr（主程序存活）"));
    reporter.check(g_settingsCrashCalls == 1
                       && manager.isPluginFailed(QStringLiteral("dev.smoke.settingscrash")),
                   QStringLiteral("设置面板崩溃的插件被隔离"));

    // -----------------------------------------------------------------------
    //  6) 其余功能完全不受影响（对应验收标准"主程序其余功能正常"）
    // -----------------------------------------------------------------------
    reporter.check(manager.setPluginEnabled(QStringLiteral("dev.smoke.healthy"), true),
                   QStringLiteral("多个插件崩溃后，正常插件仍可启用"));
    reporter.check(g_healthyEnableCalls == 1 && manager.isPluginEnabled(QStringLiteral("dev.smoke.healthy")),
                   QStringLiteral("正常插件的 onEnable 被调用且状态为已启用"));
    reporter.check(manager.dispatchHotkey(QStringLiteral("dev.smoke.healthy::default"))
                       && g_healthyHotkeyCalls == 1,
                   QStringLiteral("正常插件的全局快捷键分发成功"));
    reporter.check(!manager.dispatchHotkey(QStringLiteral("app::showMainWindow")),
                   QStringLiteral("app:: 前缀的快捷键不由插件管理器处理（返回 false 交主程序）"));
    const int changed = manager.setAllEnabled(false);
    reporter.check(changed >= 1 && g_healthyDisableCalls == 1,
                   QStringLiteral("批量停用会跳过已隔离插件且正常插件被停用"),
                   QStringLiteral("changed=%1").arg(changed));
    reporter.check(manager.pluginCount() == 3,
                   QStringLiteral("活动插件数仍为 3（2 个崩在加载期被拒，另外 2 个被隔离但保留卡片）"),
                   QStringLiteral("实际 %1").arg(manager.pluginCount()));
    reporter.check(manager.failedPluginIds().size() == 2,
                   QStringLiteral("已隔离插件列表包含 2 个插件"),
                   manager.failedPluginIds().join(QLatin1Char(',')));

    // -----------------------------------------------------------------------
    //  7) 真实插件目录：崩溃隔离不影响正常 DLL 插件加载
    // -----------------------------------------------------------------------
    const QString pluginDir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    const QStringList dlls = QDir(pluginDir).entryList({ QStringLiteral("*.dll") }, QDir::Files);
    if (dlls.isEmpty()) {
        reporter.info(QStringLiteral("plugins/ 目录为空，跳过真实 DLL 插件加载验证"));
    } else {
        const int loaded = manager.loadPlugins(pluginDir);
        reporter.check(loaded >= 1,
                       QStringLiteral("真实插件目录仍可正常加载 DLL 插件"),
                       QStringLiteral("目录 %1 中加载了 %2 个").arg(pluginDir).arg(loaded));
    }

    // -----------------------------------------------------------------------
    //  8) 卸载阶段：已隔离插件不会被触碰，进程能正常退出
    // -----------------------------------------------------------------------
    manager.saveStates();
    reporter.check(SettingsManager::instance().isPluginEnabled(QStringLiteral("dev.smoke.enablecrash"), true) == false,
                   QStringLiteral("已隔离插件在配置里被强制停用（避免下次启动又被拉起）"));
    manager.unloadAll();
    reporter.check(g_enableCrashShutdowns == 0,
                   QStringLiteral("已隔离插件在卸载阶段没有被调用 shutdown()"),
                   QStringLiteral("实际调用 %1 次").arg(g_enableCrashShutdowns));
    reporter.check(g_healthyShutdowns == 1,
                   QStringLiteral("正常插件的 shutdown() 正常执行"),
                   QStringLiteral("实际调用 %1 次").arg(g_healthyShutdowns));

    std::printf("=== 共 %d 项，失败 %d 项 ===\n", reporter.total(), reporter.failures());

    // 清理临时配置目录（在 QApplication 析构前完成）
    SettingsManager::instance().sync();
    QDir(tempAppData).removeRecursively();

    return reporter.failures() == 0 ? 0 : 1;
}

#include "main.moc"
