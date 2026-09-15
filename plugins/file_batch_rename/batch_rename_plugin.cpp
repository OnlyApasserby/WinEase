#include "batch_rename_plugin.h"

#include "sdk/PluginServices.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace RenameEngine = WinEase::FeaturePlugins::RenameEngine;
using WinEase::FeaturePlugins::RenameEngine::CaseMode;
using WinEase::FeaturePlugins::RenameEngine::ExtensionMode;
using WinEase::FeaturePlugins::RenameEngine::Item;
using WinEase::FeaturePlugins::RenameEngine::Options;
using WinEase::FeaturePlugins::RenameEngine::Plan;

namespace {

constexpr int kMaxPreviewRows = 500;

/// 目录里的文件名（只取文件、不递归、按名称排序 —— 顺序稳定，预览才不会"跳"）
QStringList fileNamesIn(const QString &directory, const QString &filter)
{
    QDir dir(directory);
    if (!dir.exists()) {
        return {};
    }
    const QStringList filters = filter.trimmed().isEmpty()
        ? QStringList{ QStringLiteral("*") }
        : filter.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    return dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
}

/// 每项的时间戳（与 names 同序），供模板里的 {date}/{time}/{datetime} 使用。
///
/// 优先级：**EXIF 拍摄时间** → 文件修改时间。
/// 为什么值得读 EXIF：相机拍的照片拷来拷去，修改时间经常被改（下载、解压、同步），
/// 唯一稳定的是照片里的拍摄时间。QImageReader 只读文件头里的文本段，
/// 不把整张图解码进内存，所以对一批大图也只花几毫秒。
QVector<QDateTime> timestampsFor(const QString &directory, const QStringList &names)
{
    QVector<QDateTime> result;
    result.reserve(names.size());
    const QDir dir(directory);
    for (const QString &name : names) {
        const QString path = dir.filePath(name);
        QDateTime stamp = QFileInfo(path).lastModified();
        QImageReader reader(path);
        if (reader.canRead()) {
            QString exif = reader.text(QStringLiteral("DateTimeOriginal"));
            if (exif.isEmpty()) {
                exif = reader.text(QStringLiteral("DateTime"));
            }
            // EXIF 的时间格式是 "2026:09:14 10:20:30"（冒号分隔日期，和 ISO 不一样）
            const QDateTime parsed =
                QDateTime::fromString(exif, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
            if (parsed.isValid()) {
                stamp = parsed;
            }
        }
        result.append(stamp);
    }
    return result;
}

QJsonObject optionsToJson(const Options &options)
{
    QJsonObject json;
    json.insert(QStringLiteral("findPattern"), options.findPattern);
    json.insert(QStringLiteral("replaceText"), options.replaceText);
    json.insert(QStringLiteral("nameTemplate"), options.nameTemplate);
    json.insert(QStringLiteral("startNumber"), options.startNumber);
    json.insert(QStringLiteral("numberStep"), options.numberStep);
    json.insert(QStringLiteral("numberPadding"), options.numberPadding);
    json.insert(QStringLiteral("prefix"), options.prefix);
    json.insert(QStringLiteral("suffix"), options.suffix);
    json.insert(QStringLiteral("caseMode"), RenameEngine::caseModeKey(options.caseMode));
    json.insert(QStringLiteral("extensionMode"),
                RenameEngine::extensionModeKey(options.extensionMode));
    json.insert(QStringLiteral("newExtension"), options.newExtension);
    return json;
}

Options optionsFromJson(const QJsonObject &json)
{
    Options options;
    options.findPattern = json.value(QStringLiteral("findPattern")).toString();
    options.replaceText = json.value(QStringLiteral("replaceText")).toString();
    options.nameTemplate = json.value(QStringLiteral("nameTemplate")).toString();
    options.startNumber = json.value(QStringLiteral("startNumber")).toInt(1);
    options.numberStep = json.value(QStringLiteral("numberStep")).toInt(1);
    options.numberPadding = json.value(QStringLiteral("numberPadding")).toInt(3);
    options.prefix = json.value(QStringLiteral("prefix")).toString();
    options.suffix = json.value(QStringLiteral("suffix")).toString();
    options.caseMode =
        RenameEngine::caseModeFromKey(json.value(QStringLiteral("caseMode")).toString());
    options.extensionMode = RenameEngine::extensionModeFromKey(
        json.value(QStringLiteral("extensionMode")).toString());
    options.newExtension = json.value(QStringLiteral("newExtension")).toString();
    return options;
}

} // namespace

// ============================================================================
//  面板：选目录 → 设规则 → 实时预览 → 应用（预览与执行是同一份引擎结果）
// ============================================================================

class BatchRenamePanel : public QWidget
{
public:
    BatchRenamePanel(BatchRenamePlugin *plugin, QWidget *parent)
        : QWidget(parent)
        , m_plugin(plugin)
    {
        setObjectName(QStringLiteral("winease_batch_rename_panel")); // 自检按名字找得到
        setWindowTitle(QStringLiteral("批量重命名 - WinEase"));
        setWindowFlag(Qt::Window, true);
        setAttribute(Qt::WA_DeleteOnClose, true);
        setMinimumSize(900, 600);
        buildUi();
        loadFromPlugin();
        m_refreshTimer.setSingleShot(true);
        m_refreshTimer.setInterval(200);
        QObject::connect(&m_refreshTimer, &QTimer::timeout, this, [this] { refreshPreview(); });
        refreshPreview();
        updateUndoButton(); // refreshPreview() 在"目录为空/没有匹配文件"时会早退，撤销按钮得单独定一次状态
    }

