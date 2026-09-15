#pragma once

// ============================================================================
//  HelpDialog.h —— 帮助（**栏目式**对话框）
//
//  栏目：
//    · 关于 WinEase —— 应用版本、插件目录、配置目录
//    · 关于插件     —— **每个插件一段**：名称、标识、版本、作者、完整介绍
//
//  ★ 为什么要有「关于插件」这一栏（2026-09-14 按用户要求新增）：
//    插件卡片只有 336×150，塞不下"标识 / 版本号 / 完整说明"。现在的分工是：
//      · 卡片：图标 + 名称 + **两行**描述（完整描述进 Tooltip）+ 开关；
//      · 本对话框：标识、版本号、作者、以及插件自己写的**完整说明**
//        （`IFeaturePlugin::detailedDescription()`，能力边界/限制/为什么这么做的都在这）。
//
//  ⚠ 插件信息一律走 `PluginManager` 的**缓存读**接口（`idOf/nameOf/versionOf/...`）：
//    已崩溃被隔离的插件**不能再调用它的任何方法**（见 PluginManager.h 的崩溃隔离说明），
//    而这些 getter 读的是加载期快照，插件的"死"不影响界面。
// ============================================================================

#include <QDialog>

class QListWidget;
class QStackedWidget;

namespace WinEase {

class PluginManager;

class HelpDialog : public QDialog
{
    Q_OBJECT

public:
    /// 栏目（左右顺序即列表顺序）
    enum Topic {
        AboutApp = 0, ///< 关于 WinEase
        AboutPlugins, ///< 关于插件
    };

    explicit HelpDialog(PluginManager *manager, QWidget *parent = nullptr);

    /// 切到指定栏目（菜单里「关于插件」直接落到那一栏）
    void showTopic(Topic topic);

private:
    QWidget *createAboutPage();
    QWidget *createPluginsPage();

    PluginManager *m_pluginManager = nullptr;
    QListWidget *m_topicList = nullptr;
    QStackedWidget *m_pages = nullptr;
};

} // namespace WinEase
