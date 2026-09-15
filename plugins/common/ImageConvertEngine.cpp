#include "ImageConvertEngine.h"

#include <QColor>
#include <QColorSpace>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QMap>
#include <QPainter>
#include <QSet>

#include <algorithm>

namespace WinEase::FeaturePlugins::ImageConvertEngine {

namespace {

/// 中间产物后缀：先写它、成功了再改名成正式目标（纪律 2）
const QString kTempSuffix = QStringLiteral(".winease-part");

/// ICO 的硬上限（格式本身如此，不是我们的取舍）
constexpr int kIcoMaxEdge = 256;

struct FormatInfo
{
    Format format;
    const char *key;       ///< 配置键（ASCII）
    const char *extension; ///< 规范扩展名
    const char *writer;    ///< QImageWriter 认的格式名
    bool lossy;
    bool alpha;
    const char *display; ///< 界面显示名
};

// ⚠ 顺序必须与枚举一致（availableFormats() 依赖它）
const FormatInfo kFormats[] = {
    { Format::Png, "png", "png", "png", false, true, "PNG（无损，支持透明）" },
    { Format::Jpeg, "jpeg", "jpg", "jpeg", true, false, "JPEG（有损，体积小，不支持透明）" },
    { Format::Bmp, "bmp", "bmp", "bmp", false, false, "BMP（无损不压缩，体积大）" },
    { Format::Webp, "webp", "webp", "webp", true, true, "WebP（有损，体积最小）" },
    { Format::Tiff, "tiff", "tiff", "tiff", false, true, "TIFF（无损，印刷/归档常用）" },
    { Format::Ico, "ico", "ico", "ico", false, true, "ICO（图标，单张最大 256×256）" },
};

const FormatInfo &infoOf(Format format)
{
    for (const FormatInfo &info : kFormats) {
        if (info.format == format) {
            return info;
        }
    }
    return kFormats[0];
}

QString readerErrorText(QImageReader::ImageReaderError code, const QString &raw)
{
    switch (code) {
    case QImageReader::FileNotFoundError:
        return QStringLiteral("文件不存在或读不到");
    case QImageReader::DeviceError:
        return QStringLiteral("读文件出错（可能被占用或权限不足）");
    case QImageReader::UnsupportedFormatError:
        return QStringLiteral("不认识的图片格式（本机 Qt 没有对应的解码插件）");
    case QImageReader::InvalidDataError:
        return QStringLiteral("文件内容不是有效的图片（可能损坏，或扩展名与实际格式不符）");
    default:
        break;
    }
    return raw.isEmpty() ? QStringLiteral("读取失败") : QStringLiteral("读取失败：%1").arg(raw);
}

QString writerErrorText(QImageWriter::ImageWriterError code, const QString &raw)
{
    switch (code) {
    case QImageWriter::DeviceError:
        return QStringLiteral("写文件出错（目标目录只读、磁盘满或文件被占用）");
    case QImageWriter::UnsupportedFormatError:
        return QStringLiteral("目标格式不可写（本机 Qt 缺少该编码插件）");
    default:
        break;
    }
    return raw.isEmpty() ? QStringLiteral("写入失败") : QStringLiteral("写入失败：%1").arg(raw);
}

/// 按比例算出目标尺寸（`ResizeMode::Keep` 返回原尺寸）
QSize targetSize(const QSize &source, const Options &options)
{
    if (source.isEmpty() || options.resizeMode == ResizeMode::Keep || options.resizeValue <= 0) {
        return source;
    }

    const int value = options.resizeValue;
    switch (options.resizeMode) {
    case ResizeMode::Width:
        return QSize(value, qMax(1, qRound(double(source.height()) * value / source.width())));
    case ResizeMode::Height:
        return QSize(qMax(1, qRound(double(source.width()) * value / source.height())), value);
    case ResizeMode::MaxEdge: {
        const int longest = qMax(source.width(), source.height());
        if (longest <= value) {
            return source; // 只缩小、不放大（放大只会变糊，用户不会想要）
        }
        const double scale = double(value) / longest;
        return QSize(qMax(1, qRound(source.width() * scale)),
                     qMax(1, qRound(source.height() * scale)));
    }
    case ResizeMode::Percent: {
        const double scale = value / 100.0;
        return QSize(qMax(1, qRound(source.width() * scale)),
                     qMax(1, qRound(source.height() * scale)));
    }
    case ResizeMode::Keep:
    default:
        return source;
    }
}

/// 透明像素在不支持 alpha 的格式上会被铺成什么颜色（白底，与"另存为 JPG"的通行做法一致）
QImage flattenFor(Format format, const QImage &source)
{
    if (formatSupportsAlpha(format) || !source.hasAlphaChannel()) {
        return source;
    }

    QImage flattened(source.size(), QImage::Format_RGB32);
    flattened.fill(Qt::white);
    QPainter painter(&flattened);
    painter.drawImage(0, 0, source);
    painter.end();
    return flattened;
}

/// 只取像素、不带任何元数据的副本。
/// 为什么要这么绕：`QImage::setText(key, 空串)` **不会**删掉这个键（Qt 没有提供删除接口），
/// 而缩放 / 铺底 / 加水印都会生成新图、把元数据丢掉 —— "元数据到底还在不在"取决于
/// 走没走到那一步，这种不确定性正是自检最容易漏掉的地方。所以统一从像素重建，
/// 元数据在最后按"保留 / 不保留"一次性搬过来。
QImage pixelsOnly(const QImage &source)
{
    QImage result(source.size(), source.format());
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.drawImage(0, 0, source);
    painter.end();
    return result;
}

void drawWatermark(QImage &image, const Options &options)
{
    if (options.watermarkPosition == WatermarkPosition::Disabled
        || options.watermarkText.trimmed().isEmpty()) {
        return;
    }

    const int longest = qMax(image.width(), image.height());
    const int pixelSize = qMax(8, qRound(longest * options.watermarkSizePercent / 100.0));
    const int padding = qMax(4, qRound(longest * 0.02));

    QColor color(Qt::white);
    color.setAlphaF(std::clamp(options.watermarkOpacityPercent, 1, 100) / 100.0);

    QFont font;
    font.setPixelSize(pixelSize);
    font.setBold(true);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(color);

    const QFontMetricsF metrics(font);
    const QString text = options.watermarkText;
    const QSizeF box(metrics.horizontalAdvance(text), metrics.height());

    switch (options.watermarkPosition) {
    case WatermarkPosition::TopLeft:
        painter.drawText(QPointF(padding, padding + box.height()), text);
        break;
    case WatermarkPosition::TopRight:
        painter.drawText(QPointF(image.width() - padding - box.width(), padding + box.height()),
                         text);
        break;
    case WatermarkPosition::BottomLeft:
        painter.drawText(QPointF(padding, image.height() - padding), text);
        break;
    case WatermarkPosition::BottomRight:
        painter.drawText(
            QPointF(image.width() - padding - box.width(), image.height() - padding), text);
        break;
    case WatermarkPosition::Center:
        painter.drawText(QPointF((image.width() - box.width()) / 2.0,
                                 (image.height() + box.height()) / 2.0 - metrics.descent()),
                         text);
        break;
    case WatermarkPosition::Tile: {
        const double stepX = box.width() + padding * 2.0;
        const double stepY = box.height() + padding * 2.0;
        for (double y = padding; y < image.height(); y += stepY) {
            for (double x = padding; x < image.width(); x += stepX) {
                painter.drawText(QPointF(x, y + box.height() * 0.8), text);
            }
        }
        break;
    }
    case WatermarkPosition::Disabled:
    default:
        break;
    }

    painter.end();
}

/// 序号占位符 {n}：按"该项在传入列表里的下标"算，与其它项是否成功无关
QString expandTemplate(const QString &templ, const QString &sourceName, int index, int numberStart)
{
    const QString stem = QFileInfo(sourceName).completeBaseName();
    if (templ.isEmpty()) {
        return stem;
    }

    QString result = templ;
    result.replace(QStringLiteral("{name}"), stem);
    result.replace(QStringLiteral("{n}"),
                   QStringLiteral("%1").arg(index + numberStart, 3, 10, QLatin1Char('0')));
    result.replace(QStringLiteral("{date}"),
                   QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    result.replace(QStringLiteral("{ext}"), QFileInfo(sourceName).suffix());
    return result;
}

QString buildOutputPath(const QString &dirPath, const QString &sourceName, const Options &options,
                        int index, const QString &extension)
{
    const QString stem = expandTemplate(options.nameTemplate, sourceName, index, options.numberStart);
    return QDir(dirPath).filePath(QStringLiteral("%1.%2").arg(stem, extension));
}

/// 同名冲突时找一个没被占用的路径（-1 / -2 …）
QString uniquePath(const QString &desired, const QSet<QString> &taken)
{
    const QFileInfo info(desired);
    const QString dir = info.absolutePath();
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();

    for (int counter = 1; counter < 10000; ++counter) {
        const QString candidate =
            QDir(dir).filePath(QStringLiteral("%1-%2.%3").arg(stem).arg(counter).arg(suffix));
        if (!taken.contains(QDir::cleanPath(candidate).toLower())
            && !QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return desired;
}

} // namespace

// ============================================================================
//  格式
// ============================================================================

QString formatExtension(Format format)
{
    return QString::fromLatin1(infoOf(format).extension);
}

QString formatKey(Format format)
{
    return QString::fromLatin1(infoOf(format).key);
}

bool formatFromKey(const QString &key, Format *formatOut)
{
    const QString normalized = key.trimmed().toLower();
    for (const FormatInfo &info : kFormats) {
        if (normalized == QLatin1String(info.key)
            || normalized == QLatin1String(info.extension)) {
            if (formatOut != nullptr) {
                *formatOut = info.format;
            }
            return true;
        }
    }
    return false;
}

QString formatDisplayName(Format format)
{
    return QString::fromUtf8(infoOf(format).display);
}

QVector<Format> availableFormats()
{
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    QVector<Format> result;
    for (const FormatInfo &info : kFormats) {
        if (supported.contains(QByteArray(info.writer))) {
            result.append(info.format);
        }
    }
    return result;
}

bool isFormatAvailable(Format format)
{
    return QImageWriter::supportedImageFormats().contains(QByteArray(infoOf(format).writer));
}

QVector<Format> allFormats()
{
    QVector<Format> result;
    for (const FormatInfo &info : kFormats) {
        result.append(info.format);
    }
    return result;
}

QString formatUnavailableReason(Format format)
{
    return QStringLiteral("本机 Qt 缺少 %1 的编码插件，无法输出这种格式")
        .arg(formatExtension(format).toUpper());
}

bool formatSupportsAlpha(Format format)
{
    return infoOf(format).alpha;
}

bool formatIsLossy(Format format)
{
    return infoOf(format).lossy;
}

bool isSupportedImageFile(const QString &fileNameOrPath)
{
    const QString suffix = QFileInfo(fileNameOrPath).suffix().toLower();
    if (suffix.isEmpty()) {
        return false;
    }

    const QList<QByteArray> supported = QImageReader::supportedImageFormats();
    for (const QByteArray &format : supported) {
        const QString name = QString::fromLatin1(format).toLower();
        if (suffix == name) {
            return true;
        }
        // jpg / tif 这类别名：扩展名与格式名不完全一致，单独兜一层
        if ((name == QLatin1String("jpeg") && suffix == QLatin1String("jpg"))
            || (name == QLatin1String("tiff") && suffix == QLatin1String("tif"))) {
            return true;
        }
    }
    return false;
}

// ============================================================================
//  尺寸 / 水印 / 冲突策略 的键与显示名
// ============================================================================

QString resizeModeKey(ResizeMode mode)
{
    switch (mode) {
    case ResizeMode::Width:
        return QStringLiteral("width");
    case ResizeMode::Height:
        return QStringLiteral("height");
    case ResizeMode::MaxEdge:
        return QStringLiteral("max_edge");
    case ResizeMode::Percent:
        return QStringLiteral("percent");
    case ResizeMode::Keep:
    default:
        return QStringLiteral("keep");
    }
}

bool resizeModeFromKey(const QString &key, ResizeMode *modeOut)
{
    const QString normalized = key.trimmed().toLower();
    const QVector<QPair<QString, ResizeMode>> table = {
        { QStringLiteral("keep"), ResizeMode::Keep },
        { QStringLiteral("width"), ResizeMode::Width },
        { QStringLiteral("height"), ResizeMode::Height },
        { QStringLiteral("max_edge"), ResizeMode::MaxEdge },
        { QStringLiteral("percent"), ResizeMode::Percent },
    };
    for (const auto &entry : table) {
        if (normalized == entry.first) {
            if (modeOut != nullptr) {
                *modeOut = entry.second;
            }
            return true;
        }
    }
    return false;
}

QString resizeModeDisplayName(ResizeMode mode)
{
    switch (mode) {
    case ResizeMode::Width:
        return QStringLiteral("固定宽度（高度按比例）");
    case ResizeMode::Height:
        return QStringLiteral("固定高度（宽度按比例）");
    case ResizeMode::MaxEdge:
        return QStringLiteral("限制长边（只缩小，不放大）");
    case ResizeMode::Percent:
        return QStringLiteral("按百分比缩放");
    case ResizeMode::Keep:
    default:
        return QStringLiteral("保持原始尺寸");
    }
}

QString watermarkPositionKey(WatermarkPosition position)
{
    switch (position) {
    case WatermarkPosition::TopLeft:
        return QStringLiteral("top_left");
    case WatermarkPosition::TopRight:
        return QStringLiteral("top_right");
    case WatermarkPosition::BottomLeft:
        return QStringLiteral("bottom_left");
    case WatermarkPosition::BottomRight:
        return QStringLiteral("bottom_right");
    case WatermarkPosition::Center:
        return QStringLiteral("center");
    case WatermarkPosition::Tile:
        return QStringLiteral("tile");
    case WatermarkPosition::Disabled:
    default:
        return QStringLiteral("off");
    }
}

bool watermarkPositionFromKey(const QString &key, WatermarkPosition *positionOut)
{
    const QString normalized = key.trimmed().toLower();
    const QVector<QPair<QString, WatermarkPosition>> table = {
        { QStringLiteral("off"), WatermarkPosition::Disabled },
        { QStringLiteral("top_left"), WatermarkPosition::TopLeft },
        { QStringLiteral("top_right"), WatermarkPosition::TopRight },
        { QStringLiteral("bottom_left"), WatermarkPosition::BottomLeft },
        { QStringLiteral("bottom_right"), WatermarkPosition::BottomRight },
        { QStringLiteral("center"), WatermarkPosition::Center },
        { QStringLiteral("tile"), WatermarkPosition::Tile },
    };
    for (const auto &entry : table) {
        if (normalized == entry.first) {
            if (positionOut != nullptr) {
                *positionOut = entry.second;
            }
            return true;
        }
    }
    return false;
}

QString watermarkPositionDisplayName(WatermarkPosition position)
{
    switch (position) {
    case WatermarkPosition::TopLeft:
        return QStringLiteral("左上角");
    case WatermarkPosition::TopRight:
        return QStringLiteral("右上角");
    case WatermarkPosition::BottomLeft:
        return QStringLiteral("左下角");
    case WatermarkPosition::BottomRight:
        return QStringLiteral("右下角");
    case WatermarkPosition::Center:
        return QStringLiteral("正中");
    case WatermarkPosition::Tile:
        return QStringLiteral("平铺整张图");
    case WatermarkPosition::Disabled:
    default:
        return QStringLiteral("不加水印");
    }
}

QString conflictPolicyKey(ConflictPolicy policy)
{
    switch (policy) {
    case ConflictPolicy::Overwrite:
        return QStringLiteral("overwrite");
    case ConflictPolicy::AutoRename:
        return QStringLiteral("auto_rename");
    case ConflictPolicy::Skip:
    default:
        return QStringLiteral("skip");
    }
}

bool conflictPolicyFromKey(const QString &key, ConflictPolicy *policyOut)
{
    const QString normalized = key.trimmed().toLower();
    const QVector<QPair<QString, ConflictPolicy>> table = {
        { QStringLiteral("skip"), ConflictPolicy::Skip },
        { QStringLiteral("overwrite"), ConflictPolicy::Overwrite },
        { QStringLiteral("auto_rename"), ConflictPolicy::AutoRename },
    };
    for (const auto &entry : table) {
        if (normalized == entry.first) {
            if (policyOut != nullptr) {
                *policyOut = entry.second;
            }
            return true;
        }
    }
    return false;
}

QString conflictPolicyDisplayName(ConflictPolicy policy)
{
    switch (policy) {
    case ConflictPolicy::Overwrite:
        return QStringLiteral("覆盖已存在的目标文件");
    case ConflictPolicy::AutoRename:
        return QStringLiteral("自动改名（加序号）");
    case ConflictPolicy::Skip:
    default:
        return QStringLiteral("跳过已存在的目标文件");
    }
}

// ============================================================================
//  计划
// ============================================================================

int Plan::convertibleCount() const
{
    int count = 0;
    for (const Item &item : items) {
        if (item.convertible()) {
            ++count;
        }
    }
    return count;
}

int Plan::problemCount() const
{
    return items.size() - convertibleCount();
}

int Plan::warnedCount() const
{
    int count = 0;
    for (const Item &item : items) {
        if (item.convertible() && !item.warning.isEmpty()) {
            ++count;
        }
    }
    return count;
}

QString Plan::summaryText() const
{
    if (!error.isEmpty()) {
        return QStringLiteral("计划不成立：%1").arg(error);
    }
    if (items.isEmpty()) {
        return QStringLiteral("还没有加入任何图片");
    }

    QStringList parts;
    parts << QStringLiteral("将转换 %1 张").arg(convertibleCount());
    if (problemCount() > 0) {
        parts << QStringLiteral("%1 张有问题（不会动它）").arg(problemCount());
    }
    if (warnedCount() > 0) {
        parts << QStringLiteral("%1 张需要留意").arg(warnedCount());
    }
    parts << QStringLiteral("输出到：%1").arg(outputDir);
    return parts.join(QStringLiteral("；"));
}

QStringList expandInputs(const QStringList &inputs, bool recursive, QStringList *problemOut)
{
    QStringList files;
    QSet<QString> seen;

    const auto appendFile = [&files, &seen](const QString &path) {
        const QString key = QDir::cleanPath(path).toLower();
        if (seen.contains(key)) {
            return; // 同一个文件被加了两次（比如先加文件夹又单独加了它）
        }
        seen.insert(key);
        files.append(QDir::cleanPath(path));
    };

    for (const QString &raw : inputs) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        const QString path = QDir::cleanPath(QFileInfo(trimmed).absoluteFilePath());
        const QFileInfo info(path);

        if (info.isDir()) {
            if (recursive) {
                QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString file = it.next();
                    if (isSupportedImageFile(file)) {
                        appendFile(file);
                    }
                }
            } else {
                const QFileInfoList entries =
                    QDir(path).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
                for (const QFileInfo &entry : entries) {
                    if (isSupportedImageFile(entry.fileName())) {
                        appendFile(entry.absoluteFilePath());
                    }
                }
            }
        } else if (info.isFile()) {
            appendFile(info.absoluteFilePath());
        } else if (problemOut != nullptr) {
            *problemOut << QStringLiteral("%1（路径不存在）").arg(path);
        }
    }

    return files;
}

Plan plan(const QStringList &inputs, const Options &options)
{
    Plan result;

    const QStringList files = expandInputs(inputs, options.recursive);
    if (files.isEmpty()) {
        result.error = QStringLiteral("没有找到任何图片文件（本机可读的格式：%1）")
                           .arg(QString::fromLatin1(
                               QImageReader::supportedImageFormats().join(" / ")));
        return result;
    }

    if (!isFormatAvailable(options.format)) {
        result.error = formatUnavailableReason(options.format);
        return result;
    }
    if (options.nameTemplate.contains(QLatin1Char('/'))
        || options.nameTemplate.contains(QLatin1Char('\\'))) {
        result.error = QStringLiteral("命名模板里不能包含路径分隔符");
        return result;
    }
    if (options.resizeMode != ResizeMode::Keep && options.resizeValue <= 0) {
        result.error = QStringLiteral("缩放参数不合法：数值必须大于 0");
        return result;
    }
    if (options.resizeMode == ResizeMode::Percent
        && (options.resizeValue < 1 || options.resizeValue > 1000)) {
        result.error = QStringLiteral("缩放百分比应在 1 ~ 1000 之间");
        return result;
    }

    // 输出目录：空 = 与源文件同目录（逐项决定）
    if (!options.outputDir.trimmed().isEmpty()) {
        const QString dir =
            QDir::cleanPath(QFileInfo(options.outputDir.trimmed()).absoluteFilePath());
        if (!QDir().mkpath(dir)) {
            result.error = QStringLiteral("输出目录建不出来：%1").arg(dir);
            return result;
        }
        result.outputDir = dir;
    } else {
        result.outputDir = QStringLiteral("与源文件同目录");
    }

    if (formatIsLossy(options.format)) {
        result.notes << QStringLiteral("%1 是有损格式：细节会比原图少（质量 %2）")
                            .arg(formatExtension(options.format).toUpper())
                            .arg(std::clamp(options.quality, 1, 100));
        if (!formatSupportsAlpha(options.format)) {
            result.notes << QStringLiteral("%1 不支持透明：透明像素会被铺成白底")
                                .arg(formatExtension(options.format).toUpper());
        }
    }
    if (options.format == Format::Ico) {
        result.notes << QStringLiteral("ICO 单张最大 %1×%1，超过的会被缩到这个尺寸")
                            .arg(kIcoMaxEdge);
    }

    const QString extension = formatExtension(options.format);
    QSet<QString> taken; // 本批已经安排出去的目标路径（转小写：Windows 下不区分大小写）

    for (int index = 0; index < files.size(); ++index) {
        const QString source = files.at(index);
        const QString sourceName = QFileInfo(source).fileName();

        Item item;
        item.inputPath = source;

        const QString dir = options.outputDir.trimmed().isEmpty() ? QFileInfo(source).absolutePath()
                                                                 : result.outputDir;
        QString desired = buildOutputPath(dir, sourceName, options, index, extension);

        // ★ 纪律 1：绝不原地覆盖源文件
        if (QDir::cleanPath(source).toLower() == QDir::cleanPath(desired).toLower()) {
            item.outputPath = desired;
            item.problem = QStringLiteral("目标与源文件是同一个文件（%1）—— 本功能不会原地覆盖，"
                                          "请换一种目标格式，或把输出目录设到别处")
                               .arg(sourceName);
            result.items.append(item);
            continue;
        }

        // 同一批里两项算出同一个目标：永远自动加序号（否则会"悄悄少一张"）
        if (taken.contains(QDir::cleanPath(desired).toLower())) {
            desired = uniquePath(desired, taken);
            item.warning = QStringLiteral("与本批的另一项算出同一个文件名，已自动加序号：%1")
                               .arg(QFileInfo(desired).fileName());
        }

        if (QFileInfo::exists(desired)) {
            switch (options.conflictPolicy) {
            case ConflictPolicy::Skip:
                item.problem = QStringLiteral("目标已存在（%1），按当前策略跳过 —— "
                                              "想覆盖请把「已存在时」改成「覆盖」")
                                   .arg(QFileInfo(desired).fileName());
                item.skippedByConflict = true;
                break;
            case ConflictPolicy::Overwrite:
                item.warning = QStringLiteral("%1 已存在，转换后会覆盖它")
                                   .arg(QFileInfo(desired).fileName());
                break;
            case ConflictPolicy::AutoRename:
                desired = uniquePath(desired, taken);
                item.warning = QStringLiteral("目标已存在，已自动改名：%1")
                                   .arg(QFileInfo(desired).fileName());
                break;
            }
        }

        item.outputPath = QDir::cleanPath(desired);
        taken.insert(QDir::cleanPath(desired).toLower());
        result.items.append(item);
    }

    return result;
}

// ============================================================================
//  执行
// ============================================================================

QString ApplyResult::summaryText() const
{
    if (!error.isEmpty()) {
        return QStringLiteral("没能开始：%1").arg(error);
    }

    QStringList parts;
    if (cancelled) {
        parts << QStringLiteral("已取消：完成 %1 张").arg(converted.size());
    } else {
        parts << QStringLiteral("完成 %1 张").arg(converted.size());
    }
    if (!skipped.isEmpty()) {
        parts << QStringLiteral("跳过 %1 张").arg(skipped.size());
    }
    if (!failed.isEmpty()) {
        parts << QStringLiteral("失败 %1 张（失败原因逐条列在下面）").arg(failed.size());
    }
    return parts.join(QStringLiteral("；"));
}

QString imageNoteText(Format format, bool hasAlpha)
{
    if (hasAlpha && !formatSupportsAlpha(format)) {
        return QStringLiteral("原图有透明像素，%1 不支持透明：透明处会被铺成白底")
            .arg(formatExtension(format).toUpper());
    }
    if (hasAlpha && formatSupportsAlpha(format)) {
        return QStringLiteral("%1 支持透明，透明像素会原样保留")
            .arg(formatExtension(format).toUpper());
    }
    return QString();
}

bool convertOne(const QString &inputPath, const QString &outputPath, const Options &options,
                QString *errorOut)
{
    const auto fail = [errorOut](const QString &text) {
        if (errorOut != nullptr) {
            *errorOut = text;
        }
        return false;
    };

    QImageReader reader(inputPath);
    reader.setAutoTransform(true); // 手机竖拍的照片带 EXIF 方向，不转正会"躺着"
    const QImage original = reader.read();
    if (original.isNull()) {
        return fail(readerErrorText(reader.error(), reader.errorString()));
    }

    // 元数据要在缩放/铺底之前取：那两步都会生成新图，新图不带文字元数据与色彩配置
    // ⚠ Qt 6 的 QImage::text() 返回的是**单个字符串**（要键），键列表走 textKeys()
    const QStringList sourceTextKeys = original.textKeys();
    const QColorSpace sourceSpace = original.colorSpace();

    QImage image = pixelsOnly(original);
    const QSize target = targetSize(image.size(), options);
    if (target != image.size()) {
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (options.format == Format::Ico
        && (image.width() > kIcoMaxEdge || image.height() > kIcoMaxEdge)) {
        image = image.scaled(kIcoMaxEdge, kIcoMaxEdge, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    image = flattenFor(options.format, image);
    drawWatermark(image, options);

    // 纪律 3 的落地：元数据要么完整带过去，要么彻底不带 —— 不做"带一半"
    // （`image` 已经是"只有像素"的副本，所以这里只负责"要不要放回来"）
    if (options.keepMetadata) {
        for (const QString &key : sourceTextKeys) {
            image.setText(key, original.text(key));
        }
        image.setColorSpace(sourceSpace);
    }

    const QString tempPath = outputPath + kTempSuffix;
    QFile::remove(tempPath); // 上次异常退出留下的残渣

    // ⚠ 写手必须在这个作用域里析构（踩坑 #70）：Windows 上"自己还开着的文件"是改不了名的，
    //   writer 活到 rename 之后 → 每一张都会以"改名这一步出错"收场（自检抓出来的真 bug：盘上留着
    //   一堆 .winease-part 残骸，而用户看到的是"全部失败"）。
    QString writeError;
    {
        QImageWriter writer(tempPath, QByteArray(infoOf(options.format).writer));
        if (formatIsLossy(options.format)) {
            writer.setQuality(std::clamp(options.quality, 1, 100));
        }
        if (options.keepMetadata) {
            for (const QString &key : sourceTextKeys) {
                writer.setText(key, original.text(key));
            }
        }
        if (!writer.write(image)) {
            writeError = writerErrorText(writer.error(), writer.errorString());
        }
    }
    if (!writeError.isEmpty()) {
        QFile::remove(tempPath);
        return fail(writeError);
    }

    // 目标已存在（覆盖策略）时 Windows 的 rename 不会覆盖，得先删
    if (QFileInfo::exists(outputPath) && !QFile::remove(outputPath)) {
        QFile::remove(tempPath);
        return fail(QStringLiteral("目标文件 %1 已存在且删不掉（可能正被别的程序占用）")
                        .arg(QFileInfo(outputPath).fileName()));
    }
    if (!QFile::rename(tempPath, outputPath)) {
        QFile::remove(tempPath);
        return fail(QStringLiteral("写入 %1 失败（改名这一步出错：磁盘满、目录只读或目标被占用）")
                        .arg(QFileInfo(outputPath).fileName()));
    }

    if (options.keepMetadata) {
        QFile written(outputPath);
        if (written.open(QIODevice::ReadWrite)) {
            written.setFileTime(QFileInfo(inputPath).lastModified(),
                                QFileDevice::FileModificationTime);
        }
    }

    if (errorOut != nullptr) {
        errorOut->clear();
    }
    return true;
}

ApplyResult apply(const Plan &plan, const Options &options, Cancel *cancel, const Progress &progress)
{
    ApplyResult result;
    if (!plan.ok()) {
        result.error = plan.error;
        return result;
    }

    const int total = plan.convertibleCount();
    if (progress) {
        progress(QStringLiteral("准备"), 0, total);
    }

    int done = 0;
    for (const Item &item : plan.items) {
        if (!item.convertible()) {
            if (item.skippedByConflict) {
                result.skipped << QStringLiteral("%1（目标已存在，按策略跳过）")
                                      .arg(QFileInfo(item.inputPath).fileName());
            }
            continue;
        }

        // ★ 取消只在"每个文件开工前"检查：已经开始的那一张一定会写完再退出，
        //   磁盘上不会留下半张图
        if (cancel != nullptr && cancel->isRequested()) {
            result.cancelled = true;
            break;
        }

        QString reason;
        if (convertOne(item.inputPath, item.outputPath, options, &reason)) {
            result.converted.append(qMakePair(item.inputPath, item.outputPath));
        } else {
            result.failed << QStringLiteral("%1：%2")
                                 .arg(QFileInfo(item.inputPath).fileName(), reason);
        }

        ++done;
        if (progress) {
            progress(QStringLiteral("转换"), done, total);
        }
    }

    result.ok = true;
    return result;
}

} // namespace WinEase::FeaturePlugins::ImageConvertEngine
