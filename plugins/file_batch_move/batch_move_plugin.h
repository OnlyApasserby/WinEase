#pragma once

// ============================================================================
//  BatchMovePlugin —— 批量移动文件（正则匹配）
//
//  形态：**工具面板型插件**（同 P1-10 文本格式化 / P2-01 批量重命名），
//  没有悬浮层、不挂钩子、不写系统状态 —— 它只动用户自己挑的那个目录。
//
//  工作流（把"危险动作"拆成四步，任何一步都能反悔）：
//      ① 选源目录 / 目标目录 / 写正则
//      ② 生成**计划**（BatchMoveEngine::buildPlan，纯函数，只算不动盘）
//      ③ 表里逐项看到"会搬到哪、为什么不动"，可选"只预演"跑一遍
//      ④ 点"执行"，落盘记录写进撤销日志；不满意点"撤销上次"
//
//  ⚠ 正则、命名模板、目标路径都来自用户输入 → 引擎侧有目录穿越拦截 + 非法名校验；
//    插件侧不做"二次判断"，一切以 Plan 为准（自检也按这份 Plan 断言）。
// ============================================================================

#include "BatchMoveEngine.h"
#include "sdk/IFeaturePlugin.h"

#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace WinEase::FeaturePlugins {

class BatchMovePlugin : public WinEase::IFeaturePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WinEase_IFeaturePlugin_iid FILE "batch_move_plugin.json")
    Q_INTERFACES(WinEase::IFeaturePlugin)

public:
    explicit BatchMovePlugin(QObject *parent = nullptr);
    ~BatchMovePlugin() override;

    QString id() const override;
    QString name() const override;
    QString description() const override;
    QString detailedDescription() const override;
    QIcon icon() const override;
    WinEase::FeatureCategory category() const override;
    QStringList tags() const override;

    bool supportsHotkey() const override;
    QKeySequence defaultHotkey() const override;

    bool initialize() override;
    void shutdown() override;
    QWidget *createSettingsWidget(QWidget *parent = nullptr) override;

protected:
    bool canEnable(QString *reason) const override;
    bool onEnable() override;
    void onDisable() override;
    void onHotkey(const QString &hotkeyId) override;

private:
    /// 读控件 → 组 Options（纯数据）
    BatchMove::Options collectOptions() const;
    /// 生成计划并**把结果画到界面上**；返回计划本身供执行/自检用
    BatchMove::Plan refreshPlan();
    /// 预演（dryRun）：只报告会移动多少条，不动盘
    void runDryRun();
    /// 真落盘
    void runApply();
    /// 用撤销日志把上一次移动搬回去
    void runUndo();

    void loadConfig();
    void saveConfig() const;

    QString undoLogPath() const;
    void appendUndoLog(const QList<BatchMove::MoveRecord> &records);

    void appendLogLine(const QString &line);

private:
    // 界面控件（QPointer 不必：这些控件随宿主窗口销毁，插件只在自己存活期内用）
    QLineEdit *m_sourceEdit = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QLineEdit *m_patternEdit = nullptr;
    QLineEdit *m_templateEdit = nullptr;
    QCheckBox *m_caseCheck = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    QCheckBox *m_keepStructureCheck = nullptr;
    QCheckBox *m_fullPathCheck = nullptr;
    QComboBox *m_policyCombo = nullptr;
    QPlainTextEdit *m_planView = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_dryRunButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_undoButton = nullptr;

    BatchMove::Plan m_lastPlan;
};

} // namespace WinEase::FeaturePlugins
