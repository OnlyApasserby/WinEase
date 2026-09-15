// ============================================================================
//  batch_move_plugin.cpp —— 批量移动文件（正则匹配）
//
//  界面是"一个表单 + 一块计划表 + 三个按钮"：
//      [源目录][目标目录]   正则 [可选项]        → 「生成计划」「预演」「执行」
//      ┌ 计划表（逐项：状态 / 源 → 目标）      ┐
//      └ 汇总 + 撤销上次移动                   ┘
// ============================================================================

#include "batch_move_plugin.h"

#include "sdk/PluginServices.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace WinEase::FeaturePlugins {

namespace {

using BatchMove::ConflictPolicy;
using BatchMove::ItemStatus;
using BatchMove::Plan;
using BatchMove::PlannedItem;

// ---------------- 配置键 ----------------
const QString kKeySource = QStringLiteral("sourceDirectory");
const QString kKeyTarget = QStringLiteral("targetDirectory");
const QString kKeyPattern = QStringLiteral("pattern");
const QString kKeyTemplate = QStringLiteral("nameTemplate");
const QString kKeyCase = QStringLiteral("caseSensitive");
const QString kKeyRecursive = QStringLiteral("recursive");
const QString kKeyKeepStructure = QStringLiteral("keepStructure");
const QString kKeyFullPath = QStringLiteral("matchFullPath");
const QString kKeyPolicy = QStringLiteral("conflictPolicy");

/// 撤销日志文件名（放配置文件目录，跟 config.ini 做邻居）
const QString kUndoFileName = QStringLiteral("batch_move_undo.log");

/// 计划表最多显示多少行（多了界面卡；真实条数在汇总里说）
constexpr int kMaxPlanRows = 500;

QString policyKey(ConflictPolicy policy)
{
    switch (policy) {
    case ConflictPolicy::Skip:
        return QStringLiteral("skip");
    case ConflictPolicy::Rename:
        return QStringLiteral("rename");
    case ConflictPolicy::Overwrite:
        return QStringLiteral("overwrite");
    }
    return QStringLiteral("skip");
}

ConflictPolicy policyFromKey(const QString &key)
{
    if (key == QLatin1String("rename")) {
        return ConflictPolicy::Rename;
    }
    if (key == QLatin1String("overwrite")) {
        return ConflictPolicy::Overwrite;
    }
    return ConflictPolicy::Skip;
}

/// 一行计划文本：状态图标 + 源 → 目标
QString planLine(const PlannedItem &item)
{
    const QString mark = (item.status == ItemStatus::Ready) ? QStringLiteral("✔")
                                                            : QStringLiteral("·");
    if (item.status == ItemStatus::Ready) {
        return QStringLiteral("%1 %2 → %3").arg(mark, item.fileName, item.targetPath);
    }
    if (item.status == ItemStatus::NotMatched) {
        return QStringLiteral("%1 %2（%3）").arg(mark, item.fileName, item.note);
    }
    return QStringLiteral("%1 %2（%3）").arg(mark, item.fileName, item.note);
}

QString defaultSourceDirectory()
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return QDir::toNativeSeparators(desktop.isEmpty() ? QDir::homePath() : desktop);
}

} // namespace

// ============================================================================
//  构造 / 元信息
// ============================================================================

BatchMovePlugin::BatchMovePlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

BatchMovePlugin::~BatchMovePlugin() = default;

QString BatchMovePlugin::id() const
{
    return QStringLiteral("file.batch_move");
}

QString BatchMovePlugin::name() const
{
    return QStringLiteral("批量移动文件");
}

QString BatchMovePlugin::description() const
{
    return QStringLiteral("用正则挑文件，先出计划再落盘，可一键撤销");
}