    void scheduleRefresh() { m_refreshTimer.start(); }

    void refreshPreview()
    {
        m_table->setRowCount(0);
        m_conflictLabel->clear();
        m_plan = Plan{};
        m_planDirectory.clear();

        const QString directory = m_directoryEdit->text().trimmed();
        if (directory.isEmpty()) {
            setStatus(QStringLiteral("请先选择要处理的目录"), true);
            return;
        }
        if (!QFileInfo::exists(directory)) {
            setStatus(QStringLiteral("目录不存在：%1").arg(directory), true);
            return;
        }
        const QStringList names = fileNamesIn(directory, m_filterEdit->text());
        if (names.isEmpty()) {
            setStatus(QStringLiteral("该目录下没有匹配「%1」的文件").arg(m_filterEdit->text()), true);
            return;
        }

        const Options options = currentOptions();
        QString error;
        const Plan plan = RenameEngine::plan(names, fileNamesIn(directory, QStringLiteral("*")),
                                            timestampsFor(directory, names), options, &error);
        if (!error.isEmpty()) {
            setStatus(error, true);
            return;
        }
        m_plan = plan;
        m_planDirectory = directory;

        const int rows = qMin(m_plan.items.size(), kMaxPreviewRows);
        m_table->setRowCount(rows);
        const QColor conflictColor(QStringLiteral("#ff8a80"));
        const QColor problemColor(QStringLiteral("#9e9e9e"));
        const QColor unchangedColor(QStringLiteral("#bdbdbd"));
        for (int row = 0; row < rows; ++row) {
            const Item &item = m_plan.items.at(row);
            QString state = QStringLiteral("改名");
            if (!item.valid()) {
                state = item.problem;
            } else if (!item.changed()) {
                state = QStringLiteral("不变");
            } else if (item.caseOnlyChange()) {
                state = QStringLiteral("仅大小写");
            }
            const bool conflict = conflictsTarget(item.newName);
            auto *original = new QTableWidgetItem(item.originalName);
            auto *renamed = new QTableWidgetItem(item.newName);
            auto *stateItem = new QTableWidgetItem(state);
            if (conflict) {
                original->setForeground(conflictColor);
                renamed->setForeground(conflictColor);
                stateItem->setForeground(conflictColor);
            } else if (!item.valid()) {
                original->setForeground(problemColor);
                renamed->setForeground(problemColor);
                stateItem->setForeground(problemColor);
            } else if (!item.changed()) {
                renamed->setForeground(unchangedColor);
                stateItem->setForeground(unchangedColor);
            }
            m_table->setItem(row, 0, original);
            m_table->setItem(row, 1, renamed);
            m_table->setItem(row, 2, stateItem);
        }

        QString status = m_plan.summaryText();
        if (m_plan.items.size() > rows) {
            status += QStringLiteral("（表格只显示前 %1 项，执行时按全部 %2 项处理）")
                          .arg(rows)
                          .arg(m_plan.items.size());
        }
        setStatus(status, !m_plan.ok());
        if (!m_plan.conflicts.isEmpty()) {
            m_conflictLabel->setText(QStringLiteral("⚠ %1")
                                         .arg(m_plan.conflicts.mid(0, 3).join(QStringLiteral("；"))));
            m_conflictLabel->setStyleSheet(QStringLiteral("color:#ff8a80;"));
        }
        m_applyButton->setEnabled(m_plan.ok() && m_plan.changedCount() > 0);
        updateUndoButton();
    }

