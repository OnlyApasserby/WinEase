#include "encoder_plugin.h"

#include "sdk/PluginServices.h"

#include "ClipboardTools.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

// 这两个是命名空间，必须用命名空间别名（using X::Y 只对类/函数有效）
namespace ClipboardTools = WinEase::FeaturePlugins::ClipboardTools;
namespace TextTools = WinEase::FeaturePlugins::TextTools;

EncoderPlugin::EncoderPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString EncoderPlugin::id() const
{
    return QStringLiteral("dev.encoder");
}

QString EncoderPlugin::name() const
{
    return QStringLiteral("编码转换");
}

QString EncoderPlugin::description() const
{
    return QStringLiteral("Base64 / URL 编解码与常用哈希摘要，选中一段就能算");
}

QIcon EncoderPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Development);
}

WinEase::FeatureCategory EncoderPlugin::category() const
{
    return WinEase::FeatureCategory::Development;
}

QStringList EncoderPlugin::tags() const
{
    return { QStringLiteral("base64"), QStringLiteral("url"), QStringLiteral("编码"),
             QStringLiteral("解码"), QStringLiteral("hash"), QStringLiteral("md5"),
             QStringLiteral("sha256"), QStringLiteral("摘要") };
}

bool EncoderPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence EncoderPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+E"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool EncoderPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_useSelection = svc->configValue(id(), QStringLiteral("useSelection"), true).toBool();
        m_pasteBack = svc->configValue(id(), QStringLiteral("pasteBack"), true).toBool();
    }
    return true;
}

void EncoderPlugin::shutdown()
{
}

bool EncoderPlugin::onEnable()
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }

    if (!svc->registerHotkey(id(), QStringLiteral("hash"),
                             QKeySequence(QStringLiteral("Ctrl+Shift+Alt+E")),
                             QStringLiteral("WinEase：计算选中文本的四种哈希摘要"))) {
        logMessage(WinEase::PluginLogLevel::Warning,
                   QStringLiteral("「哈希摘要」快捷键注册失败（可能被占用）"));
    }

    Q_EMIT statusMessage(QStringLiteral("就绪：默认 Base64 编码，%1")
                             .arg(m_useSelection ? QStringLiteral("优先取选中文本")
                                                 : QStringLiteral("只处理剪贴板内容")));
    return true;
}

void EncoderPlugin::onDisable()
{
    if (WinEase::PluginServices *svc = services()) {
        svc->unregisterHotkey(id(), QStringLiteral("hash"));
    }
    Q_EMIT statusMessage(QStringLiteral("已停用编码转换"));
}

void EncoderPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("decode")) {
        runPipeline(QStringLiteral("decode"), QStringLiteral("Base64 解码"));
    } else if (action == QLatin1String("url")) {
        runPipeline(QStringLiteral("url"), QStringLiteral("URL 编码"));
    } else if (action == QLatin1String("urldecode")) {
        runPipeline(QStringLiteral("urldecode"), QStringLiteral("URL 解码"));
    } else if (action == QLatin1String("hash")) {
        runPipeline(QStringLiteral("hash"), QStringLiteral("哈希摘要"));
    } else {
        runPipeline(QStringLiteral("default"), QStringLiteral("Base64 编码")); // "default"
    }
}

// ---------------------------------------------------------------------------
//  流水线
// ---------------------------------------------------------------------------

