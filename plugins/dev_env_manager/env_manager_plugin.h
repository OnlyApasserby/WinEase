#pragma once

// ============================================================================
//  env_manager_plugin.h —— P2-14 环境变量管理（dev.env_manager）
//
//  两条写通道，能力与风险完全不同：
//      * **用户变量**（`HKCU\Environment`）—— 免提权，直接写注册表 + 广播
//        `WM_SETTINGCHANGE("Environment")`；
//      * **系统变量**（`HKLM\...\Session Manager\Environment`）—— 必须经提权助手
//        （`setEnvVar`，助手内部会广播）。缺助手时**如实报错**，不假装成功。
//
//  ---------------------------------------------------------------------------
//  这个功能的"难"不在写，而在**让用户看清 PATH**：
//      * **空条目不隐藏**：`;;` 与首尾分号会生成空条目 —— 它在 PATH 里等价于
//        "当前目录"，是真实的安全隐患，也是常见的手滑；
//      * **重复项要能定位**：归一化（去引号/去尾部反斜杠/大小写不敏感）后比较，
//        并指出"跟第几条重复"，而不是只说"有重复"；
//      * **展开预览**：`%VAR%` 展开后到底是什么、那条路径还在不在，
//        是用户决定"这条能不能删"的唯一依据。未定义的变量**原样保留**，
//        让用户一眼看出是哪个变量没定义（静默变空串会造出"看起来正常实则残废"的路径）。
//
//  ⚠ 一条刻意的克制：**「去重」只改编辑区、不自动落盘**，PATH 的保存必须由用户
//     再点一次「保存 PATH」。PATH 是"能让一堆程序突然找不到"的东西，
//     任何自动写入都是在赌用户的手速。
// ============================================================================

#include "EnvTools.h"
#include "sdk/IFeaturePlugin.h"

#include <QMap>
#include <QPointer>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QWidget;

namespace WinEase {
class ElevationService;
}

class EnvManagerPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "env_manager_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit EnvManagerPlugin(QObject *parent = nullptr);

    // ---------------- 元信息 ----------------
    QString id() const override;
    QString name() const override;
    QString description() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    // ---------------- 生命周期 ----------------
    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;

private:
    WinEase::Common::EnvScope currentScope() const;

    /// 从注册表读回当前作用域的全部变量（原始值，不展开）
    void reloadFromRegistry();
    /// 把当前作用域的 PATH 拆进 PATH 编辑区
    void loadPathEditor();
    /// PATH 编辑区 → 值（并刷新预览）
    void syncPathEntriesFromList();
    QString currentPathValue() const;

    /// 新增/修改变量（用户级直写，系统级走助手）
    void saveVariable();
    /// 删除变量（走同一条写通道）
    void deleteVariable();
    /// 保存 PATH（与 saveVariable 共用写通道 —— 只是变量名固定为 PATH）
    void savePathValue();

    /// 唯一的写入口
    bool writeVariable(const QString &name, const QString &value, bool remove, QString *errorOut);

    void addPathEntry();
    void removeSelectedPathEntry();
    void moveSelectedPathEntry(int delta);
    void dedupePathEntries();

    WinEase::ElevationService *elevation() const;
    bool elevationAvailable() const;

    void refreshPanel();
    void refreshTable();
    void refreshPathList();
    void updateExpandPreview();
    QString statusText() const;
    QString pathSummary() const;

    QMap<QString, QString> m_variables;
    QList<WinEase::Common::PathEntry> m_pathEntries; ///< 编辑中的 PATH 条目
    QString m_lastEvent;
    QString m_lastError;

    QPointer<QWidget> m_panel;
    QPointer<QComboBox> m_scopeCombo;
    QPointer<QLabel> m_scopeLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_expandLabel;
    QPointer<QLabel> m_pathSummaryLabel;
    QPointer<QTableWidget> m_table;
    QPointer<QLineEdit> m_nameEdit;
    QPointer<QLineEdit> m_valueEdit;
    QPointer<QPushButton> m_saveButton;
    QPointer<QPushButton> m_deleteButton;
    QPointer<QListWidget> m_pathList;
    QPointer<QLineEdit> m_pathAddEdit;
    QPointer<QPushButton> m_pathAddButton;
    QPointer<QPushButton> m_pathRemoveButton;
    QPointer<QPushButton> m_pathUpButton;
    QPointer<QPushButton> m_pathDownButton;
    QPointer<QPushButton> m_pathDedupeButton;
    QPointer<QPushButton> m_pathSaveButton;
};
