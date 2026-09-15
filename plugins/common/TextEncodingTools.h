#pragma once

// ============================================================================
//  TextEncodingTools.h —— 文本文件的**字符集**转换（P3-03 批量格式转换的文本侧）
//
//  与 P1-11「编码转换」（Base64 / URL / 哈希）无关：那一个转的是"同一段文字换个
//  表示法"，这一个解决的是**"这段字节到底是什么字符"** —— 中文用户最典型的困境是
//  "老程序导出的 GBK 文件在新工具里全是乱码"。
//
//  ---------------------------------------------------------------------------
//  三条纪律：
//
//   1. **探测要给出理由**（`Detection::reason`）。"我判定它是 GBK"必须能说出
//      依据（有 BOM / 字节序列是合法 UTF-8 / 按 GBK 解得出全部字符），
//      否则用户没法判断该不该信。**猜错编码 = 整篇乱码**，比转换失败更糟。
//   2. **解不干净就不解**：任何一步出现非法字节序列都直接失败并说清是哪一步，
//      **绝不**用替换字符（U+FFFD / '?'）糊过去 —— 那等于静默毁掉用户的原文。
//   3. **绝不原地覆盖源文件**：目标路径与源文件重合时直接拒绝（与图片侧同一纪律）。
//      写文件同样先写临时文件、成功后改名。
//
//  ⚠ 本头文件不含 Q_OBJECT；实现依赖 Win32 代码页 API（GBK=936 / 系统 ANSI）。
// ============================================================================

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::TextEncodingTools {

/// 支持的字符集。`Ansi` = 本机默认 ANSI 代码页（简中系统即 GBK，但按系统实际取值）
enum class Encoding {
    Utf8 = 0, ///< UTF-8，不带 BOM
    Utf8Bom,  ///< UTF-8 with BOM
    Utf16Le,  ///< UTF-16 小端（Windows 记事本的"Unicode"）
    Utf16Be,  ///< UTF-16 大端
    Gbk,      ///< 中文 GBK / CP936
    Ansi      ///< 本机 ANSI 代码页（用 GetACP() 取，不硬编码 936）
};

QString encodingKey(Encoding encoding);
bool encodingFromKey(const QString &key, Encoding *encodingOut);
QString encodingDisplayName(Encoding encoding);
/// 该编码在写文件时是否会自动带 BOM（Utf8 与 Utf8Bom 的区别就在这里）
bool encodingWritesBom(Encoding encoding);

/// 换行风格
enum class LineEnding { Keep = 0, Lf, Crlf };

QString lineEndingKey(LineEnding lineEnding);
bool lineEndingFromKey(const QString &key, LineEnding *lineEndingOut);
QString lineEndingDisplayName(LineEnding lineEnding);

// ---------------------------------------------------------------------------
//  探测
// ---------------------------------------------------------------------------

struct Detection
{
    Encoding encoding = Encoding::Utf8;
    /// false = 结论只是"最合理的猜测"（面板要如实标出来，不能假装确定）
    bool confident = false;
    /// 中文依据，例如"开头是 UTF-8 BOM"、"字节序列是合法 UTF-8"、
    /// "不是合法 UTF-8，但按 GBK 能解出全部字符"
    QString reason;
};

Detection detect(const QByteArray &data);

// ---------------------------------------------------------------------------
//  解码 / 编码
// ---------------------------------------------------------------------------

/// 解码为字符串。失败返回 false 并给中文原因（**不会**返回带替换字符的半成品）
bool decode(const QByteArray &data, Encoding encoding, QString *textOut,
            QString *errorOut = nullptr);

/// 编码为字节。`withBom` 为 true 时按编码规则加上 BOM（GBK / ANSI 无 BOM 概念，忽略）。
/// **目标编码装不下的字符是失败、不是降级**：emoji / 生僻字在 GBK 里没有对应，
/// 这种时候返回 false 并说清有几个字符装不下 —— 绝不悄悄替换成 '?'。
bool encode(const QString &text, Encoding encoding, bool withBom, QByteArray *bytesOut,
            QString *errorOut = nullptr);

// ---------------------------------------------------------------------------
//  文件级
// ---------------------------------------------------------------------------

/// 按扩展名判断"这看起来是文本文件"（txt / csv / json / xml / ini / log / md / 源码…）
bool isTextFile(const QString &fileNameOrPath);

/// 转换一个文本文件：读入 → 探测（`sourceEncoding` 为 nullptr 时自动探测）→
/// 按目标编码与换行写新文件。
/// @param detectionOut 可选：把探测结论回传给面板（"为什么按这个编码读"）
bool convertFile(const QString &inputPath, const QString &outputPath, Encoding targetEncoding,
                 LineEnding lineEnding, Encoding *detectedSourceOut = nullptr,
                 QString *errorOut = nullptr);

/// 探测结论的人话（"按 GBK 读取（不是合法 UTF-8，但按 GBK 能解出全部字符）"）
QString detectionSummary(const Detection &detection);

} // namespace WinEase::FeaturePlugins::TextEncodingTools
