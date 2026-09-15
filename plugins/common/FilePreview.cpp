#include "FilePreview.h"

#include "win32/ShellUtils.h"
#include "win32/WinRtSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QStringConverter>
#include <QStringList>

#include <winerror.h> // E_FAIL（throw winrt::hresult_error 用）

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <vector>

namespace WinEase::FeaturePlugins::FilePreview {

namespace {

/// 各类扩展名（不含点、小写）。刻意**只列常见项**：
/// 认不出来的一律走"系统缩略图/类型图标"这条路，比塞几百个后缀更省心。
const QStringList &textExtensions()
{
    static const QStringList list = {
        QStringLiteral("txt"),   QStringLiteral("log"),    QStringLiteral("md"),
        QStringLiteral("markdown"), QStringLiteral("ini"), QStringLiteral("cfg"),
        QStringLiteral("conf"),  QStringLiteral("json"),   QStringLiteral("xml"),
        QStringLiteral("yaml"),  QStringLiteral("yml"),    QStringLiteral("toml"),
        QStringLiteral("csv"),   QStringLiteral("tsv"),    QStringLiteral("sql"),
        QStringLiteral("bat"),   QStringLiteral("cmd"),    QStringLiteral("ps1"),
        QStringLiteral("sh"),    QStringLiteral("py"),     QStringLiteral("js"),
        QStringLiteral("ts"),    QStringLiteral("jsx"),    QStringLiteral("tsx"),
        QStringLiteral("c"),     QStringLiteral("h"),      QStringLiteral("cpp"),
        QStringLiteral("hpp"),   QStringLiteral("cc"),     QStringLiteral("cxx"),
        QStringLiteral("cs"),    QStringLiteral("java"),   QStringLiteral("go"),
        QStringLiteral("rs"),    QStringLiteral("rb"),     QStringLiteral("php"),
        QStringLiteral("html"),  QStringLiteral("htm"),    QStringLiteral("css"),
        QStringLiteral("scss"),  QStringLiteral("less"),   QStringLiteral("vue"),
        QStringLiteral("srt"),   QStringLiteral("ass"),    QStringLiteral("vtt"),
        QStringLiteral("reg"),   QStringLiteral("env"),    QStringLiteral("properties"),
        QStringLiteral("gradle"), QStringLiteral("cmake"), QStringLiteral("diff"),
        QStringLiteral("patch"), QStringLiteral("gitignore"), QStringLiteral("editorconfig"),
    };
    return list;
}

const QStringList &imageExtensions()
{
    // svg 也算进来：Qt 没装 QtSvg 时 loadImage 会失败，由 loadBitmap 回退到系统缩略图
    static const QStringList list = {
        QStringLiteral("png"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"),  QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("ico"),
        QStringLiteral("svg"), QStringLiteral("heic"), QStringLiteral("avif"),
    };
    return list;
}

const QStringList &mediaExtensions()
{
    static const QStringList list = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"),  QStringLiteral("wmv"), QStringLiteral("flv"),
        QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("mp3"), QStringLiteral("wav"),
        QStringLiteral("flac"), QStringLiteral("m4a"), QStringLiteral("aac"),
        QStringLiteral("ogg"),  QStringLiteral("wma"),
    };
    return list;
}

const QStringList &archiveExtensions()
{
    static const QStringList list = {
        QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"),
        QStringLiteral("tar"), QStringLiteral("gz"),  QStringLiteral("bz2"),
        QStringLiteral("xz"),  QStringLiteral("iso"), QStringLiteral("cab"),
    };
    return list;
}

/// QString → winrt::hstring
///
/// ⚠ 不走 `QString::toStdWString()`：Debug 与 Release 的 STL 布局不一致
///   （见 docs/ROADMAP.md 环境约束 1），跨 STL 传 std::wstring 时会踩内存。
///   直接对 QString 的 UTF-16 缓冲取一个 string_view，hstring 构造时自己拷贝。
winrt::hstring toHString(const QString &text)
{
    const auto *data = reinterpret_cast<const wchar_t *>(text.utf16());
    return winrt::hstring(std::wstring_view(data, static_cast<size_t>(text.size())));
}

} // namespace

// ============================================================================
//  类型判定
// ============================================================================

QString kindDisplayName(Kind kind)
{
    switch (kind) {
    case Kind::Text:
        return QStringLiteral("文本");
    case Kind::Image:
        return QStringLiteral("图片");
    case Kind::Pdf:
        return QStringLiteral("PDF");
    case Kind::Media:
        return QStringLiteral("音视频");
    case Kind::Archive:
        return QStringLiteral("压缩包");
    case Kind::Other:
        break;
    }
    return QStringLiteral("其它");
}

Kind classifyByExtension(const QString &extension)
{
    const QString ext = extension.toLower();
    if (ext.isEmpty()) {
        return Kind::Other;
    }
    if (textExtensions().contains(ext)) {
        return Kind::Text;
    }
    if (imageExtensions().contains(ext)) {
        return Kind::Image;
    }
    if (ext == QLatin1String("pdf")) {
        return Kind::Pdf;
    }
    if (mediaExtensions().contains(ext)) {
        return Kind::Media;
    }
    if (archiveExtensions().contains(ext)) {
        return Kind::Archive;
    }
    return Kind::Other;
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("-");
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    static const char *const units[] = { "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(QString::number(value, 'f', value < 10.0 ? 1 : 0), QString::fromLatin1(units[unit]));
}

Info describe(const QString &path)
{
    Info info;
    info.path = path;
    const QFileInfo fileInfo(path);
    info.exists = fileInfo.exists();
    info.isDirectory = fileInfo.isDir();
    info.fileName = fileInfo.fileName();
    info.extension = fileInfo.suffix().toLower();
    info.sizeBytes = fileInfo.size();
    info.modified = fileInfo.lastModified();
    info.kind = info.isDirectory ? Kind::Other : classifyByExtension(info.extension);
    if (info.exists) {
        info.typeDescription = WinEase::Win32::fileTypeDescription(path);
    }
    return info;
}

QString Info::summary() const
{
    QStringList parts;
    parts.append(typeDescription.isEmpty() ? kindDisplayName(kind) : typeDescription);
    if (!isDirectory) {
        parts.append(formatBytes(sizeBytes));
    }
    if (modified.isValid()) {
        parts.append(modified.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    const QString title = fileName.isEmpty() ? path : fileName;
    return QStringLiteral("%1 · %2").arg(title, parts.join(QStringLiteral(" · ")));
}

// ============================================================================
//  文本预览
// ============================================================================

QString hexDump(const QByteArray &data, int maxBytes)
{
    QStringList lines;
    const int count = qMin(data.size(), qMax(maxBytes, 0));
    for (int offset = 0; offset < count; offset += 16) {
        const QByteArray chunk = data.mid(offset, 16);
        QString hexPart;
        QString textPart;
        for (int i = 0; i < 16; ++i) {
            if (i < chunk.size()) {
                const auto byte = static_cast<uchar>(chunk.at(i));
                hexPart += QStringLiteral("%1 ").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
                textPart += (byte >= 32 && byte < 127) ? QLatin1Char(static_cast<char>(byte))
                                                       : QLatin1Char('.');
            } else {
                hexPart += QStringLiteral("   ");
                textPart += QLatin1Char(' ');
            }
        }
        lines.append(QStringLiteral("%1  %2 |%3|")
                         .arg(QString::number(offset, 16).rightJustified(8, QLatin1Char('0')))
                         .arg(hexPart, textPart));
    }
    return lines.join(QLatin1Char('\n'));
}

TextResult readText(const QString &path, qint64 maxBytes)
{
    TextResult result;
    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        result.error = QStringLiteral("文件不存在");
        return result;
    }
    if (fileInfo.isDir()) {
        result.error = QStringLiteral("这是文件夹，不支持预览内容");
        return result;
    }
    result.fileSize = fileInfo.size();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        return result;
    }

    // ★ 只读文件头部：100 MB 的日志也只有这么点进内存
    const qint64 want = qBound<qint64>(1024, maxBytes, 64LL * 1024 * 1024);
    const QByteArray head = file.read(want);
    result.bytesRead = head.size();
    result.truncated = result.bytesRead < result.fileSize;
    result.textBytes = result.bytesRead;

    // 二进制判定：文件头里有 NUL 就基本可以断定不是文本
    // （不这么做的话，十六进制文件会被当"乱码文本"显示，用户完全看不懂）
    const int probe = static_cast<int>(qMin<qint64>(head.size(), 4096));
    const bool hasNul = head.left(probe).contains('\0');
    if (hasNul) {
        result.binary = true;
        result.encoding = QStringLiteral("二进制");
        result.text = hexDump(head, 4096);
        result.textBytes = qMin<qint64>(head.size(), 4096);
        result.ok = true;
        return result;
    }

    QStringDecoder utf8Decoder(QStringDecoder::Utf8);
    QString decoded = utf8Decoder.decode(head);
    if (utf8Decoder.hasError()) {
        // 不是合法 UTF-8：多半是本地代码页（GBK 等）的老文件
        result.encoding = QStringLiteral("非 UTF-8（按系统代码页解读）");
        decoded = QString::fromLocal8Bit(head);
    } else {
        result.encoding = QStringLiteral("UTF-8");
    }
    if (decoded.startsWith(QChar(0xFEFF))) {
        decoded.remove(0, 1); // 去掉 BOM：否则界面上会显示一个看不见的"空格"
    }
    result.text = decoded;
    result.ok = true;
    return result;
}

QString TextResult::sizeText() const
{
    if (fileSize <= 0) {
        return QString();
    }
    if (!truncated) {
        return QStringLiteral("已完整载入 %1").arg(formatBytes(fileSize));
    }
    return QStringLiteral("已显示前 %1 / 共 %2").arg(formatBytes(bytesRead), formatBytes(fileSize));
}

// ============================================================================
//  位图预览
// ============================================================================

QString BitmapResult::describe() const
{
    if (!ok) {
        return error;
    }
    if (!scaled || originalSize.isEmpty()) {
        return QStringLiteral("%1：%2×%3").arg(source).arg(image.width()).arg(image.height());
    }
    return QStringLiteral("%1：%2×%3 → 按窗口缩放为 %4×%5")
        .arg(source)
        .arg(originalSize.width())
        .arg(originalSize.height())
        .arg(image.width())
        .arg(image.height());
}

BitmapResult loadImage(const QString &path, const QSize &maxSize)
{
    BitmapResult result;
    result.source = QStringLiteral("图片解码");

    QImageReader reader(path);
    reader.setAutoTransform(true); // 手机照片的 EXIF 方向要认
    const QSize original = reader.size();
    if (!original.isValid()) {
        result.error = QStringLiteral("无法识别图片格式：%1").arg(reader.errorString());
        return result;
    }
    result.originalSize = original;

    // 只缩小、不放大：把小图放大成糊的大图没有任何意义
    // ⚠ QSize::scaled() 是"缩放去适配"（目标框比原图大时它会**放大**），
    //   所以必须先自己判一次"是不是真的超出目标框"——只靠 target != original 判不出来
    const bool tooLarge = original.width() > maxSize.width() || original.height() > maxSize.height();
    if (maxSize.width() > 0 && maxSize.height() > 0 && tooLarge) {
        const QSize target = original.scaled(maxSize, Qt::KeepAspectRatio);
        if (target.width() > 0 && target.height() > 0 && target != original) {
            reader.setScaledSize(target); // ★ 让解码器直接出目标尺寸
            result.scaled = true;
        }
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        result.error = QStringLiteral("解码失败：%1").arg(reader.errorString());
        return result;
    }
    result.image = image;
    result.ok = true;
    return result;
}

BitmapResult loadShellThumbnail(const QString &path, const QSize &maxSize)
{
    BitmapResult result;
    result.source = QStringLiteral("系统缩略图");

    const int edge = qMax(qMax(maxSize.width(), maxSize.height()), 256);
    const WinEase::Win32::ThumbnailResult thumbnail = WinEase::Win32::filePreview(path, edge);
    if (!thumbnail.isValid()) {
        result.error = thumbnail.error.isEmpty() ? QStringLiteral("系统无法生成缩略图")
                                                : thumbnail.error;
        return result;
    }
    result.image = thumbnail.image;
    result.originalSize = thumbnail.image.size();
    if (thumbnail.fromShellIcon) {
        // 只是类型图标（= "系统也拿不出内容"），界面上要如实标出来
        result.source = QStringLiteral("类型图标");
    }
    result.ok = true;
    return result;
}

bool pdfAvailable(QString *errorOut)
{
    // RoGetActivationFactory 要求当前线程已初始化 COM；
    // ensureWinRtReady 会做这件事（并且是幂等的）
    QString error;
    if (!WinEase::Win32::ensureWinRtReady(&error)) {
        if (errorOut != nullptr) {
            *errorOut = error;
        }
        return false;
    }
    return WinEase::Win32::probeWinRtClass(L"Windows.Data.Pdf.PdfDocument", errorOut);
}

int pdfPageCount(const QString &path, QString *errorOut)
{
    int count = 0;
    QString error;
    const bool ok = WinEase::Win32::runWinRt(QStringLiteral("读取 PDF 页数"), &error, [&] {
        using namespace winrt::Windows::Data::Pdf;
        using namespace winrt::Windows::Storage;

        const StorageFile file = StorageFile::GetFileFromPathAsync(toHString(path)).get();
        const PdfDocument document = PdfDocument::LoadFromFileAsync(file).get();
        count = static_cast<int>(document.PageCount());
    });
    if (!ok) {
        count = -1;
    }
    if (errorOut != nullptr) {
        *errorOut = error;
    }
    return count;
}

BitmapResult renderPdfPage(const QString &path, int pageIndex, const QSize &maxSize)
{
    BitmapResult result;
    result.source = QStringLiteral("PDF 页面");

    QString error;
    QByteArray pixels;
    int renderedWidth = 0;
    int renderedHeight = 0;

    const bool ok = WinEase::Win32::runWinRt(QStringLiteral("渲染 PDF 页面"), &error, [&] {
        using namespace winrt::Windows::Data::Pdf;
        using namespace winrt::Windows::Storage;
        using namespace winrt::Windows::Storage::Streams;

        const StorageFile file = StorageFile::GetFileFromPathAsync(toHString(path)).get();
        const PdfDocument document = PdfDocument::LoadFromFileAsync(file).get();
        const uint32_t pageCount = document.PageCount();
        if (pageCount == 0) {
            throw winrt::hresult_error(E_FAIL, L"这个 PDF 里没有页面");
        }
        // ★ 只取一页：整本载入是"打开一个 PDF 等三秒"的根源
        const uint32_t index = static_cast<uint32_t>(qBound(0, pageIndex, static_cast<int>(pageCount) - 1));
        const PdfPage page = document.GetPage(index);

        PdfPageRenderOptions options;
        const auto pageSize = page.Size(); // DIP（1/96 英寸）
        double scale = 1.0;
        if (maxSize.width() > 0 && maxSize.height() > 0 && pageSize.Width > 0.0
            && pageSize.Height > 0.0) {
            const double byWidth = maxSize.width() / pageSize.Width;
            const double byHeight = maxSize.height() / pageSize.Height;
            scale = qMin(byWidth, byHeight);
            if (scale < 1.0) {
                options.DestinationWidth(static_cast<uint32_t>(pageSize.Width * scale * 1.0));
                options.DestinationHeight(static_cast<uint32_t>(pageSize.Height * scale));
                result.scaled = true;
            } else {
                scale = 1.0;
            }
        }

        InMemoryRandomAccessStream stream;
        page.RenderToStreamAsync(stream, options).get();

        const uint32_t total = static_cast<uint32_t>(stream.Size());
        DataReader reader = DataReader(stream.GetInputStreamAt(0));
        reader.LoadAsync(total).get();
        std::vector<uint8_t> bytes(total);
        reader.ReadBytes(bytes);

        pixels = QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(total));
        renderedWidth = static_cast<int>(pageSize.Width * scale);
        renderedHeight = static_cast<int>(pageSize.Height * scale);
    });

    if (!ok) {
        result.error = error;
        return result;
    }

    // WinRT 渲染出来的是 PNG 字节流；交给 Qt 解码（格式自动识别）
    QImage image = QImage::fromData(pixels);
    if (image.isNull()) {
        result.error = QStringLiteral("PDF 页面字节流无法解码");
        return result;
    }
    result.image = image;
    result.originalSize = QSize(renderedWidth, renderedHeight);
    result.ok = true;
    return result;
}

BitmapResult loadBitmap(const QString &path, Kind kind, const QSize &maxSize)
{
    switch (kind) {
    case Kind::Image: {
        const BitmapResult decoded = loadImage(path, maxSize);
        if (decoded.ok) {
            return decoded;
        }
        // Qt 解码不了（例如没装 SVG 插件）→ 至少给个系统缩略图，别显示一片空白
        BitmapResult fallback = loadShellThumbnail(path, maxSize);
        if (!fallback.ok) {
            fallback.error = decoded.error;
        }
        return fallback;
    }
    case Kind::Pdf: {
        QString pdfError;
        if (pdfAvailable(&pdfError)) {
            const BitmapResult page = renderPdfPage(path, 0, maxSize);
            if (page.ok) {
                return page;
            }
            BitmapResult fallback = loadShellThumbnail(path, maxSize);
            if (!fallback.ok) {
                fallback.error = page.error;
            }
            return fallback;
        }
        BitmapResult fallback = loadShellThumbnail(path, maxSize);
        if (!fallback.ok) {
            fallback.error = pdfError.isEmpty() ? QStringLiteral("本机不支持 PDF 渲染") : pdfError;
        }
        return fallback;
    }
    case Kind::Text:
        // 文本走 readText()；走到这里说明调用方传错了 kind
        break;
    case Kind::Media:
    case Kind::Archive:
    case Kind::Other:
    case Kind::Unsupported:
        break;
    }
    // 音视频（首帧）、Office、压缩包、未知类型：一律交给系统缩略图
    return loadShellThumbnail(path, maxSize);
}

} // namespace WinEase::FeaturePlugins::FilePreview
