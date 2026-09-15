#include "EnvTools.h"

#include "win32/RegistryUtils.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>

namespace WinEase::Common {

namespace {

using WinEase::Win32::RegistryKey;
using WinEase::Win32::RegistryRoot;
using WinEase::Win32::RegistryView;

constexpr wchar_t kMachineEnvSubPath[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

/// 归一化：去首尾空白、去成对引号、去尾部反斜杠（`C:\` 这种根目录除外）
QString normalizePathEntry(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"'))) {
        text = text.mid(1, text.size() - 2).trimmed();
    }
    while (text.size() > 3 && (text.endsWith(QLatin1Char('\\')) || text.endsWith(QLatin1Char('/')))) {
        text.chop(1);
    }
    return text;
}

} // namespace

QString envScopeKey(EnvScope scope)
{
    return scope == EnvScope::User ? QStringLiteral("user") : QStringLiteral("machine");
}

QString envScopeText(EnvScope scope)
{
    return scope == EnvScope::User ? QStringLiteral("用户变量") : QStringLiteral("系统变量");
}

EnvScope envScopeFromKey(const QString &key, bool *ok)
{
    if (ok != nullptr) {
        *ok = true;
    }
    if (key == QLatin1String("user")) {
        return EnvScope::User;
    }
    if (key == QLatin1String("machine")) {
        return EnvScope::Machine;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return EnvScope::User;
}

QString envScopeRegistryPath(EnvScope scope)
{
    return scope == EnvScope::User ? QStringLiteral("Environment")
                                   : QString::fromWCharArray(kMachineEnvSubPath);
}

bool envScopeNeedsElevation(EnvScope scope)
{
    return scope != EnvScope::User;
}

QList<EnvScope> allEnvScopes()
{
    return { EnvScope::User, EnvScope::Machine };
}

// ---------------------------------------------------------------------------
//  变量名
// ---------------------------------------------------------------------------

bool isValidVariableName(const QString &name)
{
    if (name.isEmpty() || name.size() > 255) {
        return false;
    }

    const QChar first = name.at(0);
    const bool firstOk = (first >= QLatin1Char('a') && first <= QLatin1Char('z'))
                         || (first >= QLatin1Char('A') && first <= QLatin1Char('Z'))
                         || first == QLatin1Char('_');
    if (!firstOk) {
        return false;
    }

    for (const QChar ch : name) {
        // ⚠ 只用 ASCII 范围：`isLetterOrNumber()` 会把中文/全角字符放进来（踩坑 #58）
        const bool ok = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
                        || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
                        || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                        || ch == QLatin1Char('_');
        if (!ok) {
            return false;
        }
    }
    return true;
}

QString variableNameProblem(const QString &name)
{
    if (name.isEmpty()) {
        return QStringLiteral("变量名不能为空");
    }
    if (name.size() > 255) {
        return QStringLiteral("变量名过长（%1 字符，上限 255）").arg(name.size());
    }
    if (name.contains(QLatin1Char('='))) {
        return QStringLiteral("变量名不能包含 `=`");
    }
    if (!isValidVariableName(name)) {
        return QStringLiteral("变量名只能用半角字母、数字与下划线，且不能以数字开头");
    }
    return QString();
}

// ---------------------------------------------------------------------------
//  PATH
// ---------------------------------------------------------------------------

QList<PathEntry> splitPathEntries(const QString &value)
{
    QList<PathEntry> entries;
    if (value.isEmpty()) {
        return entries;
    }

    const QStringList parts = value.split(QLatin1Char(';'), Qt::KeepEmptyParts);
    entries.reserve(parts.size());

    QHash<QString, int> seen; // 归一化（小写）→ 首次出现的下标
    for (int index = 0; index < parts.size(); ++index) {
        PathEntry entry;
        entry.raw = parts.at(index);
        entry.normalized = normalizePathEntry(entry.raw);
        entry.empty = entry.normalized.isEmpty();

        if (!entry.empty) {
            const QString key = entry.normalized.toLower();
            if (seen.contains(key)) {
                entry.duplicate = true;
                entry.duplicateOf = seen.value(key);
            } else {
                seen.insert(key, index);
            }
        }
        entries.append(entry);
    }
    return entries;
}

QString joinPathEntries(const QList<PathEntry> &entries)
{
    QStringList parts;
    parts.reserve(entries.size());
    for (const PathEntry &entry : entries) {
        parts << entry.raw;
    }
    return parts.join(QLatin1Char(';'));
}

QString normalizePathValue(const QString &value)
{
    QList<PathEntry> kept;
    QHash<QString, bool> seen;
    for (const PathEntry &entry : splitPathEntries(value)) {
        if (entry.empty) {
            continue;
        }
        const QString key = entry.normalized.toLower();
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key, true);
        kept.append(entry);
    }
    return joinPathEntries(kept);
}

int countPathProblems(const QList<PathEntry> &entries)
{
    int count = 0;
    for (const PathEntry &entry : entries) {
        if (entry.empty || entry.duplicate) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
//  展开与体检
// ---------------------------------------------------------------------------

QString expandVariables(const QString &value, const QHash<QString, QString> &variables)
{
    QString result;
    result.reserve(value.size());

    // 大小写不敏感查表：Windows 的环境变量名不区分大小写
    QHash<QString, QString> lower;
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
        lower.insert(it.key().toLower(), it.value());
    }

    int index = 0;
    while (index < value.size()) {
        const int open = value.indexOf(QLatin1Char('%'), index);
        if (open < 0) {
            result += value.mid(index);
            break;
        }
        result += value.mid(index, open - index);

        const int close = value.indexOf(QLatin1Char('%'), open + 1);
        if (close < 0) {
            result += value.mid(open); // 落单的 `%`：原样保留
            break;
        }

        const QString name = value.mid(open + 1, close - open - 1);
        if (name.isEmpty()) {
            result += QStringLiteral("%%"); // `%%` 不是引用，原样保留
        } else if (lower.contains(name.toLower())) {
            result += lower.value(name.toLower());
        } else {
            // ⚠ 未定义的变量**原样保留**：静默变成空串会让用户看到一条
            //   "看起来正常但其实是残废"的路径（那正是最难查的那种问题）
            result += value.mid(open, close - open + 1);
        }
        index = close + 1;
    }
    return result;
}

QString pathEntryProblem(const QString &expandedEntry)
{
    if (expandedEntry.trimmed().isEmpty()) {
        return QStringLiteral("空条目（等价于当前目录，通常应当删掉）");
    }
    if (expandedEntry.contains(QLatin1Char('%'))) {
        return QStringLiteral("含未展开的变量（该变量没有定义）");
    }

    const QFileInfo info(expandedEntry);
    if (!info.exists()) {
        return QStringLiteral("目录不存在");
    }
    if (!info.isDir()) {
        return QStringLiteral("不是目录");
    }
    return QString();
}

// ---------------------------------------------------------------------------
//  导入导出
// ---------------------------------------------------------------------------

QString exportVariables(const QMap<QString, QString> &variables, EnvScope scope)
{
    QJsonObject object;
    object.insert(QStringLiteral("scope"), envScopeKey(scope));
    object.insert(QStringLiteral("exportedAt"),
                  QDateTime::currentDateTime().toString(Qt::ISODate));

    QJsonObject values;
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
        values.insert(it.key(), it.value());
    }
    object.insert(QStringLiteral("variables"), values);

    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

bool importVariables(const QString &text, QMap<QString, QString> *out, QString *errorOut)
{
    if (out == nullptr) {
        return false;
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("不是合法的 JSON（偏移 %1）").arg(error.offset);
        }
        return false;
    }

    const QJsonObject object = document.object();
    const QJsonObject values = object.value(QStringLiteral("variables")).toObject();
    if (values.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("JSON 里没有 variables 字段（或它是空的）");
        }
        return false;
    }

