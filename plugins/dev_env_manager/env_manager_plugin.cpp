#include "env_manager_plugin.h"

#include "sdk/ElevationService.h"
#include "sdk/PluginServices.h"
#include "win32/RegistryUtils.h"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace {

using WinEase::Common::EnvScope;
using WinEase::Common::PathEntry;

const QString kPathVariable = QStringLiteral("Path");

/// PATH 条目的展示文字：归一化后的路径 + 问题标记
QString pathEntryText(const PathEntry &entry, const QHash<QString, QString> &variables)
{
    if (entry.empty) {
        return QStringLiteral("（空条目 —— 等价于当前目录，通常应当删掉）");
    }

    const QString expanded = WinEase::Common::expandVariables(entry.normalized, variables);
    QString text = entry.normalized;
    if (expanded != entry.normalized) {
        text += QStringLiteral("    → %1").arg(expanded);
    }

    QStringList marks;
    if (entry.duplicate) {
        marks << QStringLiteral("重复（与第 %1 条相同）").arg(entry.duplicateOf + 1);
    }
    const QString problem = WinEase::Common::pathEntryProblem(expanded);
    if (!problem.isEmpty() && !entry.duplicate) {
        marks << problem;
    }
    // 即使重复也要报出路径问题：两件事都要让用户看见
    if (entry.duplicate) {
        const QString duplicateProblem = WinEase::Common::pathEntryProblem(expanded);
        if (!duplicateProblem.isEmpty()) {
            marks << duplicateProblem;
        }
    }
    if (!marks.isEmpty()) {
        text += QStringLiteral("    ⚠ %1").arg(marks.join(QStringLiteral("；")));
    }
    return text;
}

} // namespace

EnvManagerPlugin::EnvManagerPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString EnvManagerPlugin::id() const
{
    return QStringLiteral("dev.env_manager");
}

QString EnvManagerPlugin::name() const
{
    return QStringLiteral("环境变量管理");
}

QString EnvManagerPlugin::description() const
{
    return QStringLiteral("用户/系统环境变量与 PATH 可视化编辑：重复项、空条目、展开预览");
}

QIcon EnvManagerPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Development);
}

WinEase::FeatureCategory EnvManagerPlugin::category() const
{
    return WinEase::FeatureCategory::Development;
}

QStringList EnvManagerPlugin::tags() const
{
    return { QStringLiteral("环境变量"), QStringLiteral("PATH"), QStringLiteral("path"),
             QStringLiteral("环境"), QStringLiteral("huanjingbianliang") };
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool EnvManagerPlugin::initialize()
{
    logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("环境变量管理已就绪"));
    return true;
}

void EnvManagerPlugin::shutdown()
{
    // 停用不动任何东西：本插件只在"保存/删除"那两次点击里写注册表
}

bool EnvManagerPlugin::canEnable(QString *reason) const
{
    // 读注册表不需要提权；写系统级变量才需要助手，缺助手时逐次如实报错
    Q_UNUSED(reason)
    return true;
}

