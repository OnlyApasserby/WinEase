#include "HostsDocument.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace WinEase::Common {

namespace {

constexpr char kBomBytes[] = "\xEF\xBB\xBF";

/// 去掉行首空白（判断"是不是注释"时要允许缩进的注释）
QString stripLeading(const QString &text)
{
    int index = 0;
    while (index < text.size() && text.at(index).isSpace()) {
        ++index;
    }
    return text.mid(index);
}

QString lineErrorText(int number, const QString &reason)
{
    return QStringLiteral("第 %1 行：%2").arg(number).arg(reason);
}

} // namespace

bool HostsDocument::isBlankLine(const QString &text)
{
    return text.trimmed().isEmpty();
}

bool HostsDocument::isCommentLine(const QString &text)
{
    return stripLeading(text).startsWith(QLatin1Char('#'));
}

bool HostsDocument::isValidAddress(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }

    if (text.contains(QLatin1Char(':'))) {
        // IPv6：只允许十六进制、`:`、`.`（IPv4 映射写法）与 `%`（作用域 ID）。
        // 刻意不引 Qt6::Network（多引一个模块 = 每个插件多一份 DLL 依赖，架构约定 6）
        for (const QChar ch : text) {
            const bool hex = (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                             || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
                             || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
            if (!hex && ch != QLatin1Char(':') && ch != QLatin1Char('.')
                && ch != QLatin1Char('%')) {
                return false;
            }
        }
        return text.count(QLatin1Char(':')) >= 2;
    }

    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4) {
        return false;
    }
    for (const QString &part : parts) {
        if (part.isEmpty() || part.size() > 3) {
            return false;
        }
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok || value < 0 || value > 255) {
            return false;
        }
    }
    return true;
}

bool HostsDocument::isValidHostName(const QString &text)
{
    if (text.isEmpty() || text.size() > 253) {
        return false;
    }
    const QStringList labels = text.split(QLatin1Char('.'));
    for (const QString &label : labels) {
        if (label.isEmpty() || label.size() > 63) {
            return false;
        }
        // ⚠ **只接受 ASCII**：hosts 的主机名必须是半角字母/数字/`-`/`_`。
        //   不能图省事写成 `QChar::isLetterOrNumber()` —— 它对中文、日文、全角字符
        //   一律返回 true，于是"用中文输入法打出来的域名"会被放行，保存进 hosts
        //   之后永远解析不了（用户会以为"我明明写了怎么不生效"）
        for (const QChar ch : label) {
            const bool asciiAlnum = (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                                    || (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
                                    || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'));
            if (!asciiAlnum && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
                return false;
            }
        }
        if (label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-'))) {
            return false;
        }
    }
    return true;
}

HostsLine HostsDocument::parseLine(const QString &text, int number)
{
    HostsLine line;
    line.number = number;
    line.text = text;

    if (isBlankLine(text)) {
        line.kind = HostsLineKind::Blank;
        return line;
    }
    if (isCommentLine(text)) {
        line.kind = HostsLineKind::Comment;
        return line;
    }

    // 行尾注释：`127.0.0.1  host  # 说明` —— 只保留 `#` 之前的部分参与解析
    QString payload = text;
    const int hash = payload.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        payload = payload.left(hash);
    }

    const QStringList fields = payload.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
    if (fields.isEmpty()) {
        line.kind = HostsLineKind::Invalid;
        line.problem = QStringLiteral("空记录");
        return line;
    }

    line.address = fields.first();
    if (!isValidAddress(line.address)) {
        line.kind = HostsLineKind::Invalid;
        line.problem = QStringLiteral("地址格式非法（%1）").arg(line.address);
        return line;
    }

    for (int index = 1; index < fields.size(); ++index) {
        if (!isValidHostName(fields.at(index))) {
            line.kind = HostsLineKind::Invalid;
            line.problem = QStringLiteral("主机名非法（%1）").arg(fields.at(index));
            line.hostNames.clear();
            return line;
        }
        line.hostNames << fields.at(index);
    }

    if (line.hostNames.isEmpty()) {
        line.kind = HostsLineKind::Invalid;
        line.problem = QStringLiteral("缺少主机名");
        return line;
    }

    line.kind = HostsLineKind::Entry;
    return line;
}

HostsDocument HostsDocument::parse(const QByteArray &bytes)
{
    HostsDocument document;

    QByteArray payload = bytes;
    if (payload.startsWith(kBomBytes)) {
        document.m_hasBom = true;
        payload.remove(0, 3);
    }

    // 换行风格：以第一次出现的换行为准（Windows 的 hosts 通常两种混着有，
    // 我们只在**新增行**时使用检测到的主风格，已有行的原文不动）
    const int crlf = payload.indexOf("\r\n");
    const int lf = payload.indexOf('\n');
    document.m_newline = (crlf >= 0 && (lf < 0 || crlf <= lf)) ? QStringLiteral("\r\n")
                                                               : QStringLiteral("\n");

    QString text = QString::fromUtf8(payload);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    document.parseLines(text);
    return document;
}

void HostsDocument::parseLines(const QString &text)
{
    m_lines.clear();

    // 用 split 而不是 QTextStream：需要**逐行保留原文**（含行内多余空白）
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    int number = 0;
    for (int index = 0; index < rawLines.size(); ++index) {
        // 末尾换行会产生一个多余的"空行"，且它不该算作文件内容
        if (index == rawLines.size() - 1 && rawLines.at(index).isEmpty()) {
            break;
        }
        m_lines.append(parseLine(rawLines.at(index), ++number));
    }
}

QString HostsDocument::text() const
{
    QStringList out;
    out.reserve(m_lines.size());
    for (const HostsLine &line : m_lines) {
        out << line.text;
    }
    return out.join(QLatin1Char('\n'));
}

void HostsDocument::setText(const QString &text)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    parseLines(normalized);
    m_dirty = true;
}

