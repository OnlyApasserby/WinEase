#include "TextTools.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <QVector>
#include <QXmlStreamReader>

#include <cmath>

namespace WinEase::FeaturePlugins::TextTools {

namespace {

Result success(const QString &output)
{
    Result result;
    result.ok = true;
    result.output = output;
    return result;
}

Result failure(const QString &error, int line = -1, int column = -1)
{
    Result result;
    result.ok = false;
    result.error = error;
    result.line = line;
    result.column = column;
    return result;
}

// ---------------------------------------------------------------------------
//  JSON
// ---------------------------------------------------------------------------

/// QJsonParseError::offset 是**字节**偏移（Qt 内部按 UTF-8 解析）
/// → 要给出用户能用的行列号，必须把"同一段前缀"重新按字符数一遍。
/// 直接把字节偏移当列号，在含中文的 JSON 上会明显偏大。
void locateInUtf8(const QString &text, int byteOffset, int *line, int *column)
{
    const QByteArray utf8 = text.toUtf8();
    const int bounded = qBound(0, byteOffset, utf8.size());
    const QString prefix = QString::fromUtf8(utf8.constData(), bounded);

    int currentLine = 1;
    int currentColumn = 1;
    for (const QChar ch : prefix) {
        if (ch == QLatin1Char('\n')) {
            ++currentLine;
            currentColumn = 1;
        } else if (ch != QLatin1Char('\r')) {
            ++currentColumn;
        }
    }
    if (line != nullptr) {
        *line = currentLine;
    }
    if (column != nullptr) {
        *column = currentColumn;
    }
}

Result jsonParseFailure(const QString &text, const QJsonParseError &error)
{
    int line = -1;
    int column = -1;
    locateInUtf8(text, error.offset, &line, &column);
    return failure(describeJsonError(static_cast<int>(error.error)), line, column);
}

/// 校验 JSON；失败时把中文错误（含行列）写进 *failureOut
bool validateJson(const QString &text, Result *failureOut)
{
    QJsonParseError error{};
    // 注意：这里只用来**校验**。格式化/压缩都不用它重新序列化 ——
    // QJsonObject 内部按 key 排序，toJson() 会把用户原始键顺序打乱，
    // 对"只是想过一遍格式"的用户来说这是不可接受的副作用。
    QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError) {
        return true;
    }
    if (failureOut != nullptr) {
        *failureOut = jsonParseFailure(text, error);
    }
    return false;
}

/// JSON 词法单元
struct JsonToken {
    enum class Kind {
        Punctuation, ///< { } [ ] , :
        String,      ///< 含引号的原文（内部转义原样保留）
        Literal      ///< 数字 / true / false / null
    };

