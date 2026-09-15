// ============================================================================
//  batch_move_group.cpp —— 批量移动文件（file.batch_move）的自检
//
//  这一组的断言分三层，缺一不可：
//    ① **引擎**（plugins/common/BatchMoveEngine，与插件编同一份源码）：
//         正则非法 / 目录穿越 / 目标重名 → 必须拦下并给中文原因；
//    ② **预演不动盘**：dryRun 前后磁盘快照必须一模一样；
//    ③ **真落盘 + 真撤销**：通过**插件的设置面板**（不是直接调引擎）点按钮，
//         然后回磁盘上核对"文件真的搬走了 / 真的搬回来了"。
//         这样验的是"插件 → 引擎"整条链，而不是只验引擎自己。
// ============================================================================

#include "batch_move_group.h"

#include "BatchMoveEngine.h"
#include "app/core/PluginManager.h"
#include "sdk/IFeaturePlugin.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QWidget>

#include <climits>

namespace FeatureSmoke {

namespace {

// 引擎住在 WinEase::FeaturePlugins::BatchMove 里；自检自己也在 FeatureSmoke 命名空间，
// 短名找不到嵌套命名空间，这里给它一个**命名空间别名**（比把每处都写全限定更不容易写漏）。
namespace BatchMove = WinEase::FeaturePlugins::BatchMove;

using BatchMove::ConflictPolicy;
using BatchMove::ItemStatus;
using BatchMove::Options;
using BatchMove::Plan;

const QString kPluginId = QStringLiteral("file.batch_move");

/// 建一个源目录 → 写一批文件 → 返回绝对路径列表
QStringList makeFiles(const QString &directory, const QStringList &names)
{
    QDir().mkpath(directory);
    QStringList paths;
    for (const QString &name : names) {
        // ⚠ 名字里可能带子目录（"sub/a.pdf"），必须先把父目录建出来，
        //   否则 QFile::open 直接失败、文件根本没落到盘上 ——
        //   而后面的断言是"文件真的存在"，这类"夹具自己没造出前提"最费时间
        const QString path = QDir(directory).absoluteFilePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write("winEase batch move probe\n");
            file.close();
        }
        paths.append(BatchMove::normalizePath(path));
    }
    return paths;
}

/// 目录里的文件名集合（用于比对"磁盘上到底有什么"）
QStringList fileNamesIn(const QString &directory)
{
    QStringList names;
    const QDir dir(directory);
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        names.append(info.fileName());
    }
    return names;
}

Options baseOptions(const QString &source, const QString &target, const QString &pattern)
{
    Options options;
    options.sourceDirectory = source;
    options.targetDirectory = target;
    options.pattern = pattern;
    return options;
}

/// 通过插件拿到设置面板，并按 objectName 找控件
QWidget *settingsPageOf(WinEase::PluginManager &manager, QString *error)
{
    for (WinEase::IFeaturePlugin *plugin : manager.plugins()) {
        if (plugin->id() != kPluginId) {
            continue;
        }
        QWidget *page = plugin->createSettingsWidget(nullptr);
        if (page != nullptr) {
            return page;
        }
        *error = QStringLiteral("createSettingsWidget() 返回空");
        return nullptr;
    }
    *error = QStringLiteral("插件列表里没有 file.batch_move");
    return nullptr;
}

} // namespace

