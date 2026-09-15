#include "ui/HotkeySettingsDialog.h"

#include "core/GlobalHotkeyManager.h"
#include "core/Logging.h"
#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "sdk/IFeaturePlugin.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

namespace WinEase {

namespace {
constexpr int kColumnId = 0;
constexpr int kColumnDescription = 1;
constexpr int kColumnSequence = 2;
} // namespace

HotkeySettingsDialog::HotkeySettingsDialog(GlobalHotkeyManager *hotkeyManager,
                                           PluginManager *pluginManager,
                                           QWidget *parent)
    : QDialog(parent)
    , m_hotkeyManager(hotkeyManager)
    , m_pluginManager(pluginManager)
{
    setObjectName(QStringLiteral("HotkeySettingsDialog"));
    setWindowTitle(QStringLiteral("全局快捷键设置"));
    setModal(false);
    resize(620, 460);

    setupUi();
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &HotkeySettingsDialog::onCurrentBindingChanged);
    reload();
}

HotkeySettingsDialog::~HotkeySettingsDialog() = default;

void HotkeySettingsDialog::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(12);

    auto *tip = new QLabel(QStringLiteral(
                               "提示：全局快捷键必须包含 Ctrl / Alt / Shift / Win 中至少一个修饰键，"
                               "否则会抢占正常键盘输入。留空表示不启用该快捷键。"),
                           this);
    tip->setWordWrap(true);
    tip->setObjectName(QStringLiteral("DialogHint"));
    rootLayout->addWidget(tip);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("标识"), QStringLiteral("功能"),
                                         QStringLiteral("快捷键") });
    m_table->horizontalHeader()->setSectionResizeMode(kColumnId, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColumnDescription, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColumnSequence, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rootLayout->addWidget(m_table, 1);

    auto *editLayout = new QHBoxLayout();
    editLayout->setSpacing(8);
    editLayout->addWidget(new QLabel(QStringLiteral("按键组合："), this));

    m_sequenceEdit = new QKeySequenceEdit(this);
    m_sequenceEdit->setObjectName(QStringLiteral("HotkeySequenceEdit"));
    m_sequenceEdit->setClearButtonEnabled(true);
    editLayout->addWidget(m_sequenceEdit, 1);

    m_applyButton = new QPushButton(QStringLiteral("应用"), this);
    m_clearButton = new QPushButton(QStringLiteral("清除"), this);
    m_defaultsButton = new QPushButton(QStringLiteral("恢复默认"), this);
    editLayout->addWidget(m_applyButton);
    editLayout->addWidget(m_clearButton);
    editLayout->addWidget(m_defaultsButton);
    rootLayout->addLayout(editLayout);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setObjectName(QStringLiteral("DialogHint"));
    rootLayout->addWidget(m_hintLabel);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    rootLayout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::hide);
    if (auto *closeButton = buttonBox->button(QDialogButtonBox::Close)) {
        connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);
    }

    connect(m_applyButton, &QPushButton::clicked, this, &HotkeySettingsDialog::onApplyClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &HotkeySettingsDialog::onClearClicked);
    connect(m_defaultsButton, &QPushButton::clicked, this, &HotkeySettingsDialog::onRestoreDefaultsClicked);
    connect(m_sequenceEdit, &QKeySequenceEdit::keySequenceChanged, this,
            &HotkeySettingsDialog::onSequenceEdited);
}

void HotkeySettingsDialog::reload()
{
    if (!m_hotkeyManager) {
        return;
    }

    // 收集插件声明的默认快捷键，用于"恢复默认"
    m_defaults.clear();
    if (m_pluginManager) {
        for (IFeaturePlugin *plugin : m_pluginManager->plugins()) {
            // ⚠ 已崩溃隔离的插件不再被调用（对象可能已损坏）；其快捷键分发也已被管理器拒绝（P0-5）
            if (m_pluginManager->isPluginFailed(m_pluginManager->idOf(plugin))) {
                continue;
            }
            if (!plugin->supportsHotkey() || plugin->defaultHotkey().isEmpty()) {
                continue;
            }
            const QString hotkeyId = plugin->id() + QStringLiteral("::default");
            m_defaults.insert(hotkeyId, GlobalHotkeyManager::sequenceToText(plugin->defaultHotkey()));
        }
    }

    fillTable();
}

void HotkeySettingsDialog::fillTable()
{
    const QVector<HotkeyBinding> bindings = m_hotkeyManager->bindings();
    m_table->setRowCount(bindings.size());

    for (int row = 0; row < bindings.size(); ++row) {
        const HotkeyBinding &binding = bindings.at(row);

        auto *idItem = new QTableWidgetItem(binding.hotkeyId);
        idItem->setData(Qt::UserRole, binding.hotkeyId);

        auto *descItem = new QTableWidgetItem(binding.description);

        QString sequenceText = GlobalHotkeyManager::sequenceToText(binding.sequence);
        if (sequenceText.isEmpty()) {
            sequenceText = QStringLiteral("（未设置）");
        } else if (!binding.registered) {
            sequenceText += QStringLiteral("  ⚠ 注册失败");
        }
        auto *seqItem = new QTableWidgetItem(sequenceText);
        seqItem->setToolTip(sequenceText);

        m_table->setItem(row, kColumnId, idItem);
        m_table->setItem(row, kColumnDescription, descItem);
        m_table->setItem(row, kColumnSequence, seqItem);
    }

    if (bindings.isEmpty()) {
        m_hintLabel->setText(QStringLiteral("当前没有已登记的全局快捷键。"));
        m_sequenceEdit->setEnabled(false);
        m_applyButton->setEnabled(false);
        m_clearButton->setEnabled(false);
        m_defaultsButton->setEnabled(false);
        return;
    }

    m_sequenceEdit->setEnabled(true);
    m_applyButton->setEnabled(true);
    m_clearButton->setEnabled(true);
    m_defaultsButton->setEnabled(!m_defaults.isEmpty());
    m_hintLabel->clear();

    if (m_table->currentRow() < 0) {
        m_table->selectRow(0);
    } else {
        onCurrentBindingChanged();
    }
}

