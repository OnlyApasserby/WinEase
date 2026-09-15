#include "hosts_editor_plugin.h"

#include "sdk/ElevationService.h"
#include "sdk/PluginServices.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWidget>

namespace {

/// 语法高亮：注释行压暗、IP 地址着色、主机名保持原色。
/// 刻意做得克制 —— hosts 文件里 90% 是注释，颜色太花反而看不清有效记录。
class HostsHighlighter : public QSyntaxHighlighter
{
public:
    explicit HostsHighlighter(QTextDocument *document)
        : QSyntaxHighlighter(document)
    {
        m_commentFormat.setForeground(QColor(0x6a, 0x99, 0x55));
        m_addressFormat.setForeground(QColor(0x56, 0x9c, 0xd6));
        m_invalidFormat.setForeground(QColor(0xe0, 0x6c, 0x75));
        m_invalidFormat.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        m_invalidFormat.setUnderlineColor(QColor(0xe0, 0x6c, 0x75));
    }

protected:
    void highlightBlock(const QString &text) override
    {
        const WinEase::Common::HostsLine line =
            WinEase::Common::HostsDocument::parseLine(text, 0);

        if (line.kind == WinEase::Common::HostsLineKind::Comment) {
            setFormat(0, text.size(), m_commentFormat);
            return;
        }
        if (line.kind == WinEase::Common::HostsLineKind::Invalid) {
            // 出错的行直接标红：用户改完一行就看得见反馈，不必等保存
            setFormat(0, text.size(), m_invalidFormat);
            return;
        }
        if (line.kind == WinEase::Common::HostsLineKind::Entry && !line.address.isEmpty()) {
            const int at = text.indexOf(line.address);
            if (at >= 0) {
                setFormat(at, line.address.size(), m_addressFormat);
            }
            const int hash = text.indexOf(QLatin1Char('#'));
            if (hash >= 0) {
                setFormat(hash, text.size() - hash, m_commentFormat);
            }
        }
    }

private:
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_addressFormat;
    QTextCharFormat m_invalidFormat;
};

} // namespace

HostsEditorPlugin::HostsEditorPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString HostsEditorPlugin::id() const
{
    return QStringLiteral("dev.hosts_editor");
}

QString HostsEditorPlugin::name() const
{
    return QStringLiteral("Hosts 快速编辑");
}

QString HostsEditorPlugin::description() const
{
    return QStringLiteral("语法高亮 + 行号校验的 hosts 编辑器：写前自动备份，可一键还原历史版本");
}

QIcon HostsEditorPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Development);
}

WinEase::FeatureCategory HostsEditorPlugin::category() const
{
    return WinEase::FeatureCategory::Development;
}

QStringList HostsEditorPlugin::tags() const
{
    return { QStringLiteral("hosts"), QStringLiteral("DNS"), QStringLiteral("域名"),
             QStringLiteral("开发"), QStringLiteral("hosts编辑") };
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool HostsEditorPlugin::initialize()
{
    logMessage(WinEase::PluginLogLevel::Info,
               QStringLiteral("hosts 编辑器已就绪（文件：%1）")
                   .arg(WinEase::Common::HostsDocument::hostsFilePath()));
    return true;
}

void HostsEditorPlugin::shutdown()
{
    m_document.markClean();
}

bool HostsEditorPlugin::canEnable(QString *reason) const
{
    // 读 hosts 不需要提权；写需要助手，但缺助手时应该**如实报错**而不是不让启用
    Q_UNUSED(reason)
    return true;
}

bool HostsEditorPlugin::onEnable()
{
    reloadFromDisk();
    refreshBackupList();

    Q_EMIT statusMessage(statusText());

    // ⚠ 时序（踩坑 #51 / #57）：onEnable() 期间宿主的"已启用"状态还没落定
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
    return true;
}

void HostsEditorPlugin::onDisable()
{
    // 停用不动任何文件：编辑器只是个界面，真正的副作用只在"保存/还原"那两次点击里
    m_lastEvent = QStringLiteral("已停用（hosts 未被修改）");
    QMetaObject::invokeMethod(this, [this] { refreshPanel(); }, Qt::QueuedConnection);
    refreshPanel();
}

// ---------------------------------------------------------------------------
//  读 / 写
// ---------------------------------------------------------------------------

void HostsEditorPlugin::reloadFromDisk()
{
    const QString path = WinEase::Common::HostsDocument::hostsFilePath();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastEvent = QStringLiteral("无法读取 hosts（%1）：%2").arg(path, file.errorString());
        setLastError(m_lastEvent);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
        if (m_editor != nullptr) {
            m_editor->setPlainText(QString());
        }
        return;
    }

    const QByteArray bytes = file.readAll();
    m_document = WinEase::Common::HostsDocument::parse(bytes);
    m_document.markClean();

    if (m_editor != nullptr) {
        m_editor->setPlainText(m_document.text());
    }
    clearLastError();
    m_lastEvent = QStringLiteral("已读取 hosts（%1 字节，%2）")
                      .arg(bytes.size())
                      .arg(m_document.check().summary());
}

