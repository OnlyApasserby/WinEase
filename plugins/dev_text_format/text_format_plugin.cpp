#include "text_format_plugin.h"

#include "sdk/PluginServices.h"

#include "ClipboardTools.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

// 这两个是命名空间，必须用命名空间别名（using X::Y 只对类/函数有效）
namespace ClipboardTools = WinEase::FeaturePlugins::ClipboardTools;
namespace TextTools = WinEase::FeaturePlugins::TextTools;

namespace {

/// 内容类型的人话名字（状态文本里用）
QString kindName(const QString &text)
{
    if (TextTools::looksLikeJson(text)) {
        return QStringLiteral("JSON");
    }
    if (TextTools::looksLikeXml(text)) {
        return QStringLiteral("XML");
    }
    return QStringLiteral("文本");
}

} // namespace

TextFormatPlugin::TextFormatPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString TextFormatPlugin::id() const
{
    return QStringLiteral("dev.text_format");
}

QString TextFormatPlugin::name() const
{
    return QStringLiteral("JSON / XML 格式化");
}

QString TextFormatPlugin::description() const
{
    return QStringLiteral("选中一段 JSON/XML 按一下，就地排好版；出错时告诉你在第几行第几列");
}

QIcon TextFormatPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Development);
}

WinEase::FeatureCategory TextFormatPlugin::category() const
{
    return WinEase::FeatureCategory::Development;
}

QStringList TextFormatPlugin::tags() const
{
    return { QStringLiteral("json"), QStringLiteral("xml"), QStringLiteral("格式化"),
             QStringLiteral("format"), QStringLiteral("美化"), QStringLiteral("压缩"),
             QStringLiteral("text") };
}

bool TextFormatPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence TextFormatPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+J"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool TextFormatPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_indent = qBound(0, svc->configValue(id(), QStringLiteral("indent"), 2).toInt(), 8);
        m_useSelection = svc->configValue(id(), QStringLiteral("useSelection"), true).toBool();
        m_pasteBack = svc->configValue(id(), QStringLiteral("pasteBack"), true).toBool();
    }
    return true;
}

void TextFormatPlugin::shutdown()
{
    // 没有跨次状态需要保存：原文与结果都在剪贴板/前台窗口里
}

bool TextFormatPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    if (!svc->registerHotkey(id(), QStringLiteral("minify"),
                             QKeySequence(QStringLiteral("Ctrl+Shift+Alt+J")),
                             QStringLiteral("WinEase：压缩 JSON/XML"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「压缩」快捷键注册失败（可能被占用）"));
    }

    Q_EMIT statusMessage(QStringLiteral("缩进 %1 空格 · %2")
                             .arg(m_indent)
                             .arg(m_useSelection ? QStringLiteral("优先取选中文本")
                                                 : QStringLiteral("只处理剪贴板内容")));
    return true;
}

void TextFormatPlugin::onDisable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("minify"));
    }
    Q_EMIT statusMessage(QStringLiteral("已停用格式化"));
}

void TextFormatPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("minify")) {
        runPipeline(QStringLiteral("minify"), QStringLiteral("压缩"));
    } else if (action == QLatin1String("json")) {
        runPipeline(QStringLiteral("json"), QStringLiteral("格式化 JSON"));
    } else if (action == QLatin1String("xml")) {
        runPipeline(QStringLiteral("xml"), QStringLiteral("格式化 XML"));
    } else if (action == QLatin1String("escape")) {
        runPipeline(QStringLiteral("escape"), QStringLiteral("转义"));
    } else if (action == QLatin1String("unescape")) {
        runPipeline(QStringLiteral("unescape"), QStringLiteral("去转义"));
    } else {
        runPipeline(QStringLiteral("default"), QStringLiteral("格式化")); // "default"
    }
}

// ---------------------------------------------------------------------------
//  流水线
// ---------------------------------------------------------------------------