    QMap<QString, QString> parsed;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const QString problem = variableNameProblem(it.key());
        if (!problem.isEmpty()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("变量名 %1：%2").arg(it.key(), problem);
            }
            return false;
        }
        parsed.insert(it.key(), it.value().toString());
    }

    *out = parsed;
    return true;
}

// ---------------------------------------------------------------------------
//  读注册表
// ---------------------------------------------------------------------------

QMap<QString, QString> readEnvironment(EnvScope scope, QString *errorOut)
{
    QMap<QString, QString> variables;

    const RegistryRoot root = scope == EnvScope::User ? RegistryRoot::CurrentUser
                                                      : RegistryRoot::LocalMachine;
    QString error;
    RegistryKey key = RegistryKey::open(root, envScopeRegistryPath(scope), true,
                                        RegistryView::Default, &error);
    if (!key.isValid()) {
        if (errorOut != nullptr) {
            *errorOut = error;
        }
        return variables;
    }

    const QStringList names = key.valueNames();
    for (const QString &name : names) {
        const QVariant raw = key.value(name);
        if (raw.isValid()) {
            variables.insert(name, raw.toString());
        }
    }
    return variables;
}

bool valueNeedsExpandType(const QString &value)
{
    return value.contains(QLatin1Char('%'));
}

} // namespace WinEase::Common