    Kind kind = Kind::Punctuation;
    QString text; ///< 原文切片，拼接即可还原（压缩就是全拼接）
};

/// 保序词法扫描：只做"切分"，不做任何重排
bool tokenizeJson(const QString &text, QVector<JsonToken> *tokens, QString *error)
{
    int i = 0;
    const int size = text.size();
    while (i < size) {
        const QChar ch = text.at(i);

        if (ch.isSpace()) {
            ++i;
            continue;
        }

        const bool isPunctuation = ch == QLatin1Char('{') || ch == QLatin1Char('}')
                                   || ch == QLatin1Char('[') || ch == QLatin1Char(']')
                                   || ch == QLatin1Char(',') || ch == QLatin1Char(':');
        if (isPunctuation) {
            JsonToken token;
            token.kind = JsonToken::Kind::Punctuation;
            token.text = QString(ch);
            tokens->append(token);
            ++i;
            continue;
        }

        if (ch == QLatin1Char('"')) {
            const int start = i;
            ++i;
            bool closed = false;
            while (i < size) {
                const QChar current = text.at(i);
                if (current == QLatin1Char('\\')) {
                    i += 2; // 跳过被转义的字符（含 \" 与 \\）
                    continue;
                }
                if (current == QLatin1Char('"')) {
                    ++i;
                    closed = true;
                    break;
                }
                ++i;
            }
            if (!closed) {
                if (error != nullptr) {
                    *error = QStringLiteral("字符串没有闭合的双引号");
                }
                return false;
            }
            JsonToken token;
            token.kind = JsonToken::Kind::String;
            token.text = text.mid(start, i - start);
            tokens->append(token);
            continue;
        }

        // 数字 / true / false / null：吃到下一个分隔符为止
        const int start = i;
        while (i < size) {
            const QChar current = text.at(i);
            if (current.isSpace() || current == QLatin1Char(',') || current == QLatin1Char('}')
                || current == QLatin1Char(']') || current == QLatin1Char('{') || current == QLatin1Char('[')
                || current == QLatin1Char(':')) {
                break;
            }
            ++i;
        }
        JsonToken token;
        token.kind = JsonToken::Kind::Literal;
        token.text = text.mid(start, i - start);
        tokens->append(token);
    }
    return true;
}

/// 压缩：所有 token 原文直接相接
QString joinCompact(const QVector<JsonToken> &tokens)
{
    QString output;
    output.reserve(tokens.size() * 8);
    for (const JsonToken &token : tokens) {
        output += token.text;
    }
    return output;
}

/// 容器是否为空（{} 或 []）—— 空容器写成 {} / [] 而不是两行
bool isEmptyContainer(const QVector<JsonToken> &tokens, int openIndex)
{
    if (openIndex + 1 >= tokens.size()) {
        return false;
    }
    const JsonToken &next = tokens.at(openIndex + 1);
    return next.kind == JsonToken::Kind::Punctuation
           && (next.text == QLatin1String("}") || next.text == QLatin1String("]"));
}

// ---------------------------------------------------------------------------
//  XML
// ---------------------------------------------------------------------------

QString escapeXmlAttribute(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    escaped.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    escaped.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    escaped.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return escaped;
}

Result xmlFailure(const QXmlStreamReader &reader)
{
    return failure(reader.errorString(),
                   static_cast<int>(reader.lineNumber()),
                   static_cast<int>(reader.columnNumber()));
}

/// 开始标签（**不含**结尾的 '>'）：调用方决定补 '>' 还是 '/>'
QString xmlStartTag(const QXmlStreamReader &reader)
{
    QString tag = QStringLiteral("<") + reader.qualifiedName().toString();
    const auto attributes = reader.attributes();
    for (const QXmlStreamAttribute &attribute : attributes) {
        tag += QStringLiteral(" %1=\"%2\"")
                   .arg(attribute.qualifiedName(), escapeXmlAttribute(attribute.value().toString()));
    }
    return tag;
}

QString xmlEndTag(const QXmlStreamReader &reader)
{
    return QStringLiteral("</") + reader.qualifiedName().toString() + QStringLiteral(">");
}

// ---------------------------------------------------------------------------
//  转义 / 去转义
// ---------------------------------------------------------------------------

QChar hexQuadChar(const QString &text, int index, int *consumed, bool *ok)
{
    // text[index] 应为 '\'，text[index+1] 应为 'u'
    if (index + 5 >= text.size()) {
        *ok = false;
        return QChar();
    }
    const QString hex = text.mid(index + 2, 4);
    bool converted = false;
    const ushort value = static_cast<ushort>(hex.toUShort(&converted, 16));
    if (!converted) {
        *ok = false;
        return QChar();
    }
    *consumed = 6;
    *ok = true;
    return QChar(value);
}

} // namespace

// ===========================================================================
//  JSON
// ===========================================================================

QString describeJsonError(int parseErrorCode)
{
    switch (static_cast<QJsonParseError::ParseError>(parseErrorCode)) {
    case QJsonParseError::NoError:
        return QStringLiteral("没有错误");
    case QJsonParseError::UnterminatedObject:
        return QStringLiteral("对象没有闭合（缺少 }）");
    case QJsonParseError::MissingNameSeparator:
        return QStringLiteral("缺少键名与值之间的冒号");
    case QJsonParseError::UnterminatedArray:
        return QStringLiteral("数组没有闭合（缺少 ]）");
    case QJsonParseError::MissingValueSeparator:
        return QStringLiteral("缺少成员之间的逗号");
    case QJsonParseError::IllegalValue:
        return QStringLiteral("非法的值（数字/字符串/true/false/null 都不是）");
    case QJsonParseError::TerminationByNumber:
        return QStringLiteral("数字后面还有内容");
    case QJsonParseError::IllegalNumber:
        return QStringLiteral("非法的数字格式");
    case QJsonParseError::IllegalEscapeSequence:
        return QStringLiteral("非法的转义序列");
    case QJsonParseError::IllegalUTF8String:
        return QStringLiteral("非法的 UTF-8 字节序列");
    case QJsonParseError::UnterminatedString:
        return QStringLiteral("字符串没有闭合（缺少结尾的引号）");
    case QJsonParseError::MissingObject:
        return QStringLiteral("这里应该是一个对象");
    case QJsonParseError::DeepNesting:
        return QStringLiteral("嵌套层级过深");
    case QJsonParseError::DocumentTooLarge:
        return QStringLiteral("文本过大");
    case QJsonParseError::GarbageAtEnd:
        return QStringLiteral("JSON 结束之后还有多余内容");
    default:
        return QStringLiteral("JSON 语法错误（错误码 %1）").arg(parseErrorCode);
    }
}

