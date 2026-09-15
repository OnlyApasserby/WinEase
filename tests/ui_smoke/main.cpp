// ============================================================================
//  ui_smoke —— 主界面呈现验收：**卡片只放两行** + **标识/版本/完整说明进帮助**
//
//  对应用户要求（逐条都要有可执行证据）：
//    1. "插件卡片的功能介绍改为固定两行和完整文本 Tooltip"
//       → 第一节（真实插件批量）+ 第二节（**人造超长描述样本**，确定性覆盖截断路径）。
//    2. "标识和版本号以及完整实现全部迁移到帮助（新增「关于插件」栏目）"
//       → 第三节：栏目存在、每个插件一段、段里带标识与版本号；第一节断言卡片上不再出现它们。
//
//  ⚠ 为什么要有"人造超长描述样本"（第二节）：真实插件的描述**碰巧**都很短的话，
//     第一节就退化成"短文本原样显示"，压根没测到"压到两行"这条路径 —— 而它正是本次的核心。
//     所以额外造一个描述一定超过两行的 IFeaturePlugin 桩（不注册进管理器，只喂卡片）。
//
//  运行方式：.\build\bin\ui_smoke.exe    # 0 = 全部通过；2 = 前置条件不成立
// ============================================================================

#include "core/AdminHelper.h"
#include "core/PluginManager.h"
#include "core/SettingsManager.h"
#include "sdk/FeatureCategory.h"
#include "sdk/IFeaturePlugin.h"
#include "sdk/PluginServices.h"
#include "ui/FeatureCard.h"
#include "ui/HelpDialog.h"
#include "ui/ThemeManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFontMetrics>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>

#include <windows.h>

using WinEase::FeatureCard;
using WinEase::FeatureCategory;
using WinEase::HelpDialog;
using WinEase::IFeaturePlugin;
using WinEase::PluginManager;
using WinEase::PluginServices;
using WinEase::SettingsManager;

namespace {

constexpr const char *kGreen = "\x1b[32m";
constexpr const char *kRed = "\x1b[31m";
constexpr const char *kGray = "\x1b[90m";
constexpr const char *kCyan = "\x1b[36m";
constexpr const char *kReset = "\x1b[0m";

void printLine(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
    std::fputc('\n', stdout);
}

class Reporter
{
public:
    void section(const QString &title)
    {
        printLine(QStringLiteral("\n%1== %2 ==%3")
                      .arg(QString::fromUtf8(kCyan), title, QString::fromUtf8(kReset)));
    }

    void check(bool ok, const QString &name, const QString &detail = QString())
    {
        if (ok) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3")
                          .arg(QString::fromUtf8(kGreen), QString::fromUtf8(kReset), name));
        } else {
            ++m_failed;
            printLine(QStringLiteral("  %1[失败]%2 %3")
                          .arg(QString::fromUtf8(kRed), QString::fromUtf8(kReset), name));
        }
        if (!detail.isEmpty()) {
            printLine(QStringLiteral("         %1%2%3")
                          .arg(QString::fromUtf8(kGray), detail, QString::fromUtf8(kReset)));
        }
    }

    void note(const QString &text)
    {
        printLine(QStringLiteral("  %1· %2%3")
                      .arg(QString::fromUtf8(kGray), text, QString::fromUtf8(kReset)));
    }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    int m_passed = 0;
    int m_failed = 0;
};

void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

/// 省略号（U+2026）：描述被截断时行尾的那个字符
const QChar kEllipsis = QChar(0x2026);

/// 描述占几行。★ 故意用**与实现不同的 API**（这里 QFontMetrics 换行排版，
/// 实现是 QTextLayout 逐行 layout），避免"自己证明自己"
int wrappedLineCount(const QString &text, const QFontMetrics &metrics, int width)
{
    if (text.isEmpty() || width <= 0) {
        return 0;
    }
    const QRect needed = metrics.boundingRect(QRect(0, 0, width, 0), Qt::TextWordWrap, text);
    return std::max(1, qRound(static_cast<qreal>(needed.height()) / std::max(1, metrics.lineSpacing())));
}

