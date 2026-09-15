#include "convert_group.h"

// 引擎与插件、自检**共用同一份源码**（见 tests/feature_smoke/CMakeLists.txt 的说明）：
// 断言的目标就是用户真正跑的那一套实现，而不是"照着再写一遍"
#include "ImageConvertEngine.h"
#include "TaskCancel.h"
#include "TextEncodingTools.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QUrl>
#include <QWidget>

namespace FeatureSmoke {

namespace {

namespace Engine = WinEase::FeaturePlugins::ImageConvertEngine;
namespace TextTools = WinEase::FeaturePlugins::TextEncodingTools;
using WinEase::FeaturePlugins::Cancel;
using WinEase::FeaturePlugins::Progress;

const QString kConvertId = QStringLiteral("file.convert");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kPanel = QStringLiteral("convertPanel");
const QString kKindCombo = QStringLiteral("convertKindCombo");
const QString kFileList = QStringLiteral("convertFileList");
const QString kAddFilesButton = QStringLiteral("convertAddFilesButton");
const QString kAddFolderButton = QStringLiteral("convertAddFolderButton");
const QString kPasteButton = QStringLiteral("convertPasteButton");
const QString kRemoveButton = QStringLiteral("convertRemoveButton");
const QString kClearButton = QStringLiteral("convertClearButton");
const QString kRecursiveCheck = QStringLiteral("convertRecursiveCheck");
const QString kImageGroup = QStringLiteral("imageOptionsGroup");
const QString kFormatCombo = QStringLiteral("imageFormatCombo");
const QString kQualitySpin = QStringLiteral("imageQualitySpin");
const QString kResizeCombo = QStringLiteral("imageResizeCombo");
const QString kResizeSpin = QStringLiteral("imageResizeSpin");
const QString kWatermarkEdit = QStringLiteral("imageWatermarkEdit");
const QString kWatermarkPosCombo = QStringLiteral("imageWatermarkPosCombo");
const QString kWatermarkOpacitySpin = QStringLiteral("imageWatermarkOpacitySpin");
const QString kWatermarkSizeSpin = QStringLiteral("imageWatermarkSizeSpin");
const QString kKeepMetadataCheck = QStringLiteral("imageKeepMetadataCheck");
const QString kTemplateEdit = QStringLiteral("imageTemplateEdit");
const QString kNumberStartSpin = QStringLiteral("imageNumberStartSpin");
const QString kTextGroup = QStringLiteral("textOptionsGroup");
const QString kTextEncodingCombo = QStringLiteral("textEncodingCombo");
const QString kLineEndingCombo = QStringLiteral("textLineEndingCombo");
const QString kOutputDirEdit = QStringLiteral("outputDirEdit");
const QString kCustomDirRadio = QStringLiteral("outputCustomDirRadio");
const QString kSameDirRadio = QStringLiteral("outputSameDirRadio");
const QString kConflictCombo = QStringLiteral("convertConflictCombo");
const QString kPlanLabel = QStringLiteral("convertPlanLabel");
const QString kProgress = QStringLiteral("convertProgress");
const QString kStartButton = QStringLiteral("convertStartButton");
const QString kCancelButton = QStringLiteral("convertCancelButton");
const QString kStageLabel = QStringLiteral("convertStageLabel");
const QString kStatusLabel = QStringLiteral("convertStatusLabel");
const QString kResultList = QStringLiteral("convertResultList");

/// 文本用例的原文：含中文（GBK 能装下）与换行
const QString kSampleText = QStringLiteral("第一行：编码转换自检\n第二行：GBK → UTF-8\n");

template <typename T>
T *childNamed(QWidget *root, const QString &objectName)
{
    return root != nullptr ? root->findChild<T *>(objectName) : nullptr;
}

/// 按 itemData 里的键选下拉项（与插件写进去的键同一套 `*Key()` 函数）
bool selectComboByKey(QComboBox *combo, const QString &key)
{
    if (combo == nullptr) {
        return false;
    }
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i).toString() == key) {
            combo->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

/// 按**用户看得见的文字**选下拉项。
/// 「转换类型」那一栏写进去的是 int（Kind 枚举是插件私有的），跨 DLL 拿不到那个枚举，
/// 但用户本来就是按文字选的 —— 断言用文字反而更贴近真实路径。
bool selectComboByText(QComboBox *combo, const QString &needle)
{
    if (combo == nullptr) {
        return false;
    }
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i).contains(needle)) {
            combo->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

/// 目录里的文件（只看名字，排序；临时文件 .tmp 之类也一并数进来）
QStringList fileNamesIn(const QString &dirPath)
{
    QStringList names = QDir(dirPath).entryList(QDir::Files, QDir::Name);
    names.sort();
    return names;
}

/// 一张图能不能被完整读出来（"磁盘上留了半张图"就是这样被抓出来的）
bool readableImage(const QString &path)
{
    QImage probe;
    return probe.load(path) && !probe.isNull() && probe.width() > 0 && probe.height() > 0;
}

/// 拖入文件的"用户动作"：造一个带文件 URL 的拖放事件发给面板
/// （面板的 eventFilter 就是这条入口；自检不点模态文件对话框）
/// 返回事件是否被面板**收下** —— "面板没接上"与"收到了但一个文件都没加"是两回事，
/// 分开报才能一眼定位（只报"清单里 0 项"是查不出来的）。
bool dropFiles(QWidget *panel, const QStringList &paths)
{
    auto *mime = new QMimeData();
    QList<QUrl> urls;
    for (const QString &path : paths) {
        urls << QUrl::fromLocalFile(path);
    }
    mime->setUrls(urls);

    // 真实拖放一定先有 DragEnter（Qt 内部投递给窗口时也是这么走的）
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(panel, &enter);

    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier,
                    QEvent::Drop);
    QApplication::sendEvent(panel, &drop);
    // DragEnter 被 acceptProposedAction() 收下 = 事件真的进过面板的 eventFilter
    const bool accepted = drop.isAccepted() || enter.isAccepted();

    delete mime;
    return accepted;
}

// ---------------------------------------------------------------------------
//  素材
// ---------------------------------------------------------------------------

struct ConvertFixtures
{
    QString root;
    QString srcDir;      ///< 3 张 PNG + 1 个 GBK 文本
    QString manyDir;     ///< 24 张 PNG（取消用例）
    QString outDir;      ///< 引擎用例的输出目录
    QString conflictDir; ///< 冲突三态的输出目录
    QString panelOutDir; ///< 面板级图片用例的输出目录
    QString textOutDir;  ///< 文本用例的输出目录
    QString cancelOutDir;///< 引擎级取消用例的输出目录
    QString panelCancelDir; ///< 面板级取消用例的输出目录（必须和上一个分开：那个里面已经有 3 张了）
    QString pngA;        ///< 带透明像素（用来验"有损会铺底"的提示）
    QString pngB;
    QString pngC;
    QString gbkText;
    QStringList pngs;
    QStringList manyPngs;
    QString err;
};

ConvertFixtures createConvertFixtures(const QString &sandboxDir)
{
    ConvertFixtures fx;
    fx.root = QDir(sandboxDir).filePath(QStringLiteral("convert"));
    fx.srcDir = QDir(fx.root).filePath(QStringLiteral("src"));
    fx.manyDir = QDir(fx.root).filePath(QStringLiteral("many"));
    fx.outDir = QDir(fx.root).filePath(QStringLiteral("out"));
    fx.conflictDir = QDir(fx.root).filePath(QStringLiteral("conflict"));
    fx.panelOutDir = QDir(fx.root).filePath(QStringLiteral("panel_out"));
    fx.textOutDir = QDir(fx.root).filePath(QStringLiteral("text_out"));
    fx.cancelOutDir = QDir(fx.root).filePath(QStringLiteral("cancel_out"));
    fx.panelCancelDir = QDir(fx.root).filePath(QStringLiteral("panel_cancel"));

    for (const QString &dir : { fx.root, fx.srcDir, fx.manyDir, fx.outDir, fx.conflictDir,
                                fx.panelOutDir, fx.textOutDir, fx.cancelOutDir, fx.panelCancelDir }) {
        if (!QDir().mkpath(dir)) {
            fx.err = QStringLiteral("建不出目录：%1").arg(dir);
            return fx;
        }
    }

    const auto savePng = [](const QString &path, int edge, const QColor &color) {
        QImage image(edge, edge, QImage::Format_ARGB32);
        image.fill(color);
        return image.save(path, "PNG");
    };

    fx.pngA = QDir(fx.srcDir).filePath(QStringLiteral("a.png"));
    fx.pngB = QDir(fx.srcDir).filePath(QStringLiteral("b.png"));
    fx.pngC = QDir(fx.srcDir).filePath(QStringLiteral("c.png"));
    if (!savePng(fx.pngA, 16, QColor(255, 0, 0, 128)) || !savePng(fx.pngB, 16, QColor(0, 255, 0))
        || !savePng(fx.pngC, 16, QColor(0, 0, 255))) {
        fx.err = QStringLiteral("测试用 PNG 写不出来（Qt 的 PNG 编码器缺失？）");
        return fx;
    }
    fx.pngs = { fx.pngA, fx.pngB, fx.pngC };

    // 取消用例要"一批真的有得转的图"：张数与边长都刻意够大，
    // 否则整批可能在主线程点到「取消」之前就跑完了（那样测的就不是取消）
    for (int i = 0; i < 24; ++i) {
        const QString path = QDir(fx.manyDir)
                                 .filePath(QStringLiteral("i_%1.png").arg(i, 2, 10, QLatin1Char('0')));
        if (!savePng(path, 256, QColor(20 + i * 8, 40, 200 - i * 5))) {
            fx.err = QStringLiteral("批量测试用 PNG 写不出来：%1").arg(path);
            return fx;
        }
        fx.manyPngs << path;
    }

    // GBK 文本：用引擎自己的 encode() 造（"怎么写出 GBK" 这件事不另写一套）
    QByteArray gbkBytes;
    QString encodeError;
    if (!TextTools::encode(kSampleText, TextTools::Encoding::Gbk, false, &gbkBytes,
                           &encodeError)) {
        fx.err = QStringLiteral("造不出 GBK 素材：%1").arg(encodeError);
        return fx;
    }
    fx.gbkText = QDir(fx.srcDir).filePath(QStringLiteral("gbk.txt"));
    if (!writeBytes(fx.gbkText, gbkBytes)) {
        fx.err = QStringLiteral("GBK 素材写不进去：%1").arg(fx.gbkText);
        return fx;
    }

    return fx;
}

// ===========================================================================
//  ① 引擎：计划与落盘是同一份计算（纪律 5）+ 有损必须提前说（纪律 3）
// ===========================================================================

void runEnginePlanCase(Reporter &reporter, const ConvertFixtures &fx)
{
    Engine::Options options;
    options.format = Engine::Format::Jpeg;
    options.quality = 88;
    options.outputDir = fx.outDir;
    options.conflictPolicy = Engine::ConflictPolicy::Skip;

    const Engine::Plan plan = Engine::plan(fx.pngs, options);
    reporter.check(plan.ok(),
                   QStringLiteral("P3-03 图片：计划算得出来（3 张 PNG → JPEG）"), plan.error);
    if (!plan.ok()) {
        return;
    }
    reporter.check(plan.items.size() == 3 && plan.convertibleCount() == 3,
                   QStringLiteral("P3-03 计划里恰好 3 项、3 项都可转换"),
                   QStringLiteral("%1 项 / %2 项可转换")
                       .arg(plan.items.size())
                       .arg(plan.convertibleCount()));

    bool placementOk = true;
    QStringList planned;
    for (const Engine::Item &item : plan.items) {
        const QFileInfo info(item.outputPath);
        planned << info.fileName();
        if (QDir::cleanPath(info.absolutePath()) != QDir::cleanPath(fx.outDir)
            || info.suffix().compare(QStringLiteral("jpg"), Qt::CaseInsensitive) != 0
            || info.completeBaseName() != QFileInfo(item.inputPath).completeBaseName()) {
            placementOk = false;
        }
    }
    reporter.check(placementOk,
                   QStringLiteral("P3-03 目标路径 = 指定目录 + 原主干 + 目标格式扩展名"),
                   planned.join(QStringLiteral("，")));

    // ★ 纪律 3：有损 / 丢透明必须**在转换之前**说出来，而且这里是界面唯一的信息源
    const QString notes = plan.notes.join(QStringLiteral("；"));
    reporter.check(notes.contains(QStringLiteral("有损")),
                   QStringLiteral("P3-03 ★ 有损格式在计划里就说清（JPG 会丢细节）"), notes);
    reporter.check(notes.contains(QStringLiteral("透明")),
                   QStringLiteral("P3-03 ★ 不支持透明的格式在计划里就说清（透明处会铺白底）"), notes);

    QStringList stages;
    const Engine::ApplyResult result =
        Engine::apply(plan, options, nullptr, Progress([&stages](const QString &stage, int, int) {
                          if (!stages.contains(stage)) {
                              stages << stage;
                          }
                      }));
    reporter.check(result.ok && result.failed.isEmpty(),
                   QStringLiteral("P3-03 执行没有失败项"), result.failed.join(QStringLiteral("；")));
    reporter.check(result.converted.size() == plan.convertibleCount(),
                   QStringLiteral("P3-03 ★ 计划里说转几张就真写几张（预览与落盘同一份计算）"),
                   QStringLiteral("计划 %1 / 实际 %2")
                       .arg(plan.convertibleCount())
                       .arg(result.converted.size()));

    // 计划里的路径与磁盘上的文件必须**逐一**对上（对不上就是"用户看到的名字不是磁盘上的名字"）
    QStringList missing;
    for (const Engine::Item &item : plan.items) {
        if (!QFileInfo::exists(item.outputPath)) {
            missing << QFileInfo(item.outputPath).fileName();
        }
    }
    reporter.check(missing.isEmpty(),
                   QStringLiteral("P3-03 ★ 计划里写的目标路径就是磁盘上真正写出的文件"),
                   missing.join(QStringLiteral("，")));

    int readable = 0;
    for (const Engine::Item &item : plan.items) {
        readable += readableImage(item.outputPath) ? 1 : 0;
    }
    reporter.check(readable == 3, QStringLiteral("P3-03 每一张输出都能被完整读出来（不是半张）"),
                   QStringLiteral("%1 / 3").arg(readable));

    reporter.check(stages.size() >= 2,
                   QStringLiteral("P3-03 进度回调报过阶段名（面板的「阶段：x / y」靠它）"),
                   stages.join(QStringLiteral("，")));

    // ★ 纪律 1：整批转完，源文件一张都没少、一张都没被改
    QStringList lost;
    for (const QString &source : fx.pngs) {
        if (!QFileInfo::exists(source) || !readableImage(source)) {
            lost << QFileInfo(source).fileName();
        }
    }
    reporter.check(lost.isEmpty(),
                   QStringLiteral("P3-03 ★★ 端到端：转完 3 张之后源 PNG 一张都没少、都还能读"),
                   lost.join(QStringLiteral("，")));
    reporter.check(fileNamesIn(fx.outDir).size() == 3,
                   QStringLiteral("P3-03 输出目录里恰好 3 个文件（没有多写临时文件残留）"),
                   fileNamesIn(fx.outDir).join(QStringLiteral("，")));
}

// ===========================================================================
//  ② 绝不原地覆盖源文件（纪律 1 的判定点）
// ===========================================================================

void runNoOverwriteCase(Reporter &reporter, const ConvertFixtures &fx)
{
    Engine::Options options;
    options.format = Engine::Format::Png; // 与源同格式 → 同目录同名 = 与源文件重合
    options.outputDir.clear();            // 输出到源目录
    options.conflictPolicy = Engine::ConflictPolicy::Skip;

    const QByteArray before = readBytes(fx.pngA);
    const Engine::Plan plan = Engine::plan({ fx.pngA }, options);
    reporter.check(plan.ok() && plan.items.size() == 1,
                   QStringLiteral("P3-03 前置：同目录同名的计划算得出来"), plan.error);
    if (plan.items.isEmpty()) {
        return;
    }

    reporter.check(plan.convertibleCount() == 0 && !plan.items.first().problem.isEmpty(),
                   QStringLiteral("P3-03 ★★ 目标与源文件重合 → 该项被标成「有问题」而不是转起来"),
                   plan.items.first().problem);
    reporter.check(plan.items.first().problem.contains(QStringLiteral("同一个文件")),
                   QStringLiteral("P3-03 原因说清了是「同一个文件」"),
                   plan.items.first().problem);

    const Engine::ApplyResult result = Engine::apply(plan, options);
    reporter.check(result.ok && result.converted.isEmpty() && result.failed.isEmpty(),
                   QStringLiteral("P3-03 整批可转换项为 0 时什么都不做（既不写也不报失败）"));
    reporter.check(readBytes(fx.pngA) == before && !before.isEmpty(),
                   QStringLiteral("P3-03 ★★ 端到端：源文件字节一个都没变"),
                   QStringLiteral("%1 字节").arg(before.size()));
}

// ===========================================================================
//  ③ 目标已存在的三种策略
// ===========================================================================

void runConflictCase(Reporter &reporter, const ConvertFixtures &fx)
{
    const QString sentinelPath = QDir(fx.conflictDir).filePath(QStringLiteral("a.jpg"));
    const QByteArray sentinel("sentinel-not-a-jpeg");
    if (!writeBytes(sentinelPath, sentinel)) {
        reporter.check(false, QStringLiteral("P3-03 前置：造一个「目标已存在」的文件失败"),
                       sentinelPath);
        return;
    }

    Engine::Options options;
    options.format = Engine::Format::Jpeg;
    options.outputDir = fx.conflictDir;

    // ---- ① 跳过（默认：批量任务不该悄悄动别的文件）----
    options.conflictPolicy = Engine::ConflictPolicy::Skip;
    const Engine::Plan skipPlan = Engine::plan({ fx.pngA }, options);
    const bool skipMarked = skipPlan.items.size() == 1 && skipPlan.convertibleCount() == 0
        && skipPlan.items.first().skippedByConflict;
    reporter.check(skipMarked,
                   QStringLiteral("P3-03 ★ 目标已存在 + 跳过策略 → 计划里标的是「跳过」而不是「出错」"),
                   skipPlan.items.value(0).problem);
    const Engine::ApplyResult skipResult = Engine::apply(skipPlan, options);
    reporter.check(skipResult.skipped.size() == 1 && skipResult.failed.isEmpty(),
                   QStringLiteral("P3-03 执行结果把跳过单独列出来（跳过 ≠ 失败）"),
                   skipResult.skipped.join(QStringLiteral("；")));
    reporter.check(readBytes(sentinelPath) == sentinel,
                   QStringLiteral("P3-03 ★★ 跳过策略下磁盘上那个文件**一个字节都没动**"));

    // ---- ② 覆盖（必须提前说）----
    options.conflictPolicy = Engine::ConflictPolicy::Overwrite;
    const Engine::Plan overwritePlan = Engine::plan({ fx.pngA }, options);
    reporter.check(overwritePlan.items.size() == 1
                       && overwritePlan.items.first().warning.contains(QStringLiteral("覆盖")),
                   QStringLiteral("P3-03 ★ 覆盖策略下计划里明说「已存在，转换后会覆盖它」"),
                   overwritePlan.items.value(0).warning);
    const Engine::ApplyResult overwriteResult = Engine::apply(overwritePlan, options);
    reporter.check(overwriteResult.converted.size() == 1 && readableImage(sentinelPath),
                   QStringLiteral("P3-03 覆盖策略下目标真的被换成了新图"),
                   QStringLiteral("转换 %1 张").arg(overwriteResult.converted.size()));

    // ---- ③ 自动改名（不覆盖别人的东西）----
    options.conflictPolicy = Engine::ConflictPolicy::AutoRename;
    const Engine::Plan renamePlan = Engine::plan({ fx.pngA }, options);
    const QString renamed = QFileInfo(renamePlan.items.value(0).outputPath).fileName();
    reporter.check(renamePlan.items.size() == 1
                       && renamed.compare(QStringLiteral("a-1.jpg"), Qt::CaseInsensitive) == 0
                       && renamePlan.items.first().warning.contains(QStringLiteral("自动改名")),
                   QStringLiteral("P3-03 自动改名策略下算出的名字是 a-1.jpg 并提前告知"),
                   QStringLiteral("%1 / %2").arg(renamed, renamePlan.items.value(0).warning));
    const Engine::ApplyResult renameResult = Engine::apply(renamePlan, options);
    reporter.check(renameResult.converted.size() == 1 && readableImage(sentinelPath),
                   QStringLiteral("P3-03 自动改名后原来那个 a.jpg 还在（没被顺手覆盖）"));

    // ---- ④ 同一批里两项算出同一个目标：永远自动加序号（不能"悄悄少一张"）----
    //      造法：模板里带固定名字 → 两个不同源文件算出同一个目标
    Engine::Options templateOptions;
    templateOptions.format = Engine::Format::Jpeg;
    templateOptions.outputDir = QDir(fx.root).filePath(QStringLiteral("same_name"));
    templateOptions.nameTemplate = QStringLiteral("merged");
    QDir().mkpath(templateOptions.outputDir);
    const Engine::Plan sameNamePlan =
        Engine::plan({ fx.pngA, fx.pngB }, templateOptions);
    const QString firstName = QFileInfo(sameNamePlan.items.value(0).outputPath).fileName();
    const QString secondName = QFileInfo(sameNamePlan.items.value(1).outputPath).fileName();
    reporter.check(sameNamePlan.items.size() == 2 && firstName != secondName
                       && sameNamePlan.convertibleCount() == 2,
                   QStringLiteral("P3-03 ★ 同一批里两项撞成同一个目标 → 第二项自动加序号（不会悄悄少一张）"),
                   QStringLiteral("%1 / %2").arg(firstName, secondName));
}

// ===========================================================================
//  ④ 引擎级：取消只在"开工前"生效 → 磁盘上不留半张图
//
//  确定性做法：在进度回调里"数到第 N 张完成就请求取消" —— 不靠掐时间
//  （掐时间的取消测试要么太晚（已经跑完）要么太早（一个都没开始），
//   两种都测不到"已开工的那张会写完"这条语义）
// ===========================================================================

void runCancelSemanticsCase(Reporter &reporter, const ConvertFixtures &fx)
{
    Engine::Options options;
    options.format = Engine::Format::Jpeg;
    options.outputDir = fx.cancelOutDir;
    options.conflictPolicy = Engine::ConflictPolicy::Skip;

    const Engine::Plan plan = Engine::plan(fx.manyPngs, options);
    reporter.check(plan.ok() && plan.convertibleCount() == fx.manyPngs.size(),
                   QStringLiteral("P3-03 前置：24 张图全部可转换"),
                   QStringLiteral("%1 项可转换").arg(plan.convertibleCount()));
    if (!plan.ok()) {
        return;
    }

    Cancel cancel;
    const Engine::ApplyResult result =
        Engine::apply(plan, options, &cancel, Progress([&cancel](const QString &, int done, int) {
                          if (done >= 3) {
                              cancel.request();
                          }
                      }));

    reporter.check(result.cancelled,
                   QStringLiteral("P3-03 ★ 中途取消被如实报告（cancelled = true，不假装跑完了）"));
    reporter.check(result.converted.size() == 3,
                   QStringLiteral("P3-03 ★★ 取消只在每张**开工前**生效：已完成的 3 张保持不变"),
                   QStringLiteral("实际完成 %1 张").arg(result.converted.size()));

    const QStringList produced = fileNamesIn(fx.cancelOutDir);
    reporter.check(produced.size() == result.converted.size(),
                   QStringLiteral("P3-03 ★★ 取消后磁盘上恰好留下已完成的那几张（半成品会被清掉）"),
                   QStringLiteral("磁盘 %1 个 / 完成 %2 张")
                       .arg(produced.size())
                       .arg(result.converted.size()));

    int readable = 0;
    for (const QString &name : produced) {
        readable += readableImage(QDir(fx.cancelOutDir).filePath(name)) ? 1 : 0;
    }
    reporter.check(readable == produced.size(),
                   QStringLiteral("P3-03 ★★ 取消后留下的每一张都是完整可读的图（没有半张图）"),
                   QStringLiteral("%1 / %2").arg(readable).arg(produced.size()));
    reporter.check(fileNamesIn(fx.cancelOutDir).filter(QStringLiteral(".tmp")).isEmpty()
                       && QDir(fx.cancelOutDir).entryList(QDir::Hidden | QDir::Files).size()
                           == produced.size(),
                   QStringLiteral("P3-03 临时文件也被清干净了（目录里没有多余的东西）"),
                   QDir(fx.cancelOutDir).entryList(QDir::Hidden | QDir::Files).join(QStringLiteral("，")));
}

// ===========================================================================
//  ⑤ 面板级：图片模式走一遍真实用户路径（拖入 → 开始 → 逐条结果 → 再跑一遍）
//
//  面板在插件 DLL 里、又不导出 C++ 符号，所以只能按 objectName 抓控件
//  （见踩坑 #18）；抓不到就**直接报缺哪个**，别让断言退化成"什么都没验到"。
// ===========================================================================

struct PanelControls
{
    QComboBox *kind = nullptr;
    QListWidget *files = nullptr;
    QPushButton *paste = nullptr;
    QPushButton *remove = nullptr;
    QPushButton *clear = nullptr;
    QGroupBox *imageGroup = nullptr;
    QGroupBox *textGroup = nullptr;
    QComboBox *format = nullptr;
    QSpinBox *quality = nullptr;
    QLineEdit *outputDir = nullptr;
    QRadioButton *customDir = nullptr;
    QRadioButton *sameDir = nullptr;
    QComboBox *conflict = nullptr;
    QComboBox *encoding = nullptr;
    QLabel *plan = nullptr;
    QProgressBar *progress = nullptr;
    QPushButton *start = nullptr;
    QPushButton *cancel = nullptr;
    QLabel *stage = nullptr;
    QLabel *status = nullptr;
    QListWidget *results = nullptr;
};

template <typename T>
void grabOne(QWidget *panel, const QString &name, T **slot, QStringList *missing)
{
    *slot = childNamed<T>(panel, name);
    if (*slot == nullptr) {
        *missing << name;
    }
}

/// 抓控件；返回缺失的对象名列表（空 = 全齐）
QStringList grabPanelControls(QWidget *panel, PanelControls *out)
{
    QStringList missing;
    grabOne(panel, kKindCombo, &out->kind, &missing);
    grabOne(panel, kFileList, &out->files, &missing);
    grabOne(panel, kPasteButton, &out->paste, &missing);
    grabOne(panel, kRemoveButton, &out->remove, &missing);
    grabOne(panel, kClearButton, &out->clear, &missing);
    grabOne(panel, kImageGroup, &out->imageGroup, &missing);
    grabOne(panel, kTextGroup, &out->textGroup, &missing);
    grabOne(panel, kFormatCombo, &out->format, &missing);
    grabOne(panel, kQualitySpin, &out->quality, &missing);
    grabOne(panel, kOutputDirEdit, &out->outputDir, &missing);
    grabOne(panel, kCustomDirRadio, &out->customDir, &missing);
    grabOne(panel, kSameDirRadio, &out->sameDir, &missing);
    grabOne(panel, kConflictCombo, &out->conflict, &missing);
    grabOne(panel, kTextEncodingCombo, &out->encoding, &missing);
    grabOne(panel, kPlanLabel, &out->plan, &missing);
    grabOne(panel, kProgress, &out->progress, &missing);
    grabOne(panel, kStartButton, &out->start, &missing);
    grabOne(panel, kCancelButton, &out->cancel, &missing);
    grabOne(panel, kStageLabel, &out->stage, &missing);
    grabOne(panel, kStatusLabel, &out->status, &missing);
    grabOne(panel, kResultList, &out->results, &missing);
    return missing;
}

void runImagePanelCase(Reporter &reporter, const ConvertFixtures &fx, QWidget *panel)
{
    PanelControls ui;
    const QStringList missing = grabPanelControls(panel, &ui);
    reporter.check(missing.isEmpty(),
                   QStringLiteral("P3-03 ★ 面板控件按 objectName 全部找得到（缺一个都说明接线断了）"),
                   missing.join(QStringLiteral("，")));
    if (!missing.isEmpty()) {
        return;
    }

    reporter.check(panel->objectName() == kPanel,
                   QStringLiteral("P3-03 面板对象名是 convertPanel"), panel->objectName());
    reporter.check(!ui.start->isEnabled() && !ui.cancel->isEnabled(),
                   QStringLiteral("P3-03 前置：清单为空时「开始」与「取消」都点不动"));
    reporter.check(ui.plan->text().contains(QStringLiteral("还没有加入文件"))
                       && ui.results->count() == 0 && ui.progress->value() == 0,
                   QStringLiteral("P3-03 前置：空清单时计划行如实说明，结果与进度都是初始态"),
                   ui.plan->text());
    reporter.check(ui.kind->currentText().contains(QStringLiteral("图片"))
                       && !ui.imageGroup->isHidden() && ui.textGroup->isHidden(),
                   QStringLiteral("P3-03 默认是「图片」模式：图片选项可见、文本选项收起"),
                   ui.kind->currentText());

    // ---- 先按用户实际顺序把选项配好（选格式 / 策略 / 输出目录），再拖文件进来 ----
    // （顺序反过来的话，拖入那一刻目标还是"与源文件同目录 + PNG" → 计划行只能如实报
    //   "0 张可转、3 张有问题"，那条"拖入就刷新计划行"的断言就测不到想测的东西了）
    reporter.check(selectComboByKey(ui.format, QStringLiteral("jpeg")),
                   QStringLiteral("P3-03 目标格式下拉里找得到 JPEG"));
    selectComboByKey(ui.conflict, QStringLiteral("skip"));
    ui.quality->setValue(85);
    ui.customDir->setChecked(true);
    ui.outputDir->setText(fx.panelOutDir);
    settleEvents(20);

    // ---- 入口一：拖入（一次拖 3 张）----
    const bool dropped = dropFiles(panel, fx.pngs);
    settleEvents(20);
    reporter.check(ui.files->count() == 3,
                   QStringLiteral("P3-03 拖入 3 个文件 → 清单里出现 3 项"),
                   QStringLiteral("%1 项（事件被收下：%2；状态行：%3）")
                       .arg(ui.files->count())
                       .arg(dropped ? QStringLiteral("是") : QStringLiteral("否"), ui.status->text()));
    // 清单里存的是完整路径（列表按 Windows 习惯显示成反斜杠 → 比的时候要同一套写法）
    const QString expectedFirst = QDir::toNativeSeparators(fx.pngA);
    reporter.check(ui.files->item(0) != nullptr && ui.files->item(0)->text() == expectedFirst,
                   QStringLiteral("P3-03 清单里存的是完整路径（不是文件名）"),
                   ui.files->item(0) != nullptr ? ui.files->item(0)->text() : QString());
    reporter.check(ui.plan->text().contains(QStringLiteral("将转换 3 张")),
                   QStringLiteral("P3-03 拖入之后就刷新了计划行（不用再点一次）"), ui.plan->text());
    reporter.check(ui.start->isEnabled(), QStringLiteral("P3-03 清单非空后「开始」可以点"));

    if (ui.files->count() != 3) {
        reporter.info(QStringLiteral("P3-03 拖入这条入口没接上 → 面板级图片用例余下部分跳过"
                                     "（否则会在 60 秒超时上白等，后面几条也只是连锁报错）"));
        return;
    }

    // ★ 纪律 3：有损 / 丢透明必须在用户点「开始」之前就说出来，
    //   而界面上唯一的落点就是计划行那一行的 Tooltip
    reporter.check(ui.plan->toolTip().contains(QStringLiteral("有损"))
                       && ui.plan->toolTip().contains(QStringLiteral("透明")),
                   QStringLiteral("P3-03 ★★ 有损与丢透明在开始之前就说清（计划行 Tooltip）"),
                   ui.plan->toolTip());
    reporter.check(ui.plan->text().contains(QStringLiteral("panel_out")),
                   QStringLiteral("P3-03 计划行写明了目标目录（点开始之前就知道文件会去哪）"),
                   ui.plan->text());

    // ---- 真的转一遍 ----
    ui.start->click();
    const bool finished = waitFor([&ui] { return ui.start->isEnabled(); }, 60000);
    settleEvents(60);
    reporter.check(finished, QStringLiteral("P3-03 3 张图片的批量转换在 60 秒内跑完"));
    if (!finished) {
        return;
    }

    QStringList resultSample;
    int doneLines = 0;
    for (int i = 0; i < ui.results->count(); ++i) {
        const QString line = ui.results->item(i)->text();
        resultSample << line;
        if (line.startsWith(QStringLiteral("完成："))) {
            ++doneLines;
        }
    }
    reporter.check(ui.results->count() == 3 && doneLines == 3,
                   QStringLiteral("P3-03 结果逐条列出 3 行「完成：源 → 目标」（不是只报一个总数）"),
                   resultSample.join(QStringLiteral(" / ")));
    reporter.check(ui.progress->value() == 100,
                   QStringLiteral("P3-03 跑完后进度条是 100%"),
                   QStringLiteral("%1%").arg(ui.progress->value()));
    reporter.check(ui.stage->text().contains(QStringLiteral("3 / 3")),
                   QStringLiteral("P3-03 阶段行停在第 3 张 / 共 3 张"), ui.stage->text());
    reporter.check(ui.status->text().contains(QStringLiteral("完成 3 张")),
                   QStringLiteral("P3-03 状态行给出结论：完成 3 张"), ui.status->text());
    reporter.check(!ui.cancel->isEnabled() && ui.start->isEnabled(),
                   QStringLiteral("P3-03 跑完后「取消」自动变灰、「开始」恢复可用（不卡在转换中）"));

    const QStringList produced = fileNamesIn(fx.panelOutDir);
    reporter.check(produced.size() == 3,
                   QStringLiteral("P3-03 ★ 端到端：输出目录里真的多了 3 个文件"),
                   produced.join(QStringLiteral("，")));
    int readable = 0;
    for (const QString &name : produced) {
        readable += readableImage(QDir(fx.panelOutDir).filePath(name)) ? 1 : 0;
    }
    reporter.check(readable == 3,
                   QStringLiteral("P3-03 这 3 个文件都能当图片读出来（不是写坏的半成品）"),
                   QStringLiteral("%1 / 3").arg(readable));
    QStringList lost;
    for (const QString &source : fx.pngs) {
        if (!readableImage(source)) {
            lost << QFileInfo(source).fileName();
        }
    }
    reporter.check(lost.isEmpty(),
                   QStringLiteral("P3-03 ★★ 端到端：源 PNG 一张都没少（「不会原地覆盖」是真的）"),
                   lost.join(QStringLiteral("，")));

    // ---- 再跑一遍：目标已存在 + 跳过策略 → 磁盘上那张图一个字节都不该变 ----
    const QString targetA = QDir(fx.panelOutDir).filePath(QStringLiteral("a.jpg"));
    const QByteArray firstBytes = readBytes(targetA);
    ui.start->click();
    const bool secondDone = waitFor([&ui] { return ui.start->isEnabled(); }, 60000);
    settleEvents(60);
    reporter.check(secondDone, QStringLiteral("P3-03 第二遍（目标已存在）也在 60 秒内结束"));
    int skippedLines = 0;
    for (int i = 0; i < ui.results->count(); ++i) {
        if (ui.results->item(i)->text().startsWith(QStringLiteral("跳过："))) {
            ++skippedLines;
        }
    }
    reporter.check(skippedLines == 3 && ui.results->count() == 3,
                   QStringLiteral("P3-03 ★ 第二遍把 3 张都如实报成「跳过」（跳过 ≠ 失败，也不静默）"),
                   QStringLiteral("%1 行 / 其中跳过 %2 行")
                       .arg(ui.results->count())
                       .arg(skippedLines));
    reporter.check(ui.status->text().contains(QStringLiteral("跳过")),
                   QStringLiteral("P3-03 状态行也说了跳过几张"), ui.status->text());
    reporter.check(readBytes(targetA) == firstBytes && !firstBytes.isEmpty(),
                   QStringLiteral("P3-03 ★★ 跳过策略下磁盘上那张图一个字节都没动"));
    reporter.check(fileNamesIn(fx.panelOutDir).size() == 3,
                   QStringLiteral("P3-03 第二遍没有留下任何新文件"),
                   fileNamesIn(fx.panelOutDir).join(QStringLiteral("，")));

    // ---- 移除 / 清空 ----
    ui.files->item(0)->setSelected(true);
    ui.remove->click();
    settleEvents(20);
    reporter.check(ui.files->count() == 2,
                   QStringLiteral("P3-03 「移除选中」把选中的那一项拿掉"),
                   QStringLiteral("%1 项").arg(ui.files->count()));
    ui.clear->click();
    settleEvents(20);
    reporter.check(ui.files->count() == 0 && !ui.start->isEnabled()
                       && ui.plan->text().contains(QStringLiteral("还没有加入文件")),
                   QStringLiteral("P3-03 「清空」之后回到初始态（清单空、开始变灰、计划行如实说）"),
                   ui.plan->text());
}

// ===========================================================================
//  ⑥ 面板级：文本模式（剪贴板入口 + 没指定目录必须先拦住）
// ===========================================================================

void runTextPanelCase(Reporter &reporter, const ConvertFixtures &fx, QWidget *panel)
{
    PanelControls ui;
    if (!grabPanelControls(panel, &ui).isEmpty()) {
        return; // 控件缺失已在图片用例里报过，这里不重复刷屏
    }

    ClipboardGuard clipboard; // 用户可能正拿着剪贴板里的一段东西 → 整段用完还原

    reporter.check(selectComboByText(ui.kind, QStringLiteral("文本")),
                   QStringLiteral("P3-03 模式下拉里找得到「文本」"));
    settleEvents(20);
    reporter.check(!ui.textGroup->isHidden() && ui.imageGroup->isHidden(),
                   QStringLiteral("P3-03 切到「文本」后文本选项出现、图片选项收起"));

    // ---- 入口二：剪贴板里的路径 ----
    clipboard.set(fx.gbkText);
    ui.paste->click();
    settleEvents(20);
    const QString expectedClipPath = QDir::toNativeSeparators(fx.gbkText);
    reporter.check(ui.files->count() == 1 && ui.files->item(0) != nullptr
                       && ui.files->item(0)->text() == expectedClipPath,
                   QStringLiteral("P3-03 剪贴板里的路径能被「取剪贴板路径」收进清单"),
                   QStringLiteral("%1 项 / %2")
                       .arg(ui.files->count())
                       .arg(ui.files->item(0) != nullptr ? ui.files->item(0)->text()
                                                        : QString()));
    if (ui.files->count() != 1) {
        reporter.info(QStringLiteral("P3-03 剪贴板入口没把路径收进清单 → 文本用例余下部分跳过"));
        return;
    }

    // ★ 文本侧同名同目录 = 直接把源文件写坏 → 没指定输出目录时必须**在开转之前**拦住
    // （必须显式选「与源文件同目录」：上一段图片用例把「自定目录」留着没清，
    //   不换选项就一直在用那个目录，这条断言测不到想测的东西 —— 上一轮就是这么假绿的）
    ui.sameDir->setChecked(true);
    settleEvents(20);
    reporter.check(ui.plan->text().contains(QStringLiteral("需要指定输出目录")),
                   QStringLiteral("P3-03 ★★ 文本模式没指定输出目录时计划不成立（从源头挡住原地覆盖）"),
                   ui.plan->text());
    ui.start->click();
    settleEvents(20);
    reporter.check(ui.status->text().contains(QStringLiteral("没能开始"))
                       && ui.status->text().contains(QStringLiteral("输出目录")),
                   QStringLiteral("P3-03 ★ 这时点「开始」被拦下来并说明原因（不是转一半才失败）"),
                   ui.status->text());
    reporter.check(fileNamesIn(fx.textOutDir).isEmpty() && !readBytes(fx.gbkText).isEmpty(),
                   QStringLiteral("P3-03 被拦下来时什么都没写：输出目录还是空的、源文件还在"),
                   fileNamesIn(fx.textOutDir).join(QStringLiteral("，")));

    // ---- 指定目录与目标字符集 ----
    ui.customDir->setChecked(true);
    ui.outputDir->setText(fx.textOutDir);
    settleEvents(20);
    reporter.check(ui.plan->text().contains(QStringLiteral("将转换 1 个文本文件")),
                   QStringLiteral("P3-03 指定目录后计划行给出「将转换 1 个文本文件」"), ui.plan->text());
    reporter.check(ui.plan->text().contains(QStringLiteral("自动探测")),
                   QStringLiteral("P3-03 计划行说明源字符集是自动探测的（用户不用自己猜）"),
                   ui.plan->text());

    int utf8Index = -1;
    for (int i = 0; i < ui.encoding->count(); ++i) {
        if (ui.encoding->itemText(i).contains(QStringLiteral("UTF-8"), Qt::CaseInsensitive)) {
            utf8Index = i;
            break;
        }
    }
    reporter.check(utf8Index >= 0, QStringLiteral("P3-03 目标字符集下拉里有 UTF-8"),
                   ui.encoding->itemText(0));
    if (utf8Index >= 0) {
        ui.encoding->setCurrentIndex(utf8Index);
        settleEvents(20);
    }
    reporter.check(ui.plan->text().contains(QStringLiteral("UTF-8")),
                   QStringLiteral("P3-03 选中 UTF-8 后计划行写出为 UTF-8"), ui.plan->text());
    reporter.check(ui.plan->toolTip().contains(QStringLiteral("gbk.txt")),
                   QStringLiteral("P3-03 计划行 Tooltip 逐条写清「源 → 目标」"), ui.plan->toolTip());

    // ---- 转一遍：GBK → UTF-8 ----
    ui.start->click();
    const bool finished = waitFor([&ui] { return ui.start->isEnabled(); }, 60000);
    settleEvents(60);
    reporter.check(finished, QStringLiteral("P3-03 文本转换在 60 秒内跑完"));
    if (!finished) {
        return;
    }

    const QString firstResult =
        ui.results->item(0) != nullptr ? ui.results->item(0)->text() : QString();
    reporter.check(ui.results->count() == 1
                       && firstResult.startsWith(QStringLiteral("完成："))
                       && firstResult.contains(QStringLiteral("GBK")),
                   QStringLiteral("P3-03 ★ 结果行给出「完成：源 → 目标」以及**按什么编码读的**"
                                  "（自动探测的结果要对用户可见）"),
                   firstResult);

    const QString outputText = QDir(fx.textOutDir).filePath(QStringLiteral("gbk.txt"));
    reporter.check(QFileInfo::exists(outputText),
                   QStringLiteral("P3-03 ★ 端到端：输出目录里出现了 gbk.txt"),
                   fileNamesIn(fx.textOutDir).join(QStringLiteral("，")));

    QString decoded;
    QString decodeError;
    const bool decodeOk =
        TextTools::decode(readBytes(outputText), TextTools::Encoding::Utf8, &decoded, &decodeError);
    reporter.check(decodeOk && decoded == kSampleText,
                   QStringLiteral("P3-03 ★★ 端到端：输出文件是 UTF-8，解出来和原文逐字相同"),
                   decodeOk ? decoded : decodeError);

    QByteArray expectedGbk;
    QString encodeError;
    const bool encodeOk =
        TextTools::encode(kSampleText, TextTools::Encoding::Gbk, false, &expectedGbk, &encodeError);
    reporter.check(encodeOk && readBytes(fx.gbkText) == expectedGbk && !expectedGbk.isEmpty(),
                   QStringLiteral("P3-03 ★★ 端到端：源 GBK 文件字节一个都没变（改写的是副本）"),
                   encodeOk ? QStringLiteral("%1 字节").arg(expectedGbk.size()) : encodeError);

    // ---- 第二批：上一批的线程对象**必须先收掉**（踩坑 #71）----
    // ★★ 这是自检抓出来的真 bug：`m_worker = std::thread(...)` 遇到自己还 joinable 会
    //    直接 `std::terminate`（进程当场消失）。而"跑完一批再点第二批"恰恰是最普通的用法 ——
    //    图片侧那条路径因为"第二遍全被跳过策略排空了"（压根没开线程）反而漏掉了这个坑。
    const QString secondDir = fx.textOutDir + QStringLiteral("_2");
    QDir().mkpath(secondDir);
    ui.outputDir->setText(secondDir);
    settleEvents(20);
    ui.start->click();
    const bool secondDone = waitFor([&ui] { return ui.start->isEnabled(); }, 60000);
    settleEvents(60);
    reporter.check(secondDone && ui.status->text().contains(QStringLiteral("完成 1 个")),
                   QStringLiteral("P3-03 ★★ 跑完一批再跑第二批还能正常开工（线程对象要先收尸）"),
                   ui.status->text());

    QString secondText;
    QString secondError;
    const QString secondOutput = QDir(secondDir).filePath(QStringLiteral("gbk.txt"));
    const bool secondOk = TextTools::decode(readBytes(secondOutput), TextTools::Encoding::Utf8,
                                            &secondText, &secondError);
    reporter.check(secondOk && secondText == kSampleText,
                   QStringLiteral("P3-03 ★★ 第二批的输出和第一批一样对（不是上一轮没收干净）"),
                   secondOk ? secondText : secondError);

    ui.clear->click();
    settleEvents(20);
    reporter.check(ui.files->count() == 0,
                   QStringLiteral("P3-03 文本用例结束前把清单清空（不给下一个用例留状态）"));
}

// ===========================================================================
//  ⑦ 面板级：取消（"点完就停"这条用户路径必须真的能走）
// ===========================================================================

void runPanelCancelCase(Reporter &reporter, const ConvertFixtures &fx, QWidget *panel)
{
    PanelControls ui;
    if (!grabPanelControls(panel, &ui).isEmpty()) {
        return;
    }

    reporter.check(selectComboByText(ui.kind, QStringLiteral("图片")),
                   QStringLiteral("P3-03 切回「图片」模式"));
    selectComboByKey(ui.format, QStringLiteral("jpeg"));
    selectComboByKey(ui.conflict, QStringLiteral("skip"));
    ui.customDir->setChecked(true);
    ui.outputDir->setText(fx.panelCancelDir);
    settleEvents(20);

    dropFiles(panel, fx.manyPngs);
    settleEvents(20);
    reporter.check(ui.files->count() == 24,
                   QStringLiteral("P3-03 前置：24 张图全部进清单"),
                   QStringLiteral("%1 项").arg(ui.files->count()));
    reporter.check(ui.plan->text().contains(QStringLiteral("将转换 24 张")),
                   QStringLiteral("P3-03 前置：计划行说将转换 24 张（目标目录是空的 → 一张都不会被跳过）"),
                   ui.plan->text());
    if (ui.files->count() != 24) {
        reporter.info(QStringLiteral("P3-03 前置不成立（清单里不是 24 张）→ 面板级取消用例跳过"));
        return;
    }

    // 点完「开始」立刻点「取消」：这不是掐时间，而是"用户真的这么点"的路径 ——
    // 断言的是**不变式**（没转完 + 没留下坏文件 + 按钮回到可用），不是"转了几张"
    ui.start->click();
    ui.cancel->click();
    const bool stopped = waitFor([&ui] { return ui.start->isEnabled(); }, 60000);
    settleEvents(60);
    reporter.check(stopped, QStringLiteral("P3-03 取消之后「开始」恢复可用（不会卡在转换中）"));
    if (!stopped) {
        return;
    }
    reporter.check(!ui.cancel->isEnabled(),
                   QStringLiteral("P3-03 结束后「取消」自己是灰的"));

    const QStringList produced = fileNamesIn(fx.panelCancelDir);
    int readableOutputs = 0;
    for (const QString &name : produced) {
        readableOutputs += readableImage(QDir(fx.panelCancelDir).filePath(name)) ? 1 : 0;
    }
    reporter.check(readableOutputs == produced.size(),
                   QStringLiteral("P3-03 ★★ 取消后磁盘上留下的每一张都是完整可读的图（没有半张图）"),
                   QStringLiteral("%1 / %2").arg(readableOutputs).arg(produced.size()));

    int doneLines = 0;
    for (int i = 0; i < ui.results->count(); ++i) {
        if (ui.results->item(i)->text().startsWith(QStringLiteral("完成："))) {
            ++doneLines;
        }
    }
    reporter.check(doneLines == produced.size(),
                   QStringLiteral("P3-03 ★★ 取消后「完成」的行数与磁盘上的文件数一致"
                                  "（既不少报也不多报——多报是「说了却没写」，少报是「写了却没说」）"),
                   QStringLiteral("结果 %1 行 / 磁盘 %2 个").arg(doneLines).arg(produced.size()));

    int sourcesIntact = 0;
    for (const QString &source : fx.manyPngs) {
        sourcesIntact += readableImage(source) ? 1 : 0;
    }
    reporter.check(sourcesIntact == 24,
                   QStringLiteral("P3-03 取消也没伤到源文件（24 张都还在）"),
                   QStringLiteral("%1 / 24").arg(sourcesIntact));

    if (ui.status->text().contains(QStringLiteral("已取消"))) {
        reporter.check(produced.size() < 24,
                       QStringLiteral("P3-03 ★ 取消真的有作用：24 张没有全部转完"),
                       QStringLiteral("磁盘上 %1 个").arg(produced.size()));
    } else {
        // 如实报，不静默跳过：这一批可能在点「取消」之前就跑完了。
        // 取消的**语义**（已开工的那张会写完、不留半成品）由引擎级用例确定性覆盖，
        // 所以这里只是"这一条没赶上"，不是通过也不是失败。
        reporter.info(QStringLiteral("P3-03 这一批在点取消之前就跑完了（状态行：%1）"
                                     "—— 取消的语义由引擎级确定性用例覆盖")
                          .arg(ui.status->text()));
    }

    ui.clear->click();
    settleEvents(20);
    reporter.check(ui.files->count() == 0 && !ui.start->isEnabled(),
                   QStringLiteral("P3-03 取消用例收尾：清单清空、开始变灰"));
}

} // namespace