    void applyRename()
    {
        if (m_planDirectory != m_directoryEdit->text().trimmed()) {
            refreshPreview(); // 目录刚被改过还没刷新
        }
        if (!m_plan.ok()) {
            setStatus(QStringLiteral("计划里有 %1 处冲突，已拒绝执行").arg(m_plan.conflicts.size()),
                      true);
            return;
        }
        if (m_plan.changedCount() == 0) {
            setStatus(QStringLiteral("没有需要改名的文件"), true);
            return;
        }

        // 二次确认：默认开（这是"批量改别人文件名"的最后一道闸）。
        // 自检把 [Plugins/file.batch_rename] confirm 设为 false —— 否则会卡在模态对话框上。
        if (m_plugin->confirmBeforeApply()) {
            const QMessageBox::StandardButton answer = QMessageBox::question(
                this, QStringLiteral("确认重命名"),
                QStringLiteral("将重命名 %1 个文件，跳过 %2 项。\n改完可以点「撤销上一次」还原，是否继续？")
                    .arg(m_plan.changedCount())
                    .arg(m_plan.problemCount()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }

        setCursor(Qt::WaitCursor);
        const RenameEngine::ApplyResult result = RenameEngine::apply(m_planDirectory, m_plan);
        unsetCursor();

        // ⚠ 执行结果留到最后再写状态栏：refreshPreview() 会把状态栏改写成"新的计划概要"
        //   （那是"如果现在再点一次会怎样"），先 setStatus 再 refresh 的话，
        //   用户点完「应用」当场看不到刚刚到底干了什么
        QString resultText;
        bool resultWarning = false;
        if (result.ok) {
            m_plugin->pushUndo(m_planDirectory, result.applied, result.applied.size());
            resultText = QStringLiteral("%1（目录：%2）").arg(result.summaryText(), m_planDirectory);
            if (WinEase::PluginServices *svc = m_plugin->services()) {
                svc->notify(QStringLiteral("批量重命名"),
                            QStringLiteral("已重命名 %1 个文件").arg(result.applied.size()));
            }
        } else {
            // 部分失败：先如实记下"确实改成功的"，再报错（撤销只撤已完成的）
            if (!result.applied.isEmpty()) {
                m_plugin->pushUndo(m_planDirectory, result.applied, result.applied.size());
            }
            resultText = QStringLiteral("%1；失败项：%2")
                             .arg(result.error, result.failed.mid(0, 3).join(QStringLiteral("；")));
            resultWarning = true;
        }
        updateUndoButton();
        refreshPreview();
        setStatus(resultText, resultWarning);
    }

    void undoLast()
    {
        QString message;
        const bool ok = m_plugin->undoLast(&message);
        updateUndoButton();
        refreshPreview();
        setStatus(message, !ok); // 同上：撤销结果不能被"新计划概要"顶掉
    }

private:
    void buildUi()
    {
        auto *layout = new QVBoxLayout(this);

        // ---------------- ① 处理范围 ----------------
        auto *targetBox = new QGroupBox(QStringLiteral("① 处理范围（只处理当前目录，不改子目录）"), this);
        auto *targetForm = new QFormLayout(targetBox);
        auto *directoryRow = new QWidget(targetBox);
        auto *directoryLayout = new QHBoxLayout(directoryRow);
        directoryLayout->setContentsMargins(0, 0, 0, 0);
        m_directoryEdit = new QLineEdit(directoryRow);
        m_directoryEdit->setPlaceholderText(QStringLiteral("例如 D:\\照片\\2026-09"));
        auto *browseButton = new QPushButton(QStringLiteral("浏览…"), directoryRow);
        directoryLayout->addWidget(m_directoryEdit, 1);
        directoryLayout->addWidget(browseButton);
        targetForm->addRow(QStringLiteral("目录"), directoryRow);
        m_filterEdit = new QLineEdit(QStringLiteral("*"), targetBox);
        m_filterEdit->setPlaceholderText(QStringLiteral("通配符，分号分隔多个，例如 *.jpg;*.png"));
        targetForm->addRow(QStringLiteral("只处理"), m_filterEdit);
        layout->addWidget(targetBox);

        // ---------------- ② 规则 ----------------
        auto *ruleBox = new QGroupBox(QStringLiteral("② 重命名规则"), this);
        auto *ruleGrid = new QGridLayout(ruleBox);
        int row = 0;
        const auto addFullRow = [&](const QString &label, QWidget *widget) {
            ruleGrid->addWidget(new QLabel(label, ruleBox), row, 0);
            ruleGrid->addWidget(widget, row, 1, 1, 3);
            ++row;
        };

        m_findEdit = new QLineEdit(ruleBox);
        m_findEdit->setPlaceholderText(QStringLiteral("留空 = 不做查找替换，例如 (\\d{4})-(\\d{2})"));
        addFullRow(QStringLiteral("查找（正则）"), m_findEdit);

        m_replaceEdit = new QLineEdit(ruleBox);
        m_replaceEdit->setPlaceholderText(QStringLiteral("可用 \\1 \\2 引用捕获组"));
        addFullRow(QStringLiteral("替换为"), m_replaceEdit);

        m_templateEdit = new QLineEdit(ruleBox);
        m_templateEdit->setPlaceholderText(
            QStringLiteral("留空 = 沿用原名；占位符 {name} {ext} {n} {1}..{9} {date} {datetime}"));
        addFullRow(QStringLiteral("命名模板"), m_templateEdit);

        auto *numberRow = new QWidget(ruleBox);
        auto *numberLayout = new QHBoxLayout(numberRow);
        numberLayout->setContentsMargins(0, 0, 0, 0);
        m_startSpin = new QSpinBox(numberRow);
        m_startSpin->setRange(-999999, 999999);
        m_stepSpin = new QSpinBox(numberRow);
        m_stepSpin->setRange(-999, 999);
        m_stepSpin->setValue(1);
        m_padSpin = new QSpinBox(numberRow);
        m_padSpin->setRange(0, 8);
        m_padSpin->setValue(3);
        numberLayout->addWidget(new QLabel(QStringLiteral("从"), numberRow));
        numberLayout->addWidget(m_startSpin);
        numberLayout->addWidget(new QLabel(QStringLiteral("每次"), numberRow));
        numberLayout->addWidget(m_stepSpin);
        numberLayout->addWidget(new QLabel(QStringLiteral("补零"), numberRow));
        numberLayout->addWidget(m_padSpin);
        numberLayout->addWidget(new QLabel(QStringLiteral("位（模板里用 {n}）"), numberRow));
        numberLayout->addStretch(1);
        addFullRow(QStringLiteral("序号"), numberRow);

        m_prefixEdit = new QLineEdit(ruleBox);
        m_suffixEdit = new QLineEdit(ruleBox);
        ruleGrid->addWidget(new QLabel(QStringLiteral("前缀"), ruleBox), row, 0);
        ruleGrid->addWidget(m_prefixEdit, row, 1);
        ruleGrid->addWidget(new QLabel(QStringLiteral("后缀"), ruleBox), row, 2);
        ruleGrid->addWidget(m_suffixEdit, row, 3);
        ++row;

        m_caseCombo = new QComboBox(ruleBox);
        for (const CaseMode mode : { CaseMode::Keep, CaseMode::Lower, CaseMode::Upper,
                                     CaseMode::Capitalize, CaseMode::Title }) {
            m_caseCombo->addItem(RenameEngine::caseModeDisplayName(mode),
                                 RenameEngine::caseModeKey(mode));
        }
        auto *extensionRow = new QWidget(ruleBox);
        auto *extensionLayout = new QHBoxLayout(extensionRow);
        extensionLayout->setContentsMargins(0, 0, 0, 0);
        m_extensionCombo = new QComboBox(extensionRow);
        for (const ExtensionMode mode : { ExtensionMode::Keep, ExtensionMode::LowerCase,
                                          ExtensionMode::UpperCase, ExtensionMode::Remove,
                                          ExtensionMode::Replace }) {
            m_extensionCombo->addItem(RenameEngine::extensionModeDisplayName(mode),
                                      RenameEngine::extensionModeKey(mode));
        }
        m_newExtensionEdit = new QLineEdit(extensionRow);
        m_newExtensionEdit->setPlaceholderText(QStringLiteral("新扩展名"));
        m_newExtensionEdit->setMaximumWidth(140);
        extensionLayout->addWidget(m_extensionCombo, 1);
        extensionLayout->addWidget(m_newExtensionEdit);
        ruleGrid->addWidget(new QLabel(QStringLiteral("大小写"), ruleBox), row, 0);
        ruleGrid->addWidget(m_caseCombo, row, 1);
        ruleGrid->addWidget(new QLabel(QStringLiteral("扩展名"), ruleBox), row, 2);
        ruleGrid->addWidget(extensionRow, row, 3);
        layout->addWidget(ruleBox);

        // ---------------- ③ 预览 ----------------
        auto *previewHeader = new QHBoxLayout();
        previewHeader->addWidget(new QLabel(QStringLiteral("③ 预览（改名前核对一遍）"), this));
        previewHeader->addStretch(1);
        auto *refreshButton = new QPushButton(QStringLiteral("刷新预览"), this);
        m_undoButton = new QPushButton(QStringLiteral("撤销上一次"), this);
        m_undoButton->setEnabled(false); // 还没有可撤的记录（refreshPreview 的早退分支不会碰它）
        m_applyButton = new QPushButton(QStringLiteral("应用重命名"), this);
        previewHeader->addWidget(refreshButton);
        previewHeader->addWidget(m_undoButton);
        previewHeader->addWidget(m_applyButton);
        layout->addLayout(previewHeader);

        m_table = new QTableWidget(this);
        m_table->setColumnCount(3);
        m_table->setHorizontalHeaderLabels({ QStringLiteral("原文件名"), QStringLiteral("新文件名"),
                                             QStringLiteral("状态") });
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_table->verticalHeader()->setVisible(false);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(m_table, 1);

        m_conflictLabel = new QLabel(this);
        m_conflictLabel->setWordWrap(true);
        layout->addWidget(m_conflictLabel);
        m_statusLabel = new QLabel(this);
        m_statusLabel->setWordWrap(true);
        layout->addWidget(m_statusLabel);

        // 控件对象名：端到端自检要隔着 DLL 边界把规则"填"进去、再点按钮，
        // 这是它唯一可靠的抓手（面板类没有头文件，测试也不该去猜控件顺序）
        const QList<QPair<QString, QWidget *>> namedWidgets = {
            { QStringLiteral("batchRenameDirectory"), m_directoryEdit },
            { QStringLiteral("batchRenameFilter"), m_filterEdit },
            { QStringLiteral("batchRenameFind"), m_findEdit },
            { QStringLiteral("batchRenameReplace"), m_replaceEdit },
            { QStringLiteral("batchRenameTemplate"), m_templateEdit },
            { QStringLiteral("batchRenamePrefix"), m_prefixEdit },
            { QStringLiteral("batchRenameSuffix"), m_suffixEdit },
            { QStringLiteral("batchRenameCase"), m_caseCombo },
            { QStringLiteral("batchRenameExtension"), m_extensionCombo },
            { QStringLiteral("batchRenameTable"), m_table },
            { QStringLiteral("batchRenameConflict"), m_conflictLabel },
            { QStringLiteral("batchRenameStatus"), m_statusLabel },
            { QStringLiteral("batchRenameApply"), m_applyButton },
            { QStringLiteral("batchRenameUndo"), m_undoButton },
        };
        for (const auto &entry : namedWidgets) {
            entry.second->setObjectName(entry.first);
        }

        // ---------------- 信号：任何规则变化都自动重算预览 ----------------
        QObject::connect(browseButton, &QPushButton::clicked, this, [this] {
            const QString chosen = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择要批量重命名的目录"),
                QDir::fromNativeSeparators(m_directoryEdit->text()));
            if (!chosen.isEmpty()) {
                m_directoryEdit->setText(QDir::toNativeSeparators(chosen));
                scheduleRefresh();
            }
        });
        QObject::connect(refreshButton, &QPushButton::clicked, this, [this] { refreshPreview(); });
        QObject::connect(m_applyButton, &QPushButton::clicked, this, [this] { applyRename(); });
        QObject::connect(m_undoButton, &QPushButton::clicked, this, [this] { undoLast(); });

        for (QLineEdit *edit : { m_directoryEdit, m_filterEdit, m_findEdit, m_replaceEdit,
                                 m_templateEdit, m_prefixEdit, m_suffixEdit, m_newExtensionEdit }) {
            QObject::connect(edit, &QLineEdit::textChanged, this, [this] { scheduleRefresh(); });
        }
        for (QSpinBox *spin : { m_startSpin, m_stepSpin, m_padSpin }) {
            QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                             [this] { scheduleRefresh(); });
        }
        QObject::connect(m_caseCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this] { scheduleRefresh(); });
        QObject::connect(m_extensionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this] {
                             syncExtensionEdit();
                             scheduleRefresh();
                         });
        syncExtensionEdit();
    }

    void loadFromPlugin()
    {
        const Options options = m_plugin->panelOptions();
        m_directoryEdit->setText(QDir::toNativeSeparators(m_plugin->panelDirectory()));
        m_filterEdit->setText(m_plugin->panelFilter());
        m_findEdit->setText(options.findPattern);
        m_replaceEdit->setText(options.replaceText);
        m_templateEdit->setText(options.nameTemplate);
        m_startSpin->setValue(options.startNumber);
        m_stepSpin->setValue(options.numberStep);
        m_padSpin->setValue(options.numberPadding);
        m_prefixEdit->setText(options.prefix);
        m_suffixEdit->setText(options.suffix);
        m_caseCombo->setCurrentIndex(m_caseCombo->findData(RenameEngine::caseModeKey(options.caseMode)));
        m_extensionCombo->setCurrentIndex(
            m_extensionCombo->findData(RenameEngine::extensionModeKey(options.extensionMode)));
        m_newExtensionEdit->setText(options.newExtension);
        syncExtensionEdit();
    }

    Options currentOptions() const
    {
        Options options;
        options.findPattern = m_findEdit->text();
        options.replaceText = m_replaceEdit->text();
        options.nameTemplate = m_templateEdit->text();
        options.startNumber = m_startSpin->value();
        options.numberStep = m_stepSpin->value();
        options.numberPadding = m_padSpin->value();
        options.prefix = m_prefixEdit->text();
        options.suffix = m_suffixEdit->text();
        options.caseMode = RenameEngine::caseModeFromKey(m_caseCombo->currentData().toString());
        options.extensionMode =
            RenameEngine::extensionModeFromKey(m_extensionCombo->currentData().toString());
        options.newExtension = m_newExtensionEdit->text();
        // 面板里改过的规则要留住：下次打开（甚至重启后）不用再设一遍
        m_plugin->savePanelState(options, m_directoryEdit->text().trimmed(),
                                 m_filterEdit->text());
        return options;
    }

    void syncExtensionEdit()
    {
        m_newExtensionEdit->setEnabled(
            RenameEngine::extensionModeFromKey(m_extensionCombo->currentData().toString())
            == ExtensionMode::Replace);
    }

    bool conflictsTarget(const QString &newName) const
    {
        for (const QString &conflict : m_plan.conflicts) {
            if (conflict.contains(newName)) {
                return true;
            }
        }
        return false;
    }

    void setStatus(const QString &text, bool warning)
    {
        m_statusLabel->setText(text);
        m_statusLabel->setStyleSheet(warning ? QStringLiteral("color:#ffd54f;") : QString());
    }

    void updateUndoButton()
    {
        const int count = m_plugin->undoCount();
        m_undoButton->setEnabled(count > 0);
        m_undoButton->setText(count > 0 ? QStringLiteral("撤销上一次（%1 次可撤）").arg(count)
                                        : QStringLiteral("撤销上一次"));
    }

    BatchRenamePlugin *m_plugin = nullptr;
    QLineEdit *m_directoryEdit = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QLineEdit *m_templateEdit = nullptr;
    QSpinBox *m_startSpin = nullptr;
    QSpinBox *m_stepSpin = nullptr;
    QSpinBox *m_padSpin = nullptr;
    QLineEdit *m_prefixEdit = nullptr;
    QLineEdit *m_suffixEdit = nullptr;
    QComboBox *m_caseCombo = nullptr;
    QComboBox *m_extensionCombo = nullptr;
    QLineEdit *m_newExtensionEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_conflictLabel = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_undoButton = nullptr;
    QTimer m_refreshTimer;
    Plan m_plan;
    QString m_planDirectory;
};