int runBatchMoveGroupTests(Reporter &reporter,
                           WinEase::PluginManager &manager,
                           StubServices &services)
{
    const int failuresAtStart = reporter.failures();
    Q_UNUSED(services);

    reporter.info(QStringLiteral("---- file.batch_move 批量移动（正则匹配）----"));

    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        reporter.check(false, QStringLiteral("批量移动前置：能建临时目录"));
        return reporter.failures() - failuresAtStart;
    }
    const QString source = sandbox.filePath(QStringLiteral("source"));
    const QString target = sandbox.filePath(QStringLiteral("target"));
    QDir().mkpath(target);

    const QStringList sources = makeFiles(source, {QStringLiteral("2024-01.pdf"),
                                                   QStringLiteral("2024-02.pdf"),
                                                   QStringLiteral("note.txt"),
                                                   QStringLiteral("sub/2024-03.pdf")});

    // =======================================================================
    //  ① 引擎：危险输入必须被拦下
    // =======================================================================
    {
        Options invalid = baseOptions(source, target, QStringLiteral("([unclosed"));
        const Plan plan = BatchMove::buildPlan(sources, invalid);
        reporter.check(!plan.valid && !plan.error.isEmpty() && plan.readyCount() == 0,
                       QStringLiteral("正则非法时计划**直接作废**（不降级、不静默忽略），"
                                      "并给出中文原因"),
                       plan.error);
    }
    {
        Options traversal = baseOptions(source, target, QStringLiteral(".*"));
        traversal.nameTemplate = QStringLiteral("../逃逸-{n}.txt");
        const Plan plan = BatchMove::buildPlan(sources, traversal);
        bool allBlocked = plan.valid;
        int blocked = 0;
        for (const auto &item : plan.items) {
            if (item.status == ItemStatus::InvalidName) {
                ++blocked;
            }
        }
        allBlocked = allBlocked && blocked == plan.items.size() && plan.readyCount() == 0;
        reporter.check(allBlocked,
                       QStringLiteral("★ 命名模板里塞 ../ 想做目录穿越 → 计划里每一项都被标成"
                                      "「目标名非法」，一条 Ready 都没有"),
                       QStringLiteral("拦下 %1 / 共 %2 项").arg(blocked).arg(plan.items.size()));
    }
    {
        // 重名 + 默认策略（跳过）：目标里预置一个同名文件
        makeFiles(target, {QStringLiteral("2024-01.pdf")});
        Options options = baseOptions(source, target, QStringLiteral("^2024.*\\.pdf$"));
        const Plan plan = BatchMove::buildPlan(sources, options);
        int skipped = 0;
        int ready = 0;
        for (const auto &item : plan.items) {
            if (item.status == ItemStatus::Conflict) {
                ++skipped;
            } else if (item.status == ItemStatus::Ready) {
                ++ready;
            }
        }
        reporter.check(plan.valid && skipped == 1 && ready == 2 && !plan.warnings.isEmpty(),
                       QStringLiteral("目标已存在且策略=跳过 → 该项标 Conflict 并给出提示，"
                                      "其余两项仍可移动（**不静默覆盖**）"),
                       QStringLiteral("跳过 %1 / 待移动 %2").arg(skipped).arg(ready));
        QFile::remove(QDir(target).absoluteFilePath(QStringLiteral("2024-01.pdf")));
    }

    // =======================================================================
    //  ② 预演不动盘
    // =======================================================================
    {
        Options options = baseOptions(source, target, QStringLiteral("^2024.*\\.pdf$"));
        options.recursive = true;
        options.keepStructure = true;
        const Plan plan = BatchMove::buildPlan(BatchMove::listFiles(source, true), options);

        const QStringList before = fileNamesIn(target);
        const BatchMove::ApplyResult preview = BatchMove::applyPlan(plan, true);
        const QStringList after = fileNamesIn(target);

        reporter.check(plan.valid && preview.moved == 3 && before == after,
                       QStringLiteral("★ 预演（dryRun）：报告会移动 3 个文件，"
                                      "但**目标目录一个文件都没多**"),
                       QStringLiteral("预演前 %1 / 预演后 %2")
                           .arg(before.join(QStringLiteral(",")),
                                after.join(QStringLiteral(","))));
        reporter.check(preview.records.isEmpty(),
                       QStringLiteral("预演不产出撤销记录（没动盘就没有可撤销的东西）"));
    }

    // =======================================================================
    //  ③ 真落盘 + 真撤销（走插件的设置面板）
    // =======================================================================
    {
        QString error;
        QWidget *page = settingsPageOf(manager, &error);
        reporter.check(page != nullptr,
                       QStringLiteral("批量移动插件能提供设置面板（跨 DLL 边界不崩）"), error);
        if (page != nullptr) {
            auto *sourceEdit =
                page->findChild<QLineEdit *>(QStringLiteral("batchMoveSourceEdit"));
            auto *targetEdit =
                page->findChild<QLineEdit *>(QStringLiteral("batchMoveTargetEdit"));
            auto *patternEdit =
                page->findChild<QLineEdit *>(QStringLiteral("batchMovePatternEdit"));
            auto *recursiveCheck =
                page->findChild<QCheckBox *>(QStringLiteral("batchMoveRecursiveCheck"));
            auto *structureCheck =
                page->findChild<QCheckBox *>(QStringLiteral("batchMoveStructureCheck"));
            auto *planButton =
                page->findChild<QPushButton *>(QStringLiteral("batchMovePlanButton"));
            auto *dryRunButton =
                page->findChild<QPushButton *>(QStringLiteral("batchMoveDryRunButton"));
            auto *applyButton =
                page->findChild<QPushButton *>(QStringLiteral("batchMoveApplyButton"));
            auto *undoButton =
                page->findChild<QPushButton *>(QStringLiteral("batchMoveUndoButton"));
            auto *planView =
                page->findChild<QPlainTextEdit *>(QStringLiteral("batchMovePlanView"));

            const bool controlsOk = sourceEdit != nullptr && targetEdit != nullptr
                                    && patternEdit != nullptr && recursiveCheck != nullptr
                                    && structureCheck != nullptr && planButton != nullptr
                                    && dryRunButton != nullptr && applyButton != nullptr
                                    && undoButton != nullptr && planView != nullptr;
            reporter.check(controlsOk,
                           QStringLiteral("批量移动设置面板控件齐全（源/目标/正则/递归/结构/"
                                          "计划/预演/执行/撤销/计划表）"));
            if (controlsOk) {
                sourceEdit->setText(source);
                targetEdit->setText(target);
                patternEdit->setText(QStringLiteral("^2024.*\\.pdf$"));
                recursiveCheck->setChecked(true);
                structureCheck->setChecked(true);

                planButton->click();
                const QString planText = planView->toPlainText();
                reporter.check(planText.contains(QStringLiteral("待移动"))
                                   || planText.contains(QStringLiteral("→")),
                               QStringLiteral("点「生成计划」后计划表里列出了会搬到哪"),
                               planText.left(120));

                dryRunButton->click();
                reporter.check(fileNamesIn(target).isEmpty(),
                               QStringLiteral("点「预演」后磁盘上依然没有新文件"),
                               fileNamesIn(target).join(QStringLiteral(",")));

                applyButton->click();
                const QStringList moved = fileNamesIn(target);
                const bool movedOk =
                    moved.contains(QStringLiteral("2024-01.pdf"))
                    && moved.contains(QStringLiteral("2024-02.pdf"))
                    && moved.size() == 2
                    // 保留了目录结构：sub/2024-03.pdf
                    && QFile::exists(QDir(target).absoluteFilePath(
                           QStringLiteral("sub/2024-03.pdf")));
                reporter.check(movedOk,
                               QStringLiteral("★ 点「执行」后文件**真的**出现在目标目录，"
                                              "且递归+保留结构生效（sub/ 跟着建出来了）"),
                               QStringLiteral("目标目录：%1").arg(moved.join(QStringLiteral(","))));
                reporter.check(!QFile::exists(QDir(source).absoluteFilePath(
                                   QStringLiteral("2024-01.pdf"))),
                               QStringLiteral("源目录里的文件确实搬走了（不是复制）"));
                reporter.check(QFile::exists(
                                   QDir(source).absoluteFilePath(QStringLiteral("note.txt"))),
                               QStringLiteral("没匹配上正则的文件**一个都没动**"));

                undoButton->click();
                reporter.check(QFile::exists(QDir(source).absoluteFilePath(
                                   QStringLiteral("2024-01.pdf")))
                                   && QFile::exists(QDir(source).absoluteFilePath(
                                       QStringLiteral("sub/2024-03.pdf")))
                                   && fileNamesIn(target).isEmpty(),
                               QStringLiteral("★ 点「撤销上次移动」后文件搬回原位、"
                                              "目标目录清空（撤销日志真的被用上了）"),
                               QStringLiteral("目标目录剩：%1")
                                   .arg(fileNamesIn(target).join(QStringLiteral(","))));
            }
            delete page;
        }
    }

    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