void HostsEditorPlugin::saveToHosts()
{
    if (m_editor.isNull()) {
        return;
    }

    m_document.setText(m_editor->toPlainText());
    const WinEase::Common::HostsCheck check = m_document.check();

    if (!check.ok) {
        // ★ 验收原文："非法语法拒绝保存并指出行号" —— **拒绝就是真的不写**，
        //   连提权请求都不会发出去（"写了再报错"对 hosts 这种文件是不可接受的）
        const QString message = QStringLiteral("未保存：%1").arg(check.errors.first());
        setLastError(message);
        m_lastEvent = message;
        logMessage(WinEase::PluginLogLevel::Warning, message);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), message);
        }
        Q_EMIT statusMessage(message);

        // 把光标跳到第一处错误行（错误信息里给了行号，界面要能跟着走）
        if (!check.errorLines.isEmpty()) {
            const QTextBlock block = m_editor->document()->findBlockByNumber(check.errorLines.first() - 1);
            if (block.isValid()) {
                QTextCursor cursor(block);
                m_editor->setTextCursor(cursor);
                m_editor->centerCursor();
            }
        }
        refreshPanel();
        return;
    }

    WinEase::ElevationService *service = elevation();
    if (service == nullptr) {
        const QString message = QStringLiteral("提权助手不可用，无法写入 hosts"
                                               "（写 `System32\\drivers\\etc\\hosts` 需要管理员权限）");
        setLastError(message);
        m_lastEvent = message;
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), message);
        }
        Q_EMIT statusMessage(message);
        refreshPanel();
        return;
    }

    const QByteArray bytes = m_document.render();
    QVariantMap arguments;
    arguments.insert(QStringLiteral("mode"), QStringLiteral("replace"));
    // base64：字节级精确（含 BOM 与 CRLF）；JSON 文本通道会把 BOM 剥掉
    arguments.insert(QStringLiteral("contentBase64"), QString::fromLatin1(bytes.toBase64()));

    const WinEase::ElevationResult result =
        service->execute(QStringLiteral("writeHosts"), arguments);

    if (!result.isSuccess()) {
        setLastError(result.error);
        m_lastEvent = QStringLiteral("写入 hosts 失败：%1").arg(result.error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), m_lastEvent);
        }
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    clearLastError();
    m_document.markClean();
    const QString backupPath = result.data.value(QStringLiteral("backupPath")).toString();
    m_lastEvent = backupPath.isEmpty()
                      ? QStringLiteral("hosts 已保存（%1 字节 / %2）")
                            .arg(bytes.size())
                            .arg(check.summary())
                      : QStringLiteral("hosts 已保存（%1 字节 / %2），写入前已备份为 %3")
                            .arg(bytes.size())
                            .arg(check.summary())
                            .arg(QFileInfo(backupPath).fileName());
    Q_EMIT statusMessage(m_lastEvent);

    refreshBackupList(); // 助手刚做了一份新备份，列表要跟着更新
}

