#include "ui/HelpDialog.h"

#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "sdk/FeatureCategory.h"
#include "sdk/IFeaturePlugin.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace WinEase {

namespace {

/// 标签页里的长文本一律用这个（与项目其它对话框一个风格：QLabel + wordWrap）
QLabel *makeParagraph(QWidget *parent, const QString &html)
{
    auto *label = new QLabel(html, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    return label;
}

/// 把插件写来的纯文本转成 HTML：转义 + 换行变 <br/>（插件里写的是 `\n` 分段）
QString toHtmlParagraphs(const QString &text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
}

} // namespace

HelpDialog::HelpDialog(PluginManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_pluginManager(manager)
{
    setObjectName(QStringLiteral("HelpDialog"));
    setWindowTitle(QStringLiteral("帮助"));
    resize(780, 580);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(14, 14, 14, 14);
    rootLayout->setSpacing(10);

    auto *bodyLayout = new QHBoxLayout();
    bodyLayout->setSpacing(12);

    // ---------------- 左侧：栏目 ----------------
    m_topicList = new QListWidget(this);
    m_topicList->setObjectName(QStringLiteral("HelpTopicList"));
    m_topicList->setFixedWidth(150);
    m_topicList->addItem(QStringLiteral("关于 WinEase"));
    m_topicList->addItem(QStringLiteral("关于插件"));
    bodyLayout->addWidget(m_topicList, 0);

    // ---------------- 右侧：栏目内容 ----------------
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("HelpPages"));
    m_pages->addWidget(createAboutPage());
    m_pages->addWidget(createPluginsPage());
    bodyLayout->addWidget(m_pages, 1);

    rootLayout->addLayout(bodyLayout, 1);

    auto *footerLayout = new QHBoxLayout();
    footerLayout->addStretch(1);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setObjectName(QStringLiteral("HelpCloseButton"));
    closeButton->setDefault(true);
    footerLayout->addWidget(closeButton);
    rootLayout->addLayout(footerLayout);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_topicList, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

    m_topicList->setCurrentRow(AboutApp);
}

void HelpDialog::showTopic(Topic topic)
{
    if (m_topicList != nullptr) {
        m_topicList->setCurrentRow(static_cast<int>(topic));
    }
}

QWidget *HelpDialog::createAboutPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("HelpAboutPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(8);

    layout->addWidget(makeParagraph(
        page,
        QStringLiteral("<h3>WinEase %1</h3>"
                       "<p>Windows 易用性增强工具集，插件化架构。</p>")
            .arg(QApplication::applicationVersion().toHtmlEscaped())));

    layout->addWidget(makeParagraph(
        page,
        QStringLiteral("<p>插件目录：%1<br/>配置目录：%2</p>")
            .arg((QApplication::applicationDirPath() + QStringLiteral("/plugins")).toHtmlEscaped(),
                 SettingsManager::instance().dataDirectory().toHtmlEscaped())));

    layout->addWidget(makeParagraph(
        page,
        QStringLiteral("<p style=\"color:#8C8C8C;\">"
                       "功能卡片上只显示**两行**介绍（鼠标停上去看完整描述）；"
                       "每个功能的<b>标识、版本号与完整说明</b>都在左侧的「关于插件」里。<br/>"
                       "功能太多时用主界面右上角的搜索框，按功能名 / 标识 / 标签都能搜到。"
                       "</p>")));

    layout->addStretch(1);
    return page;
}

QWidget *HelpDialog::createPluginsPage()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("HelpPluginsPage"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("HelpPluginsContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(8, 4, 8, 4);
    contentLayout->setSpacing(14);

    QList<IFeaturePlugin *> plugins = (m_pluginManager != nullptr) ? m_pluginManager->plugins()
                                                                   : QList<IFeaturePlugin *>();
    // 按"分类 → 名称"排序：帮助页是给人查的，顺序得稳定、成组
    std::sort(plugins.begin(), plugins.end(),
              [this](IFeaturePlugin *left, IFeaturePlugin *right) {
                  if (m_pluginManager == nullptr) {
                      return left < right;
                  }
                  const FeatureCategory leftCategory = m_pluginManager->categoryOf(left);
                  const FeatureCategory rightCategory = m_pluginManager->categoryOf(right);
                  if (leftCategory != rightCategory) {
                      return static_cast<int>(leftCategory) < static_cast<int>(rightCategory);
                  }
                  return m_pluginManager->nameOf(left) < m_pluginManager->nameOf(right);
              });

    contentLayout->addWidget(makeParagraph(
        content,
        QStringLiteral("<p style=\"color:#8C8C8C;\">共 %1 个功能。"
                       "「标识」用于配置文件里的段落名与快捷键前缀，"
                       "「完整说明」里写着这个功能<b>做不到什么、为什么</b>。</p>")
            .arg(plugins.size())));

    for (IFeaturePlugin *plugin : plugins) {
        QString name;
        QString id;
        QString version;
        QString author;
        QString description;
        QString detailed;
        bool failed = false;
        QString failureReason;

        if (m_pluginManager != nullptr) {
            // ★ 全部走缓存读（插件崩溃被隔离后仍然安全，见 PluginManager.h）
            name = m_pluginManager->nameOf(plugin);
            id = m_pluginManager->idOf(plugin);
            version = m_pluginManager->versionOf(plugin);
            author = m_pluginManager->authorOf(plugin);
            description = m_pluginManager->descriptionOf(plugin);
            detailed = m_pluginManager->detailedDescriptionOf(plugin);
            failed = m_pluginManager->isPluginFailed(id);
            if (failed) {
                failureReason = m_pluginManager->pluginFailureReason(id);
            }
        }

        QString html;
        html += QStringLiteral("<h3 style=\"margin:0;\">%1"
                               "<span style=\"font-size:11px; font-weight:normal; color:#8C8C8C;\">"
                               "&nbsp;&nbsp;标识 %2 · 版本 %3%4</span></h3>")
                    .arg(name.isEmpty() ? QStringLiteral("（未知功能）") : name.toHtmlEscaped(),
                         id.isEmpty() ? QStringLiteral("（无）") : id.toHtmlEscaped(),
                         version.isEmpty() ? QStringLiteral("（无）") : version.toHtmlEscaped(),
                         author.isEmpty() ? QString()
                                          : QStringLiteral(" · 作者 %1")
                                                .arg(author.toHtmlEscaped()));
        if (!description.isEmpty()) {
            html += QStringLiteral("<p style=\"margin:4px 0 0 0;\">%1</p>")
                        .arg(toHtmlParagraphs(description));
        }
        if (!detailed.isEmpty()) {
            html += QStringLiteral("<p style=\"margin:6px 0 0 0; color:#B4B4B4;\">%1</p>")
                        .arg(toHtmlParagraphs(detailed));
        }
        if (failed) {
            html += QStringLiteral("<p style=\"margin:6px 0 0 0; color:#E06C75;\">"
                                   "⚠ 这个功能已因崩溃被隔离：%1</p>")
                        .arg(toHtmlParagraphs(failureReason));
        }

        auto *entry = makeParagraph(content, html);
        entry->setObjectName(QStringLiteral("HelpPluginEntry"));
        contentLayout->addWidget(entry);
    }

    contentLayout->addStretch(1);
    scroll->setWidget(content);
    pageLayout->addWidget(scroll);
    return page;
}

} // namespace WinEase
