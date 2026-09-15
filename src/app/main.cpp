// ============================================================================
//  main.cpp —— WinEase 入口
//
//  启动流程：
//      1. 单实例检测（QSharedMemory）—— 已有实例则唤醒它并退出
//      2. 创建 QApplication，注册元类型、加载样式表
//      3. 初始化 AppContext（配置 → 日志 → 宿主服务 → 主程序快捷键）
//      4. 扫描 plugins/ 目录加载功能插件，并按配置恢复启用状态
//      5. 创建主窗口与系统托盘，连线信号
//      6. 进入事件循环
// ============================================================================

#include "core/AdminHelper.h"
#include "core/AppContext.h"
#include "core/GlobalHotkeyManager.h"
#include "core/Logging.h"
#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "core/SingleInstanceGuard.h"
#include "sdk/FeatureCategory.h"
#include "sdk/IFeaturePlugin.h"
#include "ui/MainWindow.h"
#include "ui/SystemTrayManager.h"
#include "ui/ThemeManager.h"

#include <QApplication>
#include <QMessageBox>

#include <memory>

namespace {

/// 注册跨模块/跨线程需要使用的自定义类型
void registerMetaTypes()
{
    qRegisterMetaType<WinEase::FeatureCategory>("WinEase::FeatureCategory");
    qRegisterMetaType<WinEase::IFeaturePlugin *>("WinEase::IFeaturePlugin*");
    qRegisterMetaType<WinEase::PluginState>("WinEase::PluginState");
    qRegisterMetaType<WinEase::HotkeyBinding>("WinEase::HotkeyBinding");
}

} // namespace

