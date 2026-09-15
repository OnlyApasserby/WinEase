#pragma once

// ============================================================================
//  ThemeSwitchPlugin —— P1-13 系统主题切换
//
//  改的是 **Windows 自己的**主题（其它应用、任务栏、开始菜单），
//  不是 WinEase 界面 —— WinEase 界面按设计**固定深色**、不随系统切换
//  （见 UI 主题那一节），这一点在设置面板里必须说清楚，否则用户会以为没生效。
//
//  注册表（都是 HKCU，免提权）：
//      HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
//          AppsUseLightTheme    (DWORD) 1 = 应用用浅色
//          SystemUsesLightTheme (DWORD) 1 = 系统（任务栏等）用浅色
//  改完必须 `broadcastSettingChange("ImmersiveColorSet")`，
//  否则已运行的程序要重启才看得到变化（验收项里明确要求"无需重启"）。
//
//  ★ 完整还原的落点在"**停用功能**"：
//      启用时先存下原值 → 停用时写回（回到"你启用本功能之前"的样子）。
//      **退出 WinEase 不还原**：主题是用户看得见的外观，不该因为退出而跳变；
//      每次启用都重新取快照，所以"这次启用前是什么样"才是还原目标。
//      （"无残留"针对的是"指向已不存在程序的引用"——见 P1-05 的右键菜单项；
//        主题只是一个合法的系统取值，不属于残留。）
//
//  ⚠ 写入前必须先读取，而不是"猜当前值"：用户可能处在 Win11 的
//    「自定义模式」（应用浅色 + 系统深色），只写一个值会把它改成第三种状态。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QVariant>

class ThemeSwitchPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "theme_switch_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit ThemeSwitchPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 能力标记 ----------------
    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 两个注册表值的快照（用于完整还原）
    struct Snapshot {
        QVariant apps;
        QVariant system;
        bool valid = false;
    };

    static QString personalizePath();
    Snapshot readSnapshot() const;
    /// 当前状态的人话描述，如"深色（应用深色 / 系统深色）"
    QString describeCurrent() const;
    /// 切换模式：dark / light / custom / toggle
    bool applyMode(const QString &modeKey);
    /// 把快照写回注册表
    bool restoreSnapshot();
    /// 只写一个值（内部统一走这里，保证广播不漏）
    bool writeValues(int appsLight, int systemLight, QString *errorOut);

    Snapshot m_original;      ///< 启用时的原值
    bool m_hasOriginal = false;
    bool m_changed = false;   ///< 是否被本插件改过（决定停用时要不要还原）
};