void TextFormatPlugin::runPipeline(const QString &action, const QString &label)
{
    const ClipboardTools::AcquireResult acquired = ClipboardTools::acquireText(m_useSelection);
    if (!acquired.ok) {
        setLastError(acquired.error);
        Q_EMIT statusMessage(QStringLiteral("%1失败：%2").arg(label, acquired.error));
        return;
    }

    TextTools::Result result;
    if (action == QLatin1String("minify")) {
        result = TextTools::minifyAuto(acquired.text);
    } else if (action == QLatin1String("json")) {
        result = TextTools::formatJson(acquired.text, m_indent);
    } else if (action == QLatin1String("xml")) {
        result = TextTools::formatXml(acquired.text, m_indent);
    } else if (action == QLatin1String("escape")) {
        result = TextTools::escapeText(acquired.text);
    } else if (action == QLatin1String("unescape")) {
        result = TextTools::unescapeText(acquired.text);
    } else {
        result = TextTools::formatAuto(acquired.text, m_indent);
    }

    if (!result.ok) {
        // 关键：失败时**不写剪贴板**，用户的原文原样留着，才能照着行列号去改
        setLastError(result.errorText());
        logMessage(WinEase::PluginLogLevel::Info, QStringLiteral("%1失败：%2").arg(label, result.errorText()));
        Q_EMIT statusMessage(QStringLiteral("%1失败：%2").arg(label, result.errorText()));
        if (WinEase::PluginServices *svc = services()) {
            // 用户此刻的注意力多半在被编辑的窗口上，弹个气泡才看得见
            svc->notify(name(), QStringLiteral("%1失败\n%2").arg(label, result.errorText()));
        }
        return;
    }

    if (!ClipboardTools::setText(result.output)) {
        setLastError(QStringLiteral("写入剪贴板失败（可能被其它程序占用）"));
        Q_EMIT statusMessage(QStringLiteral("%1成功，但写入剪贴板失败").arg(label));
        return;
    }

    if (m_pasteBack) {
        // 就地替换：把结果粘贴回原来的位置。失败不影响"已复制到剪贴板"这件事，
        // 所以只在状态文本里如实说明，不报错。
        ClipboardTools::pasteBack();
    }

    const QString kind = (action == QLatin1String("escape") || action == QLatin1String("unescape"))
                             ? QStringLiteral("文本")
                             : kindName(acquired.text);
    Q_EMIT statusMessage(QStringLiteral("%1%2完成：%3 → %4 字符%5")
                             .arg(acquired.fromSelection ? QStringLiteral("已就地") : QString())
                             .arg(label, kind)
                             .arg(acquired.text.size())
                             .arg(result.output.size())
                             .arg(m_pasteBack && acquired.fromSelection ? QStringLiteral("，已替换选中")
                                                                        : QString()));
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *TextFormatPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(QStringLiteral("输入（也可以直接按快捷键处理选中文本）："), widget));
    auto *input = new QPlainTextEdit(widget);
    input->setPlaceholderText(QStringLiteral(R"({"b":1,"a":[1,2,{"c":true}]})"));
    input->setMinimumHeight(90);
    layout->addWidget(input);

    auto *buttonRow = new QHBoxLayout();
    auto *formatButton = new QPushButton(QStringLiteral("格式化"), widget);
    auto *minifyButton = new QPushButton(QStringLiteral("压缩"), widget);
    auto *escapeButton = new QPushButton(QStringLiteral("转义"), widget);
    auto *unescapeButton = new QPushButton(QStringLiteral("去转义"), widget);
    buttonRow->addWidget(formatButton);
    buttonRow->addWidget(minifyButton);
    buttonRow->addWidget(escapeButton);
    buttonRow->addWidget(unescapeButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *output = new QPlainTextEdit(widget);
    output->setReadOnly(true);
    output->setMinimumHeight(120);
    layout->addWidget(output);

    auto *errorLabel = new QLabel(widget);
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);

    auto *copyButton = new QPushButton(QStringLiteral("复制结果"), widget);
    layout->addWidget(copyButton, 0, Qt::AlignLeft);

    auto *queryRow = new QHBoxLayout();
    queryRow->addWidget(new QLabel(QStringLiteral("JSON 路径查询："), widget));
    auto *pathEdit = new QLineEdit(widget);
    pathEdit->setPlaceholderText(QStringLiteral("$.a[2].c"));
    queryRow->addWidget(pathEdit);
    auto *queryButton = new QPushButton(QStringLiteral("查询"), widget);
    queryRow->addWidget(queryButton);
    layout->addLayout(queryRow);

    auto *settingsRow = new QHBoxLayout();
    settingsRow->addWidget(new QLabel(QStringLiteral("缩进："), widget));
    auto *indentBox = new QSpinBox(widget);
    indentBox->setRange(0, 8);
    indentBox->setSuffix(QStringLiteral(" 空格"));
    indentBox->setValue(m_indent);
    settingsRow->addWidget(indentBox);
    settingsRow->addStretch(1);
    layout->addLayout(settingsRow);

    auto *selectionBox = new QCheckBox(QStringLiteral("优先处理「当前选中文本」（关掉则只处理剪贴板）"), widget);
    selectionBox->setChecked(m_useSelection);
    layout->addWidget(selectionBox);
    auto *pasteBox = new QCheckBox(QStringLiteral("处理完就地把结果替换回原处（发 Ctrl+V）"), widget);
    pasteBox->setChecked(m_pasteBack);
    layout->addWidget(pasteBox);
    layout->addStretch(1);

    const auto showResult = [output, errorLabel](const TextTools::Result &result) {
        if (result.ok) {
            output->setPlainText(result.output);
            errorLabel->setText(QString());
        } else {
            errorLabel->setText(QStringLiteral("转换失败：%1（原文未改动）").arg(result.errorText()));
        }
    };

    QObject::connect(formatButton, &QPushButton::clicked, widget, [this, input, showResult] {
        showResult(TextTools::formatAuto(input->toPlainText(), m_indent));
    });
    QObject::connect(minifyButton, &QPushButton::clicked, widget, [input, showResult] {
        showResult(TextTools::minifyAuto(input->toPlainText()));
    });
    QObject::connect(escapeButton, &QPushButton::clicked, widget, [input, showResult] {
        showResult(TextTools::escapeText(input->toPlainText()));
    });
    QObject::connect(unescapeButton, &QPushButton::clicked, widget, [input, showResult] {
        showResult(TextTools::unescapeText(input->toPlainText()));
    });
    QObject::connect(queryButton, &QPushButton::clicked, widget, [input, pathEdit, showResult] {
        showResult(TextTools::queryJson(input->toPlainText(), pathEdit->text()));
    });
    QObject::connect(copyButton, &QPushButton::clicked, widget, [output] {
        ClipboardTools::setText(output->toPlainText());
    });
    QObject::connect(indentBox, &QSpinBox::valueChanged, widget, [this](int value) {
        m_indent = value;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("indent"), value);
        }
    });
    QObject::connect(selectionBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_useSelection = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("useSelection"), checked);
        }
    });
    QObject::connect(pasteBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_pasteBack = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("pasteBack"), checked);
        }
    });

    return widget;
}