Result formatJson(const QString &text, int indent)
{
    Result jsonFailure;
    if (!validateJson(text, &jsonFailure)) {
        return jsonFailure;
    }

    QVector<JsonToken> tokens;
    QString tokenError;
    if (!tokenizeJson(text, &tokens, &tokenError)) {
        return failure(tokenError);
    }

    const QString indentUnit(qMax(0, indent), QLatin1Char(' '));
    QString output;
    output.reserve(text.size() + text.size() / 4);
    int depth = 0;

    const auto newlineIndent = [&output, &indentUnit](int level) {
        if (indentUnit.isEmpty()) {
            return; // indent == 0：退化成"压缩但保留结构"的换行风格
        }
        output += QLatin1Char('\n');
        output += indentUnit.repeated(level);
    };

    for (int i = 0; i < tokens.size(); ++i) {
        const JsonToken &token = tokens.at(i);
        if (token.kind != JsonToken::Kind::Punctuation) {
            output += token.text;
            continue;
        }

        if (token.text == QLatin1String("{") || token.text == QLatin1String("[")) {
            output += token.text;
            if (isEmptyContainer(tokens, i)) {
                continue; // 右括号紧跟其后 → 空容器，不加深缩进、不换行
            }
            ++depth;
            newlineIndent(depth);
        } else if (token.text == QLatin1String("}") || token.text == QLatin1String("]")) {
            // 空容器判定只能看"前一个 token 恰好是开括号"：
            // 若反过来问"下一个是不是闭括号"，对任何闭括号都成立（它自己就是），
            // 会让非空容器的闭合括号也丢掉换行缩进。
            const bool closesEmptyContainer =
                (i > 0) && tokens.at(i - 1).kind == JsonToken::Kind::Punctuation
                && (tokens.at(i - 1).text == QLatin1String("{")
                    || tokens.at(i - 1).text == QLatin1String("["));
            if (closesEmptyContainer) {
                output += token.text; // 空容器：开括号时没加深缩进，这里也不能减
            } else {
                --depth;
                newlineIndent(depth);
                output += token.text;
            }
        } else if (token.text == QLatin1String(",")) {
            output += token.text;
            newlineIndent(depth);
        } else if (token.text == QLatin1String(":")) {
            output += QLatin1String(": ");
        } else {
            output += token.text;
        }
    }
    return success(output);
}

Result minifyJson(const QString &text)
{
    Result jsonFailure;
    if (!validateJson(text, &jsonFailure)) {
        return jsonFailure;
    }

    QVector<JsonToken> tokens;
    QString tokenError;
    if (!tokenizeJson(text, &tokens, &tokenError)) {
        return failure(tokenError);
    }
    return success(joinCompact(tokens));
}

// ===========================================================================
//  XML
// ===========================================================================