// ============================================================================
//  插件本体
// ============================================================================

namespace {

bool sameOptions(const Options &left, const Options &right)
{
    return left.findPattern == right.findPattern && left.replaceText == right.replaceText
        && left.nameTemplate == right.nameTemplate && left.startNumber == right.startNumber
        && left.numberStep == right.numberStep && left.numberPadding == right.numberPadding
        && left.prefix == right.prefix && left.suffix == right.suffix
        && left.caseMode == right.caseMode && left.extensionMode == right.extensionMode
        && left.newExtension == right.newExtension;
}

} // namespace

BatchRenamePlugin::BatchRenamePlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString BatchRenamePlugin::id() const
{
    return QStringLiteral("file.batch_rename");
}

QString BatchRenamePlugin::name() const
{
    return QStringLiteral("批量重命名");
}

QString BatchRenamePlugin::description() const
{
    return QStringLiteral("按规则批量改名：实时预览、有冲突直接拒绝、随时一键撤销");
}

QIcon BatchRenamePlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::FileEnhancement);
}

WinEase::FeatureCategory BatchRenamePlugin::category() const
{
    return WinEase::FeatureCategory::FileEnhancement;
}

QStringList BatchRenamePlugin::tags() const
{
    return { QStringLiteral("重命名"), QStringLiteral("改名"), QStringLiteral("批量"),
             QStringLiteral("rename"), QStringLiteral("序号"), QStringLiteral("正则"),
             QStringLiteral("撤销") };
}

