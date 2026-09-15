#pragma once

// ============================================================================
//  BatchMoveEngine.h —— 批量移动文件（正则匹配）的"计划 → 预演 → 执行 → 撤销"引擎
//
//  ★ 核心纪律（沿用 P2-01 批量重命名与 D12）：
//      **先出计划、再预演、最后才动盘**。
//      引擎把"要动哪些文件、动到哪、为什么不动"全部算成一份 `Plan`，
//      插件只负责把 Plan 显示给人看；真正落盘只有 `applyPlan()` 一个出口，
//      而且它返回的 `MoveRecord` 就是"撤销凭据"。
//
//  ★ 正则匹配是**用户输入**，属于不可信数据：
//      · 非法正则 → Plan::valid == false + error，绝不"忽略非法模式继续搬"
//      · 目录穿越（正则里塞 `..\`、绝对路径）→ 目标路径一律被限制在目标目录内
//
//  ⚠ 本文件与实现不含 Q_OBJECT，也不碰任何 Win32 头：插件与自检编译同一份源码
//    （同 TextTools / RenameEngine / HudMetrics 的纪律）。
// ============================================================================

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::BatchMove {

/// 目标重名时怎么办
enum class ConflictPolicy {
    Skip,      ///< 跳过（默认：什么都不做，最安全）
    Rename,    ///< 自动改名 "名字 (1).ext"
    Overwrite, ///< 覆盖目标（**危险**，插件里要二次确认）
};

/// 计划里每一项的状态。界面按它上色，自检按它断言
enum class ItemStatus {
    Ready,            ///< 可以被移动
    NotMatched,       ///< 正则没匹配上（列出来是为了让用户看见"我扫到了但没选它"）
    InvalidName,      ///< 命名模板算出来的名字非法（空 / 含非法字符 / 目录穿越）
    SamePlace,        ///< 源与目标同一个路径（不用动）
    Conflict,         ///< 目标已存在且策略是跳过
    DuplicateTarget,  ///< 多个源要搬到同一个目标（同一次计划内部撞车）
};

/// 计划参数
struct Options {
    QString pattern;                ///< 正则表达式（Qt 语法 = PCRE2）
    bool caseSensitive = false;     ///< 大小写敏感（默认不敏感：用户写 `\.jpg$` 更顺手）
    bool matchFullPath = false;     ///< 匹配绝对路径（true）还是仅文件名（false，默认）
    QString sourceDirectory;        ///< 扫描根目录
    QString targetDirectory;        ///< 目标根目录
    bool recursive = false;         ///< 递归子目录
    bool keepStructure = false;     ///< 递归时保留相对目录结构
    QString nameTemplate;           ///< 可选改名模板：`{name}` 原名、`{ext}` 扩展名（含点）、`{n}` 序号
    ConflictPolicy policy = ConflictPolicy::Skip;
};

/// 计划中的一项
struct PlannedItem {
    QString sourcePath;   ///< 源绝对路径（正斜杠归一化）
    QString targetPath;   ///< 目标绝对路径
    QString fileName;     ///< 仅文件名（界面显示用）
    ItemStatus status = ItemStatus::Ready;
    QString note;         ///< 状态说明（中文，直接显示给人看）
};

/// 一份计划（dry-run 的产物）
struct Plan {
    bool valid = false;        ///< 计划本身是否可用（false 时 error 非空，界面必须拦住不放行）
    QString error;             ///< 致命错误（正则非法 / 目录不存在 / 目录相同）
    QList<PlannedItem> items;  ///< 全部候选（含被跳过的，界面要能解释"为什么没搬它"）
    QStringList warnings;      ///< 非致命提醒（如"有 3 个目标重名，按策略跳过"）

    int readyCount() const;
    int skippedCount() const;
};

/// 落盘记录（撤销凭据）：一条记录 = 一次成功的移动
struct MoveRecord {
    QString sourcePath;  ///< 搬之前在哪
    QString targetPath;  ///< 搬之后在哪
};

/// 执行结果
struct ApplyResult {
    int moved = 0;                    ///< 成功条数
    int failed = 0;                   ///< 失败条数
    QStringList errors;               ///< 每条失败的中文原因（直接显示）
    QList<MoveRecord> records;        ///< 撤销凭据（`applyPlan(dryRun = true)` 时为空）
    QString summary() const;          ///< 一句话汇总
};

/// 枚举目录下的文件（不含目录本身；`recursive` 时含子目录里的文件）
QStringList listFiles(const QString &directory, bool recursive);

/// 生成计划（**纯函数**：只算不动盘，同输入必同输出）
Plan buildPlan(const QStringList &files, const Options &options);

/// 执行计划。`dryRun == true` 时只把"将会移动"的条数算出来，一个字节都不动盘。
ApplyResult applyPlan(const Plan &plan, bool dryRun);

/// 撤销：按记录反向搬回去。**逐条独立**——某条失败不影响其余条
ApplyResult undoMoves(const QList<MoveRecord> &records);

/// 撤销日志的序列化 / 解析（一行一条：`源\t目标`；解析时跳过非法行）
QString serializeMoveRecords(const QList<MoveRecord> &records);
QList<MoveRecord> parseMoveRecords(const QString &text);

/// 目标重名时的自动改名（不判盘上是否存在，纯字符串运算）：
///   "报告.txt" → "报告 (1).txt"
QString autoRename(const QString &fileName, int index);

/// 把路径归一化成正斜杠形式（计划与日志里一律用它，避免 `\` 在正则里被当转义符）
QString normalizePath(const QString &path);

/// 路径是否落在 `root` 之内（含 root 本身）。用于拦住 `..` 目录穿越
bool isWithinDirectory(const QString &path, const QString &root);

/// 状态的中文说明（界面与自检共用同一份口径）
QString statusText(ItemStatus status);

} // namespace WinEase::FeaturePlugins::BatchMove