Result formatXml(const QString &text, int indent)
{
    QXmlStreamReader reader(text);
    const QString indentUnit(qMax(0, indent), QLatin1Char(' '));

    QString output;
    output.reserve(text.size() + text.size() / 4);
    int depth = 0;
    bool lineHasContent = false;   ///< 当前行是否已有内容
    bool textJustWritten = false;  ///< 上一个 token 是文本（结束标签要贴着写）
    bool startTagOpen = false;     ///< 已写出 "<name attrs" 但还没写 '>'

    const auto breakLine = [&](int level) {
        if (!lineHasContent) {
            return;
        }
        output += QLatin1Char('\n');
        output += indentUnit.repeated(level);
    };
    // 任何"非开始标签"的 token 出现前，都必须先把待定的 '>' 补上
    const auto flushStartTag = [&] {
        if (startTagOpen) {
            output += QLatin1Char('>');
            startTagOpen = false;
        }
    };

    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::Invalid) {
            return xmlFailure(reader);
        }

        switch (token) {
        case QXmlStreamReader::StartDocument:
        case QXmlStreamReader::EndDocument:
            break;

        case QXmlStreamReader::StartElement: {
            flushStartTag();
            breakLine(depth);
            output += xmlStartTag(reader); // "<name attrs"，'>' 留到需要时再补
            lineHasContent = true;
            textJustWritten = false;
            startTagOpen = true;
            ++depth;
            break;
        }

        case QXmlStreamReader::EndElement: {
            --depth;
            if (startTagOpen) {
                // 开始标签之后什么都没有 → 写"空元素"形式 <name/>
                output += QStringLiteral("/>");
                startTagOpen = false;
            } else {
                if (!textJustWritten) {
                    breakLine(depth);
                }
                output += xmlEndTag(reader);
            }
            lineHasContent = true;
            textJustWritten = false;
            break;
        }

        case QXmlStreamReader::Characters: {
            // 注意：QXmlStreamReader 没有独立的 CDATA token，
            //       CDATA 是通过 Characters + isCDATA() 报告的
            if (reader.isCDATA()) {
                flushStartTag();
                breakLine(depth);
                output += QStringLiteral("<![CDATA[") + reader.text().toString()
                          + QStringLiteral("]]>");
                lineHasContent = true;
                textJustWritten = true;
                break;
            }
            if (reader.isWhitespace()) {
                // 标签之间的排版空白：交给格式化重新生成，直接丢
                break;
            }
            flushStartTag();
            output += reader.text().toString();
            lineHasContent = true;
            textJustWritten = true;
            break;
        }

        case QXmlStreamReader::Comment: {
            flushStartTag();
            breakLine(depth);
            output += QLatin1String("<!--") + reader.text().toString() + QLatin1String("-->");
            lineHasContent = true;
            textJustWritten = false;
            break;
        }

        case QXmlStreamReader::ProcessingInstruction: {
            flushStartTag();
            breakLine(depth);
            const QString data = reader.processingInstructionData().toString();
            output += QLatin1String("<?") + reader.processingInstructionTarget().toString();
            if (!data.isEmpty()) {
                output += QLatin1Char(' ') + data;
            }
            output += QLatin1String("?>");
            lineHasContent = true;
            textJustWritten = false;
            break;
        }

        case QXmlStreamReader::DTD: {
            // 注意：QXmlStreamReader 只给出 DTD 的文本，内部子集会被规范化
            flushStartTag();
            breakLine(depth);
            output += QLatin1String("<!DOCTYPE ") + reader.text().toString() + QLatin1Char('>');
            lineHasContent = true;
            textJustWritten = false;
            break;
        }

        default:
            break;
        }
    }

    if (reader.hasError()) {
        return xmlFailure(reader);
    }
    return success(output);
}

