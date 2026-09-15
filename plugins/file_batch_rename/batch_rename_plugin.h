#pragma once

// ============================================================================
//  BatchRenamePlugin —— P2-01 批量重命名
//
//  行为：
//    * `default`（Ctrl+Alt+N）：打开重命名面板（独立工具窗口，可一直开着对照资源管理器）
//    * `undo`（Ctrl+Shift+Alt+N）：撤销**最近一次**批量重命名
//    * 面板：选目录 → 设规则 → **实时预览**（原名 / 新名 / 状态）→ 应用
//
//  ★ 本功能的纪律（批处理里最容易出人命的一条）：
//    ① 规则计算全部走 `plugins/common/RenameEngine`（纯函数）——
//       "预览看到的"与"落盘执行的"是同一份计算结果，不可能不一致；
//    ② 有冲突（目标重名 / 撞上目录里别的文件）→ **直接拒绝执行**，不猜用户想怎样；
//    ③ 每一项执行结果都记成"原→新"映射，**一键撤销**（并且写进配置，
//       重启 WinEase 之后仍然能撤销上一次）；
//    ④ 只处理**当前目录**下的文件（不递归、不动子目录），范围小到用户能一眼看完。
//
//  ⚠ 有意不做的事：
//    * 不处理子目录（"递归改名"一旦规则写错，害的是整棵树）
//    * 不做"仅大小写变化"之外的元数据改写；扩展名规则只改名字，不改文件内容
// ============================================================================

#include "RenameEngine.h"
#include "sdk/IFeaturePlugin.h"

#include <QPointer>
#include <QVector>

class BatchRenamePanel;

class BatchRenamePlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "batch_rename_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit BatchRenamePlugin(QObject *parent = nullptr);

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
    /// 打开（或前置）工具面板
    void openPanel();
    /// 面板当前要用的规则（含面板里改过的临时值）
    WinEase::FeaturePlugins::RenameEngine::Options panelOptions() const;
    QString panelDirectory() const;
    QString panelFilter() const;
    void savePanelState(const WinEase::FeaturePlugins::RenameEngine::Options &options,
                        const QString &directory,
                        const QString &filter);
    /// 记一次可撤销的操作（目录 + 原→新映射）；只保留最近 kMaxUndo 次
    void pushUndo(const QString &directory,
                  const QVector<QPair<QString, QString>> &mapping,
                  int fileCount);
    int undoCount() const { return m_undoStack.size(); }
    /// 最近一次撤销的说明文本（"8 个文件（2026-09-14 10:20）"）
    QString lastUndoDescription() const;
    /// 撤销最近一次；失败时 messageOut 给出中文原因
    bool undoLast(QString *messageOut);
    /// 执行前是否弹二次确认（`[Plugins/file.batch_rename] confirm`，默认 true）
    bool confirmBeforeApply() const { return m_confirm; }

protected:
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    struct UndoRecord
    {
        QString directory;
        QVector<QPair<QString, QString>> mapping; ///< old → new（执行顺序）
        QString timestamp;
        int fileCount = 0;
    };

    void loadFromConfig();
    void loadUndoStack();
    void saveUndoStack() const;
    void closePanel();
    static constexpr int kMaxUndo = 20;

    WinEase::FeaturePlugins::RenameEngine::Options m_options;
    QString m_directory;
    QString m_filter = QStringLiteral("*");
    bool m_confirm = true;

    QVector<UndoRecord> m_undoStack;
    QPointer<BatchRenamePanel> m_panel;
};