/// 卡片上所有"给人看的文字"（标签 + 按钮）
QString joinedTextOf(const QWidget *root)
{
    QStringList texts;
    const QList<QLabel *> labels = root->findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        if (!label->text().isEmpty()) {
            texts.append(label->text());
        }
    }
    const QList<QAbstractButton *> buttons = root->findChildren<QAbstractButton *>();
    for (const QAbstractButton *button : buttons) {
        if (!button->text().isEmpty()) {
            texts.append(button->text());
        }
    }
    return texts.join(QChar(' '));
}

/// 宿主服务桩：够 PluginManager 与插件初始化用即可，不做任何真事
class StubServices : public PluginServices
{
public:
    QVariant configValue(const QString &, const QString &, const QVariant &defaultValue) const override
    {
        return defaultValue;
    }
    void setConfigValue(const QString &, const QString &, const QVariant &) override {}
    void syncConfig() override {}
    void log(const QString &, WinEase::PluginLogLevel, const QString &) override {}
    void notify(const QString &, const QString &) override {}
    void setTrayBadge(const QString &, const QString &, const QString &, const QString &) override {}
    bool registerHotkey(const QString &, const QString &, const QKeySequence &, const QString &) override
    {
        return true;
    }
    void unregisterHotkey(const QString &, const QString &) override {}
    WinEase::HookService *hookService() const override { return nullptr; }
    WinEase::OverlayHost *overlayHost() const override { return nullptr; }
    WinEase::ElevationService *elevationService() const override { return nullptr; }
    QString appVersion() const override { return QStringLiteral("ui-smoke"); }
    bool isElevated() const override { return WinEase::Admin::isProcessElevated(); }
    QString configFilePath() const override { return SettingsManager::instance().configFilePath(); }
    QString logDirectory() const override { return QDir::tempPath(); }
    QWidget *mainWindow() const override { return nullptr; }
};

/// ★ 人造样本：完整描述**一定**超过两行
class LongDescriptionPlugin : public IFeaturePlugin
{
public:
    QString id() const override { return QStringLiteral("test.long_description"); }
    QString name() const override { return QStringLiteral("超长描述样本"); }
    QString version() const override { return QStringLiteral("9.9.9-sample"); }
    QString author() const override { return QStringLiteral("ui_smoke"); }

    QString description() const override
    {
        return QStringLiteral("这是一段专门用来把排版逼到「必须截断」分支的描述文本：它的长度远远超过"
                              "卡片给描述留的两行空间，因此卡片上必须出现省略号，而 Tooltip 与"
                              "「帮助 → 关于插件」里必须能读到没有被截断的原文。这段话没有任何业务含义，"
                              "存在的唯一目的就是让「压到两行」这件事被真正执行一次。");
    }

    QString detailedDescription() const override
    {
        return QStringLiteral("**它是什么**：ui_smoke 自检里的人造样本，不参与主程序运行。\n"
                              "**为什么要有它**：真实插件描述碰巧都很短时，「压到两行 + 省略号」"
                              "这条路径永远不会被执行 —— 测试会假通过。\n"
                              "**这条说明的用途**：验证帮助页会原样展示插件写的完整说明。");
    }

    QIcon icon() const override { return QIcon(); }
    FeatureCategory category() const override { return FeatureCategory::Unknown; }
    bool initialize() override { return true; }
    void shutdown() override {}
};

/// 一张卡片的测量结果（宿主可见 → 卡片拿到真实宽度 → 描述真的被压行）
struct CardMeasurement
{
    QString shown;
    QString toolTip;
    QString texts;
    int labelWidth = 0;
    int labelHeight = 0;
    int wantedHeight = 0;
    int shownLines = 0;
    int fullLines = 0;
};

