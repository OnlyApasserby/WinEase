#pragma once

// ============================================================================
//  ShredderPlugin —— P2-12 敏感文件粉碎
//
//  ★ 这个功能是**本工具集里唯一不可撤销的破坏性操作**（批量重命名有撤销、
//    查重只动回收站），所以界面上必须把三件事说全、且不能藏在折叠面板里：
//      ① 粉碎能防什么、防不了什么（SSD 磨损均衡/TRIM、卷影副本、云同步副本）
//      ② 已粉碎的文件无法恢复，本工具**不提供撤销**
//      ③ 系统目录与盘根一律拒绝（引擎侧的安全阀 `isProtectedPath()`）
//
//  真正的实现（覆写 → 改名 → 永久删除 → 回读校验）在
//  `plugins/common/FileShredder.*`，自检与插件共用同一份源码。
// ============================================================================

#include "sdk/IFeaturePlugin.h"

#include <QPointer>
#include <QStringList>

class ShredderWindow;

class ShredderPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "shredder_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit ShredderPlugin(QObject *parent = nullptr);

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

    // ---------------- 面板与自检共用的接口 ----------------
    /// 执行前是否弹二次确认（`[Plugins/security.file_shredder] confirm`，默认 true）
    bool confirmBeforeShred() const { return m_confirm; }
    /// 配置里的默认覆写方式（键名，面板用它选初始项）
    QString modeKey() const { return m_modeKey; }
    /// 覆写后是否先改名再删除（`renameBeforeDelete`，默认 true）
    bool renameBeforeDelete() const { return m_renameBeforeDelete; }
    /// 每遍覆写后是否回读校验（`verifyEachPass`，默认 true）
    bool verifyEachPass() const { return m_verifyEachPass; }
    QWidget *panelWindow() const;
    /// 面板上报状态文本（面板类不做 Q_OBJECT，直接回调插件更省事）
    void reportStatus(const QString &message) { Q_EMIT statusMessage(message); }

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    void loadFromConfig();
    void openPanel();
    void closePanel();

    QPointer<ShredderWindow> m_window;
    bool m_confirm = true;
    QString m_modeKey;
    bool m_renameBeforeDelete = true;
    bool m_verifyEachPass = true;
};