QString BatchMovePlugin::detailedDescription() const
{
    return QStringLiteral(
        "按**正则表达式**从源目录里挑文件，整批移动到目标目录。\n"
        "\n"
        "它解决的是这类场景（比逐个另存 / 拖拽省事得多）：\n"
        "· 把下载目录里所有 2024 年的 PDF 挪到归档目录：正则 ^2024.*\\.pdf$\n"
        "· 把所有截图挪走并改名：正则 ^Screenshot，改名模板 {name}{ext}\n"
        "· 保持子目录结构批量搬：勾选「递归子目录」+「保留目录结构」\n"
        "\n"
        "纪律（本功能的重点不在能不能搬，而在不会搬错）：\n"
        "· 先出**计划表**：逐项写明会搬到哪、为什么不动，此时一个字节都没动\n"
        "· 「预演」按钮：只报告会移动多少条，不动盘\n"
        "· 执行结果写进撤销日志，随时「撤销上次移动」搬回原处\n"
        "· 正则非法、改名模板算出非法名、目标路径越出目标目录（.. 穿越）\n"
        "  一律**拦下并说明**，绝不静默跳过\n"
        "· 目标重名默认**跳过**而不是覆盖，要覆盖得手动改策略");
}

QIcon BatchMovePlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory BatchMovePlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList BatchMovePlugin::tags() const
{
    return {QStringLiteral("文件"), QStringLiteral("批量"), QStringLiteral("移动"),
            QStringLiteral("正则"), QStringLiteral("整理"), QStringLiteral("归档"),
            QStringLiteral("batch"), QStringLiteral("move"), QStringLiteral("regex")};
}

bool BatchMovePlugin::supportsHotkey() const
{
    return true;
}

QKeySequence BatchMovePlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+M"));
}

// ============================================================================
//  生命周期
// ============================================================================

bool BatchMovePlugin::initialize()
{
    loadConfig();
    return true;
}

void BatchMovePlugin::shutdown()
{
    saveConfig();
}

bool BatchMovePlugin::canEnable(QString *reason) const
{
    Q_UNUSED(reason);
    // 纯本地文件操作：不需要管理员、不依赖任何外部组件，永远可以启用
    return true;
}

bool BatchMovePlugin::onEnable()
{
    Q_EMIT statusMessage(QStringLiteral("在设置面板里选目录与正则，先看计划再执行"));
    return true;
}

void BatchMovePlugin::onDisable()
{
    // 停用没有任何"系统状态"要还原：本插件只在用户点按钮时动文件
}

void BatchMovePlugin::onHotkey(const QString &hotkeyId)
{
    if (hotkeyId.endsWith(QLatin1String("::open"))) {
        Q_EMIT statusMessage(QStringLiteral("批量移动：请在设置面板里操作"));
    }
}

// ============================================================================
//  配置
// ============================================================================

QString BatchMovePlugin::undoLogPath() const
{
    // 撤销日志放在 %APPDATA%/WinEase 下（与 config.ini 做邻居），
    // 这样重启 WinEase 之后"撤销上次移动"依然可用 —— 撤销凭据不该随进程消失。
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    QDir().mkpath(dir);
    return QDir(dir).absoluteFilePath(kUndoFileName);
}

void BatchMovePlugin::loadConfig()
{
    // 配置在 createSettingsWidget() 里回填（那里才有控件）；
    // 这里只确保撤销日志目录存在，让"撤销上次移动"在用户没打开过设置页时也能用。
    Q_UNUSED(undoLogPath());
}

void BatchMovePlugin::saveConfig() const
{
    // 实际的写入发生在界面回调里（saveConfig 只做一次同步）
    if (WinEase::PluginServices *svc = services()) {
        svc->syncConfig();
    }
}

void BatchMovePlugin::appendUndoLog(const QList<BatchMove::MoveRecord> &records)
{
    if (records.isEmpty()) {
        return;
    }
    const QString path = undoLogPath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("撤销日志写不进去：%1").arg(path));
        return;
    }
    file.write(BatchMove::serializeMoveRecords(records).toUtf8());
    file.close();
}

// ============================================================================
//  计划 / 预演 / 执行 / 撤销
// ============================================================================

