#include "TextEncodingTools.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QSet>
#include <QVector>

namespace WinEase::FeaturePlugins::TextEncodingTools {

namespace {

const char *kUtf8Bom = "\xEF\xBB\xBF";
constexpr int kUtf8BomSize = 3;
const QString kTempSuffix = QStringLiteral(".winease-part");

/// 目标代码页。`Ansi` **不硬编码 936**：本机 ANSI 是什么就用什么
UINT codePageOf(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8:
    case Encoding::Utf8Bom:
        return 65001;
    case Encoding::Utf16Le:
        return 1200;
    case Encoding::Utf16Be:
        return 1201;
    case Encoding::Gbk:
        return 936;
    case Encoding::Ansi:
    default:
        return ::GetACP();
    }
}

/// 该代码页是否支持 MB_ERR_INVALID_CHARS（"遇到非法字节就报错"）。
/// ⚠ 传了不被支持的代码页时 Win32 会**整个调用失败**，所以这里要挑着传。
bool supportsStrictFlag(UINT codePage)
{
    return codePage == 65001 || codePage == 936 || codePage == 54936;
}

bool looksLikeUtf8(const QByteArray &data)
{
    // 用"严格模式"的转换去问，而不是 QString::fromUtf8 —— 后者遇到坏字节会**静默**
    // 插替换字符，那正好是我们最不想要的结果
    const int wide = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.constData(),
                                           data.size(), nullptr, 0);
    return wide > 0 || data.isEmpty();
}

bool looksLikeCodePage(const QByteArray &data, UINT codePage)
{
    const DWORD flags = supportsStrictFlag(codePage) ? MB_ERR_INVALID_CHARS : 0;
    const int wide = ::MultiByteToWideChar(codePage, flags, data.constData(), data.size(), nullptr, 0);
    return wide > 0 || data.isEmpty();
}

bool hasHighBytes(const QByteArray &data)
{
    for (const char byte : data) {
        if (static_cast<unsigned char>(byte) >= 0x80) {
            return true;
        }
    }
    return false;
}

QString normalizeLineEndings(const QString &text, LineEnding lineEnding)
{
    if (lineEnding == LineEnding::Keep) {
        return text;
    }

    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (lineEnding == LineEnding::Crlf) {
        normalized.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    }
    return normalized;
}

} // namespace

// ============================================================================
//  编码表的键与显示名
// ============================================================================

QString encodingKey(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8Bom:
        return QStringLiteral("utf8_bom");
    case Encoding::Utf16Le:
        return QStringLiteral("utf16le");
    case Encoding::Utf16Be:
        return QStringLiteral("utf16be");
    case Encoding::Gbk:
        return QStringLiteral("gbk");
    case Encoding::Ansi:
        return QStringLiteral("ansi");
    case Encoding::Utf8:
    default:
        return QStringLiteral("utf8");
    }
}

bool encodingFromKey(const QString &key, Encoding *encodingOut)
{
    const QString normalized = key.trimmed().toLower();
    const QVector<QPair<QString, Encoding>> table = {
        { QStringLiteral("utf8"), Encoding::Utf8 },
        { QStringLiteral("utf8_bom"), Encoding::Utf8Bom },
        { QStringLiteral("utf16le"), Encoding::Utf16Le },
        { QStringLiteral("utf16be"), Encoding::Utf16Be },
        { QStringLiteral("gbk"), Encoding::Gbk },
        { QStringLiteral("ansi"), Encoding::Ansi },
    };
    for (const auto &entry : table) {
        if (normalized == entry.first) {
            if (encodingOut != nullptr) {
                *encodingOut = entry.second;
            }
            return true;
        }
    }
    return false;
}

QString encodingDisplayName(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8Bom:
        return QStringLiteral("UTF-8 带 BOM");
    case Encoding::Utf16Le:
        return QStringLiteral("UTF-16 小端（记事本的“Unicode”）");
    case Encoding::Utf16Be:
        return QStringLiteral("UTF-16 大端");
    case Encoding::Gbk:
        return QStringLiteral("GBK / CP936（简体中文）");
    case Encoding::Ansi:
        return QStringLiteral("本机 ANSI（CP%1）").arg(::GetACP());
    case Encoding::Utf8:
    default:
        return QStringLiteral("UTF-8 不带 BOM");
    }
}

bool encodingWritesBom(Encoding encoding)
{
    return encoding == Encoding::Utf8Bom || encoding == Encoding::Utf16Le
           || encoding == Encoding::Utf16Be;
}