Result minifyXml(const QString &text)
{
    QXmlStreamReader reader(text);
    QString output;
    bool startTagOpen = false;

    // 与 formatXml 同样的"待定 '>'"处理：这样 <b/> 不会膨胀成 <b></b>，
    // 从而保证 minifyXml(formatXml(x)) 能回到原样（压缩与格式化互为逆运算）
    const auto flushStartTag = [&output, &startTagOpen] {
        if (startTagOpen) {
            output += QLatin1Char('>');
            startTagOpen = false;
        }
    };

    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::Invalid) {
            return xmlFailure(reader);
        }

        switch (token) {
        case QXmlStreamReader::StartDocument:
        case QXmlStreamReader::EndDocument:
            break;
        case QXmlStreamReader::StartElement:
            flushStartTag();
            output += xmlStartTag(reader);
            startTagOpen = true;
            break;
        case QXmlStreamReader::EndElement:
            if (startTagOpen) {
                output += QStringLiteral("/>");
                startTagOpen = false;
            } else {
                output += xmlEndTag(reader);
            }
            break;
        case QXmlStreamReader::Characters:
            flushStartTag();
            if (reader.isCDATA()) {
                // CDATA 里的空白是有意义的，必须原样保留
                output += QStringLiteral("<![CDATA[") + reader.text().toString()
                          + QStringLiteral("]]>");
            } else if (!reader.isWhitespace()) {
                // 只丢掉"纯排版空白"；元素内的真实文本原样保留
                output += reader.text().toString();
            }
            break;
        case QXmlStreamReader::Comment:
            flushStartTag();
            output += QStringLiteral("<!--") + reader.text().toString() + QStringLiteral("-->");
            break;
        case QXmlStreamReader::ProcessingInstruction: {
            flushStartTag();
            const QString data = reader.processingInstructionData().toString();
            output += QStringLiteral("<?") + reader.processingInstructionTarget().toString();
            if (!data.isEmpty()) {
                output += QLatin1Char(' ') + data;
            }
            output += QStringLiteral("?>");
            break;
        }
        case QXmlStreamReader::DTD:
            flushStartTag();
            output += QStringLiteral("<!DOCTYPE ") + reader.text().toString() + QLatin1Char('>');
            break;
        default:
            break;
        }
    }

    if (reader.hasError()) {
        return xmlFailure(reader);
    }
    return success(output);
}

// ===========================================================================
//  自动识别 + 转义
// ===========================================================================

bool looksLikeJson(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QChar first = trimmed.at(0);
    return first == QLatin1Char('{') || first == QLatin1Char('[');
}

bool looksLikeXml(const QString &text)
{
    return text.trimmed().startsWith(QLatin1Char('<'));
}

Result formatAuto(const QString &text, int indent)
{
    if (looksLikeJson(text)) {
        return formatJson(text, indent);
    }
    if (looksLikeXml(text)) {
        return formatXml(text, indent);
    }
    return failure(QStringLiteral("无法识别内容类型：JSON 应以 { 或 [ 开头，XML 应以 < 开头"));
}

Result minifyAuto(const QString &text)
{
    if (looksLikeJson(text)) {
        return minifyJson(text);
    }
    if (looksLikeXml(text)) {
        return minifyXml(text);
    }
    return failure(QStringLiteral("无法识别内容类型：JSON 应以 { 或 [ 开头，XML 应以 < 开头"));
}

Result escapeText(const QString &text)
{
    QString output;
    output.reserve(text.size() + text.size() / 8);
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case u'\\':
            output += QLatin1String("\\\\");
            break;
        case u'"':
            output += QLatin1String("\\\"");
            break;
        case u'\n':
            output += QLatin1String("\\n");
            break;
        case u'\r':
            output += QLatin1String("\\r");
            break;
        case u'\t':
            output += QLatin1String("\\t");
            break;
        default:
            if (ch.unicode() < 0x20) {
                output += QStringLiteral("\\u%1").arg(ch.unicode(), 4, 16, QLatin1Char('0'));
            } else {
                output += ch;
            }
            break;
        }
    }
    return success(output);
}

Result unescapeText(const QString &text)
{
    QString output;
    output.reserve(text.size());

    int i = 0;
    while (i < text.size()) {
        const QChar ch = text.at(i);
        if (ch != QLatin1Char('\\')) {
            output += ch;
            ++i;
            continue;
        }
        if (i + 1 >= text.size()) {
            output += ch;
            break;
        }

        const QChar next = text.at(i + 1);
        switch (next.unicode()) {
        case u'n':
            output += QLatin1Char('\n');
            i += 2;
            break;
        case u'r':
            output += QLatin1Char('\r');
            i += 2;
            break;
        case u't':
            output += QLatin1Char('\t');
            i += 2;
            break;
        case u'"':
            output += QLatin1Char('"');
            i += 2;
            break;
        case u'\'':
            output += QLatin1Char('\'');
            i += 2;
            break;
        case u'\\':
            output += QLatin1Char('\\');
            i += 2;
            break;
        case u'0':
            output += QChar(0);
            i += 2;
            break;
        case u'u': {
            int consumed = 0;
            bool ok = false;
            const QChar high = hexQuadChar(text, i, &consumed, &ok);
            if (!ok) {
                return failure(QStringLiteral("\\u 后面需要 4 位十六进制数字"), -1, i + 1);
            }
            i += consumed;

            // 代理对：\uD83D\uDE00 → 一个字符
            if (high.isHighSurrogate() && i + 5 < text.size() && text.at(i) == QLatin1Char('\\')
                && text.at(i + 1) == QLatin1Char('u')) {
                int lowConsumed = 0;
                bool lowOk = false;
                const QChar low = hexQuadChar(text, i, &lowConsumed, &lowOk);
                if (lowOk && low.isLowSurrogate()) {
                    // QChar 就是 UTF-16 单元，"一个字符"= 两个代理 QChar，直接顺序追加即可
                    output += high;
                    output += low;
                    i += lowConsumed;
                    break;
                }
            }
            output += high;
            break;
        }
        default:
            // 未知转义：原样保留（不猜、不报错）
            output += ch;
            output += next;
            i += 2;
            break;
        }
    }
    return success(output);
}