bool EnvManagerPlugin::onEnable()
{
    reloadFromRegistry();
    Q_EMIT statusMessage(statusText());

    // ⚠ 时序（踩坑 #51 / #57）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void EnvManagerPlugin::onDisable()
{
    m_lastEvent = QStringLiteral("已停用（环境变量未被修改）");
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  读
// ---------------------------------------------------------------------------

WinEase::Common::EnvScope EnvManagerPlugin::currentScope() const
{
    if (!m_scopeCombo.isNull()) {
        bool ok = false;
        const WinEase::Common::EnvScope scope =
            WinEase::Common::envScopeFromKey(m_scopeCombo->currentData().toString(), &ok);
        if (ok) {
            return scope;
        }
    }
    return WinEase::Common::EnvScope::User;
}

void EnvManagerPlugin::reloadFromRegistry()
{
    QString error;
    m_variables = WinEase::Common::readEnvironment(currentScope(), &error);

    if (!error.isEmpty()) {
        m_lastError = error;
        m_lastEvent = QStringLiteral("读取%1失败：%2")
                          .arg(WinEase::Common::envScopeText(currentScope()), error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
    } else {
        m_lastError.clear();
        m_lastEvent = QStringLiteral("已读取%1：共 %2 项 · %3")
                          .arg(WinEase::Common::envScopeText(currentScope()))
                          .arg(m_variables.size())
                          .arg(pathSummary());
    }

    loadPathEditor();

    // ⚠ 重载之后必须刷面板：保存/删除成功后用户最想看的就是"列表里到底变了没"，
    //   而变量表（refreshTable）只在 refreshPanel 里重绘 ——
    //   少了这一下，用户会以为"点了保存但什么都没发生"（踩坑 #21 那一类）
    refreshPanel();
}

void EnvManagerPlugin::loadPathEditor()
{
    m_pathEntries.clear();

    if (m_variables.contains(kPathVariable)) {
        m_pathEntries = WinEase::Common::splitPathEntries(m_variables.value(kPathVariable));
    }
    refreshPathList();
    updateExpandPreview();
}

QString EnvManagerPlugin::currentPathValue() const
{
    return WinEase::Common::joinPathEntries(m_pathEntries);
}

void EnvManagerPlugin::syncPathEntriesFromList()
{
    if (m_pathList.isNull()) {
        return;
    }

    QList<PathEntry> entries;
    entries.reserve(m_pathList->count());
    for (int index = 0; index < m_pathList->count(); ++index) {
        QListWidgetItem *item = m_pathList->item(index);
        const QString raw = item != nullptr ? item->data(Qt::UserRole).toString() : QString();
        PathEntry entry;
        entry.raw = raw;
        entries.append(entry);
    }

    // 重新判定空/重复（复用共享件的同一套规则，界面上的标记不会跟判定分叉）
    m_pathEntries = WinEase::Common::splitPathEntries(WinEase::Common::joinPathEntries(entries));
}

// ---------------------------------------------------------------------------
//  写（唯一入口）
// ---------------------------------------------------------------------------

WinEase::ElevationService *EnvManagerPlugin::elevation() const
{
    WinEase::PluginServices *svc = services();
    return svc != nullptr ? svc->elevationService() : nullptr;
}

bool EnvManagerPlugin::elevationAvailable() const
{
    return elevation() != nullptr;
}

bool EnvManagerPlugin::writeVariable(const QString &name, const QString &value, bool remove,
                                     QString *errorOut)
{
    const QString problem = WinEase::Common::variableNameProblem(name);
    if (!problem.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = problem;
        }
        return false;
    }

    const EnvScope scope = currentScope();
    const QString key = WinEase::Common::envScopeRegistryPath(scope);
    const WinEase::Win32::RegistryRoot root = scope == EnvScope::User
                                                  ? WinEase::Win32::RegistryRoot::CurrentUser
                                                  : WinEase::Win32::RegistryRoot::LocalMachine;

    if (!WinEase::Common::envScopeNeedsElevation(scope)) {
        // 用户级：免提权直写 + 广播（"立即生效"就靠这一下广播）
        QString error;
        bool ok = false;
        if (remove) {
            ok = WinEase::Win32::deleteValue(root, key, name,
                                             WinEase::Win32::RegistryView::Default, &error);
            if (!ok && error.contains(QStringLiteral("找不到"))) {
                ok = true; // 本来就没有：对"删掉它"这个意图来说结论是成立的
                error.clear();
            }
        } else {
            const WinEase::Win32::RegistryValueType type =
                WinEase::Common::valueNeedsExpandType(value)
                    ? WinEase::Win32::RegistryValueType::ExpandString
                    : WinEase::Win32::RegistryValueType::String;
            ok = WinEase::Win32::writeValue(root, key, name, value, type,
                                            WinEase::Win32::RegistryView::Default, &error);
        }

        if (!ok) {
            if (errorOut != nullptr) {
                *errorOut = error;
            }
            return false;
        }
        WinEase::Win32::broadcastSettingChange(QStringLiteral("Environment"));
        return true;
    }

    // 系统级：走提权助手（助手内部会广播），缺助手就如实报错
    WinEase::ElevationService *service = elevation();
    if (service == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("提权助手不可用，无法修改系统变量"
                                       "（HKLM 需要管理员权限）");
        }
        return false;
    }

    QVariantMap arguments;
    arguments.insert(QStringLiteral("scope"), WinEase::Common::envScopeKey(scope));
    arguments.insert(QStringLiteral("name"), name);
    if (!remove) {
        arguments.insert(QStringLiteral("value"), value);
    }

    const WinEase::ElevationResult result =
        service->execute(QStringLiteral("setEnvVar"), arguments);
    if (!result.isSuccess()) {
        if (errorOut != nullptr) {
            *errorOut = result.error;
        }
        return false;
    }
    return true;
}

void EnvManagerPlugin::saveVariable()
{
    if (m_nameEdit.isNull() || m_valueEdit.isNull()) {
        return;
    }

    const QString name = m_nameEdit->text().trimmed();
    const QString value = m_valueEdit->text();

    QString error;
    if (!writeVariable(name, value, false, &error)) {
        m_lastError = error;
        m_lastEvent = QStringLiteral("保存失败：%1").arg(error);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name.isEmpty() ? this->name() : name, m_lastEvent);
        }
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    m_lastError.clear();
    m_lastEvent = QStringLiteral("已保存 %1 = %2（%3）")
                      .arg(name,
                           value.size() > 60 ? value.left(60) + QStringLiteral("…") : value,
                           WinEase::Common::envScopeText(currentScope()));
    Q_EMIT statusMessage(m_lastEvent);
    reloadFromRegistry();
}

