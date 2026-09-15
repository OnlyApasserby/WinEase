#pragma once

// ============================================================================
//  SystemTrayManager.h —— 系统托盘管理器
//
//  职责：
//    1. 常驻托盘图标 + 气泡通知
//    2. 单击/双击托盘图标 → 显示或隐藏主窗口（最小化状态会被还原）
//    3. 右键菜单按分类列出全部功能，支持直接启用/停用
//
//  右键菜单结构：
//      显示 / 隐藏主界面
//      ─────────────
//      窗口管理 ▸  [x] 功能A
//                  [ ] 功能B
//      文件增强 ▸  …
//      ─────────────
//      全部启用 / 全部停用
//      全局快捷键…
//      ─────────────
//      退出 WinEase
// ============================================================================

#include <QMap>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>

#include <memory>

class QMenu;
class QWidget;

namespace WinEase {

class PluginManager;

class SystemTrayManager : public QObject
{
    Q_OBJECT

public:
    /// @param pluginManager 用于构建功能菜单（可为 nullptr）
    /// @param mainWindow    主窗口；提供后本类直接负责其显示/隐藏
    SystemTrayManager(PluginManager *pluginManager,
                      QWidget *mainWindow = nullptr,
                      QObject *parent = nullptr);
    ~SystemTrayManager() override;

    SystemTrayManager(const SystemTrayManager &) = delete;
    SystemTrayManager &operator=(const SystemTrayManager &) = delete;

    /// 使托盘图标可见（系统不支持托盘时返回 false）
    bool show();
    void hide();
    bool isVisible() const;
    bool isSystemTrayAvailable() const;

    /// 弹出托盘气泡通知
    void showMessage(const QString &title, const QString &body, int milliseconds = 4000);

    /// 替换主窗口（例如主窗口晚于托盘创建时）
    void setMainWindow(QWidget *mainWindow);
    QWidget *mainWindow() const;

    /// 主窗口当前是否处于"可见且非最小化"状态
    bool isMainWindowActive() const;

    // ---------------- 插件状态徽标 ----------------
    /// 挂/更新某个插件的状态徽标（见 PluginServices::setTrayBadge）。
    /// text 为空等同 clearPluginBadge()。多个插件同时挂徽标时按 pluginId 顺序纵向排列。
    void setPluginBadge(const QString &pluginId,
                        const QString &text,
                        const QString &colorHex,
                        const QString &tooltip);
    /// 摘掉某个插件的徽标（插件停用/卸载/崩溃隔离时由本类自动调用）
    void clearPluginBadge(const QString &pluginId);
    /// 当前挂了几个徽标（供自检/日志观察）
    int pluginBadgeCount() const { return m_badges.size(); }
    /// 某个插件当前的徽标文字（没挂则为空）
    QString pluginBadgeText(const QString &pluginId) const;

public Q_SLOTS:
    /// 单击托盘：显示 ↔ 隐藏
    void toggleMainWindow();
    void showMainWindow();
    void hideMainWindow();
    /// 按当前插件列表重建右键菜单
    void rebuildMenu();

Q_SIGNALS:
    /// 主窗口被显示（含从最小化还原）
    void mainWindowShown();
    /// 主窗口被隐藏到托盘
    void mainWindowHidden();
    /// 托盘图标被单击（供日志/统计观察，显隐动作已由本类完成）
    void toggleRequested();
    /// 请求打开全局快捷键设置
    void hotkeySettingsRequested();
    /// 请求退出程序
    void quitRequested();

private:
    /// 一个插件挂上来的状态徽标
    struct Badge {
        QString text;
        QString colorHex;
        QString tooltip;
    };

    void setupTray();
    void buildPluginSection(QMenu *parentMenu);
    /// 按当前徽标集合重新合成托盘图标与悬停提示
    void updateTrayIcon();

    PluginManager *m_pluginManager = nullptr;
    QWidget *m_mainWindow = nullptr;
    std::unique_ptr<QSystemTrayIcon> m_trayIcon;
    std::unique_ptr<QMenu> m_menu;
    /// 用 QMap 而不是 QHash：多个徽标同时存在时排列顺序必须是稳定的（否则图标会闪）
    QMap<QString, Badge> m_badges;
};

} // namespace WinEase