void HostsEditorPlugin::fillDefaultContent()
{
    if (m_editor.isNull()) {
        return;
    }

    // ⚠ 只填内容、**不落盘**：一键抹掉用户攒了两年的 hosts 是灾难级误操作，
    //   让他先看一眼、再点保存
    const WinEase::Common::HostsDocument defaults =
        WinEase::Common::HostsDocument::parse(WinEase::Common::HostsDocument::defaultContent());
    m_editor->setPlainText(defaults.text());
    m_lastEvent = QStringLiteral("已填入默认内容（仅注释、无映射）—— 点「保存」才会写入 hosts");
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

void HostsEditorPlugin::toggleCommentOnSelection()
{
    if (m_editor.isNull()) {
        return;
    }

    m_document.setText(m_editor->toPlainText());

    QTextCursor cursor = m_editor->textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
    const int end = cursor.hasSelection() ? cursor.selectionEnd() : cursor.position();

    const QTextBlock firstBlock = m_editor->document()->findBlock(start);
    const QTextBlock lastBlock = m_editor->document()->findBlock(end);

    int changed = 0;
    QStringList updated;
    const int firstLine = firstBlock.blockNumber();
    const int lastLine = lastBlock.blockNumber();
    for (int line = firstLine; line <= lastLine; ++line) {
        if (m_document.toggleComment(line + 1)) {
            ++changed;
        }
    }

    if (changed == 0) {
        m_lastEvent = QStringLiteral("没有可切换注释的行（空行不处理）");
        refreshPanel();
        return;
    }

    // 把改过的行写回编辑器（其余行原文不动，保证往返无损）
    for (const WinEase::Common::HostsLine &line : m_document.lines()) {
        if (line.number - 1 >= firstLine && line.number - 1 <= lastLine) {
            const QTextBlock block = m_editor->document()->findBlockByNumber(line.number - 1);
            if (block.isValid()) {
                QTextCursor blockCursor(block);
                blockCursor.select(QTextCursor::LineUnderCursor);
                blockCursor.insertText(line.text);
            }
        }
    }

    m_lastEvent = QStringLiteral("已切换 %1 行的注释状态（未保存）").arg(changed);
    Q_EMIT statusMessage(m_lastEvent);
    refreshPanel();
}

void HostsEditorPlugin::refreshBackupList()
{
    m_backupNames.clear();
    m_backupStamps.clear();

    WinEase::ElevationService *service = elevation();
    if (service != nullptr) {
        QVariantMap arguments;
        arguments.insert(QStringLiteral("action"), QStringLiteral("list"));
        const WinEase::ElevationResult result =
            service->execute(QStringLiteral("hostsBackups"), arguments);
        if (result.isSuccess()) {
            m_backupNames = result.data.value(QStringLiteral("backups")).toStringList();
            m_backupStamps = result.data.value(QStringLiteral("stamps")).toStringList();
        }
    }

    if (!m_backupCombo.isNull()) {
        m_backupCombo->clear();
        for (int index = 0; index < m_backupNames.size(); ++index) {
            const QString stamp = index < m_backupStamps.size() ? m_backupStamps.at(index) : QString();
            m_backupCombo->addItem(QStringLiteral("%1  ·  %2").arg(m_backupNames.at(index), stamp),
                                   m_backupNames.at(index));
        }
    }
    refreshPanel();
}

void HostsEditorPlugin::restoreSelectedBackup()
{
    if (m_backupCombo.isNull()) {
        return;
    }

    // ⚠ 别叫 `name`：它会遮蔽 IFeaturePlugin::name()，于是 svc->notify(name(), ...)
    //    编译不过（"项不会计算为接受 0 个参数的函数"）
    const QString backupName = m_backupCombo->currentData().toString();
    if (backupName.isEmpty()) {
        m_lastEvent = QStringLiteral("没有可还原的备份");
        refreshPanel();
        return;
    }

    WinEase::ElevationService *service = elevation();
    if (service == nullptr) {
        const QString message = QStringLiteral("提权助手不可用，无法还原备份");
        setLastError(message);
        m_lastEvent = message;
        Q_EMIT statusMessage(message);
        refreshPanel();
        return;
    }

    QVariantMap readArguments;
    readArguments.insert(QStringLiteral("action"), QStringLiteral("read"));
    readArguments.insert(QStringLiteral("name"), backupName);
    const WinEase::ElevationResult read = service->execute(QStringLiteral("hostsBackups"),
                                                           readArguments);
    if (!read.isSuccess()) {
        setLastError(read.error);
        m_lastEvent = QStringLiteral("读取备份失败：%1").arg(read.error);
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    const QString encoded = read.data.value(QStringLiteral("contentBase64")).toString();
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());

    // "一键还原"：读出来直接写回去（助手会在写之前再备份一次当前内容，所以还能再撤回来）
    QVariantMap writeArguments;
    writeArguments.insert(QStringLiteral("mode"), QStringLiteral("replace"));
    writeArguments.insert(QStringLiteral("contentBase64"), encoded);
    const WinEase::ElevationResult write = service->execute(QStringLiteral("writeHosts"),
                                                            writeArguments);
    if (!write.isSuccess()) {
        setLastError(write.error);
        m_lastEvent = QStringLiteral("还原失败：%1").arg(write.error);
        logMessage(WinEase::PluginLogLevel::Warning, m_lastEvent);
        if (WinEase::PluginServices *svc = services()) {
            svc->notify(name(), m_lastEvent);
        }
        Q_EMIT statusMessage(m_lastEvent);
        refreshPanel();
        return;
    }

    clearLastError();
    m_document = WinEase::Common::HostsDocument::parse(bytes);
    m_document.markClean();
    if (!m_editor.isNull()) {
        m_editor->setPlainText(m_document.text());
    }
    m_lastEvent = QStringLiteral("已从备份 %1 还原（%2 字节 / %3）")
                      .arg(backupName)
                      .arg(bytes.size())
                      .arg(m_document.check().summary());
    Q_EMIT statusMessage(m_lastEvent);

    refreshBackupList();
}

// ---------------------------------------------------------------------------
//  助手可用性
// ---------------------------------------------------------------------------

WinEase::ElevationService *HostsEditorPlugin::elevation() const
{
    WinEase::PluginServices *svc = services();
    return svc != nullptr ? svc->elevationService() : nullptr;
}

bool HostsEditorPlugin::elevationAvailable() const
{
    WinEase::ElevationService *service = elevation();
    return service != nullptr;
}

// ---------------------------------------------------------------------------
//  界面
// ---------------------------------------------------------------------------

QString HostsEditorPlugin::checkText() const
{
    // 校验的是**编辑区当前内容**（用户一边打字一边就该看到反馈）
    WinEase::Common::HostsDocument snapshot = m_document;
    if (!m_editor.isNull()) {
        snapshot = WinEase::Common::HostsDocument::parse(m_editor->toPlainText().toUtf8());
    }

    const WinEase::Common::HostsCheck check = snapshot.check();
    if (!check.ok) {
        return QStringLiteral("✗ %1").arg(check.errors.join(QStringLiteral("；")));
    }
    if (!check.warnings.isEmpty()) {
        return QStringLiteral("⚠ %1 · %2")
            .arg(check.summary(), check.warnings.join(QStringLiteral("；")));
    }
    return QStringLiteral("✓ 语法检查通过 · %1").arg(check.summary());
}

QString HostsEditorPlugin::statusText() const
{
    return m_lastEvent.isEmpty() ? QStringLiteral("就绪") : m_lastEvent;
}

QWidget *HostsEditorPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("hostsEditorPanel"));
    auto *layout = new QVBoxLayout(widget);

    auto *pathLabel = new QLabel(WinEase::Common::HostsDocument::hostsFilePath(), widget);
    pathLabel->setObjectName(QStringLiteral("hostsPathLabel"));
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pathLabel);

    auto *checkLabel = new QLabel(widget);
    checkLabel->setObjectName(QStringLiteral("hostsCheckLabel"));
    checkLabel->setWordWrap(true);
    layout->addWidget(checkLabel);

    auto *editor = new QPlainTextEdit(widget);
    editor->setObjectName(QStringLiteral("hostsTextEdit"));
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setMinimumHeight(240);
    new HostsHighlighter(editor->document());
    layout->addWidget(editor, 1);

    auto *buttonRow = new QHBoxLayout();
    auto *saveButton = new QPushButton(QStringLiteral("保存"), widget);
    saveButton->setObjectName(QStringLiteral("hostsSaveButton"));
    buttonRow->addWidget(saveButton);
    auto *reloadButton = new QPushButton(QStringLiteral("重新读取"), widget);
    reloadButton->setObjectName(QStringLiteral("hostsReloadButton"));
    buttonRow->addWidget(reloadButton);
    auto *commentButton = new QPushButton(QStringLiteral("切换选中行注释"), widget);
    commentButton->setObjectName(QStringLiteral("hostsCommentButton"));
    buttonRow->addWidget(commentButton);
    auto *defaultButton = new QPushButton(QStringLiteral("填入默认内容"), widget);
    defaultButton->setObjectName(QStringLiteral("hostsDefaultButton"));
    buttonRow->addWidget(defaultButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *backupRow = new QHBoxLayout();
    backupRow->addWidget(new QLabel(QStringLiteral("历史备份："), widget));
    auto *backupCombo = new QComboBox(widget);
    backupCombo->setObjectName(QStringLiteral("hostsBackupCombo"));
    backupCombo->setMinimumWidth(280);
    backupRow->addWidget(backupCombo, 1);
    auto *restoreBackupButton = new QPushButton(QStringLiteral("一键还原"), widget);
    restoreBackupButton->setObjectName(QStringLiteral("hostsRestoreBackupButton"));
    backupRow->addWidget(restoreBackupButton);
    layout->addLayout(backupRow);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setObjectName(QStringLiteral("hostsStatusLabel"));
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *hint = new QLabel(
        QStringLiteral("保存前会做语法校验：**非法记录会被拒绝并指出行号**（连写入请求都不会发出）。\n"
                       "写入经提权助手完成，助手会在**写之前自动备份**当前 hosts；"
                       "「一键还原」列出的就是那些备份（还原前也会再备份一次，所以还能退回来）。\n"
                       "「填入默认内容」只填进编辑区、**不落盘** —— 抹掉多年积累的 hosts 是灾难级误操作。\n"
                       "保存时按原文风格写回（保留缩进、制表符、行尾符与 BOM），不做整体格式化。"),
        widget);
    hint->setObjectName(QStringLiteral("hostsHintLabel"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    m_panel = widget;
    m_pathLabel = pathLabel;
    m_statusLabel = statusLabel;
    m_checkLabel = checkLabel;
    m_editor = editor;
    m_saveButton = saveButton;
    m_reloadButton = reloadButton;
    m_defaultButton = defaultButton;
    m_commentButton = commentButton;
    m_backupCombo = backupCombo;
    m_restoreBackupButton = restoreBackupButton;

    connect(saveButton, &QPushButton::clicked, widget, [this] { saveToHosts(); });
    connect(reloadButton, &QPushButton::clicked, widget, [this] { reloadFromDisk(); refreshPanel(); });
    connect(commentButton, &QPushButton::clicked, widget, [this] { toggleCommentOnSelection(); });
    connect(defaultButton, &QPushButton::clicked, widget, [this] { fillDefaultContent(); });
    connect(restoreBackupButton, &QPushButton::clicked, widget,
            [this] { restoreSelectedBackup(); });
    connect(editor, &QPlainTextEdit::textChanged, widget, [this] {
        if (!m_editor.isNull()) {
            m_document.setText(m_editor->toPlainText());
        }
        refreshPanel();
    });

    if (!m_editor.isNull()) {
        m_editor->setPlainText(m_document.text());
    }
    refreshPanel();
    return widget;
}

void HostsEditorPlugin::refreshPanel()
{
    if (m_panel.isNull()) {
        return;
    }

    const bool running = isEnabled();
    const bool canWrite = running && elevationAvailable();

    if (!m_checkLabel.isNull()) {
        m_checkLabel->setText(checkText());
    }
    if (!m_statusLabel.isNull()) {
        QString text = statusText();
        if (running && !elevationAvailable()) {
            text += QStringLiteral("　（提权助手不可用：可以查看和编辑，但保存/还原会被拒绝）");
        }
        m_statusLabel->setText(text);
    }

    // 助手不可用时**禁用**保存/还原，而不是让用户点了一个注定失败的按钮
    if (!m_saveButton.isNull()) {
        m_saveButton->setEnabled(canWrite);
    }
    if (!m_restoreBackupButton.isNull()) {
        m_restoreBackupButton->setEnabled(canWrite && m_backupCombo != nullptr
                                          && m_backupCombo->count() > 0);
    }
    if (!m_backupCombo.isNull()) {
        m_backupCombo->setEnabled(running && m_backupCombo->count() > 0);
    }
    if (!m_reloadButton.isNull()) {
        m_reloadButton->setEnabled(running);
    }
    if (!m_defaultButton.isNull()) {
        m_defaultButton->setEnabled(running);
    }
    if (!m_commentButton.isNull()) {
        m_commentButton->setEnabled(running);
    }
    if (!m_editor.isNull()) {
        m_editor->setReadOnly(!running);
    }
}