QString lineEndingKey(LineEnding lineEnding)
{
    switch (lineEnding) {
    case LineEnding::Lf:
        return QStringLiteral("lf");
    case LineEnding::Crlf:
        return QStringLiteral("crlf");
    case LineEnding::Keep:
    default:
        return QStringLiteral("keep");
    }
}

bool lineEndingFromKey(const QString &key, LineEnding *lineEndingOut)
{
    const QString normalized = key.trimmed().toLower();
    const QVector<QPair<QString, LineEnding>> table = {
        { QStringLiteral("keep"), LineEnding::Keep },
        { QStringLiteral("lf"), LineEnding::Lf },
        { QStringLiteral("crlf"), LineEnding::Crlf },
    };
    for (const auto &entry : table) {
        if (normalized == entry.first) {
            if (lineEndingOut != nullptr) {
                *lineEndingOut = entry.second;
            }
            return true;
        }
    }
    return false;
}

QString lineEndingDisplayName(LineEnding lineEnding)
{
    switch (lineEnding) {
    case LineEnding::Lf:
        return QStringLiteral("改写成 LF（Unix）");
    case LineEnding::Crlf:
        return QStringLiteral("改写成 CRLF（Windows）");
    case LineEnding::Keep:
    default:
        return QStringLiteral("保持原样");
    }
}

// ============================================================================
//  探测
// ============================================================================

Detection detect(const QByteArray &data)
{
    Detection result;

    if (data.isEmpty()) {
        result.encoding = Encoding::Utf8;
        result.confident = false;
        result.reason = QStringLiteral("文件是空的，没有任何字节可供判断");
        return result;
    }

    if (data.startsWith(QByteArray(kUtf8Bom, kUtf8BomSize))) {
        result.encoding = Encoding::Utf8Bom;
        result.confident = true;
        result.reason = QStringLiteral("开头是 UTF-8 BOM（EF BB BF）");
        return result;
    }

    if (data.size() >= 2) {
        const unsigned char first = static_cast<unsigned char>(data.at(0));
        const unsigned char second = static_cast<unsigned char>(data.at(1));
        if (first == 0xFF && second == 0xFE) {
            result.encoding = Encoding::Utf16Le;
            result.confident = true;
            result.reason = QStringLiteral("开头是 UTF-16 小端 BOM（FF FE）");
            return result;
        }
        if (first == 0xFE && second == 0xFF) {
            result.encoding = Encoding::Utf16Be;
            result.confident = true;
            result.reason = QStringLiteral("开头是 UTF-16 大端 BOM（FE FF）");
            return result;
        }
    }

    if (!hasHighBytes(data)) {
        result.encoding = Encoding::Utf8;
        result.confident = true;
        result.reason = QStringLiteral("全部是 ASCII 字符（ASCII 同时也是 GBK 的子集，按 UTF-8 写出最稳）");
        return result;
    }

    if (looksLikeUtf8(data)) {
        result.encoding = Encoding::Utf8;
        result.confident = true;
        result.reason = QStringLiteral("字节序列是合法 UTF-8（一个非法字节都没有）");
        return result;
    }

    if (looksLikeCodePage(data, 936)) {
        result.encoding = Encoding::Gbk;
        result.confident = true;
        result.reason = QStringLiteral("不是合法 UTF-8，但按 GBK 能解出全部字符");
        return result;
    }

    result.encoding = Encoding::Ansi;
    result.confident = false;
    result.reason = QStringLiteral("既不是合法 UTF-8 也不是合法 GBK，按本机 ANSI（CP%1）读取，"
                                   "可能出现乱码")
                        .arg(::GetACP());
    return result;
}

// ============================================================================
//  解码 / 编码
// ============================================================================

