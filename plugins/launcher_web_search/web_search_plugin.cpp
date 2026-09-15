#include "web_search_plugin.h"

#include "sdk/PluginServices.h"

#include "ClipboardTools.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace ClipboardToolsNs = WinEase::FeaturePlugins::ClipboardTools;

namespace {

/// 前缀解析：`gh: 关键词`、`bd：关键词`（含全角冒号）。
/// 别名限制 1~12 个 ASCII 字母数字，避免把 "http://..." 里的冒号当成前缀
const QRegularExpression &prefixPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^([A-Za-z][A-Za-z0-9]{0,11})\\s*[:：]\\s*(.+)$"));
    return pattern;
}

} // namespace

WebSearchPlugin::WebSearchPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString WebSearchPlugin::id() const
{
    return QStringLiteral("launcher.web_search");
}

QString WebSearchPlugin::name() const
{
    return QStringLiteral("网页快速搜索");
}

QString WebSearchPlugin::description() const
{
    return QStringLiteral("选中文字按一下就用搜索引擎打开；输入「gh: 关键词」可临时换引擎");
}

QIcon WebSearchPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Launcher);
}

WinEase::FeatureCategory WebSearchPlugin::category() const
{
    return WinEase::FeatureCategory::Launcher;
}

QStringList WebSearchPlugin::tags() const
{
    return { QStringLiteral("搜索"), QStringLiteral("搜索引擎"), QStringLiteral("search"),
             QStringLiteral("google"), QStringLiteral("baidu"), QStringLiteral("必应") };
}

bool WebSearchPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence WebSearchPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+S"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool WebSearchPlugin::initialize()
{
    if (WinEase::PluginServices *svc = services()) {
        m_defaultEngine =
            svc->configValue(id(), QStringLiteral("defaultEngine"), QStringLiteral("bing")).toString();
        m_useSelection = svc->configValue(id(), QStringLiteral("useSelection"), true).toBool();
        m_dryRun = svc->configValue(id(), QStringLiteral("dryRun"), false).toBool();
        m_customEngines = svc->configValue(id(), QStringLiteral("engines")).toStringList();
        m_lastUrls = svc->configValue(id(), QStringLiteral("lastUrls")).toStringList();
    }
    return true;
}

void WebSearchPlugin::shutdown()
{
}

bool WebSearchPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral("就绪：%1 搜索（默认引擎 %2%3）")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText),
                                  m_defaultEngine,
                                  m_dryRun ? QStringLiteral(" · 只复制链接") : QString()));
    return true;
}

void WebSearchPlugin::onDisable()
{
    Q_EMIT statusMessage(QStringLiteral("已停用网页搜索"));
}

void WebSearchPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    const QString action = hotkeyId.section(QStringLiteral("::"), 1, 1);
    if (action == QLatin1String("copy")) {
        // "只复制链接"：临时切到 dryRun 语义（不改配置，只影响这一次）
        const bool previous = m_dryRun;
        m_dryRun = true;
        search(QString(), QStringLiteral("复制搜索链接"));
        m_dryRun = previous;
        return;
    }
    if (action == QLatin1String("default")) {
        search(QString(), QStringLiteral("搜索"));
        return;
    }
    // 其余动作名就是引擎别名（bing / baidu / google / github …）
    search(action, QStringLiteral("用 %1 搜索").arg(action));
}

// ---------------------------------------------------------------------------
//  引擎
// ---------------------------------------------------------------------------