// ===========================================================================
//  JSON 路径查询
// ===========================================================================

Result queryJson(const QString &text, const QString &path)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return jsonParseFailure(text, error);
    }

    // 把 $.a.b[0].c 拆成 ["a", "b", "0", "c"]（数字段表示数组下标）
    struct Step {
        bool isIndex = false;
        QString key;
        int index = 0;
    };
    QVector<Step> steps;

    static const QRegularExpression tokenPattern(QStringLiteral("([^.\\[\\]]+)|\\[(\\d+)\\]"));
    auto matches = tokenPattern.globalMatch(path);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        Step step;
        if (match.capturedLength(2) > 0) {
            step.isIndex = true;
            step.index = match.captured(2).toInt();
        } else {
            step.key = match.captured(1);
            if (step.key == QLatin1String("$")) {
                continue; // 根符号
            }
        }
        steps.append(step);
    }

    QJsonValue current = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
    QString walked = QStringLiteral("$");

    for (const Step &step : steps) {
        if (step.isIndex) {
            if (!current.isArray()) {
                return failure(QStringLiteral("路径 %1 处不是一个数组").arg(walked));
            }
            const QJsonArray array = current.toArray();
            if (step.index < 0 || step.index >= array.size()) {
                return failure(QStringLiteral("数组 %1 只有 %2 个元素，取不到下标 %3")
                                   .arg(walked)
                                   .arg(array.size())
                                   .arg(step.index));
            }
            current = array.at(step.index);
            walked += QStringLiteral("[%1]").arg(step.index);
        } else {
            if (!current.isObject()) {
                return failure(QStringLiteral("路径 %1 处不是一个对象").arg(walked));
            }
            const QJsonObject object = current.toObject();
            if (!object.contains(step.key)) {
                return failure(QStringLiteral("对象 %1 里没有键 \"%2\"").arg(walked, step.key));
            }
            current = object.value(step.key);
            walked += QLatin1Char('.') + step.key;
        }
    }

    if (current.isObject()) {
        return success(QString::fromUtf8(QJsonDocument(current.toObject()).toJson(QJsonDocument::Indented)));
    }
    if (current.isArray()) {
        return success(QString::fromUtf8(QJsonDocument(current.toArray()).toJson(QJsonDocument::Indented)));
    }
    if (current.isString()) {
        return success(current.toString());
    }
    if (current.isBool()) {
        return success(current.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
    }
    if (current.isNull()) {
        return success(QStringLiteral("null"));
    }
    if (current.isDouble()) {
        const double value = current.toDouble();
        // 优先按整数输出，避免 1 变成 1.0
        if (value == std::floor(value) && std::fabs(value) < 1e15) {
            return success(QString::number(static_cast<qlonglong>(value)));
        }
        return success(QString::number(value, 'g', 17));
    }
    return failure(QStringLiteral("路径 %1 没有命中任何值").arg(walked));
}

// ===========================================================================
//  编码 / 哈希
// ===========================================================================

QString toBase64(const QString &text, bool urlSafe)
{
    const QByteArray data = text.toUtf8();
    const QByteArray::Base64Options options =
        urlSafe ? QByteArray::Base64UrlEncoding : QByteArray::Base64Encoding;
    return QString::fromLatin1(data.toBase64(options));
}