int runConvertGroupTests(Reporter &reporter, WinEase::PluginManager &manager,
                         const QString &sandboxDir)
{
    const int before = reporter.failures();

    WinEase::IFeaturePlugin *plugin = manager.plugin(kConvertId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P3-03 插件已从插件目录加载（file.convert）"));
    if (plugin == nullptr) {
        return reporter.failures() - before;
    }

    reporter.check(!plugin->supportsHotkey(),
                   QStringLiteral("P3-03 有意不占用全局快捷键（C11：批量任务没有合理快捷键语义）"));

    ConvertFixtures fx = createConvertFixtures(sandboxDir);
    reporter.check(fx.err.isEmpty(),
                   QStringLiteral("P3-03 素材：临时目录里的图片（3 + 24 张）与 GBK 文本已就绪"),
                   fx.err);
    if (!fx.err.isEmpty()) {
        return reporter.failures() - before;
    }

    runEnginePlanCase(reporter, fx);
    runNoOverwriteCase(reporter, fx);
    runConflictCase(reporter, fx);
    runCancelSemanticsCase(reporter, fx);

    // 面板级：面板由插件创建、自检负责释放（这个插件没有全局快捷键，
    // 所以拿不到"快捷键打开的顶层窗口"，只有 createSettingsWidget() 这条正路）
    reporter.check(manager.setPluginEnabled(kConvertId, true),
                   QStringLiteral("P3-03 插件启用成功"));
    QWidget *panel = plugin->createSettingsWidget(nullptr);
    reporter.check(panel != nullptr && panel->objectName() == kPanel,
                   QStringLiteral("P3-03 插件能交出设置面板（convertPanel）"),
                   panel != nullptr ? panel->objectName() : QString());
    if (panel != nullptr) {
        runImagePanelCase(reporter, fx, panel);
        runTextPanelCase(reporter, fx, panel);
        runPanelCancelCase(reporter, fx, panel);
        delete panel; // 插件里存的是 QPointer，删掉之后它自己会置空
    }

    reporter.check(manager.setPluginEnabled(kConvertId, false),
                   QStringLiteral("P3-03 插件停用成功"));
    settleEvents(60);
    reporter.check(!manager.isPluginEnabled(kConvertId),
                   QStringLiteral("P3-03 停用之后管理器的状态也变了（不是只发了个信号）"));

    return reporter.failures() - before;
}

} // namespace FeatureSmoke