QList<WebSearchPlugin::Engine> WebSearchPlugin::engines() const
{
    QList<Engine> result;
    const auto appendDefaults = [&result] {
        result.append({ { QStringLiteral("bing"), QStringLiteral("bi") }, QStringLiteral("必应"),
                        QStringLiteral("https://www.bing.com/search?q=%1") });
        result.append({ { QStringLiteral("bd"), QStringLiteral("baidu") }, QStringLiteral("百度"),
                        QStringLiteral("https://www.baidu.com/s?wd=%1") });
        result.append({ { QStringLiteral("gg"), QStringLiteral("google") }, QStringLiteral("Google"),
                        QStringLiteral("https://www.google.com/search?q=%1") });
        result.append({ { QStringLiteral("gh"), QStringLiteral("github") }, QStringLiteral("GitHub"),
                        QStringLiteral("https://github.com/search?q=%1") });
        result.append({ { QStringLiteral("learn"), QStringLiteral("ms") }, QStringLiteral("微软文档"),
                        QStringLiteral("https://learn.microsoft.com/zh-cn/search/?terms=%1") });
    };

    if (m_customEngines.isEmpty()) {
        appendDefaults();
        return result;
    }

    // 用户自定义了引擎表 → 以它为准（格式 "别名[,别名…]|名称|URL模板"）
    for (const QString &line : m_customEngines) {
        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() < 3) {
            continue;
        }
        Engine engine;
        const QStringList rawAliases = parts.at(0).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &alias : rawAliases) {
            const QString trimmed = alias.trimmed();
            if (!trimmed.isEmpty()) {
                engine.aliases.append(trimmed);
            }
        }
        engine.name = parts.at(1).trimmed();
        engine.urlTemplate = parts.mid(2).join(QLatin1Char('|')).trimmed();
        if (!engine.aliases.isEmpty() && engine.urlTemplate.contains(QLatin1String("%1"))) {
            result.append(engine);
        }
    }
    if (result.isEmpty()) {
        appendDefaults(); // 配置写坏了也要能用
    }
    return result;
}

bool WebSearchPlugin::findEngine(const QString &alias, Engine *engineOut) const
{
    for (const Engine &engine : engines()) {
        for (const QString &candidate : engine.aliases) {
            if (candidate.compare(alias, Qt::CaseInsensitive) == 0) {
                if (engineOut != nullptr) {
                    *engineOut = engine;
                }
                return true;
            }
        }
    }
    return false;
}

QString WebSearchPlugin::stripPrefix(const QString &text, QString *aliasOut)
{
    const QRegularExpressionMatch match = prefixPattern().match(text);
    if (match.hasMatch()) {
        if (aliasOut != nullptr) {
            *aliasOut = match.captured(1);
        }
        return match.captured(2).trimmed();
    }
    return text.trimmed();
}

QString WebSearchPlugin::buildUrl(const Engine &engine, const QString &query)
{
    // 查询词做百分号编码：空格 → %20、中文 → UTF-8 百分号序列
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(query));
    QString url = engine.urlTemplate;
    url.replace(QLatin1String("%1"), encoded);
    return url;
}

// ---------------------------------------------------------------------------
//  主流程
// ---------------------------------------------------------------------------