Result fromBase64(const QString &text)
{
    // 宽松处理：去掉空白与换行、统一字母表、按需补足 '='。
    // 用户从 URL、日志、聊天软件里复制来的 Base64 经常带换行或缺少填充。
    QString normalized;
    normalized.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            continue;
        }
        if (ch == QLatin1Char('-')) {
            normalized += QLatin1Char('+');
        } else if (ch == QLatin1Char('_')) {
            normalized += QLatin1Char('/');
        } else {
            normalized += ch;
        }
    }

    // 先把填充剥掉再判断"有效载荷"长度：
    // 直接看总长度会把 "===="（全填充）当成"4 个字符的合法输入"
    while (normalized.endsWith(QLatin1Char('='))) {
        normalized.chop(1);
    }
    if (normalized.isEmpty()) {
        return failure(QStringLiteral("没有可解码的内容"));
    }
    const int remainder = normalized.size() % 4;
    if (remainder == 1) {
        return failure(QStringLiteral("Base64 长度不合法（去掉空白后有 %1 个字符）").arg(normalized.size()));
    }
    if (remainder != 0) {
        normalized += QString(4 - remainder, QLatin1Char('='));
    }

    const QByteArray decoded = QByteArray::fromBase64(
        normalized.toLatin1(),
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty()) {
        return failure(QStringLiteral("含非 Base64 字符或填充错误"));
    }
    // 解码结果可能是二进制；这里按 UTF-8 宽松解码（非法字节会变成替换字符）
    return success(QString::fromUtf8(decoded));
}

QString toUrlEncoded(const QString &text, bool plusForSpace)
{
    const QByteArray encoded = QUrl::toPercentEncoding(text);
    QString result = QString::fromLatin1(encoded);
    if (plusForSpace) {
        result.replace(QLatin1String("%20"), QLatin1String("+"));
    }
    return result;
}

Result fromUrlDecoded(const QString &text)
{
    QString prepared = text;
    prepared.replace(QLatin1Char('+'), QLatin1Char(' '));
    return success(QUrl::fromPercentEncoding(prepared.toUtf8()));
}

QString hashName(HashAlgorithm algorithm)
{
    switch (algorithm) {
    case HashAlgorithm::Md5:
        return QStringLiteral("MD5");
    case HashAlgorithm::Sha1:
        return QStringLiteral("SHA-1");
    case HashAlgorithm::Sha256:
        return QStringLiteral("SHA-256");
    case HashAlgorithm::Sha512:
        return QStringLiteral("SHA-512");
    }
    return QStringLiteral("未知");
}

QString hashText(const QString &text, HashAlgorithm algorithm)
{
    QCryptographicHash::Algorithm qtAlgorithm = QCryptographicHash::Md5;
    switch (algorithm) {
    case HashAlgorithm::Md5:
        qtAlgorithm = QCryptographicHash::Md5;
        break;
    case HashAlgorithm::Sha1:
        qtAlgorithm = QCryptographicHash::Sha1;
        break;
    case HashAlgorithm::Sha256:
        qtAlgorithm = QCryptographicHash::Sha256;
        break;
    case HashAlgorithm::Sha512:
        qtAlgorithm = QCryptographicHash::Sha512;
        break;
    }
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), qtAlgorithm).toHex());
}

QStringList allHashes(const QString &text)
{
    // 宽度按最长的名字对齐，粘贴出去是一张整齐的小表
    const QStringList names = { hashName(HashAlgorithm::Md5), hashName(HashAlgorithm::Sha1),
                                hashName(HashAlgorithm::Sha256), hashName(HashAlgorithm::Sha512) };
    const HashAlgorithm algorithms[4] = { HashAlgorithm::Md5, HashAlgorithm::Sha1,
                                          HashAlgorithm::Sha256, HashAlgorithm::Sha512 };

    int width = 0;
    for (const QString &name : names) {
        width = qMax(width, name.size());
    }

    QStringList lines;
    for (int i = 0; i < 4; ++i) {
        lines.append(QStringLiteral("%1: %2")
                         .arg(names.at(i).leftJustified(width, QLatin1Char(' ')),
                              hashText(text, algorithms[i])));
    }
    return lines;
}

} // namespace WinEase::FeaturePlugins::TextTools
