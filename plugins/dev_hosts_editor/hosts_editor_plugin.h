#pragma once

// ============================================================================
//  hosts_editor_plugin.h —— P2-13 Hosts 快速编辑（dev.hosts_editor）
//
//  hosts 是"用户手写的系统配置文件"，它决定了哪些域名解析到哪个 IP ——
//  写错一行的后果是"某个网站再也上不去"，而且用户往往要过很久才意识到是这里的问题。
//  所以这个插件的重心全在**安全边界**上：
//
//   1. **写盘只走提权助手**（`writeHosts` + `contentBase64` 字节级通道）：
//      助手在写之前会**自动备份**原文件，并把备份路径回给我们；
//   2. **保存前必须校验，校验不过就绝不发请求**（路线图验收原文：
//      "非法语法拒绝保存并指出行号"）—— "拒绝"是真的不写，不是写失败再报错；
//   3. **往返无损**：解析时保留每行原文，用户只是注释掉一行时，
//      保存后不该把他的缩进、制表符、行尾风格、BOM 全改掉
//      （"编辑器一保存就整文件 diff"是这类工具最大的信任杀手）；
//   4. **恢复到默认内容不自动落盘**：把内容填进编辑区让用户看一眼再保存 ——
//      一键抹掉用户攒了两年的 hosts 是灾难级误操作；
//      而"从历史备份还原"是明确的一键（用户点的就是"还原那一次"）。
//
//  ⚠ 读文件不需要提权（普通权限可读 `C:\Windows\System32\drivers\etc\hosts`），
//     所以"打开面板就能看到当前内容"不依赖助手；只有**保存/还原**才需要。
// ============================================================================

#include "HostsDocument.h"
#include "sdk/IFeaturePlugin.h"

#include <QPointer>
#include <QStringList>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QWidget;

namespace WinEase {
class ElevationService;
}

class HostsEditorPlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "hosts_editor_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit HostsEditorPlugin(QObject *parent = nullptr);

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
    /// 从磁盘读回 hosts（普通权限即可）并填进编辑区
    void reloadFromDisk();
    /// 校验 → 交给助手写入（校验不过直接拒绝，连请求都不发）
    void saveToHosts();
    /// 把"默认内容"填进编辑区（**不**自动保存）
    void fillDefaultContent();
    /// 对编辑器里选中的行（或光标所在行）切换注释
    void toggleCommentOnSelection();
    /// 通过助手列出历史备份
    void refreshBackupList();
    /// 一键从选中的备份还原（读出来 → 直接写回）
    void restoreSelectedBackup();

    WinEase::ElevationService *elevation() const;
    bool elevationAvailable() const;

    void refreshPanel();
    QString statusText() const;
    QString checkText() const;

    WinEase::Common::HostsDocument m_document;
    QString m_lastEvent;
    QStringList m_backupNames;
    QStringList m_backupStamps;

    QPointer<QWidget> m_panel;
    QPointer<QLabel> m_pathLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QLabel> m_checkLabel;
    QPointer<QPlainTextEdit> m_editor;
    QPointer<QPushButton> m_saveButton;
    QPointer<QPushButton> m_reloadButton;
    QPointer<QPushButton> m_defaultButton;
    QPointer<QPushButton> m_commentButton;
    QPointer<QComboBox> m_backupCombo;
    QPointer<QPushButton> m_restoreBackupButton;
};