CardMeasurement measureCard(IFeaturePlugin *plugin, QVBoxLayout *hostLayout)
{
    CardMeasurement result;

    auto *card = new FeatureCard(plugin);
    hostLayout->addWidget(card);
    card->show();
    pump(60); // 等布局落定：卡片的描述压行发生在 resizeEvent 里

    if (auto *description = card->findChild<QLabel *>(QStringLiteral("CardDescription"))) {
        const QFontMetrics metrics(description->fontMetrics());
        result.shown = description->text();
        result.toolTip = description->toolTip();
        result.labelWidth = description->width();
        result.labelHeight = description->height();
        result.wantedHeight = metrics.lineSpacing() * 2;
        result.shownLines = wrappedLineCount(result.shown, metrics, result.labelWidth);
        result.fullLines = wrappedLineCount(plugin->description(), metrics, result.labelWidth);
    }
    result.texts = joinedTextOf(card);

    hostLayout->removeWidget(card);
    delete card;
    return result;
}

} // namespace

// ============================================================================

int main(int argc, char *argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ui_smoke"));
    // 让「关于 WinEase」页里的版本号可判定（主程序在 main.cpp 里设置真实版本）
    QApplication::setApplicationVersion(QStringLiteral("ui-smoke-0.0.1"));

    // 配置只写临时目录：自检不得污染用户真正的 %APPDATA%/WinEase
    const QString tempAppData = QDir::temp().filePath(
        QStringLiteral("winease_ui_smoke_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(tempAppData);
    qputenv("APPDATA", tempAppData.toLocal8Bit());
    SettingsManager::instance().initialize();

    // 与主程序一致地套主题：卡片的字体/行距来自全局样式表，
    // 不套主题量出来的换行结果跟用户看到的不是一回事
    WinEase::Ui::applyForcedDarkTheme(app);
    pump(50);

    Reporter reporter;
    printLine(QStringLiteral("\n──────────── 主界面呈现验收（卡片两行 · 帮助栏目）────────────"));

    // 被测量的卡片挂在这个宿主窗口里（可见但不激活，不抢焦点）
    QWidget host;
    host.setAttribute(Qt::WA_ShowWithoutActivating, true);
    host.resize(FeatureCard::cardSize().width() + 24, FeatureCard::cardSize().height() + 24);
    auto *hostLayout = new QVBoxLayout(&host);
    hostLayout->setContentsMargins(12, 12, 12, 12);
    host.show();
    pump(150);

    // ========================================================================
    reporter.section(QStringLiteral("一、功能卡片：介绍固定两行 + 完整文本进 Tooltip"));
    // ========================================================================

    StubServices services;
    PluginManager manager;
    manager.setServices(&services);

    const QString pluginDir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    const int loaded = manager.loadPlugins(pluginDir);
    const QList<IFeaturePlugin *> plugins = manager.plugins();

    reporter.check(loaded >= 1 && !plugins.isEmpty(),
                   QStringLiteral("真实插件目录已加载（前置条件）"),
                   QStringLiteral("%1 中加载了 %2 个插件").arg(pluginDir).arg(loaded));
    if (plugins.isEmpty()) {
        printLine(QStringLiteral("  %1前置条件不成立：没有插件可供测量，本次结论无效%2")
                      .arg(QString::fromUtf8(kRed), QString::fromUtf8(kReset)));
        host.hide();
        QDir(tempAppData).removeRecursively();
        return 2;
    }

    int lineViolations = 0;
    int tooltipViolations = 0;
    int heightViolations = 0;
    int idLeaks = 0;
    int versionLeaks = 0;
    int truncatedSamples = 0; // 完整描述本身就超过两行的插件数
    int narrowSamples = 0;    // 宽度还没落定、本次测量不成立的卡片数
    QStringList lineDetail;
    QStringList tooltipDetail;
    QStringList heightDetail;
    QStringList idDetail;
    QStringList versionDetail;

    for (IFeaturePlugin *plugin : plugins) {
        const CardMeasurement measured = measureCard(plugin, hostLayout);
        const QString id = plugin->id();
        const QString version = plugin->version();

        if (measured.labelWidth <= 40) {
            ++narrowSamples; // 还没被 QSS/布局赋予真实宽度，这次测量不作数
            continue;
        }
        if (measured.fullLines > 2) {
            ++truncatedSamples;
        }

        if (measured.shownLines > 2) {
            ++lineViolations;
            lineDetail.append(QStringLiteral("%1(%2 行)").arg(id).arg(measured.shownLines));
        }
        if (measured.toolTip != plugin->description()) {
            ++tooltipViolations;
            tooltipDetail.append(id);
        }
        if (measured.labelHeight != measured.wantedHeight) {
            ++heightViolations;
            heightDetail.append(QStringLiteral("%1(%2px≠%3px)")
                                    .arg(id)
                                    .arg(measured.labelHeight)
                                    .arg(measured.wantedHeight));
        }
        // 标识/版本号本来就写在插件自己的描述里，不算"卡片泄露"
        if (measured.texts.contains(id, Qt::CaseSensitive)
            && !plugin->description().contains(id, Qt::CaseSensitive)) {
            ++idLeaks;
            idDetail.append(id);
        }
        if (!version.isEmpty() && measured.texts.contains(version, Qt::CaseSensitive)
            && !plugin->description().contains(version, Qt::CaseSensitive)) {
            ++versionLeaks;
            versionDetail.append(QStringLiteral("%1(%2)").arg(id, version));
        }
    }

    reporter.check(narrowSamples == 0,
                   QStringLiteral("所有卡片的描述标签都拿到了真实宽度（测量前提成立）"),
                   narrowSamples == 0
                       ? QStringLiteral("无卡片在宽度未落定时被测量")
                       : QStringLiteral("%1 张卡片宽度异常，已从本次统计剔除").arg(narrowSamples));

    reporter.check(lineViolations == 0,
                   QStringLiteral("描述在卡片上**最多两行**（超出的被压行）"),
                   lineViolations == 0
                       ? QStringLiteral("共检查 %1 张卡片，无一张超过两行").arg(plugins.size())
                       : QStringLiteral("超过两行：%1").arg(lineDetail.join(u"; ")));

    reporter.check(heightViolations == 0,
                   QStringLiteral("描述区高度**固定两行**（描述短的卡片也不会矮一截）"),
                   heightViolations == 0
                       ? QStringLiteral("每张卡片的描述区高度 = 2 × 行距")
                       : QStringLiteral("高度不符：%1").arg(heightDetail.join(u"; ")));

    reporter.check(tooltipViolations == 0,
                   QStringLiteral("Tooltip 给出的是**完整描述原文**"),
                   tooltipViolations == 0
                       ? QStringLiteral("逐张比对 toolTip 与 plugin->description()，全部一致")
                       : QStringLiteral("不一致：%1").arg(tooltipDetail.join(u',')));

    reporter.check(idLeaks == 0,
                   QStringLiteral("卡片上**不再出现插件标识**（已迁到帮助）"),
                   idLeaks == 0 ? QStringLiteral("无卡片显示 id")
                                : QStringLiteral("仍显示 id 的卡片：%1").arg(idDetail.join(u',')));

    reporter.check(versionLeaks == 0,
                   QStringLiteral("卡片上**不再出现版本号**（已迁到帮助）"),
                   versionLeaks == 0 ? QStringLiteral("无卡片显示版本号")
                                     : QStringLiteral("仍显示版本号：%1")
                                           .arg(versionDetail.join(u"; ")));

    reporter.note(QStringLiteral("本次 %1 个插件中，完整描述本身就超过两行的有 %2 个"
                                 "（这些卡片的描述确实被截断了）")
                      .arg(plugins.size())
                      .arg(truncatedSamples));

    // ========================================================================
    reporter.section(QStringLiteral("二、★ 超长描述样本：确定性覆盖「压到两行」这条路径"));
    // ========================================================================

    LongDescriptionPlugin stub;
    const CardMeasurement stubCard = measureCard(&stub, hostLayout);

    reporter.check(stubCard.labelWidth > 40 && stubCard.fullLines > 2,
                   QStringLiteral("样本的完整描述按卡片宽度需要超过两行（前置条件）"),
                   QStringLiteral("标签宽度 %1px，完整描述约 %2 行（%3 字）")
                       .arg(stubCard.labelWidth)
                       .arg(stubCard.fullLines)
                       .arg(stub.description().size()));

    const QString keptText = stubCard.shown.left(stubCard.shown.size() > 0 ? stubCard.shown.size() - 1 : 0);
    reporter.check(stubCard.shownLines <= 2
                       && stubCard.shown.endsWith(kEllipsis)
                       && stub.description().startsWith(keptText)
                       && stubCard.shown != stub.description(),
                   QStringLiteral("超长描述被压到两行、以省略号收尾、且是原文前缀"),
                   QStringLiteral("显示 %1 字 / %2 行；原文 %3 字")
                       .arg(stubCard.shown.size())
                       .arg(stubCard.shownLines)
                       .arg(stub.description().size()));

    reporter.check(stubCard.toolTip == stub.description(),
                   QStringLiteral("超长描述的 Tooltip 仍是**未被截断的完整原文**"),
                   QStringLiteral("Tooltip %1 字 / 原文 %2 字")
                       .arg(stubCard.toolTip.size())
                       .arg(stub.description().size()));

    reporter.check(!stubCard.texts.contains(stub.id(), Qt::CaseSensitive)
                       && !stubCard.texts.contains(stub.version(), Qt::CaseSensitive),
                   QStringLiteral("超长样本卡片上同样不出现标识 / 版本号"),
                   QStringLiteral("卡片文字中不含 %1 与 %2").arg(stub.id(), stub.version()));

    // ========================================================================
    reporter.section(QStringLiteral("三、帮助：「关于插件」栏目"));
    // ========================================================================

    HelpDialog dialog(&manager);
    dialog.setAttribute(Qt::WA_ShowWithoutActivating, true);
    dialog.show();
    pump(200);

    auto *topicList = dialog.findChild<QListWidget *>(QStringLiteral("HelpTopicList"));
    auto *pages = dialog.findChild<QStackedWidget *>(QStringLiteral("HelpPages"));

    const bool topicsOk = topicList != nullptr && topicList->count() == 2
                          && topicList->item(0)->text().contains(QStringLiteral("WinEase"))
                          && topicList->item(1)->text().contains(QStringLiteral("插件"));
    reporter.check(topicsOk,
                   QStringLiteral("左栏出现两个栏目：关于 WinEase / 关于插件"),
                   topicList != nullptr
                       ? QStringLiteral("栏目 = %1 / %2")
                             .arg(topicList->item(0)->text(), topicList->item(1)->text())
                       : QStringLiteral("没有找到栏目列表"));

    dialog.showTopic(HelpDialog::AboutPlugins);
    pump(150);

    reporter.check(pages != nullptr
                       && pages->currentIndex() == static_cast<int>(HelpDialog::AboutPlugins)
                       && topicList->currentRow() == static_cast<int>(HelpDialog::AboutPlugins),
                   QStringLiteral("切到「关于插件」真的换了内容页（不是只列在左边）"),
                   pages != nullptr ? QStringLiteral("当前页索引 = %1，选中栏目 = %2")
                                          .arg(pages->currentIndex())
                                          .arg(topicList->currentRow())
                                    : QStringLiteral("没有找到内容页容器"));

    auto *pluginsPage = dialog.findChild<QWidget *>(QStringLiteral("HelpPluginsPage"));
    const QList<QLabel *> entries =
        pluginsPage != nullptr
            ? pluginsPage->findChildren<QLabel *>(QStringLiteral("HelpPluginEntry"))
            : QList<QLabel *>();

    reporter.check(entries.size() == plugins.size(),
                   QStringLiteral("「关于插件」里**每个插件都有一段**"),
                   QStringLiteral("段落 %1 段 / 插件 %2 个").arg(entries.size()).arg(plugins.size()));

    QString entryText;
    for (const QLabel *entry : entries) {
        entryText += entry->text();
        entryText += QChar('\n');
    }

    QStringList missingId;
    QStringList missingVersion;
    QStringList missingDetailed;
    int detailedCount = 0;
    for (IFeaturePlugin *plugin : plugins) {
        // 与帮助页一致地走"缓存读"（崩溃隔离后也安全）
        const QString id = manager.idOf(plugin);
        const QString version = manager.versionOf(plugin);
        if (!id.isEmpty() && !entryText.contains(id, Qt::CaseSensitive)) {
            missingId.append(id);
        }
        if (!version.isEmpty() && !entryText.contains(version, Qt::CaseSensitive)) {
            missingVersion.append(version);
        }
        const QString detailed = manager.detailedDescriptionOf(plugin);
        if (detailed.isEmpty()) {
            continue;
        }
        ++detailedCount;
        // 完整说明是多段文本，帮助页会转成 <br/>：只校验首段是否出现
        const QString firstParagraph = detailed.split(QLatin1Char('\n')).value(0).trimmed();
        if (!firstParagraph.isEmpty() && !entryText.contains(firstParagraph)) {
            missingDetailed.append(id);
        }
    }

    reporter.check(missingId.isEmpty(),
                   QStringLiteral("每个插件的**标识**都出现在「关于插件」里"),
                   missingId.isEmpty() ? QStringLiteral("%1 个插件的标识全部可查").arg(plugins.size())
                                       : QStringLiteral("缺少：%1").arg(missingId.join(u',')));

    reporter.check(missingVersion.isEmpty(),
                   QStringLiteral("每个插件的**版本号**都出现在「关于插件」里"),
                   missingVersion.isEmpty()
                       ? QStringLiteral("%1 个插件的版本号全部可查").arg(plugins.size())
                       : QStringLiteral("缺少：%1").arg(missingVersion.join(u',')));

    reporter.check(detailedCount >= 1 && missingDetailed.isEmpty(),
                   QStringLiteral("插件写的**完整说明**（detailedDescription）也被展示出来"),
                   QStringLiteral("写了完整说明的插件 %1 个；缺失 %2 个%3")
                       .arg(detailedCount)
                       .arg(missingDetailed.size())
                       .arg(missingDetailed.isEmpty()
                                ? QString()
                                : QStringLiteral("（%1）").arg(missingDetailed.join(u','))));

    // ---- 「关于 WinEase」栏目 ----
    dialog.showTopic(HelpDialog::AboutApp);
    pump(150);

    auto *aboutPage = dialog.findChild<QWidget *>(QStringLiteral("HelpAboutPage"));
    const QString aboutText =
        aboutPage != nullptr ? joinedTextOf(aboutPage) : QString();
    const QString versionString = QApplication::applicationVersion();
    const QString pluginDirText = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");

    reporter.check(aboutText.contains(versionString) && aboutText.contains(pluginDirText),
                   QStringLiteral("「关于 WinEase」栏目仍写着版本号与插件目录"),
                   QStringLiteral("含版本 %1 = %2；含插件目录 = %3")
                       .arg(versionString)
                       .arg(aboutText.contains(versionString) ? QStringLiteral("是")
                                                             : QStringLiteral("否"),
                            aboutText.contains(pluginDirText) ? QStringLiteral("是")
                                                              : QStringLiteral("否")));

    dialog.hide();
    host.hide();
    pump(80);

    // ========================================================================
    //  收尾：卸载插件（插件在卸载阶段不得再被调用）、清理临时配置目录
    // ========================================================================
    manager.unloadAll();
    SettingsManager::instance().sync();

    const int total = reporter.passed() + reporter.failed();
    printLine(QString());
    printLine(QStringLiteral("──────────── 汇总 ────────────"));
    printLine(QStringLiteral("  通过 %1 / %2").arg(reporter.passed()).arg(total));
    if (reporter.failed() > 0) {
        printLine(QStringLiteral("  %1失败 %2%3")
                      .arg(QString::fromUtf8(kRed))
                      .arg(reporter.failed())
                      .arg(QString::fromUtf8(kReset)));
    }
    printLine(QStringLiteral("  结果：%1")
                  .arg(reporter.failed() == 0 ? QStringLiteral("全部通过")
                                              : QStringLiteral("存在失败项")));

    QDir(tempAppData).removeRecursively();
    return reporter.failed() == 0 ? 0 : 1;
}