int main(int argc, char *argv[])
{
    // ------------------------------------------------------------------
    // 1. QApplication 初始化
    // ------------------------------------------------------------------
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QString::fromLatin1(WinEase::Identifiers::kOrganization));
    QApplication::setApplicationName(QString::fromLatin1(WinEase::Identifiers::kAppName));
    QApplication::setApplicationVersion(QStringLiteral(WINEASE_APP_VERSION));
    // 常驻托盘：关闭主窗口不等于退出进程
    QApplication::setQuitOnLastWindowClosed(false);

    // 强制深色主题（黑底白字，不随系统浅色/深色偏好切换）
    // 必须在创建任何窗口之前调用：样式与调色板只对之后创建的窗口生效
    WinEase::Ui::applyForcedDarkTheme(app);
    registerMetaTypes();

    // ------------------------------------------------------------------
    // 2. 提前初始化配置与日志
    //    这样单实例判定、插件加载等早期环节的日志才能落盘
    //    （AppContext::initialize() 会再次调用，两者均为幂等）
    // ------------------------------------------------------------------
    WinEase::SettingsManager &settings = WinEase::SettingsManager::instance();
    if (!settings.initialize()) {
        qCWarning(WinEase::Logging::lcApp) << "配置初始化失败，将使用默认值继续运行";
    }
    WinEase::Logging::install(settings.logDirectory());
    WinEase::Logging::pruneOldLogs();

    // ------------------------------------------------------------------
    // 3. 单实例检测（QSharedMemory）
    //    非主实例：唤醒已有实例的窗口后立即退出，避免快捷键/托盘被重复占用
    // ------------------------------------------------------------------
    WinEase::SingleInstanceGuard instanceGuard(QString::fromLatin1(WinEase::Identifiers::kSingleInstance));
    if (!instanceGuard.isPrimaryInstance()) {
        qCInfo(WinEase::Logging::lcApp) << "检测到已有实例，已请求其激活主窗口，本进程退出";
        instanceGuard.notifyExistingInstance(QStringLiteral("activate"));
        return 0;
    }

    // ------------------------------------------------------------------
    // 4. 应用上下文（宿主服务 / 插件管理 / 全局快捷键）
    // ------------------------------------------------------------------
    WinEase::AppContext context;
    if (!context.initialize()) {
        QMessageBox::critical(nullptr,
                              QStringLiteral("WinEase"),
                              QStringLiteral("初始化失败，请检查配置目录权限。"));
        return 1;
    }

    // ------------------------------------------------------------------
    // 5. 加载插件
    // ------------------------------------------------------------------
    const int loadedCount = context.loadPlugins();
    qCInfo(WinEase::Logging::lcApp) << "已加载功能插件数量:" << loadedCount;

    // ------------------------------------------------------------------
    // 6. 主窗口 + 系统托盘
    // ------------------------------------------------------------------
    auto mainWindow = std::make_unique<WinEase::MainWindow>(context.pluginManager(),
                                                            context.hotkeyManager());
    context.setMainWindow(mainWindow.get());

    auto trayManager = std::make_unique<WinEase::SystemTrayManager>(context.pluginManager(),
                                                                    mainWindow.get());

    // 插件启用状态按配置恢复（此时窗口已就绪，界面会同步刷新）
    context.restorePluginStates();

    // ------------------------------------------------------------------
    // 7. 信号连线
    // ------------------------------------------------------------------
    // 托盘 → 窗口
    QObject::connect(trayManager.get(), &WinEase::SystemTrayManager::hotkeySettingsRequested,
                     mainWindow.get(), &WinEase::MainWindow::showHotkeySettings);
    QObject::connect(trayManager.get(), &WinEase::SystemTrayManager::quitRequested,
                     &app, &QApplication::quit);

    // 窗口 → 托盘
    QObject::connect(mainWindow.get(), &WinEase::MainWindow::minimizeToTrayRequested,
                     trayManager.get(), [&trayManager] { trayManager->show(); });
    QObject::connect(mainWindow.get(), &WinEase::MainWindow::quitRequested,
                     &app, &QApplication::quit);

    // 宿主服务 → 托盘气泡通知
    QObject::connect(&context, &WinEase::AppContext::notificationRequested,
                     trayManager.get(), [&trayManager](const QString &title, const QString &body) {
                         trayManager->showMessage(title, body);
                     });

    // 宿主服务 → 托盘状态徽标（插件没有自己的托盘图标，见 PluginServices::setTrayBadge）
    QObject::connect(&context, &WinEase::AppContext::trayBadgeRequested,
                     trayManager.get(),
                     [&trayManager](const QString &pluginId, const QString &text,
                                    const QString &colorHex, const QString &tooltip) {
                         trayManager->setPluginBadge(pluginId, text, colorHex, tooltip);
                     });

    // 全局快捷键（默认 Ctrl+Alt+W）→ 显示/隐藏主窗口
    QObject::connect(&context, &WinEase::AppContext::activationRequested,
                     trayManager.get(), &WinEase::SystemTrayManager::toggleMainWindow);

    // 二次启动 → 激活已有实例的窗口
    QObject::connect(&instanceGuard, &WinEase::SingleInstanceGuard::activationRequested,
                     mainWindow.get(), [&trayManager](const QString &) {
                         trayManager->showMainWindow();
                     });

    QObject::connect(&app, &QApplication::aboutToQuit, &context, [&context] { context.shutdown(); });

    // ------------------------------------------------------------------
    // 8. 显示
    // ------------------------------------------------------------------
    if (!trayManager->show()) {
        qCWarning(WinEase::Logging::lcTray) << "系统托盘不可用，程序将只保留主窗口";
    }

    if (loadedCount == 0) {
        qCInfo(WinEase::Logging::lcApp) << "未加载到任何插件，可参考 plugins/template 编写功能插件";
    }

    mainWindow->show();

    // 未提权但存在需要管理员权限的插件时给出明确提示（而不是静默失败）
    if (context.pluginManager() && context.pluginManager()->hasElevationBlockedPlugins()) {
        const QString names = context.pluginManager()->elevationBlockedPluginNames().join(QStringLiteral("、"));
        qCWarning(WinEase::Logging::lcApp) << "以下功能需要管理员权限:" << names;
        QMessageBox::information(mainWindow.get(),
                                 QStringLiteral("权限提示"),
                                 QStringLiteral("以下功能需要管理员权限，当前以普通权限运行：\n\n%1\n\n"
                                                "可通过菜单「设置 → 以管理员身份重启」获得完整能力。")
                                     .arg(names));
    }

    return app.exec();
}