void EnvManagerPlugin::deleteVariable()
{
    if (m_nameEdit.isNull()) {
        return;
    }
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        m_lastEvent = QStringLiteral("请先选择或填写要删除的变量名");
        refreshPanel();
        return;
    }

    QString error;
    if (!writeVariable(name, QString(), true, &error)) {
        m_lastError = error;
        m_lastEvent = QStringLiteral("删除失败：%1").arg(error);
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    m_lastError.clear();
    m_lastEvent = QStringLiteral("已删除 %1（%2）")
                      .arg(name, WinEase::Common::envScopeText(currentScope()));
    Q_EMIT statusMessage(m_lastEvent);
    reloadFromRegistry();
}

void EnvManagerPlugin::savePathValue()
{
    syncPathEntriesFromList();

    const QString value = currentPathValue();
    QString error;
    if (!writeVariable(kPathVariable, value, false, &error)) {
        m_lastError = error;
        m_lastEvent = QStringLiteral("保存 PATH 失败：%1").arg(error);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(this->name(), m_lastEvent);
        }
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    m_lastError.clear();
    m_lastEvent = QStringLiteral("已保存 PATH（%1 条 / %2）")
                      .arg(m_pathEntries.size())
                      .arg(WinEase::Common::envScopeText(currentScope()));
    Q_EMIT statusMessage(m_lastEvent);
    reloadFromRegistry();
}

// ---------------------------------------------------------------------------
//  PATH 编辑
// ---------------------------------------------------------------------------

void EnvManagerPlugin::addPathEntry()
{
    if (m_pathAddEdit.isNull() || m_pathList.isNull()) {
        return;
    }

    const QString text = m_pathAddEdit->text().trimmed();
    if (text.isEmpty()) {
        m_lastEvent = QStringLiteral("请先填写要添加的目录");
        refreshPanel();
        return;
    }

    QStringList raws;
    for (int index = 0; index < m_pathList->count(); ++index) {
        raws << m_pathList->item(index)->data(Qt::UserRole).toString();
    }
    raws << text;
    m_pathEntries = WinEase::Common::splitPathEntries(raws.join(QLatin1Char(';')));

    m_pathAddEdit->clear();
    m_lastEvent = QStringLiteral("已添加一条（未保存）");
    refreshPathList();
    updateExpandPreview();
    refreshPanel();
}

