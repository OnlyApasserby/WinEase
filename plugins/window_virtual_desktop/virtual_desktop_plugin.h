#pragma once

// ============================================================================
//  VirtualDesktopPlugin —— P3-01 虚拟桌面（window.virtual_desktop）
//
//  ★ 定位（2026-09-14 收窄）：**多桌面窗口一览** —— 只**显示**，不碰任何窗口。
//    解决的问题：**"我那个窗口跑到哪个桌面去了？"**
//
//  ---------------------------------------------------------------------------
//  为什么不做"搬移 / 切桌面"（这条是有实测证据的，不是没做）：
//
//   ROADMAP 原文的「把窗口搬到另一个虚拟桌面」用公开 API **做不到**：
//   `MoveWindowToDesktop` 只允许搬**本进程自己**的窗口 —— 本工程用零副作用探针实测：
//   自己的窗口 `S_OK`、探针子进程的窗口 `E_ACCESSDENIED`、第三方程序的窗口 `E_ACCESSDENIED`；
//   这条限制**官方文档一个字都没写**（细节见 `docs/traps.md` #66 与 ROADMAP 裁剪记录 C10）。
//   唯一替代路子是系统快捷键 `Win+Ctrl+Shift+←/→`（把**当前活动窗口**移到相邻桌面），
//   但它要**抢用户的前台焦点**、依赖键位没被改过、没有相邻桌面时还什么都不发生 ——
//   代价与收益不匹配，**已按用户要求砍掉**。
//
//  ---------------------------------------------------------------------------
//  三条能力边界（界面上也写着，别让用户误会）：
//   1. **没有"枚举虚拟桌面"的公开接口** → 靠枚举窗口反查 GUID，所以**只看得到
//      "有窗口的"桌面**（刚建好、还没放过窗口的桌面不会出现），删掉的桌面下一轮自动消失；
//   2. **拿不到桌面的名字与编号**（系统里那个"桌面 1 / 2"是外壳自己排的）→
//      用"是不是当前桌面 + 有几个窗口 + 窗口标题"帮用户认；
//   3. 列表**只读**：这里不会激活窗口、不会切桌面、不会搬走任何东西。
// ============================================================================

#include "sdk/IFeaturePlugin.h"
#include "win32/VirtualDesktop.h"

#include <QList>
#include <QPointer>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class QWidget;

class VirtualDesktopPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "virtual_desktop_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit VirtualDesktopPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    /// 帮助页「关于插件」里的完整说明（能力边界 / 为什么只读）—— 卡片上放不下这些
    QString detailedDescription() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    /// 本插件是**只读一览**，不需要快捷键（原来那条是"搬窗口"的，功能已砍）
    bool supportsHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;

private:
    /// 只读探测桌面构成（不切换、不移动、不激活任何窗口）
    void refreshDesktops();
    /// 界面刷新
    void refreshDesktopList();
    void refreshWindowList();
    void refreshDetail();
    void updateActionState();
    void refreshPanel();
    QString statusText() const;
    /// 当前选中的桌面（没选 / 已被删掉时返回 nullptr）
    const WinEase::Win32::VirtualDesktopInfo *selectedDesktop() const;

    QList<WinEase::Win32::VirtualDesktopInfo> m_desktops;
    QString m_lastEvent;   ///< 探测失败等一次性事件（优先显示）
    QTimer *m_refreshTimer = nullptr;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_detailLabel;
    QPointer<QListWidget> m_desktopList;
    QPointer<QListWidget> m_windowList;
    QPointer<QPushButton> m_refreshButton;
};
