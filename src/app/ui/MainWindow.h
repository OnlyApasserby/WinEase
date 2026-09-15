#pragma once

// ============================================================================
//  MainWindow.h —— 主界面
//
//  布局：
//      ┌──────────┬──────────────────────────────────────────────┐
//      │ 分类导航  │ 顶部：标题 + 搜索框 + 批量操作 / 快捷键设置    │
//      │ (QList)  ├──────────────────────────────────────────────┤
//      │          │ 功能卡片网格（自适应列数，实时搜索过滤）        │
//      │          ├──────────────────────────────────────────────┤
//      │          │ 状态栏：插件数量 / 运行中数量 / 权限状态        │
//      └──────────┴──────────────────────────────────────────────┘
// ============================================================================

#include "sdk/FeatureCategory.h"

#include <QHash>
#include <QMainWindow>
#include <QString>

#include <memory>

class QLabel;
class QLineEdit;
class QScrollArea;

namespace WinEase {

class CategoryNavWidget;
class FeatureCard;
class FeatureGridView;
class GlobalHotkeyManager;
class HelpDialog;
class HotkeySettingsDialog;
class IFeaturePlugin;
class PluginManager;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(PluginManager *pluginManager, GlobalHotkeyManager *hotkeyManager, QWidget *parent = nullptr);
    ~MainWindow() override;

    /// 显示、还原并激活窗口
    void showAndActivate();
    /// 当前是否有可见的设置对话框
    bool hasOpenDialog() const;

Q_SIGNALS:
    /// 用户关闭窗口时请求最小化到托盘
    void minimizeToTrayRequested();
    /// 用户明确要求退出程序
    void quitRequested();

public Q_SLOTS:
    void setSearchText(const QString &text);
    void onPluginStateChanged(WinEase::IFeaturePlugin *plugin, bool enabled);
    void onPluginLoaded(WinEase::IFeaturePlugin *plugin);
    void onCategoryChanged(WinEase::FeatureCategory category);
    void onSearchTextChanged(const QString &text);
    void setAllPluginsEnabled(bool enabled);
    void showHotkeySettings();
    /// 打开帮助对话框并落到指定栏目（取值见 `HelpDialog::Topic`：
    /// 0 = 关于 WinEase，1 = 关于插件）
    void showHelp(int topic = 0);
    /// 显示插件设置面板
    void showPluginSettings(WinEase::IFeaturePlugin *plugin);
    /// 刷新状态栏统计
    void refreshStatistics();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void setupUi();
    void setupToolBar();
    void setupMenuBar();
    void setupStatusBar();
    void connectSignals();

    void rebuildCards();
    void applyFilter();
    void updateHeader();
    void refreshCard(IFeaturePlugin *plugin);
    void refreshAllCards();
    void openPathInExplorer(const QString &path);

    PluginManager *m_pluginManager = nullptr;
    GlobalHotkeyManager *m_hotkeyManager = nullptr;

    CategoryNavWidget *m_nav = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_headerTitle = nullptr;
    QLabel *m_headerSubtitle = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    FeatureGridView *m_grid = nullptr;
    QLabel *m_emptyHint = nullptr;

    QLabel *m_statusPluginCount = nullptr;
    QLabel *m_statusRunningCount = nullptr;
    QLabel *m_statusPrivilege = nullptr;

    QHash<QString, FeatureCard *> m_cards;

    std::unique_ptr<HotkeySettingsDialog> m_hotkeyDialog;
    /// 帮助（栏目式：关于 WinEase / 关于插件）
    std::unique_ptr<HelpDialog> m_helpDialog;

    FeatureCategory m_currentCategory = FeatureCategory::Unknown;
    QString m_searchKeyword;
    bool m_quitRequested = false;
};

} // namespace WinEase
