#pragma once

// ============================================================================
//  ContextMenuPlugin —— P1-05 右键菜单扩展
//
//  做法：往 **HKCU** 写静态 verb（`Software\Classes\{*,Directory,Directory\Background}\
//        shell\WinEase.<名字>\command`）。只写 HKCU ⇒ **全程不需要管理员权限**。
//
//  为什么不写 COM Shell Extension：
//      那需要注册 CLSID、通常还要写 HKLM、并且代码跑在 explorer.exe 进程里 ——
//      一旦自己的代码崩溃，用户看到的是**资源管理器崩了**。静态 verb 只写注册表，
//      最坏情况就是"菜单项点了没反应"，风险完全不对等。
//      （代价：Windows 11 会把这类项折叠进「显示更多选项」，这是系统行为，
//        界面上要向用户说清楚，避免被当成 bug）
//
//  ★ 本插件唯一的硬指标是"**卸载即完整清理，无残留**"：
//      安装时把写过的每个键路径记进配置；卸载时先按键路径删除，
//      再逐个 root 扫一遍 `WinEase.*` 前缀的子键兜底（防止上次异常退出留下的残渣）。
//
//  命令行的引号问题（踩过的坑）：explorer 传 `%1` 时，**只有路径含空格才会加引号**，
//      所以"命令行里自己写引号"会导致含空格的路径被复制成带引号的字符串。
//      正确写法是用 cmd 的 `for %I in (%1) do @echo %~I`（`%~I` 会去掉引号），
//      两种路径都能得到干净结果 —— 这是实测出来的，写在下面的命令模板里。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QStringList>

class ContextMenuPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "context_menu_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit ContextMenuPlugin(QObject *parent = nullptr);

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
    /// 一条菜单项（一个 root 一条记录）：root 决定它在哪种右键里出现
    struct Entry {
        QString root;    ///< "*" / "Directory" / "Directory\\Background"
        QString key;     ///< 子键名，统一 WinEase.<名字>
        QString title;   ///< 菜单文字
        QString command; ///< 命令行（含 %1 / %V）
        /// 该 root 下的 HKCU 子键路径（也是卸载时的删除目标）
        QString subKeyPath() const;
    };

    /// 按当前配置生成要安装的菜单项
    QList<Entry> buildEntries() const;
    /// 安装（返回值：实际写入条数；errors 收集失败原因）
    int installEntries(QStringList *errors);
    /// 清理：先删记录里的键，再扫一遍各 root 下的 WinEase.* 兜底
    int removeEntries();
    void persistInstalled();
    void loadInstalled();

    bool m_copyPath = true;
    bool m_hashFile = true;
    bool m_openTerminal = true;
    bool m_installedFlag = false; ///< 当前是否已安装（由配置恢复）
    QStringList m_installedKeys;  ///< 已写入的 HKCU 子键路径（完整清理的依据）
};