void EncoderPlugin::runPipeline(const QString &action, const QString &label)
{
    const ClipboardTools::AcquireResult acquired = ClipboardTools::acquireText(m_useSelection);
    if (!acquired.ok) {
        setLastError(acquired.error);
        Q_EMIT statusMessage(QStringLiteral("%1失败：%2").arg(label, acquired.error));
        return;
    }

    QString output;
    if (action == QLatin1String("decode")) {
        const TextTools::Result decoded = TextTools::fromBase64(acquired.text);
        if (!decoded.ok) {
            // 失败不动剪贴板：原文留着，用户才能对照着看哪里多了一个字符
            setLastError(decoded.errorText());
            Q_EMIT statusMessage(QStringLiteral("%1失败：%2").arg(label, decoded.errorText()));
            if (WinEase::PluginServices *svc = services()) {
                svc->notify(name(), QStringLiteral("%1失败\n%2").arg(label, decoded.errorText()));
            }
            return;
        }
        output = decoded.output;
    } else if (action == QLatin1String("url")) {
        output = TextTools::toUrlEncoded(acquired.text);
    } else if (action == QLatin1String("urldecode")) {
        const TextTools::Result decoded = TextTools::fromUrlDecoded(acquired.text);
        if (!decoded.ok) {
            setLastError(decoded.errorText());
            Q_EMIT statusMessage(QStringLiteral("%1失败：%2").arg(label, decoded.errorText()));
            return;
        }
        output = decoded.output;
    } else if (action == QLatin1String("hash")) {
        output = TextTools::allHashes(acquired.text).join(QLatin1Char('\n'));
    } else {
        output = TextTools::toBase64(acquired.text, false);
    }

    if (!ClipboardTools::setText(output)) {
        setLastError(QStringLiteral("写入剪贴板失败（可能被其它程序占用）"));
        Q_EMIT statusMessage(QStringLiteral("%1成功，但写入剪贴板失败").arg(label));
        return;
    }
    if (m_pasteBack) {
        ClipboardTools::pasteBack();
    }

    // 注意占位符个数必须与 .arg() 次数一致：
    // 之前这里写成 "%1完成：%2 → …"（4 个占位符配 5 个参数），
    // 结果"已就地"占了前缀、label 掉到"完成："后面，末位参数则被静默丢弃
    Q_EMIT statusMessage(QStringLiteral("%1%2完成：%3 → %4 字符%5")
                             .arg(acquired.fromSelection ? QStringLiteral("已就地") : QString())
                             .arg(label)
                             .arg(acquired.text.size())
                             .arg(output.size())
                             .arg(m_pasteBack && acquired.fromSelection ? QStringLiteral("，已替换选中")
                                                                        : QString()));
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *EncoderPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(QStringLiteral("输入："), widget));
    auto *input = new QPlainTextEdit(widget);
    input->setPlaceholderText(QStringLiteral("hello 世界"));
    input->setMinimumHeight(80);
    layout->addWidget(input);

    auto *buttonRow = new QHBoxLayout();
    auto *base64Button = new QPushButton(QStringLiteral("Base64 编码"), widget);
    auto *decodeButton = new QPushButton(QStringLiteral("Base64 解码"), widget);
    auto *urlButton = new QPushButton(QStringLiteral("URL 编码"), widget);
    auto *urlDecodeButton = new QPushButton(QStringLiteral("URL 解码"), widget);
    buttonRow->addWidget(base64Button);
    buttonRow->addWidget(decodeButton);
    buttonRow->addWidget(urlButton);
    buttonRow->addWidget(urlDecodeButton);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    auto *output = new QPlainTextEdit(widget);
    output->setReadOnly(true);
    output->setMinimumHeight(90);
    layout->addWidget(output);

    auto *errorLabel = new QLabel(widget);
    errorLabel->setWordWrap(true);
    layout->addWidget(errorLabel);

    auto *hashGroup = new QGroupBox(QStringLiteral("哈希摘要"), widget);
    auto *hashLayout = new QVBoxLayout(hashGroup);
    auto *hashOutput = new QPlainTextEdit(hashGroup);
    hashOutput->setReadOnly(true);
    hashOutput->setMinimumHeight(96);
    hashLayout->addWidget(hashOutput);
    auto *hashButton = new QPushButton(QStringLiteral("计算 MD5 / SHA-1 / SHA-256 / SHA-512"), hashGroup);
    hashLayout->addWidget(hashButton, 0, Qt::AlignLeft);
    layout->addWidget(hashGroup);

    auto *copyRow = new QHBoxLayout();
    auto *copyButton = new QPushButton(QStringLiteral("复制上面的结果"), widget);
    auto *copyHashButton = new QPushButton(QStringLiteral("复制哈希"), widget);
    copyRow->addWidget(copyButton);
    copyRow->addWidget(copyHashButton);
    copyRow->addStretch(1);
    layout->addLayout(copyRow);

    auto *selectionBox = new QCheckBox(QStringLiteral("优先处理「当前选中文本」（关掉则只处理剪贴板）"), widget);
    selectionBox->setChecked(m_useSelection);
    layout->addWidget(selectionBox);
    auto *pasteBox = new QCheckBox(QStringLiteral("处理完就地把结果替换回原处（发 Ctrl+V）"), widget);
    pasteBox->setChecked(m_pasteBack);
    layout->addWidget(pasteBox);

    auto *hint = new QLabel(QStringLiteral("快捷键：%1 Base64 编码 · Ctrl+Shift+Alt+E 哈希摘要\n"
                                          "Base64 解码对 URL 安全字母表、换行、缺省 '=' 一律宽容处理。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto showText = [output, errorLabel](const QString &text) {
        output->setPlainText(text);
        errorLabel->setText(QString());
    };
    const auto showResult = [output, errorLabel](const TextTools::Result &result) {
        if (result.ok) {
            output->setPlainText(result.output);
            errorLabel->setText(QString());
        } else {
            errorLabel->setText(QStringLiteral("转换失败：%1").arg(result.errorText()));
        }
    };

    QObject::connect(base64Button, &QPushButton::clicked, widget, [input, showText] {
        showText(TextTools::toBase64(input->toPlainText(), false));
    });
    QObject::connect(decodeButton, &QPushButton::clicked, widget, [input, showResult] {
        showResult(TextTools::fromBase64(input->toPlainText()));
    });
    QObject::connect(urlButton, &QPushButton::clicked, widget, [input, showText] {
        showText(TextTools::toUrlEncoded(input->toPlainText()));
    });
    QObject::connect(urlDecodeButton, &QPushButton::clicked, widget, [input, showResult] {
        showResult(TextTools::fromUrlDecoded(input->toPlainText()));
    });
    QObject::connect(hashButton, &QPushButton::clicked, widget, [input, hashOutput] {
        hashOutput->setPlainText(TextTools::allHashes(input->toPlainText()).join(QLatin1Char('\n')));
    });
    QObject::connect(copyButton, &QPushButton::clicked, widget, [output] {
        ClipboardTools::setText(output->toPlainText());
    });
    QObject::connect(copyHashButton, &QPushButton::clicked, widget, [hashOutput] {
        ClipboardTools::setText(hashOutput->toPlainText());
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