bool BatchRenamePlugin::supportsHotkey() const
{
    return true;
}

QKeySequence BatchRenamePlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+N"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool BatchRenamePlugin::initialize()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法加载"));
        return false;
    }

    loadFromConfig();
    loadUndoStack();
    m_confirm = svc->configValue(id(), QStringLiteral("confirm"), true).toBool();
    svc->log(id(), WinEase::PluginLogLevel::Info,
             QStringLiteral("批量重命名已加载（可撤销记录 %1 条，执行前确认：%2）")
                 .arg(m_undoStack.size())
                 .arg(m_confirm ? QStringLiteral("开") : QStringLiteral("关")));
    return true;
}

void BatchRenamePlugin::shutdown()
{
    closePanel();
    saveUndoStack();
}

bool BatchRenamePlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    // 第二个动作：撤销最近一次（面板里也有按钮，但"改错了想马上撤"是键盘场景）
    if (!services()->registerHotkey(id(), QStringLiteral("undo"),
                                    QKeySequence(QStringLiteral("Ctrl+Shift+Alt+N")),
                                    QStringLiteral("WinEase：撤销最近一次批量重命名"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「撤销」快捷键注册失败（可能被占用）—— 面板里的按钮仍然可用"));
    }

    const QKeySequence panelKey = defaultHotkey().toString(QKeySequence::NativeText).isEmpty()
        ? QKeySequence(QStringLiteral("Ctrl+Alt+N"))
        : defaultHotkey();
    Q_EMIT statusMessage(m_undoStack.isEmpty()
                             ? QStringLiteral("就绪：按 %1 打开改名面板")
                                   .arg(panelKey.toString(QKeySequence::NativeText))
                             : QStringLiteral("就绪：按 %1 打开面板，最近一次（%2）可撤销")
                                   .arg(panelKey.toString(QKeySequence::NativeText),
                                        lastUndoDescription()));
    return true;
}

void BatchRenamePlugin::onDisable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("undo"));
    }
    closePanel();
    Q_EMIT statusMessage(QStringLiteral("已停用批量重命名"));
}

void BatchRenamePlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("undo")) {
        QString message;
        undoLast(&message);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), message);
        }
        return;
    }
    openPanel();
}

// ---------------------------------------------------------------------------
//  面板
// ---------------------------------------------------------------------------

void BatchRenamePlugin::openPanel()
{
    if (m_panel == nullptr) {
        // 独立工具窗口（不挂在主窗口下）：用户要一边看资源管理器一边核对预览，
        // 被主窗口裁剪/跟着主窗口最小化都不合适。生命周期靠 QPointer + WA_DeleteOnClose。
        m_panel = new BatchRenamePanel(this, nullptr);
    }
    m_panel->show();
    m_panel->raise();
    m_panel->activateWindow();
}

void BatchRenamePlugin::closePanel()
{
    if (m_panel != nullptr) {
        m_panel->close(); // WA_DeleteOnClose → 自己析构；QPointer 随后归零
        m_panel = nullptr;
    }
}

Options BatchRenamePlugin::panelOptions() const
{
    return m_options;
}

QString BatchRenamePlugin::panelDirectory() const
{
    return m_directory;
}

QString BatchRenamePlugin::panelFilter() const
{
    return m_filter;
}

void BatchRenamePlugin::savePanelState(const Options &options,
                                       const QString &directory,
                                       const QString &filter)
{
    // 面板每次按键都会回调进来：只有真的变了才写配置
    const bool changed = !sameOptions(options, m_options) || directory != m_directory
        || filter != m_filter;
    m_options = options;
    m_directory = directory;
    m_filter = filter;
    if (!changed) {
        return;
    }
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    svc->setConfigValue(
        id(), QStringLiteral("options"),
        QString::fromUtf8(QJsonDocument(optionsToJson(options)).toJson(QJsonDocument::Compact)));
    svc->setConfigValue(id(), QStringLiteral("directory"), directory);
    svc->setConfigValue(id(), QStringLiteral("filter"), filter);
}

