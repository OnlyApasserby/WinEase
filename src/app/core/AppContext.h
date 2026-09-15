#pragma once

// ============================================================================
//  AppContext.h —— 应用上下文 / 宿主服务实现
//
//  1. 持有主程序的核心组件（配置、插件管理、全局快捷键）
//  2. 实现 PluginServices，把宿主能力注入插件
//
//  这样插件只需要依赖 SDK 中的 PluginServices 抽象接口，完全不感知主程序实现，
//  从根本上避免"插件 DLL 与主程序各持一份单例"的问题。
// ============================================================================

#include "sdk/PluginServices.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>   // QPointer<QWidget> 需要完整类型

#include <memory>

namespace WinEase {

class ElevationClient;
class GlobalHotkeyManager;
class HookServiceImpl;
class OverlayHost;
class PluginManager;

class AppContext : public QObject, public PluginServices
{
    Q_OBJECT

public:
    explicit AppContext(QObject *parent = nullptr);
    ~AppContext() override;

    /// 初始化配置、日志与插件管理器（由 main() 调用）
    bool initialize();

    /// 扫描并加载插件
    int loadPlugins();

    /// 按配置恢复插件启用状态
    void restorePluginStates();

    /// 退出前收尾：保存配置、卸载插件、关闭日志
    void shutdown();

    // ---------------- 组件访问 ----------------
    PluginManager *pluginManager() const { return m_pluginManager.get(); }
    GlobalHotkeyManager *hotkeyManager() const { return m_hotkeyManager.get(); }
    HookServiceImpl *hookServiceImpl() const { return m_hookService.get(); }
    OverlayHost *overlayHostImpl() const { return m_overlayHost.get(); }
    ElevationClient *elevationClient() const { return m_elevation.get(); }

    void setMainWindow(QWidget *window) { m_mainWindow = window; }

    /// 主程序自身使用的快捷键 id 前缀
    static QString appHotkeyId(const QString &action);

    // ========================================================================
    //  PluginServices 实现
    // ========================================================================
    QVariant configValue(const QString &pluginId,
                         const QString &key,
                         const QVariant &defaultValue = QVariant()) const override;
    void setConfigValue(const QString &pluginId, const QString &key, const QVariant &value) override;
    void syncConfig() override;

    void log(const QString &pluginId, PluginLogLevel level, const QString &message) override;

    void notify(const QString &title, const QString &body) override;

    void setTrayBadge(const QString &pluginId,
                      const QString &text,
                      const QString &colorHex,
                      const QString &tooltip) override;

    bool registerHotkey(const QString &pluginId,
                        const QString &action,
                        const QKeySequence &sequence,
                        const QString &description) override;
    void unregisterHotkey(const QString &pluginId, const QString &action) override;

    HookService *hookService() const override;
    OverlayHost *overlayHost() const override;
    ElevationService *elevationService() const override;

    QString appVersion() const override;
    bool isElevated() const override;
    QString configFilePath() const override;
    QString logDirectory() const override;
    QWidget *mainWindow() const override;

Q_SIGNALS:
    /// 插件请求弹出系统通知（由托盘/主窗口消费）
    void notificationRequested(const QString &title, const QString &body);
    /// 插件请求挂/清托盘状态徽标（text 为空表示清除；由托盘消费）
    void trayBadgeRequested(const QString &pluginId,
                            const QString &text,
                            const QString &colorHex,
                            const QString &tooltip);
    /// 需要主窗口显示时发出（例如二次启动、托盘双击）
    void activationRequested();

private:
    std::unique_ptr<PluginManager> m_pluginManager;
    std::unique_ptr<GlobalHotkeyManager> m_hotkeyManager;
    std::unique_ptr<HookServiceImpl> m_hookService;
    std::unique_ptr<OverlayHost> m_overlayHost;
    std::unique_ptr<ElevationClient> m_elevation;
    QPointer<QWidget> m_mainWindow;
    bool m_initialized = false;
};

} // namespace WinEase