bool decode(const QByteArray &data, Encoding encoding, QString *textOut, QString *errorOut)
{
    const auto fail = [errorOut](const QString &text) {
        if (errorOut != nullptr) {
            *errorOut = text;
        }
        return false;
    };

    if (textOut == nullptr) {
        return fail(QStringLiteral("内部错误：没有接收结果的字符串"));
    }

    switch (encoding) {
    case Encoding::Utf8:
    case Encoding::Utf8Bom: {
        // 带不带 BOM 都收：BOM 只是标记，不该让"用错选项"变成失败
        QByteArray payload = data;
        if (payload.startsWith(QByteArray(kUtf8Bom, kUtf8BomSize))) {
            payload.remove(0, kUtf8BomSize);
        }
        const int wide = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, payload.constData(),
                                               payload.size(), nullptr, 0);
        if (wide <= 0) {
            return fail(QStringLiteral("不是合法的 UTF-8 字节序列（Win32 拒绝转换这段字节）"));
        }
        QVector<wchar_t> buffer(wide);
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, payload.constData(), payload.size(),
                              buffer.data(), wide);
        *textOut = QString::fromWCharArray(buffer.constData(), wide);
        return true;
    }
    case Encoding::Utf16Le:
    case Encoding::Utf16Be: {
        if (data.size() % 2 != 0) {
            return fail(QStringLiteral("UTF-16 的字节数必须是偶数，实际是 %1 字节（文件可能不完整）")
                            .arg(data.size()));
        }
        QVector<ushort> units(data.size() / 2);
        for (int i = 0; i < units.size(); ++i) {
            const int offset = i * 2;
            const ushort low = static_cast<unsigned char>(data.at(offset));
            const ushort high = static_cast<unsigned char>(data.at(offset + 1));
            units[i] = static_cast<ushort>(encoding == Encoding::Utf16Le
                                               ? (low | (high << 8))
                                               : ((low << 8) | high));
        }
        if (!units.isEmpty() && units.first() == 0xFEFF) {
            units.removeFirst(); // 吃掉 BOM，不把它留在正文里
        }
        *textOut = QString::fromUtf16(reinterpret_cast<const char16_t *>(units.constData()),
                                      units.size());
        return true;
    }
    case Encoding::Gbk:
    case Encoding::Ansi:
    default: {
        const UINT codePage = codePageOf(encoding);
        const DWORD flags = supportsStrictFlag(codePage) ? MB_ERR_INVALID_CHARS : 0;
        const int wide =
            ::MultiByteToWideChar(codePage, flags, data.constData(), data.size(), nullptr, 0);
        if (wide <= 0) {
            return fail(QStringLiteral("按 %1 解不出来（包含该编码里不合法的字节）")
                            .arg(encodingDisplayName(encoding)));
        }
        QVector<wchar_t> buffer(wide);
        ::MultiByteToWideChar(codePage, flags, data.constData(), data.size(), buffer.data(), wide);
        *textOut = QString::fromWCharArray(buffer.constData(), wide);
        return true;
    }
    }
}

bool encode(const QString &text, Encoding encoding, bool withBom, QByteArray *bytesOut,
            QString *errorOut)
{
    const auto fail = [errorOut](const QString &message) {
        if (errorOut != nullptr) {
            *errorOut = message;
        }
        return false;
    };

    if (bytesOut == nullptr) {
        return fail(QStringLiteral("内部错误：没有接收结果的缓冲区"));
    }

    if (encoding == Encoding::Utf16Le || encoding == Encoding::Utf16Be) {
        QByteArray bytes;
        if (withBom) {
            bytes.append(encoding == Encoding::Utf16Le ? QByteArray("\xFF\xFE", 2)
                                                       : QByteArray("\xFE\xFF", 2));
        }
        for (int i = 0; i < text.size(); ++i) {
            const ushort unit = text.at(i).unicode();
            const char low = static_cast<char>(unit & 0xFF);
            const char high = static_cast<char>((unit >> 8) & 0xFF);
            if (encoding == Encoding::Utf16Le) {
                bytes.append(low);
                bytes.append(high);
            } else {
                bytes.append(high);
                bytes.append(low);
            }
        }
        *bytesOut = bytes;
        return true;
    }

    const UINT codePage = codePageOf(encoding);
    const wchar_t *wide = reinterpret_cast<const wchar_t *>(text.utf16());
    const int wideLength = text.size();

    // ⚠ 65001 不允许传 lpDefaultChar / lpUsedDefaultChar（Win32 会直接失败）
    const bool canDetectLossy = codePage != 65001 && supportsStrictFlag(codePage);
    const DWORD flags = canDetectLossy ? WC_NO_BEST_FIT_CHARS : 0;
    BOOL usedDefault = FALSE;

    const int narrow = ::WideCharToMultiByte(codePage, flags, wide, wideLength, nullptr, 0, nullptr,
                                             canDetectLossy ? &usedDefault : nullptr);
    if (narrow <= 0) {
        return fail(QStringLiteral("写不出 %1 格式").arg(encodingDisplayName(encoding)));
    }

    QByteArray bytes(narrow, Qt::Uninitialized);
    ::WideCharToMultiByte(codePage, flags, wide, wideLength, bytes.data(), narrow, nullptr,
                          canDetectLossy ? &usedDefault : nullptr);

    if (canDetectLossy && usedDefault) {
        return fail(QStringLiteral("有字符在 %1 里没有对应（emoji、生僻字、日文假名等）—— "
                                   "换成 UTF-8 才能把它们保住")
                        .arg(encodingDisplayName(encoding)));
    }

    if (encoding == Encoding::Utf8Bom && withBom) {
        bytes.prepend(QByteArray(kUtf8Bom, kUtf8BomSize));
    }

    *bytesOut = bytes;
    return true;
}

