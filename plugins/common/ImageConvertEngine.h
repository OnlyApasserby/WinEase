#pragma once

// ============================================================================
//  ImageConvertEngine.h —— 批量图片格式转换引擎（P3-03 插件与自检共用）
//
//  解决的问题：**"把这一堆图换成另一种格式 / 换小一点"** —— 一次几百张，
//  中途要能停，失败的要单独列出来。
//
//  ---------------------------------------------------------------------------
//  五条设计纪律（都是"批量 + 不可逆"逼出来的）：
//
//   1. **绝不原地覆盖源文件**。目标路径与源文件重合（同目录 + 同名 + 同扩展名）
//      时该项直接标 `problem` —— 转换的产物**永远是另一个文件**。
//      想压缩原图的人请自己删掉原件；"转着转着原图没了"是不可接受的。
//   2. **先写临时文件，成功后再改名**（`apply()` 内部实现）。中途取消 / 编码失败
//      时磁盘上**不会留下半张图**；临时文件也会被清掉。
//   3. **有损转换必须提前说**：输出 jpg / webp(有损) 会把透明像素铺成白底、
//      也会丢掉无损精度 —— 这不是"转换失败"，但用户必须**在转换前**看见它
//      （`Item::warning`，面板显示在计划摘要里）。
//   4. **失败项单独列出**：每一张为什么失败都要有中文原因（源文件坏了 / 没权限 /
//      目标格式不支持 / 目标被占用）。绝不"跳过 3 个"就完事。
//   5. **计划与落盘是同一份计算**：`plan()` 算出的目标路径就是 `apply()` 真正写的位置。
//      预览与执行分家 = 用户看到的名字和磁盘上的名字对不上。
//
//  ⚠ 线程约定（与 `TaskCancel.h` 一致）：`apply()` 一般跑在**工作线程**上，
//    `Progress` 回调因而是从工作线程发出的 —— 插件侧必须排队转回界面线程再碰 QWidget。
//  ⚠ 本头文件不含 Q_OBJECT。
// ============================================================================

#include "TaskCancel.h"

#include <QPair>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace WinEase::FeaturePlugins::ImageConvertEngine {

// ============================================================================
//  目标格式
// ============================================================================

/// 输出格式。`Html` 之外都是 Qt 自带/官方插件提供编码器的位图格式。
/// ⚠ 真实可用性必须在运行期用 `availableFormats()` 问一次：webp / tiff / ico
///   都由 Qt 的 imageformats 插件提供，缺插件时 `QImageWriter` 会失败。
enum class Format {
    Png = 0,
    Jpeg,
    Bmp,
    Webp,
    Tiff,
    Ico
};

/// 该格式的规范扩展名（小写，不含点）
QString formatExtension(Format format);
/// 配置里存的小写键（"png" / "jpeg" / …）
QString formatKey(Format format);
/// 从键解析；无法识别时返回 false（调用方决定"用默认"还是"当场报错"）
bool formatFromKey(const QString &key, Format *formatOut);
/// 界面显示名（"PNG（无损，支持透明）"这类）
QString formatDisplayName(Format format);

/// **本机**当前真正支持哪些格式（问 `QImageWriter::supportedImageFormats()`）。
/// 顺序与上面的枚举一致；用来在界面上禁用缺插件的格式，而不是让用户排到最后才失败。
QVector<Format> availableFormats();
/// 全部格式（不论本机支不支持）。界面用它把"缺插件的格式"也列出来并置灰，
/// 用户才看得懂"我这里为什么没有 webp"
QVector<Format> allFormats();
/// 本机是否支持该格式（同上判据）
bool isFormatAvailable(Format format);
/// 缺失格式时的中文提示（"本机 Qt 缺少 webp 编码插件"）
QString formatUnavailableReason(Format format);

/// 该格式是否支持 alpha 通道（不支持时透明像素会被铺底）
bool formatSupportsAlpha(Format format);
/// 该格式是否有损（= 质量参数有意义、精度会丢）
bool formatIsLossy(Format format);
/// 该格式的扩展名是否就是"图片"（用来判断输入是否值得送进来）
bool isSupportedImageFile(const QString &fileNameOrPath);

// ============================================================================
//  尺寸
// ============================================================================

enum class ResizeMode {
    Keep = 0, ///< 保持原尺寸
    Width,    ///< 固定宽（高按比例）
    Height,   ///< 固定高（宽按比例）
    MaxEdge,  ///< 长边不超过 N（只缩小，不放大）
    Percent   ///< 按百分比缩放
};

QString resizeModeKey(ResizeMode mode);
bool resizeModeFromKey(const QString &key, ResizeMode *modeOut);
QString resizeModeDisplayName(ResizeMode mode);

/// 水印位置（九宫格里的四个角 + 平铺；够用且不会让人选半天）
enum class WatermarkPosition {
    Disabled = 0,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Center,
    Tile
};

QString watermarkPositionKey(WatermarkPosition position);
bool watermarkPositionFromKey(const QString &key, WatermarkPosition *positionOut);
QString watermarkPositionDisplayName(WatermarkPosition position);

// ============================================================================
//  目标文件已存在时的处理
// ============================================================================

