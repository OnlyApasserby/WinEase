#pragma once

// ============================================================================
//  EnvTools.h —— 环境变量与 PATH 的处理（纯函数为主）
//
//  环境变量是"用户看得见、但改坏了很难查"的那类配置：
//  PATH 里多一个分号、少一个变量名，表现是"某个程序突然找不到了"，
//  而用户往往几天后才意识到是这里的问题。所以这一层的重点全在**看清问题**上：
//
//   1. **空条目不隐藏**：`;;` 与首尾分号会生成空条目，必须原样列出来 ——
//      空条目在 PATH 里等价于"当前目录"，是真实的安全隐患，也是常见的手滑；
//   2. **重复项要能定位**：归一化后（去引号、去尾部反斜杠、大小写不敏感）比较，
//      并指出"跟第几条重复"，而不是只说"有重复";
//   3. **展开预览**：`%VAR%` 展开后到底是什么、那条路径还在不在，
//      是用户决定"这条能不能删"的唯一依据（未定义的变量**原样保留**，
//      让用户一眼看出是哪个变量没定义，而不是静默变成空串）。
//
//  ⚠ 变量名校验只接受 **ASCII**（`[A-Za-z_][A-Za-z0-9_]*`）：Windows 其实允许
//     更宽的名字，但那种名字没法在 `%VAR%` 里可靠引用，也会让导入导出/脚本出问题。
//     注意别用 `isLetter()` 这类**语言相关**的判断（它对中日韩汉字也返回 true，踩坑 #58）。
//
//  ⚠ 本文件不含 Q_OBJECT；读注册表的部分是 IO，其余全是纯函数。
// ============================================================================

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace WinEase::Common {

/// 环境变量作用域（枚举值即界面顺序）
enum class EnvScope {
    User = 0, ///< 用户变量：HKCU\Environment，**免提权**
    Machine,  ///< 系统变量：HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment，需提权
};

QString envScopeKey(EnvScope scope);      ///< "user" / "machine"（与提权助手的参数一致）
QString envScopeText(EnvScope scope);     ///< "用户变量" / "系统变量"
EnvScope envScopeFromKey(const QString &key, bool *ok = nullptr);
QString envScopeRegistryPath(EnvScope scope);
bool envScopeNeedsElevation(EnvScope scope);
QList<EnvScope> allEnvScopes();

// ---------------------------------------------------------------------------
//  变量名
// ---------------------------------------------------------------------------

/// ASCII-only：`[A-Za-z_][A-Za-z0-9_]*`
bool isValidVariableName(const QString &name);
/// 不合法的原因（合法时返回空串）
QString variableNameProblem(const QString &name);

// ---------------------------------------------------------------------------
//  PATH
// ---------------------------------------------------------------------------

struct PathEntry {
    QString raw;        ///< 原样（可能带引号 / 尾部反斜杠）
    QString normalized; ///< 归一化后的比较用形式（去引号、去尾部 `\`）
    bool empty = false; ///< 空条目（`;;` / 首尾分号）—— 等价于"当前目录"，必须让用户看见
    bool duplicate = false;
    int duplicateOf = -1; ///< 重复时指向首个相同条目的下标
};

/// 按 `;` 拆分（**保留空条目**，这样才能把 `;;` 这种问题摆到用户面前）
QList<PathEntry> splitPathEntries(const QString &value);
/// 拼回 PATH 值（空条目原样保留）
QString joinPathEntries(const QList<PathEntry> &entries);
/// 去掉重复项与空条目（保留首次出现的顺序）
QString normalizePathValue(const QString &value);
/// 统计问题条目（重复 + 空）
int countPathProblems(const QList<PathEntry> &entries);

// ---------------------------------------------------------------------------
//  展开与体检
// ---------------------------------------------------------------------------

/// 展开 `%VAR%`（大小写不敏感）。**未定义的变量原样保留**——
/// 静默变成空串会让用户看到一条"看起来正常但其实是残废"的路径
QString expandVariables(const QString &value, const QHash<QString, QString> &variables);

/// 一条（已展开的）PATH 条目的体检结论：空串 = 没问题；
/// 否则返回"含未展开变量"或"目录不存在"
QString pathEntryProblem(const QString &expandedEntry);

// ---------------------------------------------------------------------------
//  导入导出
// ---------------------------------------------------------------------------

/// 导出为可读的 JSON 文本（按变量名排序，便于 diff 与版本管理）
QString exportVariables(const QMap<QString, QString> &variables, EnvScope scope);
bool importVariables(const QString &text, QMap<QString, QString> *out, QString *errorOut = nullptr);

// ---------------------------------------------------------------------------
//  读注册表（插件与自检共用同一份实现）
// ---------------------------------------------------------------------------

/// 读一个作用域的全部变量（**原始值**，不展开 `%VAR%`）。
/// 读 HKCU 与 HKLM 都不需要提权（写 HKLM 才需要）
QMap<QString, QString> readEnvironment(EnvScope scope, QString *errorOut = nullptr);

/// 值里含 `%` 时必须用 REG_EXPAND_SZ，否则 PATH 之类会失去展开能力
bool valueNeedsExpandType(const QString &value);

} // namespace WinEase::Common