BatchMove::Options BatchMovePlugin::collectOptions() const
{
    BatchMove::Options options;
    if (m_sourceEdit != nullptr) {
        options.sourceDirectory = m_sourceEdit->text().trimmed();
    }
    if (m_targetEdit != nullptr) {
        options.targetDirectory = m_targetEdit->text().trimmed();
    }
    if (m_patternEdit != nullptr) {
        options.pattern = m_patternEdit->text();
    }
    if (m_templateEdit != nullptr) {
        options.nameTemplate = m_templateEdit->text();
    }
    options.caseSensitive = (m_caseCheck != nullptr) && m_caseCheck->isChecked();
    options.recursive = (m_recursiveCheck != nullptr) && m_recursiveCheck->isChecked();
    options.keepStructure =
        (m_keepStructureCheck != nullptr) && m_keepStructureCheck->isChecked();
    options.matchFullPath = (m_fullPathCheck != nullptr) && m_fullPathCheck->isChecked();
    if (m_policyCombo != nullptr) {
        options.policy = policyFromKey(m_policyCombo->currentData().toString());
    }
    return options;
}

Plan BatchMovePlugin::refreshPlan()
{
    const BatchMove::Options options = collectOptions();
    const QStringList files =
        BatchMove::listFiles(options.sourceDirectory, options.recursive);
    m_lastPlan = BatchMove::buildPlan(files, options);

    if (m_planView != nullptr) {
        m_planView->clear();
        if (!m_lastPlan.valid) {
            m_planView->setPlainText(
                QStringLiteral("计划无法生成：%1").arg(m_lastPlan.error));
        } else {
            QStringList lines;
            int shown = 0;
            for (const PlannedItem &item : m_lastPlan.items) {
                if (shown >= kMaxPlanRows) {
                    lines.append(QStringLiteral("…（还有 %1 项未列出）")
                                     .arg(m_lastPlan.items.size() - shown));
                    break;
                }
                lines.append(planLine(item));
                ++shown;
            }
            if (lines.isEmpty()) {
                lines.append(QStringLiteral("源目录里一个文件都没有。"));
            }
            for (const QString &warning : m_lastPlan.warnings) {
                lines.append(QStringLiteral("提示：%1").arg(warning));
            }
            m_planView->setPlainText(lines.join(QLatin1Char('\n')));
        }
    }

    if (m_summaryLabel != nullptr) {
        m_summaryLabel->setText(
            m_lastPlan.valid
                ? QStringLiteral("扫描 %1 个文件：待移动 %2，跳过 %3")
                      .arg(m_lastPlan.items.size())
                      .arg(m_lastPlan.readyCount())
                      .arg(m_lastPlan.skippedCount())
                : QStringLiteral("计划不可用：%1").arg(m_lastPlan.error));
    }

    const bool canAct = m_lastPlan.valid && m_lastPlan.readyCount() > 0;
    if (m_applyButton != nullptr) {
        m_applyButton->setEnabled(canAct);
    }
    if (m_dryRunButton != nullptr) {
        m_dryRunButton->setEnabled(m_lastPlan.valid);
    }

    Q_EMIT statusMessage(m_summaryLabel != nullptr ? m_summaryLabel->text() : QString());
    return m_lastPlan;
}

void BatchMovePlugin::runDryRun()
{
    const Plan plan = refreshPlan();
    if (!plan.valid) {
        appendLogLine(QStringLiteral("预演中止：%1").arg(plan.error));
        return;
    }
    const BatchMove::ApplyResult result = BatchMove::applyPlan(plan, true);
    appendLogLine(QStringLiteral("【预演】%1，未改动任何文件").arg(result.summary()));
}

void BatchMovePlugin::runApply()
{
    const Plan plan = refreshPlan();
    if (!plan.valid) {
        appendLogLine(QStringLiteral("执行中止：%1").arg(plan.error));
        return;
    }
    const BatchMove::ApplyResult result = BatchMove::applyPlan(plan, false);
    appendUndoLog(result.records);

    appendLogLine(QStringLiteral("【执行】%1").arg(result.summary()));
    for (const QString &error : result.errors) {
        appendLogLine(QStringLiteral("  × %1").arg(error));
    }
    if (result.moved > 0) {
        Q_EMIT notificationRequested(
            QStringLiteral("批量移动完成"),
            QStringLiteral("%1（撤销日志已写入，可一键搬回）").arg(result.summary()));
    }
    Q_EMIT statusMessage(result.summary());
}

