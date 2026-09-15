#pragma once

// ============================================================================
//  HostsDocument.h —— hosts 文件的解析 / 校验 / 渲染（**全纯函数**）
//
//  为什么单独抽一层：hosts 是"用户手写的配置文件"，它有两个硬要求：
//    1. **往返无损**：用户只是想把某一行注释掉，保存后不该把他的缩进、
//       制表符、行尾风格、BOM 全改掉（那种"编辑器一保存就整文件 diff"的行为
//       是这类工具最大的信任杀手）；
//    2. **保存前必须校验**：hosts 写错一行，用户可能再也上不了某个网站，
//       而错误行号就是用户唯一能定位问题的线索（路线图验收原文：
//       "非法语法拒绝保存并指出行号"）。
//
//  因此：解析时**保留每行原文**，编辑只改被编辑的那一行，渲染时按原始风格拼回去。
//  插件、面板、自检三方共用这一份实现（自检直接对纯函数断言，不依赖文件系统）。
//
//  ⚠ 本文件不含 Q_OBJECT、不碰文件系统（读写由插件负责，写经提权助手）。
// ============================================================================

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace WinEase::Common {

enum class HostsLineKind {
    Blank = 0, ///< 空行
    Comment,   ///< 注释行（`#` 开头，允许前导空白）
    Entry,     ///< 有效的映射记录
    Invalid,   ///< 既不是注释也不是有效记录（保存时会被拒绝，并报出行号）
};

struct HostsLine {
    int number = 0; ///< 1 基行号（报错信息里用的就是它）
    QString text;   ///< **原文**（不含换行符）——往返无损的关键
    HostsLineKind kind = HostsLineKind::Blank;
    QString address;      ///< Entry：IP 地址
    QStringList hostNames; ///< Entry：主机名列表
    QString problem;      ///< Invalid：为什么不行（中文）
};

struct HostsCheck {
    bool ok = true;
    QList<int> errorLines;
    QStringList errors; ///< "第 3 行：地址格式非法（192.168.1）"
    QList<int> warningLines;
    QStringList warnings; ///< 不阻止保存，但要说出来（同一主机名映射到多个地址）
    int entryCount = 0;
    int commentCount = 0;

    /// 一句话摘要（面板状态栏用）
    QString summary() const;
};

class HostsDocument
{
public:
    HostsDocument() = default;

    /// 解析原始字节（识别并**保留** UTF-8 BOM 与换行风格）
    static HostsDocument parse(const QByteArray &bytes);

    /// 系统 hosts 路径（`%SystemRoot%\System32\drivers\etc\hosts`）
    static QString hostsFilePath();

    /// "恢复默认"用的内容：只有注释、没有任何有效映射
    /// （Windows 装好时的那份默认 hosts 也正是这样：所有条目都被注释掉了）
    static QByteArray defaultContent();

    bool hasBom() const { return m_hasBom; }
    const QList<HostsLine> &lines() const { return m_lines; }
    bool dirty() const { return m_dirty; }
    void markClean() { m_dirty = false; }

    /// 编辑区文本（换行统一为 `\n`）
    QString text() const;
    /// 用编辑区文本替换内容（保留 BOM 与换行风格），并标记为"已改动"
    void setText(const QString &text);
    /// 渲染回原始字节风格（BOM + 原换行符 + 结尾换行）
    QByteArray render() const;

    HostsCheck check() const;

    /// 切换某一行的注释状态（空行不动）；返回是否真的改了内容
    bool toggleComment(int lineNumber);

    /// 追加一条映射（末尾补一行）
    bool appendEntry(const QString &address, const QString &hostName, QString *errorOut = nullptr);

    /// 解析单行（校验与自检都直接用它）
    static HostsLine parseLine(const QString &text, int number);

    static bool isBlankLine(const QString &text);
    static bool isCommentLine(const QString &text);
    static bool isValidAddress(const QString &text);
    static bool isValidHostName(const QString &text);

private:
    void parseLines(const QString &text);

    bool m_hasBom = false;
    QString m_newline = QStringLiteral("\r\n");
    bool m_dirty = false;
    QList<HostsLine> m_lines;
};

} // namespace WinEase::Common