void BatchRenamePlugin::loadFromConfig()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }

    m_directory = svc->configValue(id(), QStringLiteral("directory")).toString();
    if (m_directory.isEmpty()) {
        // 第一次用：默认落在"下载"目录（改名需求十有八九是从下载/截图目录开始的）
        m_directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    m_filter = svc->configValue(id(), QStringLiteral("filter"), QStringLiteral("*")).toString();

    const QString optionsJson = svc->configValue(id(), QStringLiteral("options")).toString();
    if (!optionsJson.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(optionsJson.toUtf8());
        if (document.isObject()) {
            m_options = optionsFromJson(document.object());
        }
    }
}

// ---------------------------------------------------------------------------
//  撤销栈（跨重启保留：改完名字关掉程序，第二天照样能撤）
// ---------------------------------------------------------------------------

void BatchRenamePlugin::pushUndo(const QString &directory,
                                 const QVector<QPair<QString, QString>> &mapping,
                                 int fileCount)
{
    if (mapping.isEmpty()) {
        return;
    }
    UndoRecord record;
    record.directory = directory;
    record.mapping = mapping;
    record.fileCount = fileCount;
    record.timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_undoStack.append(record);
    while (m_undoStack.size() > kMaxUndo) {
        m_undoStack.removeFirst();
    }
    saveUndoStack();
}

QString BatchRenamePlugin::lastUndoDescription() const
{
    if (m_undoStack.isEmpty()) {
        return QString();
    }
    const UndoRecord &record = m_undoStack.last();
    return QStringLiteral("%1 个文件（%2）").arg(record.fileCount).arg(record.timestamp);
}

bool BatchRenamePlugin::undoLast(QString *messageOut)
{
    const auto report = [&](const QString &text, bool ok) {
        if (messageOut != nullptr) {
            *messageOut = text;
        }
        Q_EMIT statusMessage(text);
        return ok;
    };

    if (m_undoStack.isEmpty()) {
        return report(QStringLiteral("没有可撤销的批量重命名记录"), false);
    }

    const UndoRecord record = m_undoStack.last();
    const QDir dir(record.directory);
    QStringList failures;
    int restored = 0;

    // ★ 必须**逆序**还原：正向执行时 A→B、B→C 的连锁改名，
    //   反向要先把 C 变回 B，再把 B 变回 A；顺过来的话第二步会撞上第一步的结果。
    for (int i = record.mapping.size() - 1; i >= 0; --i) {
        const QString &oldName = record.mapping.at(i).first;
        const QString &newName = record.mapping.at(i).second;
        const QString newPath = dir.filePath(newName);
        const QString oldPath = dir.filePath(oldName);

        if (!QFileInfo::exists(newPath)) {
            failures.append(QStringLiteral("%1（改名后的文件已不在）").arg(newName));
            continue;
        }
        if (QFileInfo::exists(oldPath)) {
            failures.append(QStringLiteral("%1（原文件名已被占用）").arg(oldName));
            continue;
        }
        if (!QFile::rename(newPath, oldPath)) {
            failures.append(QStringLiteral("%1 → %2（系统拒绝）").arg(newName, oldName));
            continue;
        }
        ++restored;
    }

    if (restored == 0) {
        return report(QStringLiteral("撤销失败：%1").arg(failures.mid(0, 3).join(QStringLiteral("；"))),
                      false);
    }

    // 部分失败时把这条记录保留下来，让用户能再点一次（撤销是幂等的：已还原的项会被"已不在"跳过）
    if (failures.isEmpty()) {
        m_undoStack.removeLast();
        saveUndoStack();
        return report(QStringLiteral("已撤销 %1 个文件的重命名（%2）")
                          .arg(restored)
                          .arg(QStringLiteral("%1 %2").arg(record.timestamp.left(10)).arg(record.timestamp.mid(11))),
                      true);
    }
    return report(QStringLiteral("撤销完成 %1 项，%2 项失败：%3")
                      .arg(restored)
                      .arg(failures.size())
                      .arg(failures.mid(0, 3).join(QStringLiteral("；"))),
                  false);
}