void BatchMovePlugin::runUndo()
{
    const QString path = undoLogPath();
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendLogLine(QStringLiteral("没有可撤销的记录（撤销日志不存在：%1）").arg(path));
        return;
    }
    const QList<BatchMove::MoveRecord> records =
        BatchMove::parseMoveRecords(QString::fromUtf8(file.readAll()));
    file.close();
    if (records.isEmpty()) {
        appendLogLine(QStringLiteral("撤销日志里没有有效记录。"));
        return;
    }

    const BatchMove::ApplyResult result = BatchMove::undoMoves(records);
    appendLogLine(QStringLiteral("【撤销】%1").arg(result.summary()));
    for (const QString &error : result.errors) {
        appendLogLine(QStringLiteral("  × %1").arg(error));
    }

    // 撤销成功后清空日志：避免"再点一次撤销"把已经搬回的文件又搬走
    if (result.failed == 0) {
        QFile::remove(path);
    }
    Q_EMIT statusMessage(result.summary());
}

void BatchMovePlugin::appendLogLine(const QString &line)
{
    if (m_planView == nullptr) {
        return;
    }
    m_planView->appendPlainText(line);
}

// ============================================================================
//  设置面板
// ============================================================================

QWidget *BatchMovePlugin::createSettingsWidget(QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // ---------------- 路径与规则 ----------------
    auto *formGroup = new QGroupBox(QStringLiteral("规则"), page);
    auto *form = new QFormLayout(formGroup);
    form->setContentsMargins(12, 12, 12, 12);
    form->setSpacing(8);

    // ⚠ title 必须**按值**捕获进 connect 的 lambda：它只是本 lambda 的形参，
    //   按引用捕获会留下悬垂引用（点击"浏览"时才用，那时形参早没了）。
    const auto makePathRow = [this, formGroup](QLineEdit *edit, const QString &title) {
        auto *row = new QWidget(formGroup);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);
        rowLayout->addWidget(edit, 1);
        auto *browse = new QPushButton(QStringLiteral("浏览…"), row);
        rowLayout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [edit, title] {
            const QString chosen =
                QFileDialog::getExistingDirectory(nullptr, title, edit->text().trimmed());
            if (!chosen.isEmpty()) {
                edit->setText(QDir::toNativeSeparators(chosen));
            }
        });
        return row;
    };

    m_sourceEdit = new QLineEdit(page);
    m_sourceEdit->setObjectName(QStringLiteral("batchMoveSourceEdit"));
    m_sourceEdit->setText(defaultSourceDirectory());
    m_sourceEdit->setToolTip(QStringLiteral("要扫描的目录。正则只在这个目录（及其子目录）里挑文件。"));
    form->addRow(QStringLiteral("源目录"), makePathRow(m_sourceEdit, QStringLiteral("选择源目录")));

    m_targetEdit = new QLineEdit(page);
    m_targetEdit->setObjectName(QStringLiteral("batchMoveTargetEdit"));
    m_targetEdit->setToolTip(QStringLiteral("文件会被移动到这个目录。不存在时会自动创建。"));
    form->addRow(QStringLiteral("目标目录"), makePathRow(m_targetEdit, QStringLiteral("选择目标目录")));

    m_patternEdit = new QLineEdit(page);
    m_patternEdit->setObjectName(QStringLiteral("batchMovePatternEdit"));
    m_patternEdit->setPlaceholderText(QStringLiteral("例如 ^2024.*\\.pdf$"));
    m_patternEdit->setToolTip(
        QStringLiteral("Qt/PCRE2 正则。默认只匹配**文件名**（可切到匹配完整路径）；\n"
                       "反斜杠要写成 \\\\（例如 \\.pdf$）。"));
    form->addRow(QStringLiteral("正则表达式"), m_patternEdit);

    m_templateEdit = new QLineEdit(page);
    m_templateEdit->setObjectName(QStringLiteral("batchMoveTemplateEdit"));
    m_templateEdit->setPlaceholderText(QStringLiteral("留空 = 保持原文件名"));
    m_templateEdit->setToolTip(
        QStringLiteral("可选改名模板：{name} 原名（不含扩展名）、{ext} 扩展名（含点）、{n} 序号。\n"
                       "例：{name}{ext}、归档-{n}{ext}"));
    form->addRow(QStringLiteral("改名模板"), m_templateEdit);

    layout->addWidget(formGroup);

    // ---------------- 选项 ----------------
    auto *optionGroup = new QGroupBox(QStringLiteral("选项"), page);
    auto *optionLayout = new QGridLayout(optionGroup);
    optionLayout->setContentsMargins(12, 12, 12, 12);
    optionLayout->setSpacing(8);

    m_caseCheck = new QCheckBox(QStringLiteral("大小写敏感"), optionGroup);
    m_caseCheck->setObjectName(QStringLiteral("batchMoveCaseCheck"));
    m_caseCheck->setToolTip(QStringLiteral("默认不敏感：`.pdf` 也能匹配 `.PDF`。"));
    optionLayout->addWidget(m_caseCheck, 0, 0);

    m_recursiveCheck = new QCheckBox(QStringLiteral("递归子目录"), optionGroup);
    m_recursiveCheck->setObjectName(QStringLiteral("batchMoveRecursiveCheck"));
    optionLayout->addWidget(m_recursiveCheck, 0, 1);

    m_keepStructureCheck = new QCheckBox(QStringLiteral("保留目录结构"), optionGroup);
    m_keepStructureCheck->setObjectName(QStringLiteral("batchMoveStructureCheck"));
    m_keepStructureCheck->setToolTip(
        QStringLiteral("递归时把子目录也一并建出来（搬过去的相对位置与源目录一致）。"));
    optionLayout->addWidget(m_keepStructureCheck, 0, 2);

    m_fullPathCheck = new QCheckBox(QStringLiteral("匹配完整路径"), optionGroup);
    m_fullPathCheck->setObjectName(QStringLiteral("batchMoveFullPathCheck"));
    m_fullPathCheck->setToolTip(QStringLiteral("勾上后正则对着完整路径匹配（可以按子目录名筛选）。"));
    optionLayout->addWidget(m_fullPathCheck, 1, 0);

    auto *policyLabel = new QLabel(QStringLiteral("重名时"), optionGroup);
    optionLayout->addWidget(policyLabel, 1, 1);
    m_policyCombo = new QComboBox(optionGroup);
    m_policyCombo->setObjectName(QStringLiteral("batchMovePolicyCombo"));
    m_policyCombo->addItem(QStringLiteral("跳过（最安全）"), QStringLiteral("skip"));
    m_policyCombo->addItem(QStringLiteral("自动改名"), QStringLiteral("rename"));
    m_policyCombo->addItem(QStringLiteral("覆盖（危险）"), QStringLiteral("overwrite"));
    m_policyCombo->setToolTip(
        QStringLiteral("目标目录里已有同名文件时怎么办。默认**跳过**：宁可不动，也不要静默覆盖。"));
    optionLayout->addWidget(m_policyCombo, 1, 2);

    layout->addWidget(optionGroup);

    // ---------------- 按钮 ----------------
    auto *buttonRow = new QWidget(page);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);

    auto *planButton = new QPushButton(QStringLiteral("生成计划"), buttonRow);
    planButton->setObjectName(QStringLiteral("batchMovePlanButton"));
    m_dryRunButton = new QPushButton(QStringLiteral("预演（不动文件）"), buttonRow);
    m_dryRunButton->setObjectName(QStringLiteral("batchMoveDryRunButton"));
    m_applyButton = new QPushButton(QStringLiteral("执行移动"), buttonRow);
    m_applyButton->setObjectName(QStringLiteral("batchMoveApplyButton"));
    m_undoButton = new QPushButton(QStringLiteral("撤销上次移动"), buttonRow);
    m_undoButton->setObjectName(QStringLiteral("batchMoveUndoButton"));

    buttonLayout->addWidget(planButton);
    buttonLayout->addWidget(m_dryRunButton);
    buttonLayout->addWidget(m_applyButton);
    buttonLayout->addWidget(m_undoButton);
    buttonLayout->addStretch(1);
    layout->addWidget(buttonRow);

    // ---------------- 计划表 ----------------
    m_planView = new QPlainTextEdit(page);
    m_planView->setObjectName(QStringLiteral("batchMovePlanView"));
    m_planView->setReadOnly(true);
    m_planView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_planView->setPlaceholderText(QStringLiteral("点「生成计划」后，这里会逐项列出会搬到哪、"
                                                  "以及为什么某些文件不动"));
    layout->addWidget(m_planView, 1);

    m_summaryLabel = new QLabel(QStringLiteral("尚未生成计划"), page);
    m_summaryLabel->setObjectName(QStringLiteral("batchMoveSummaryLabel"));
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summaryLabel);

    // ---------------- 信号 ----------------
    const auto persist = [this](const QString &key, const QVariant &value) {
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), key, value);
        }
    };

    connect(planButton, &QPushButton::clicked, this, [this] { refreshPlan(); });
    connect(m_dryRunButton, &QPushButton::clicked, this, &BatchMovePlugin::runDryRun);
    connect(m_applyButton, &QPushButton::clicked, this, &BatchMovePlugin::runApply);
    connect(m_undoButton, &QPushButton::clicked, this, &BatchMovePlugin::runUndo);

    connect(m_sourceEdit, &QLineEdit::textChanged, this,
            [persist](const QString &text) { persist(kKeySource, text); });
    connect(m_targetEdit, &QLineEdit::textChanged, this,
            [persist](const QString &text) { persist(kKeyTarget, text); });
    connect(m_patternEdit, &QLineEdit::textChanged, this,
            [persist](const QString &text) { persist(kKeyPattern, text); });
    connect(m_templateEdit, &QLineEdit::textChanged, this,
            [persist](const QString &text) { persist(kKeyTemplate, text); });
    connect(m_caseCheck, &QCheckBox::toggled, this,
            [persist](bool checked) { persist(kKeyCase, checked); });
    connect(m_recursiveCheck, &QCheckBox::toggled, this,
            [persist](bool checked) { persist(kKeyRecursive, checked); });
    connect(m_keepStructureCheck, &QCheckBox::toggled, this,
            [persist](bool checked) { persist(kKeyKeepStructure, checked); });
    connect(m_fullPathCheck, &QCheckBox::toggled, this,
            [persist](bool checked) { persist(kKeyFullPath, checked); });
    connect(m_policyCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, persist](int) { persist(kKeyPolicy, m_policyCombo->currentData().toString()); });

    // 回填配置（用 QSignalBlocker 包住，否则"改控件 → 信号 → 又改控件"会打转，踩坑 #28）
    if (WinEase::PluginServices *svc = services()) {
        const QSignalBlocker b1(m_sourceEdit);
        const QSignalBlocker b2(m_targetEdit);
        const QSignalBlocker b3(m_patternEdit);
        const QSignalBlocker b4(m_templateEdit);
        const QSignalBlocker b5(m_caseCheck);
        const QSignalBlocker b6(m_recursiveCheck);
        const QSignalBlocker b7(m_keepStructureCheck);
        const QSignalBlocker b8(m_fullPathCheck);
        const QSignalBlocker b9(m_policyCombo);

        const QString source = svc->configValue(id(), kKeySource).toString();
        if (!source.isEmpty()) {
            m_sourceEdit->setText(source);
        }
        m_targetEdit->setText(svc->configValue(id(), kKeyTarget).toString());
        m_patternEdit->setText(svc->configValue(id(), kKeyPattern).toString());
        m_templateEdit->setText(svc->configValue(id(), kKeyTemplate).toString());
        m_caseCheck->setChecked(svc->configValue(id(), kKeyCase, false).toBool());
        m_recursiveCheck->setChecked(svc->configValue(id(), kKeyRecursive, false).toBool());
        m_keepStructureCheck->setChecked(
            svc->configValue(id(), kKeyKeepStructure, false).toBool());
        m_fullPathCheck->setChecked(svc->configValue(id(), kKeyFullPath, false).toBool());
        const QString policy = svc->configValue(id(), kKeyPolicy).toString();
        if (!policy.isEmpty()) {
            const int index = m_policyCombo->findData(policy);
            if (index >= 0) {
                m_policyCombo->setCurrentIndex(index);
            }
        }
    }

    return page;
}

} // namespace WinEase::FeaturePlugins