// ============================================================================
//  文件级
// ============================================================================

bool isTextFile(const QString &fileNameOrPath)
{
    static const QSet<QString> kExtensions = {
        QStringLiteral("txt"),  QStringLiteral("csv"),  QStringLiteral("tsv"),
        QStringLiteral("json"), QStringLiteral("xml"),  QStringLiteral("ini"),
        QStringLiteral("log"),  QStringLiteral("md"),   QStringLiteral("yml"),
        QStringLiteral("yaml"), QStringLiteral("conf"), QStringLiteral("cfg"),
        QStringLiteral("sql"),  QStringLiteral("bat"),  QStringLiteral("cmd"),
        QStringLiteral("ps1"),  QStringLiteral("sh"),   QStringLiteral("c"),
        QStringLiteral("h"),    QStringLiteral("cpp"),  QStringLiteral("hpp"),
        QStringLiteral("cs"),   QStringLiteral("java"), QStringLiteral("py"),
        QStringLiteral("js"),   QStringLiteral("ts"),   QStringLiteral("html"),
        QStringLiteral("css"),  QStringLiteral("srt"),  QStringLiteral("lrc"),
        QStringLiteral("nfo"),  QStringLiteral("reg"),  QStringLiteral("properties"),
    };
    const QString suffix = QFileInfo(fileNameOrPath).suffix().toLower();
    return !suffix.isEmpty() && kExtensions.contains(suffix);
}

QString detectionSummary(const Detection &detection)
{
    if (detection.reason.isEmpty()) {
        return encodingDisplayName(detection.encoding);
    }
    return QStringLiteral("%1%2 ｜ 依据：%3")
        .arg(encodingDisplayName(detection.encoding),
             detection.confident ? QString() : QStringLiteral("（推测）"), detection.reason);
}

bool convertFile(const QString &inputPath, const QString &outputPath, Encoding targetEncoding,
                 LineEnding lineEnding, Encoding *detectedSourceOut, QString *errorOut)
{
    const auto fail = [errorOut](const QString &text) {
        if (errorOut != nullptr) {
            *errorOut = text;
        }
        return false;
    };

    // ★ 绝不原地覆盖源文件（与图片侧同一纪律）
    if (QDir::cleanPath(QFileInfo(inputPath).absoluteFilePath()).toLower()
        == QDir::cleanPath(QFileInfo(outputPath).absoluteFilePath()).toLower()) {
        return fail(QStringLiteral("目标与源文件是同一个文件 —— 本功能不会原地覆盖，"
                                   "请换一种目标编码，或把输出目录设到别处"));
    }

    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("读不到文件：%1").arg(input.errorString()));
    }
    const QByteArray data = input.readAll();
    input.close();

    const Detection detection = detect(data);
    if (detectedSourceOut != nullptr) {
        *detectedSourceOut = detection.encoding;
    }

    QString text;
    if (!decode(data, detection.encoding, &text, errorOut)) {
        return false;
    }

    const QString converted = normalizeLineEndings(text, lineEnding);

    QByteArray bytes;
    if (!encode(converted, targetEncoding, encodingWritesBom(targetEncoding), &bytes, errorOut)) {
        return false;
    }

    const QString parent = QFileInfo(outputPath).absolutePath();
    if (!QDir().mkpath(parent)) {
        return fail(QStringLiteral("输出目录建不出来：%1").arg(parent));
    }

    const QString tempPath = outputPath + kTempSuffix;
    QFile::remove(tempPath);

    QFile temp(tempPath);
    if (!temp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("临时文件写不进去（目录只读或磁盘满）：%1").arg(temp.errorString()));
    }
    const qint64 written = temp.write(bytes);
    temp.close();
    if (written != bytes.size()) {
        QFile::remove(tempPath);
        return fail(QStringLiteral("写入没有写全（磁盘可能满了）"));
    }

    if (QFileInfo::exists(outputPath) && !QFile::remove(outputPath)) {
        QFile::remove(tempPath);
        return fail(QStringLiteral("目标文件已存在且删不掉（可能正被别的程序占用）"));
    }
    if (!QFile::rename(tempPath, outputPath)) {
        QFile::remove(tempPath);
        return fail(QStringLiteral("写入目标文件失败（改名这一步出错）"));
    }

    return true;
}

} // namespace WinEase::FeaturePlugins::TextEncodingTools