void EnvManagerPlugin::removeSelectedPathEntry()
{
    if (m_pathList.isNull()) {
        return;
    }
    const int row = m_pathList->currentRow();
    if (row < 0) {
        m_lastEvent = QStringLiteral("请先在列表里选中一条");
        refreshPanel();
        return;
    }

    QStringList raws;
    for (int index = 0; index < m_pathList->count(); ++index) {
        if (index != row) {
            raws << m_pathList->item(index)->data(Qt::UserRole).toString();
        }
    }
    m_pathEntries = WinEase::Common::splitPathEntries(raws.join(QLatin1Char(';')));
    m_lastEvent = QStringLiteral("已删除一条（未保存）");

    refreshPathList();
    if (m_pathList->count() > 0) {
        m_pathList->setCurrentRow(qMin(row, m_pathList->count() - 1));
    }
    updateExpandPreview();
    refreshPanel();
}

void EnvManagerPlugin::moveSelectedPathEntry(int delta)
{
    if (m_pathList.isNull()) {
        return;
    }
    const int row = m_pathList->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_pathList->count()) {
        return;
    }

    QStringList raws;
    for (int index = 0; index < m_pathList->count(); ++index) {
        raws << m_pathList->item(index)->data(Qt::UserRole).toString();
    }
    raws.move(row, target);
    m_pathEntries = WinEase::Common::splitPathEntries(raws.join(QLatin1Char(';')));

    refreshPathList();
    m_pathList->setCurrentRow(target);
    m_lastEvent = QStringLiteral("顺序已调整（未保存，PATH 是**按顺序查找**的）");
    updateExpandPreview();
    refreshPanel();
}