bool WebSearchPlugin::search(const QString &forceAlias, const QString &label)
{
    const ClipboardToolsNs::AcquireResult acquired = ClipboardToolsNs::acquireText(m_useSelection);
    if (!acquired.ok) {
        setLastError(acquired.error);
        Q_EMIT statusMessage(QStringLiteral("%1失败：%2").arg(label, acquired.error));
        return false;
    }

    QString prefixAlias;
    const QString query = stripPrefix(acquired.text, &prefixAlias);

    const QString alias = !forceAlias.isEmpty() ? forceAlias
                                                : (!prefixAlias.isEmpty() ? prefixAlias : m_defaultEngine);
    Engine engine;
    if (!findEngine(alias, &engine)) {
        // 前缀看起来像别名但引擎表里没有 —— 当成普通关键词搜，别把用户的话吃掉
        Engine fallback;
        if (!findEngine(m_defaultEngine, &fallback)) {
            setLastError(QStringLiteral("引擎表为空"));
            Q_EMIT statusMessage(QStringLiteral("%1失败：没有可用引擎").arg(label));
            return false;
        }
        // 注意这里用的是**整段原文**（含"看起来像前缀"的那截）：用户可能真的想搜 "gh:xxx"
        const QString url = buildUrl(fallback, acquired.text.trimmed());
        if (m_dryRun) {
            ClipboardToolsNs::setText(url);
        } else {
            QDesktopServices::openUrl(QUrl(url));
        }
        Q_EMIT statusMessage(QStringLiteral("%1：没有叫「%2」的引擎，已用 %3 打开（%4）")
                                 .arg(label, prefixAlias, fallback.name, url));
        return true;
    }

    if (query.isEmpty()) {
        setLastError(QStringLiteral("没有可搜索的文字"));
        Q_EMIT statusMessage(QStringLiteral("%1失败：没有可搜索的文字").arg(label));
        return false;
    }

    const QString url = buildUrl(engine, query);

    if (m_dryRun) {
        if (!ClipboardToolsNs::setText(url)) {
            setLastError(QStringLiteral("写入剪贴板失败"));
            Q_EMIT statusMessage(QStringLiteral("%1：链接已生成，但写入剪贴板失败").arg(label));
            return false;
        }
        Q_EMIT statusMessage(QStringLiteral("%1：链接已复制（%2 · %3）").arg(label, engine.name, url));
    } else {
        if (!QDesktopServices::openUrl(QUrl(url))) {
            setLastError(QStringLiteral("打开浏览器失败：%1").arg(url));
            Q_EMIT statusMessage(QStringLiteral("%1失败：打不开浏览器（%2）").arg(label, url));
            return false;
        }
        Q_EMIT statusMessage(QStringLiteral("%1：已用 %2 打开（%3）").arg(label, engine.name, url));
    }

    // 记最近 5 条，便于排查"引擎配错了"这类问题
    m_lastUrls.prepend(url);
    while (m_lastUrls.size() > 5) {
        m_lastUrls.removeLast();
    }
    if (WinEase::PluginServices *svc = services()) {
        svc->setConfigValue(id(), QStringLiteral("lastUrls"), m_lastUrls);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *WebSearchPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *engineRow = new QHBoxLayout();
    engineRow->addWidget(new QLabel(QStringLiteral("默认引擎："), widget));
    auto *engineBox = new QComboBox(widget);
    for (const Engine &engine : engines()) {
        engineBox->addItem(QStringLiteral("%1（%2）").arg(engine.name, engine.aliasText()),
                           engine.primaryAlias());
    }
    const int currentIndex = engineBox->findData(m_defaultEngine);
    engineBox->setCurrentIndex(currentIndex >= 0 ? currentIndex : 0);
    engineRow->addWidget(engineBox);
    engineRow->addStretch(1);
    layout->addLayout(engineRow);

    auto *selectionBox = new QCheckBox(QStringLiteral("优先搜索「当前选中文本」（关掉则只搜剪贴板内容）"), widget);
    selectionBox->setChecked(m_useSelection);
    layout->addWidget(selectionBox);
    auto *dryRunBox = new QCheckBox(QStringLiteral("只复制搜索链接，不打开浏览器"), widget);
    dryRunBox->setChecked(m_dryRun);
    layout->addWidget(dryRunBox);

    auto *testRow = new QHBoxLayout();
    auto *testEdit = new QLineEdit(widget);
    testEdit->setPlaceholderText(QStringLiteral("试试输入 gh: qt widgets"));
    testRow->addWidget(testEdit);
    auto *testButton = new QPushButton(QStringLiteral("生成链接"), widget);
    testRow->addWidget(testButton);
    layout->addLayout(testRow);

    auto *urlOutput = new QPlainTextEdit(widget);
    urlOutput->setReadOnly(true);
    urlOutput->setMaximumHeight(70);
    layout->addWidget(urlOutput);

    auto *hint = new QLabel(QStringLiteral("快捷键：%1 搜索 · Ctrl+Shift+Alt+S 只复制链接\n"
                                          "前缀语法：<别名>: 关键词（如 gh: / bd: / gg:），中英文冒号都认。\n"
                                          "引擎表可在配置文件里改：键 engines，每行「别名[,别名…]|名称|URL模板」，"
                                          "模板用 %1 占位（第一个别名用于展示）。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    QObject::connect(engineBox, &QComboBox::currentIndexChanged, widget, [this, engineBox](int) {
        m_defaultEngine = engineBox->currentData().toString();
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("defaultEngine"), m_defaultEngine);
        }
    });
    QObject::connect(selectionBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_useSelection = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("useSelection"), checked);
        }
    });
    QObject::connect(dryRunBox, &QCheckBox::toggled, widget, [this](bool checked) {
        m_dryRun = checked;
        if (WinEase::PluginServices *svc = services()) {
            svc->setConfigValue(id(), QStringLiteral("dryRun"), checked);
        }
    });
    QObject::connect(testButton, &QPushButton::clicked, widget, [this, testEdit, urlOutput] {
        QString prefixAlias;
        const QString query = stripPrefix(testEdit->text(), &prefixAlias);
        const QString alias = prefixAlias.isEmpty() ? m_defaultEngine : prefixAlias;
        Engine engine;
        if (!findEngine(alias, &engine)) {
            urlOutput->setPlainText(QStringLiteral("没有叫「%1」的引擎").arg(alias));
            return;
        }
        urlOutput->setPlainText(buildUrl(engine, query));
    });

    return widget;
}