QString HotkeySettingsDialog::selectedHotkeyId() const
{
    const int row = m_table->currentRow();
    if (row < 0) {
        return QString();
    }
    QTableWidgetItem *item = m_table->item(row, kColumnId);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void HotkeySettingsDialog::refreshHintStyle()
{
    // 提示标签通过 state 动态属性切换配色（见 style.qss）
    m_hintLabel->style()->unpolish(m_hintLabel);
    m_hintLabel->style()->polish(m_hintLabel);
    m_hintLabel->update();
}

void HotkeySettingsDialog::onCurrentBindingChanged()
{
    const QString hotkeyId = selectedHotkeyId();
    if (hotkeyId.isEmpty()) {
        return;
    }
    const HotkeyBinding binding = m_hotkeyManager->binding(hotkeyId);
    m_sequenceEdit->setKeySequence(binding.sequence);
    m_hintLabel->clear();
}

void HotkeySettingsDialog::onSequenceEdited()
{
    const QKeySequence sequence = m_sequenceEdit->keySequence();
    if (sequence.isEmpty()) {
        m_hintLabel->setProperty("state", QString());
        m_hintLabel->setText(QString());
        refreshHintStyle();
        return;
    }

    // 实时校验：组合是否受支持 / 是否与其它功能冲突
    QString reason;
    const bool ok = m_hotkeyManager->validateSequence(sequence, selectedHotkeyId(), &reason);
    m_hintLabel->setProperty("state", ok ? QStringLiteral("ok") : QStringLiteral("error"));
    m_hintLabel->setText(ok ? QStringLiteral("该组合可用：%1")
                                  .arg(GlobalHotkeyManager::sequenceToText(sequence))
                            : reason);
    refreshHintStyle();

    m_applyButton->setEnabled(ok);
}

void HotkeySettingsDialog::onApplyClicked()
{
    const QString hotkeyId = selectedHotkeyId();
    if (hotkeyId.isEmpty()) {
        return;
    }

    const QKeySequence sequence = m_sequenceEdit->keySequence();
    if (sequence.isEmpty()) {
        m_hintLabel->setText(QStringLiteral("请先录入按键组合。"));
        return;
    }

    // 二次校验，保证与实时提示一致
    QString reason;
    if (!m_hotkeyManager->validateSequence(sequence, hotkeyId, &reason)) {
        m_hintLabel->setProperty("state", QStringLiteral("error"));
        m_hintLabel->setText(reason);
        refreshHintStyle();
        return;
    }

    if (!m_hotkeyManager->updateHotkey(hotkeyId, sequence)) {
        m_hintLabel->setProperty("state", QStringLiteral("error"));
        m_hintLabel->setText(QStringLiteral("应用失败：注册到系统时被拒绝，可能已被其它程序占用。"));
        refreshHintStyle();
        return;
    }

    // 持久化
    SettingsManager::instance().setHotkeyText(hotkeyId, GlobalHotkeyManager::sequenceToText(sequence));
    SettingsManager::instance().sync();

    m_hintLabel->setText(QStringLiteral("已更新：%1 -> %2")
                             .arg(hotkeyId, GlobalHotkeyManager::sequenceToText(sequence)));
    fillTable();
    qCInfo(lcHotkey) << "用户修改快捷键" << hotkeyId << GlobalHotkeyManager::sequenceToText(sequence);
}

void HotkeySettingsDialog::onClearClicked()
{
    const QString hotkeyId = selectedHotkeyId();
    if (hotkeyId.isEmpty()) {
        return;
    }

    m_hotkeyManager->updateHotkey(hotkeyId, QKeySequence());
    SettingsManager::instance().setHotkeyText(hotkeyId, QString());
    SettingsManager::instance().sync();

    m_sequenceEdit->clear();
    m_hintLabel->setProperty("state", QStringLiteral("ok"));
    m_hintLabel->setText(QStringLiteral("已清除 %1 的快捷键绑定。").arg(hotkeyId));
    refreshHintStyle();
    fillTable();
}

void HotkeySettingsDialog::onRestoreDefaultsClicked()
{
    int restored = 0;
    for (auto it = m_defaults.cbegin(); it != m_defaults.cend(); ++it) {
        if (!m_hotkeyManager->contains(it.key())) {
            continue;
        }
        const QKeySequence sequence = GlobalHotkeyManager::textToSequence(it.value());
        if (m_hotkeyManager->updateHotkey(it.key(), sequence)) {
            SettingsManager::instance().setHotkeyText(it.key(), it.value());
            ++restored;
        }
    }
    SettingsManager::instance().sync();

    m_hintLabel->setProperty("state", QStringLiteral("ok"));
    m_hintLabel->setText(QStringLiteral("已恢复 %1 项默认快捷键。").arg(restored));
    refreshHintStyle();
    fillTable();
}

} // namespace WinEase