enum class ConflictPolicy {
    Skip = 0,   ///< 跳过（默认：批量任务不该悄悄动别的文件）
    Overwrite,  ///< 覆盖
    AutoRename  ///< 自动改名（加 -1 / -2 …）
};

QString conflictPolicyKey(ConflictPolicy policy);
bool conflictPolicyFromKey(const QString &key, ConflictPolicy *policyOut);
QString conflictPolicyDisplayName(ConflictPolicy policy);

// ============================================================================
//  选项
// ============================================================================

struct Options
{
    Format format = Format::Png;
    int quality = 90; ///< 1..100，只对 `formatIsLossy()` 的格式有意义

    ResizeMode resizeMode = ResizeMode::Keep;
    int resizeValue = 0; ///< 宽/高/长边（像素）或百分比，含义由 resizeMode 决定

    WatermarkPosition watermarkPosition = WatermarkPosition::Disabled;
    QString watermarkText;
    int watermarkOpacityPercent = 40; ///< 10..100
    int watermarkSizePercent = 4;     ///< 字号 = 长边 × 百分比

    bool keepMetadata = true; ///< 保留 EXIF/ICC（Qt 默认保留；关掉则只写像素）

    /// 输出目录：空 = 与源文件同目录（默认）
    QString outputDir;
    /// 目标文件命名模板：{name} 原主干 {n} 序号 {date} 日期；空 = 沿用原主干
    QString nameTemplate;
    /// 序号起点（模板里用到 {n} 时）
    int numberStart = 1;

    ConflictPolicy conflictPolicy = ConflictPolicy::Skip;

    /// 递归处理子目录（面板选择文件夹时才有意义）
    bool recursive = false;
};

// ============================================================================
//  计划
// ============================================================================

struct Item
{
    QString inputPath;  ///< 源文件绝对路径
    QString outputPath; ///< 计划写入的目标绝对路径（problem 非空时可能为空）
    QString problem;    ///< 非空 = 该项**不会**被转换，内容是中文原因
    QString warning;    ///< 非空 = 会被转换，但有需要用户知道的事（有损 / 丢透明 / 已存在将被覆盖）
    /// true = 之所以不转换是因为"目标已存在 + 策略是跳过"，**不是**出错。
    /// 面板要把这两类分开显示：一个是"跳过"，一个是"有问题"。
    bool skippedByConflict = false;

    bool convertible() const { return problem.isEmpty(); }
};

struct Plan
{
    QVector<Item> items;
    /// 计划级错误（选项本身非法 / 输出目录建不出来）——此时 items 为空
    QString error;
    /// 输出目录的绝对路径（预览用）
    QString outputDir;
    /// 需要用户知道的事（有损格式、丢透明、目标已存在会被跳过…），一行一条
    QStringList notes;

    bool ok() const { return error.isEmpty(); }
    int convertibleCount() const;
    int problemCount() const;
    int warnedCount() const;
    /// "将转换 8 张（1 张跳过、2 张有问题）；目标目录：…"
    QString summaryText() const;
};

/// 展开输入列表（目录会展开成其中的图片文件，`recursive` 决定是否进子目录）。
/// 目录本身不参与转换；找不到任何图片时返回空列表（调用方如实说明）。
QStringList expandInputs(const QStringList &inputs, bool recursive, QStringList *problemOut = nullptr);

/// 生成计划：算目标路径、判冲突、判"目标与源重合"、写警告。
/// @param inputs 源文件（或目录）路径列表
Plan plan(const QStringList &inputs, const Options &options);

// ============================================================================
//  执行
// ============================================================================

struct ApplyResult
{
    bool ok = false;         ///< 整批是否"跑完了"（个别项失败不算 false）
    bool cancelled = false;  ///< 是否被取消（取消后已完成的那部分仍然有效）
    /// 真正写成功的映射（源 → 目标），按完成顺序；面板据此列"已完成"
    QVector<QPair<QString, QString>> converted;
    /// 失败项："源文件（中文原因）"
    QStringList failed;
    /// 被跳过的项（目标已存在 + 跳过策略）："源文件（目标已存在）"
    QStringList skipped;
    QString error; ///< 计划级错误（计划不成立时）

    int doneCount() const { return converted.size(); }
    QString summaryText() const;
};

/// 按计划执行转换。
/// @param cancel 取消标志（可空）；**每个文件转换前**检查一次
/// @param progress 进度回调（可空）：(阶段, 已完成, 总数)，**从工作线程调用**
ApplyResult apply(const Plan &plan, const Options &options, Cancel *cancel = nullptr,
                  const Progress &progress = nullptr);

/// 单张转换（`apply()` 的组成部分，单独暴露是为了让自检能精准断言"这一张为什么失败"）。
/// 内部：读入 → 缩放 → 铺底/水印 → 写临时文件 → 改名。失败时 errorOut 给中文原因，
/// 且**不留下任何输出文件**（临时文件会被删掉）。
bool convertOne(const QString &inputPath, const QString &outputPath, const Options &options,
                QString *errorOut = nullptr);

/// 把 QImage 的自读质量（"不支持透明被铺了底"这类）翻译成人话；无问题时返回空串
QString imageNoteText(Format format, bool hasAlpha);

} // namespace WinEase::FeaturePlugins::ImageConvertEngine