QByteArray HostsDocument::render() const
{
    QString joined = text();
    if (!joined.endsWith(QLatin1Char('\n'))) {
        joined += QLatin1Char('\n');
    }
    joined.replace(QStringLiteral("\n"), m_newline);

    QByteArray bytes = joined.toUtf8();
    if (m_hasBom) {
        bytes.prepend(QByteArray(kBomBytes, 3));
    }
    return bytes;
}

HostsCheck HostsDocument::check() const
{
    HostsCheck result;
    QHash<QString, QString> hostNameToAddress;

    for (const HostsLine &line : m_lines) {
        switch (line.kind) {
        case HostsLineKind::Blank:
            break;
        case HostsLineKind::Comment:
            ++result.commentCount;
            break;
        case HostsLineKind::Entry:
            ++result.entryCount;
            for (const QString &hostName : line.hostNames) {
                const QString key = hostName.toLower();
                if (hostNameToAddress.contains(key)
                    && hostNameToAddress.value(key) != line.address) {
                    result.warningLines.append(line.number);
                    result.warnings << lineErrorText(line.number,
                                                     QStringLiteral("%1 同时映射到 %2，"
                                                                    "最终生效的是文件中靠后的一条")
                                                         .arg(hostName,
                                                              hostNameToAddress.value(key)));
                } else {
                    hostNameToAddress.insert(key, line.address);
                }
            }
            break;
        case HostsLineKind::Invalid:
            result.errorLines.append(line.number);
            result.errors << lineErrorText(line.number, line.problem);
            break;
        }
    }

    result.ok = result.errorLines.isEmpty();
    return result;
}

bool HostsDocument::toggleComment(int lineNumber)
{
    for (int index = 0; index < m_lines.size(); ++index) {
        HostsLine &line = m_lines[index];
        if (line.number != lineNumber || line.kind == HostsLineKind::Blank) {
            continue;
        }

        QString text = line.text;
        const int hash = text.indexOf(QLatin1Char('#'));
        if (isCommentLine(text) && hash >= 0) {
            // 取消注释：删掉那个 `#`，顺带吞掉紧跟的一个空格（`# foo` → `foo`）
            text.remove(hash, 1);
            if (hash < text.size() && text.at(hash) == QLatin1Char(' ')) {
                text.remove(hash, 1);
            }
        } else {
            // 加注释：加在**缩进之后**，保持列对齐不被破坏
            int indent = 0;
            while (indent < text.size() && text.at(indent).isSpace()) {
                ++indent;
            }
            text.insert(indent, QStringLiteral("# "));
        }

        line.text = text;
        line = parseLine(text, line.number); // 重新判定类型（注释 ↔ 记录）
        m_dirty = true;
        return true;
    }
    return false;
}

bool HostsDocument::appendEntry(const QString &address, const QString &hostName,
                                QString *errorOut)
{
    if (!isValidAddress(address)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("地址格式非法（%1）").arg(address);
        }
        return false;
    }
    if (!isValidHostName(hostName)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("主机名非法（%1）").arg(hostName);
        }
        return false;
    }

    const QString text = QStringLiteral("%1\t%2").arg(address, hostName);
    m_lines.append(parseLine(text, static_cast<int>(m_lines.size()) + 1));
    m_dirty = true;
    return true;
}

QString HostsCheck::summary() const
{
    QString text = QStringLiteral("%1 条映射 · %2 行注释").arg(entryCount).arg(commentCount);
    if (!warnings.isEmpty()) {
        text += QStringLiteral(" · ⚠ %1 条提示").arg(warnings.size());
    }
    if (!errors.isEmpty()) {
        text += QStringLiteral(" · ✗ %1 处语法错误").arg(errors.size());
    }
    return text;
}

// ============================================================================
//  路径与默认内容
// ============================================================================

QString HostsDocument::hostsFilePath()
{
    const QString systemRoot =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    return QDir(systemRoot).filePath(QStringLiteral("System32/drivers/etc/hosts"));
}

QByteArray HostsDocument::defaultContent()
{
    // 只含注释、没有任何有效映射 —— 与 Windows 装好时那份默认 hosts 的语义一致
    // （那份文件里的条目也全被注释掉了）
    static const char *kDefault =
        "# WinEase 已恢复为默认内容\n"
        "#\n"
        "# hosts 用于把主机名映射到 IP 地址，优先级高于 DNS 查询。\n"
        "# 每行一条记录：  <IP 地址>  <主机名> [更多主机名...]\n"
        "# 以 # 开头的行是注释；本文件只支持英文半角字符。\n"
        "#\n"
        "# 示例（去掉行首的 # 即可生效）：\n"
        "#\t127.0.0.1\tlocalhost\n"
        "#\t::1\t\tlocalhost\n"
        "#\n"
        "# 注意：localhost 的名字解析由 DNS 自身处理，通常不需要写在这里。\n";
    return QByteArray(kDefault);
}

} // namespace WinEase::Common
