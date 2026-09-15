// ============================================================================
//  scratch_repro.cpp —— 临时排障：复现 display.focus_highlight 在 unloadAll() /
//  shutdown() 阶段抛 C++ 异常（被插件边界隔离、DLL 常驻）的问题。
//
//  只做一件事：起真实的 PluginManager + StubServices（含真实 OverlayHost），
//  按"启用 → 触发聚光灯 → 停用 → unloadAll"的顺序走一遍，把每一步的日志打出来。
//  用完即删，不进 CMake 正式目标。
// ============================================================================

#include "app/core/PluginManager.h"
#include "app/core/SettingsManager.h"
#include "sdk/OverlayHost.h"
#include "sdk/PluginServices.h"
#include "win32/ComApartment.h"

#include <QApplication>
#include <QDir>
#include <QHash>
#include <QTimer>
#include <QVariant>

#include <cstdio>

namespace {

class Stub : public WinEase::PluginServices
{
public:
    QVariant configValue(const QString &pluginId, const QString &key,
                         const QVariant &def) const override
    {
        return m_config.value(pluginId + QStringLiteral("::") + key, def);
    }
    void setConfigValue(const QString &pluginId, const QString &key,
                        const QVariant &value) override
    {
        m_config.insert(pluginId + QStringLiteral("::") + key, value);
    }
    void syncConfig() override {}
    void log(const QString &pluginId, WinEase::PluginLogLevel, const QString &message) override
    {
        std::printf("[log] %s: %s\n", qPrintable(pluginId), qPrintable(message));
    }
    void notify(const QString &, const QString &) override {}
    void setTrayBadge(const QString &, const QString &, const QString &, const QString &) override {}
    bool registerHotkey(const QString &, const QString &, const QKeySequence &,
                        const QString &) override { return true; }
    void unregisterHotkey(const QString &, const QString &) override {}
    WinEase::HookService *hookService() const override { return nullptr; }
    WinEase::OverlayHost *overlayHost() const override { return &m_host; }
    WinEase::ElevationService *elevationService() const override { return nullptr; }
    QString appVersion() const override { return QStringLiteral("repro"); }
    bool isElevated() const override { return false; }
    QString configFilePath() const override
    {
        return WinEase::SettingsManager::instance().configFilePath();
    }
    QString logDirectory() const override { return QString(); }
    QWidget *mainWindow() const override { return nullptr; }

private:
    QHash<QString, QVariant> m_config;
    mutable WinEase::OverlayHost m_host;
};

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);

    const WinEase::Win32::ComApartment com = WinEase::Win32::ComApartment::initialize();

    const QString tempAppData =
        QDir::temp().filePath(QStringLiteral("winease_repro_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(tempAppData);
    qputenv("APPDATA", tempAppData.toLocal8Bit());
    WinEase::SettingsManager::instance().initialize();

    Stub services;
    WinEase::PluginManager manager;
    manager.setServices(&services);

    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    const int loaded = manager.loadPlugins(dir);
    std::printf("== 加载 %d 个插件 ==\n", loaded);
    std::printf("== 只关心 display.focus_highlight ==\n");

    const QString id = QStringLiteral("display.focus_highlight");
    std::printf("-- 步骤 1：setPluginEnabled(true)\n");
    std::printf("   结果=%d failed=%d\n", manager.setPluginEnabled(id, true),
                manager.isPluginFailed(id));

    std::printf("-- 步骤 2：dispatchHotkey(default) 打开聚光灯\n");
    std::printf("   结果=%d failed=%d\n", manager.dispatchHotkey(id + QStringLiteral("::default")),
                manager.isPluginFailed(id));

    // 让 30fps 的 tick 跑一会儿（tick 里会读光标、刷脏区）
    QTimer::singleShot(1200, &app, [] { QCoreApplication::quit(); });
    app.exec();
    std::printf("   事件循环跑完 1.2s，failed=%d\n", manager.isPluginFailed(id));

    std::printf("-- 步骤 3：setPluginEnabled(false)\n");
    std::printf("   结果=%d failed=%d\n", manager.setPluginEnabled(id, false),
                manager.isPluginFailed(id));

    std::printf("-- 步骤 4：unloadAll()\n");
    manager.unloadAll();
    std::printf("   unloadAll 返回，failed=%d\n", manager.isPluginFailed(id));

    if (!manager.isPluginFailed(id)) {
        std::printf("== 未复现：shutdown() 正常结束 ==\n");
    } else {
        std::printf("== 复现：shutdown() 被隔离，原因=%s ==\n",
                    qPrintable(manager.pluginFailureReason(id)));
    }

    WinEase::SettingsManager::instance().sync();
    QDir(tempAppData).removeRecursively();
    return 0;
}
