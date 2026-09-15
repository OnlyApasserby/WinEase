#pragma once

// ============================================================================
//  TextTools.h —— 文本转换工具（P1-10 格式化 / P1-11 编码转换 共用）
//
//  为什么做成"插件间共享的纯函数"而不是各插件自己写一份：
//    1. dev.text_format 与 dev.encoder 都要处理同一批"剪贴板里的文本"，
//       行为必须一致（例如"非法 JSON 要给出第几行第几列"）
//    2. 纯函数 = 最容易做严苛自检的部分：自检直接调本头文件的函数逐字节比对，
//       插件那一侧只负责"取文本 → 调用 → 写回"的接线
//
//  ⚠ 本头文件不含 Q_OBJECT（与 WindowFeatureState.h 同理，避免跨插件 moc 重复定义）。
//
//  错误信息约定：
//    所有 Result 失败时 error 为**中文**，且能定位到行列的都会填 line/column
//    （1 基，与编辑器一致；-1 表示未知）。
// ============================================================================

#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::TextTools {

// ============================================================================
//  通用结果
// ============================================================================
struct Result {
    bool ok = false;
    QString output;   ///< 成功时的转换结果
    QString error;    ///< 失败原因（中文）
    int line = -1;    ///< 出错行号（1 基）；-1 表示与行号无关
    int column = -1;  ///< 出错列号（1 基）

    /// "第 3 行第 12 列：非法字符" —— 直接可展示
    QString errorText() const
    {
        if (line > 0 && column > 0) {
            return QStringLiteral("第 %1 行第 %2 列：%3").arg(line).arg(column).arg(error);
        }
        if (line > 0) {
            return QStringLiteral("第 %1 行：%2").arg(line).arg(error);
        }
        return error;
    }
};

// ============================================================================
//  JSON（P1-10）
// ============================================================================

/// 格式化 JSON（缩进 indent 个空格）。失败时给出精确的行列号，且**不改动原文**
Result formatJson(const QString &text, int indent = 2);

/// 压缩 JSON（去掉所有非必要空白）
Result minifyJson(const QString &text);

// ============================================================================
//  XML（P1-10）
// ============================================================================

/// 格式化 XML。保留原有的 XML 声明、注释、CDATA、处理指令与属性顺序
Result formatXml(const QString &text, int indent = 2);

/// 压缩 XML（去掉标签之间的缩进空白，保留元素内文本）
Result minifyXml(const QString &text);

// ============================================================================
//  自动识别 + 转义（P1-10）
// ============================================================================

bool looksLikeJson(const QString &text);
bool looksLikeXml(const QString &text);

/// 识别 JSON 或 XML 后格式化；两者都不像则返回中文错误
Result formatAuto(const QString &text, int indent = 2);
/// 识别 JSON 或 XML 后压缩
Result minifyAuto(const QString &text);

/// 把真实控制字符转义成可读形式（\n \r \t \" \\ 等）
Result escapeText(const QString &text);
/// 反向：把 \n \t \" \\ \uXXXX 还原成真实字符
Result unescapeText(const QString &text);

/// 极简 JSON 路径查询：支持 $.a.b[0].c / a.b[2] / $[0].name
/// 命中对象/数组时输出格式化 JSON，命中标量时输出其字面量
Result queryJson(const QString &text, const QString &path);

// ============================================================================
//  编码 / 哈希（P1-11）
// ============================================================================

/// Base64 编码；urlSafe 使用 URL 安全字母表（- _）
QString toBase64(const QString &text, bool urlSafe = false);

/// Base64 解码（宽松：容忍空白、换行、缺省 '=' 与 URL 安全字母表）
Result fromBase64(const QString &text);

/// URL 百分号编码；plusForSpace 为 true 时空格编成 '+'（表单风格）
QString toUrlEncoded(const QString &text, bool plusForSpace = true);

/// URL 解码（把 '+' 视为空格）
Result fromUrlDecoded(const QString &text);

enum class HashAlgorithm {
    Md5 = 0,
    Sha1,
    Sha256,
    Sha512
};

/// "MD5" / "SHA-1" / "SHA-256" / "SHA-512"
QString hashName(HashAlgorithm algorithm);

/// 十六进制小写摘要
QString hashText(const QString &text, HashAlgorithm algorithm);

/// 四种摘要一次算完，形如 "MD5:      xxx"（等宽对齐，便于直接贴给别人）
QStringList allHashes(const QString &text);

/// QJsonParseError::ParseError → 中文
QString describeJsonError(int parseErrorCode);

} // namespace WinEase::FeaturePlugins::TextTools