void BatchRenamePlugin::loadUndoStack()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    const QString raw = svc->configValue(id(), QStringLiteral("undoStack")).toString();
    if (raw.isEmpty()) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
    if (!document.isArray()) {
        return;
    }
    for (const QJsonValue &value : document.array()) {
        const QJsonObject json = value.toObject();
        UndoRecord record;
        record.directory = json.value(QStringLiteral("directory")).toString();
        record.timestamp = json.value(QStringLiteral("timestamp")).toString();
        record.fileCount = json.value(QStringLiteral("fileCount")).toInt();
        for (const QJsonValue &pair : json.value(QStringLiteral("mapping")).toArray()) {
            const QJsonArray names = pair.toArray();
            if (names.size() == 2) {
                record.mapping.append(
                    { names.at(0).toString(), names.at(1).toString() });
            }
        }
        if (!record.directory.isEmpty() && !record.mapping.isEmpty()) {
            m_undoStack.append(record);
        }
    }
    while (m_undoStack.size() > kMaxUndo) {
        m_undoStack.removeFirst();
    }
}

void BatchRenamePlugin::saveUndoStack() const
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        return;
    }
    QJsonArray records;
    for (const UndoRecord &record : m_undoStack) {
        QJsonArray mapping;
        for (const QPair<QString, QString> &pair : record.mapping) {
            QJsonArray names;
            names.append(pair.first);
            names.append(pair.second);
            mapping.append(names);
        }
        QJsonObject json;
        json.insert(QStringLiteral("directory"), record.directory);
        json.insert(QStringLiteral("timestamp"), record.timestamp);
        json.insert(QStringLiteral("fileCount"), record.fileCount);
        json.insert(QStringLiteral("mapping"), mapping);
        records.append(json);
    }
    svc->setConfigValue(
        id(), QStringLiteral("undoStack"),
        QString::fromUtf8(QJsonDocument(records).toJson(QJsonDocument::Compact)));
    svc->syncConfig();
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *BatchRenamePlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *targetBox = new QGroupBox(QStringLiteral("默认处理范围"), widget);
    auto *form = new QFormLayout(targetBox);
    auto *directoryRow = new QWidget(targetBox);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    auto *directoryEdit = new QLineEdit(m_directory, directoryRow);
    auto *browseButton = new QPushButton(QStringLiteral("浏览…"), directoryRow);
    directoryLayout->addWidget(directoryEdit, 1);
    directoryLayout->addWidget(browseButton);
    form->addRow(QStringLiteral("目录"), directoryRow);
    auto *filterEdit = new QLineEdit(m_filter, targetBox);
    form->addRow(QStringLiteral("只处理"), filterEdit);
    auto *confirmCheck = new QCheckBox(QStringLiteral("执行前弹二次确认"), targetBox);
    confirmCheck->setChecked(m_confirm);
    form->addRow(QString(), confirmCheck);
    layout->addWidget(targetBox);

    auto *undoRow = new QHBoxLayout();
    auto *undoLabel = new QLabel(widget);
    auto *undoButton = new QPushButton(QStringLiteral("撤销最近一次"), widget);
    undoRow->addWidget(undoLabel, 1);
    undoRow->addWidget(undoButton);
    layout->addLayout(undoRow);

    const auto refreshUndoRow = [this, undoLabel, undoButton] {
        undoLabel->setText(m_undoStack.isEmpty()
                               ? QStringLiteral("没有可撤销的记录")
                               : QStringLiteral("最近一次：%1").arg(lastUndoDescription()));
        undoButton->setEnabled(!m_undoStack.isEmpty());
    };
    refreshUndoRow();

    auto *statusLabel = new QLabel(widget);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *hint = new QLabel(
        QStringLiteral("快捷键 %1 打开改名面板；%2 直接撤销最近一次。\n"
                       "模板占位符：{name} 原（或查找替换后的）名称、{ext} 扩展名、{n} 序号、"
                       "{1}..{9} 正则捕获组、{date}/{time}/{datetime} 时间戳"
                       "（图片取 EXIF 拍摄时间，其它取文件修改时间）。\n"
                       "面板里的「应用重命名」有二次确认；任何冲突都会拒绝执行，"
                       "不会出现改名改到一半的状态。")
            .arg(defaultHotkey().toString(QKeySequence::NativeText),
                 QStringLiteral("Ctrl+Shift+Alt+N")),
        widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    QObject::connect(browseButton, &QPushButton::clicked, widget, [this, directoryEdit, widget] {
        const QString chosen = QFileDialog::getExistingDirectory(
            widget, QStringLiteral("选择默认目录"),
            QDir::fromNativeSeparators(directoryEdit->text()));
        if (!chosen.isEmpty()) {
            directoryEdit->setText(QDir::toNativeSeparators(chosen));
        }
    });
    QObject::connect(directoryEdit, &QLineEdit::textChanged, widget, [this, refreshUndoRow](const QString &text) {
        m_directory = text.trimmed();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("directory"), m_directory);
        }
        refreshUndoRow();
    });
    QObject::connect(filterEdit, &QLineEdit::textChanged, widget, [this](const QString &text) {
        m_filter = text;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("filter"), m_filter);
        }
    });
    QObject::connect(confirmCheck, &QCheckBox::toggled, widget, [this](bool checked) {
        m_confirm = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("confirm"), checked);
        }
    });
    QObject::connect(undoButton, &QPushButton::clicked, widget, [this, statusLabel, refreshUndoRow] {
        QString message;
        undoLast(&message);
        statusLabel->setText(message);
        refreshUndoRow();
    });

    return widget;
}