void EnvManagerPlugin::dedupePathEntries()
{
    if (m_pathList.isNull()) {
        return;
    }

    QStringList raws;
    for (int index = 0; index < m_pathList->count(); ++index) {
        raws << m_pathList->item(index)->data(Qt::UserRole).toString();
    }

    const QString before = raws.join(QLatin1Char(';'));
    const QString after = WinEase::Common::normalizePathValue(before);
    if (before == after) {
        m_lastEvent = QStringLiteral("没有可去重的条目");
        refreshPanel();
        return;
    }

    const int removedCount =
        WinEase::Common::splitPathEntries(before).size()
        - WinEase::Common::splitPathEntries(after).size();
    m_pathEntries = WinEase::Common::splitPathEntries(after);
    // ⚠ 只改编辑区、**不落盘**：PATH 是"能让一堆程序突然找不到"的东西，
    //   自动写入等于在赌用户的手速
    m_lastEvent = QStringLiteral("已去掉 %1 条重复/空条目（**未保存**，请确认后点「保存 PATH」）")
                      .arg(removedCount);

    refreshPathList();
    updateExpandPreview();
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

void EnvManagerPlugin::refreshTable()
{
    if (m_table.isNull()) {
        return;
    }

    m_table->setRowCount(m_variables.size());
    int row = 0;
    for (auto it = m_variables.constBegin(); it != m_variables.constEnd(); ++it, ++row) {
        const QString value = it.value();
        const QString shown = value.size() > 120 ? value.left(120) + QStringLiteral("…") : value;
        const QStringList cells{ it.key(), shown };
        for (int column = 0; column < cells.size(); ++column) {
            QTableWidgetItem *item = m_table->item(row, column);
            if (item == nullptr) {
                item = new QTableWidgetItem(cells.at(column));
                if (column == 1) {
                    item->setToolTip(value);
                }
                m_table->setItem(row, column, item);
            } else {
                item->setText(cells.at(column));
                if (column == 1) {
                    item->setToolTip(value);
                }
            }
        }
    }
}

void EnvManagerPlugin::refreshPathList()
{
    if (m_pathList.isNull()) {
        return;
    }

    QHash<QString, QString> variables;
    for (auto it = m_variables.constBegin(); it != m_variables.constEnd(); ++it) {
        variables.insert(it.key(), it.value());
    }

    const int keepRow = m_pathList->currentRow();
    m_pathList->clear();
    for (const PathEntry &entry : m_pathEntries) {
        auto *item = new QListWidgetItem(pathEntryText(entry, variables), m_pathList.data());
        item->setData(Qt::UserRole, entry.raw);
        item->setToolTip(entry.normalized);
    }
    if (keepRow >= 0 && keepRow < m_pathList->count()) {
        m_pathList->setCurrentRow(keepRow);
    }
}

void EnvManagerPlugin::updateExpandPreview()
{
    if (m_expandLabel.isNull()) {
        return;
    }

    QHash<QString, QString> variables;
    for (auto it = m_variables.constBegin(); it != m_variables.constEnd(); ++it) {
        variables.insert(it.key(), it.value());
    }

    const QString value = currentPathValue();
    if (value.isEmpty()) {
        m_expandLabel->setText(QStringLiteral("（当前作用域没有 PATH）"));
        return;
    }

    const QString expanded = WinEase::Common::expandVariables(value, variables);
    const QString shown = expanded.size() > 220 ? expanded.left(220) + QStringLiteral("…") : expanded;
    m_expandLabel->setText(QStringLiteral("展开预览（`%VAR%` 会变成实际路径）：\n%1").arg(shown));
}

QString EnvManagerPlugin::pathSummary() const
{
    if (m_pathEntries.isEmpty()) {
        return QStringLiteral("PATH 未设置");
    }

    int duplicates = 0;
    int empties = 0;
    int missing = 0;
    for (const PathEntry &entry : m_pathEntries) {
        if (entry.empty) {
            ++empties;
            continue;
        }
        if (entry.duplicate) {
            ++duplicates;
        }
        // 目录存在性用**原样值**判断即可（带 %VAR% 的条目交给展开预览去体检）
        if (WinEase::Common::pathEntryProblem(entry.normalized)
            == QStringLiteral("目录不存在")) {
            ++missing;
        }
    }

    return QStringLiteral("PATH 共 %1 条（重复 %2 / 空 %3 / 目录不存在 %4）")
        .arg(m_pathEntries.size())
        .arg(duplicates)
        .arg(empties)
        .arg(missing);
}

QString EnvManagerPlugin::statusText() const
{
    QString text = m_lastEvent.isEmpty() ? QStringLiteral("就绪") : m_lastEvent;
    if (!m_lastError.isEmpty()) {
        text += QStringLiteral("　（%1）").arg(m_lastError);
    }
    return text;
}

QWidget *EnvManagerPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("envPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *scopeRow = new QHBoxLayout();
    scopeRow->addWidget(new QLabel(QStringLiteral("作用域："), widget));
    auto *scopeCombo = new QComboBox(widget);
    scopeCombo->setObjectName(QStringLiteral("envScopeCombo"));
    for (const WinEase::Common::EnvScope scope : WinEase::Common::allEnvScopes()) {
        scopeCombo->addItem(WinEase::Common::envScopeText(scope),
                            WinEase::Common::envScopeKey(scope));
    }
    scopeRow->addWidget(scopeCombo);
    auto *scopeLabel = new QLabel(widget);
    scopeLabel->setObjectName(QStringLiteral("envScopeLabel"));
    scopeRow->addWidget(scopeLabel, 1);
    layout->addLayout(scopeRow);

    auto *table = new QTableWidget(0, 2, widget);
    table->setObjectName(QStringLiteral("envTable"));
    table->setHorizontalHeaderLabels({ QStringLiteral("变量名"), QStringLiteral("值") });
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(140);
    layout->addWidget(table);

    auto *editRow = new QHBoxLayout();
    auto *nameEdit = new QLineEdit(widget);
    nameEdit->setObjectName(QStringLiteral("envNameEdit"));
    nameEdit->setPlaceholderText(QStringLiteral("变量名（半角字母/数字/下划线）"));
    editRow->addWidget(nameEdit, 1);
    auto *valueEdit = new QLineEdit(widget);
    valueEdit->setObjectName(QStringLiteral("envValueEdit"));
    valueEdit->setPlaceholderText(QStringLiteral("值（可含 %SystemRoot% 这类引用）"));
    editRow->addWidget(valueEdit, 3);
    auto *saveButton = new QPushButton(QStringLiteral("保存"), widget);
    saveButton->setObjectName(QStringLiteral("envSaveButton"));
    editRow->addWidget(saveButton);
    auto *deleteButton = new QPushButton(QStringLiteral("删除"), widget);
    deleteButton->setObjectName(QStringLiteral("envDeleteButton"));
    editRow->addWidget(deleteButton);
    layout->addLayout(editRow);

    auto *pathTitle = new QLabel(widget);
    pathTitle->setObjectName(QStringLiteral("envPathSummaryLabel"));
    pathTitle->setWordWrap(true);
    layout->addWidget(pathTitle);

    auto *pathList = new QListWidget(widget);
    pathList->setObjectName(QStringLiteral("envPathList"));
    pathList->setMinimumHeight(160);
    layout->addWidget(pathList);

    auto *pathRow = new QHBoxLayout();
    auto *pathAddEdit = new QLineEdit(widget);
    pathAddEdit->setObjectName(QStringLiteral("envPathAddEdit"));
    pathAddEdit->setPlaceholderText(QStringLiteral("要添加的目录，例如 %SystemRoot%\\System32"));
    pathRow->addWidget(pathAddEdit, 1);
    auto *pathAddButton = new QPushButton(QStringLiteral("添加"), widget);
    pathAddButton->setObjectName(QStringLiteral("envPathAddButton"));
    pathRow->addWidget(pathAddButton);
    auto *pathRemoveButton = new QPushButton(QStringLiteral("删除选中"), widget);
    pathRemoveButton->setObjectName(QStringLiteral("envPathRemoveButton"));
    pathRow->addWidget(pathRemoveButton);
    auto *pathUpButton = new QPushButton(QStringLiteral("上移"), widget);
    pathUpButton->setObjectName(QStringLiteral("envPathUpButton"));
    pathRow->addWidget(pathUpButton);
    auto *pathDownButton = new QPushButton(QStringLiteral("下移"), widget);
    pathDownButton->setObjectName(QStringLiteral("envPathDownButton"));
    pathRow->addWidget(pathDownButton);
    auto *pathDedupeButton = new QPushButton(QStringLiteral("去重（不自动保存）"), widget);
    pathDedupeButton->setObjectName(QStringLiteral("envPathDedupeButton"));
    pathRow->addWidget(pathDedupeButton);
    auto *pathSaveButton = new QPushButton(QStringLiteral("保存 PATH"), widget);
    pathSaveButton->setObjectName(QStringLiteral("envPathSaveButton"));
    pathRow->addWidget(pathSaveButton);
    layout->addLayout(pathRow);

    auto *expandLabel = new QLabel(widget);
    expandLabel->setObjectName(QStringLiteral("envExpandLabel"));
    expandLabel->setWordWrap(true);
    expandLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(expandLabel);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("envStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *hint = new QLabel(
        QStringLiteral("用户变量（HKCU）免提权直接生效；系统变量（HKLM）经提权助手写入，"
                       "两者写完都会广播 WM_SETTINGCHANGE，已运行的程序收到后会刷新环境。\n"
                       "PATH 是**按顺序查找**的：上移/下移会改变查找优先级；"
                       "空条目等价于「当前目录」，通常应当删掉。\n"
                       "「去重」只改这份列表、**不会自动保存** —— 请确认后自己点「保存 PATH」。\n"
                       "展开预览里，未能展开的 `%VAR%` 会原样保留（说明那个变量没有定义）。"),
        widget);
    hint->setObjectName(QStringLiteral("envHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_scopeCombo = scopeCombo;
    m_scopeLabel = scopeLabel;
    m_statusLabel = statusLabel;
    m_expandLabel = expandLabel;
    m_pathSummaryLabel = pathTitle;
    m_table = table;
    m_nameEdit = nameEdit;
    m_valueEdit = valueEdit;
    m_saveButton = saveButton;
    m_deleteButton = deleteButton;
    m_pathList = pathList;
    m_pathAddEdit = pathAddEdit;
    m_pathAddButton = pathAddButton;
    m_pathRemoveButton = pathRemoveButton;
    m_pathUpButton = pathUpButton;
    m_pathDownButton = pathDownButton;
    m_pathDedupeButton = pathDedupeButton;
    m_pathSaveButton = pathSaveButton;

    connect(scopeCombo, &QComboBox::currentIndexChanged, widget, [this](int) {
        reloadFromRegistry();
        Q_EMIT statusMessage(statusText());
        refreshPanel();
    });
    connect(table, &QTableWidget::itemSelectionChanged, widget, [this] {
        if (m_table.isNull() || m_nameEdit.isNull() || m_valueEdit.isNull()) {
            return;
        }
        const int row = m_table->currentRow();
        if (row < 0 || row >= m_table->rowCount()) {
            return;
        }
        const QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem == nullptr) {
            return;
        }
        const QString name = nameItem->text();
        QSignalBlocker nameBlocker(m_nameEdit.data());
        m_nameEdit->setText(name);
        QSignalBlocker valueBlocker(m_valueEdit.data());
        m_valueEdit->setText(m_variables.value(name));
        refreshPanel();
    });
    connect(saveButton, &QPushButton::clicked, widget, [this] { saveVariable(); });
    connect(deleteButton, &QPushButton::clicked, widget, [this] { deleteVariable(); });
    connect(pathAddButton, &QPushButton::clicked, widget, [this] { addPathEntry(); });
    connect(pathRemoveButton, &QPushButton::clicked, widget, [this] { removeSelectedPathEntry(); });
    connect(pathUpButton, &QPushButton::clicked, widget, [this] { moveSelectedPathEntry(-1); });
    connect(pathDownButton, &QPushButton::clicked, widget, [this] { moveSelectedPathEntry(1); });
    connect(pathDedupeButton, &QPushButton::clicked, widget, [this] { dedupePathEntries(); });
    connect(pathSaveButton, &QPushButton::clicked, widget, [this] { savePathValue(); });

    reloadFromRegistry();
    refreshPanel();
    return widget;
}

void EnvManagerPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    const bool running = isEnabled();
    const bool machineScope = WinEase::Common::envScopeNeedsElevation(currentScope());
    const bool canWrite = running && (!machineScope || elevationAvailable());

    if (!m_scopeLabel.isNull()) {
        m_scopeLabel->setText(QStringLiteral("%1 · %2")
                                  .arg(WinEase::Common::envScopeRegistryPath(currentScope()),
                                       machineScope
                                           ? QStringLiteral("修改需要管理员权限（经提权助手）")
                                           : QStringLiteral("修改免提权，直接生效")));
    }
    if (!m_statusLabel.isNull()) {
        QString text = statusText();
        if (running && machineScope && !elevationAvailable()) {
            text += QStringLiteral("　（提权助手不可用：只能查看系统变量）");
        }
        m_statusLabel->setText(text);
    }
    if (!m_pathSummaryLabel.isNull()) {
        m_pathSummaryLabel->setText(QStringLiteral("PATH 条目：%1").arg(pathSummary()));
    }

    refreshTable();
    refreshPathList();
    updateExpandPreview();

    if (!m_saveButton.isNull()) {
        m_saveButton->setEnabled(canWrite);
    }
    if (!m_deleteButton.isNull()) {
        m_deleteButton->setEnabled(canWrite);
    }
    if (!m_pathSaveButton.isNull()) {
        m_pathSaveButton->setEnabled(canWrite);
    }
    if (!m_pathDedupeButton.isNull()) {
        m_pathDedupeButton->setEnabled(running);
    }
    if (!m_pathAddButton.isNull()) {
        m_pathAddButton->setEnabled(running);
    }
    if (!m_pathRemoveButton.isNull()) {
        m_pathRemoveButton->setEnabled(running);
    }
    if (!m_pathUpButton.isNull()) {
        m_pathUpButton->setEnabled(running);
    }
    if (!m_pathDownButton.isNull()) {
        m_pathDownButton->setEnabled(running);
    }
    if (!m_scopeCombo.isNull()) {
        m_scopeCombo->setEnabled(running);
    }
    if (!m_nameEdit.isNull()) {
        m_nameEdit->setEnabled(running);
    }
    if (!m_valueEdit.isNull()) {
        m_valueEdit->setEnabled(running);
    }
    if (!m_pathAddEdit.isNull()) {
        m_pathAddEdit->setEnabled(running);
    }
}
