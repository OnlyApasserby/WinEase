#include "RenameEngine.h"

// ============================================================================
//  RenameEngine.cpp —— 规则的实现（全部是纯函数 + QFile，不碰界面）
//
//  两个"看起来不重要、实际最容易出事"的地方，都在本文件里集中处理：
//
//    ① **仅大小写改名**（a.txt → A.txt）
//       Windows 文件系统不区分大小写 → Qt 的 QFile::rename() 会因为
//       "目标已存在"而失败（它真的能在目录里看到 A.txt 吗？看不到，
//       但 exists() 返回 true）。所以必须先改成临时名再改成目标名。
//
//    ② **循环改名**（a.txt → b.txt 且 b.txt → a.txt）
//       直接顺序执行必然撞车：先把 a 改成 b 会覆盖掉真正的 b。
//       同样靠"先把需要让位的源文件挪到临时名"解决。
//
//  这两件事用同一个判据：**"我的旧名字会不会是别人（或我自己）的目标名"**，
//  一旦成立就先挪到临时名，全部挪完后统一落到最终名。
//  这样执行顺序无关，也就不用去解拓扑序/环了。
// ============================================================================

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace WinEase::FeaturePlugins::RenameEngine {
namespace {

QString folded(const QString &text)
{
    return text.toCaseFolded();
}

/// 拆出主干与扩展名。以点开头的名字（.gitignore）视为"无扩展名"，
/// 否则 ".gitignore" 会被当成"主干为空 + 扩展名 gitignore"。
void splitName(const QString &name, QString *base, QString *extension)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        *base = name;
        extension->clear();
        return;
    }
    *base = name.left(dot);
    *extension = name.mid(dot + 1);
}

QString applyCaseMode(const QString &text, CaseMode mode)
{
    switch (mode) {
    case CaseMode::Lower:
        return text.toLower();
    case CaseMode::Upper:
        return text.toUpper();
    case CaseMode::Capitalize: {
        if (text.isEmpty()) {
            return text;
        }
        QString result = text.toLower();
        result[0] = result.at(0).toUpper();
        return result;
    }
    case CaseMode::Title: {
        // "每周报告 final" → "每周报告 Final"（按空白/连字符/下划线切词）
        QString result = text.toLower();
        bool atWordStart = true;
        for (int i = 0; i < result.size(); ++i) {
            const QChar ch = result.at(i);
            const bool separator = ch.isSpace() || ch == QLatin1Char('-') || ch == QLatin1Char('_');
            if (separator) {
                atWordStart = true;
                continue;
            }
            if (atWordStart) {
                result[i] = ch.toUpper();
                atWordStart = false;
            }
        }
        return result;
    }
    case CaseMode::Keep:
        break;
    }
    return text;
}

/// 临时名的唯一化：临时名要能被自己认出来（便于人工排查残留），又不能撞上真文件
QString uniqueTempName(const QString &base, const QSet<QString> &taken, int *counter)
{
    for (;;) {
        const QString candidate = QStringLiteral("%1.winease_tmp%2").arg(base).arg(*counter);
        ++(*counter);
        if (!taken.contains(folded(candidate))) {
            return candidate;
        }
    }
}

ApplyResult applyPairs(const QString &dirPath, const QVector<QPair<QString, QString>> &pairs)
{
    ApplyResult result;

    QDir dir(dirPath);
    if (!dir.exists()) {
        result.error = QStringLiteral("目录不存在：%1").arg(dirPath);
        return result;
    }

    QVector<QPair<QString, QString>> pending;
    for (const auto &pair : pairs) {
        if (pair.first == pair.second) {
            continue;
        }
        pending.append(pair);
    }
    if (pending.isEmpty()) {
        result.ok = true;
        return result;
    }

    // 目标名集合（大小写不敏感）：谁的旧名字在里面，谁就得先让位
    QSet<QString> targets;
    QSet<QString> taken;
    for (const auto &pair : pending) {
        targets.insert(folded(pair.second));
        taken.insert(folded(pair.first));
        taken.insert(folded(pair.second));
    }

    // ---------------- 第一阶段：需要让位的源文件先挪到临时名 ----------------
    struct Working {
        QString currentName; ///< 磁盘上现在的名字
        QString finalName;
        QString originalName;
    };
    QVector<Working> working;
    int tempCounter = 0;

    for (const auto &pair : pending) {
        Working item{ pair.first, pair.second, pair.first };

        if (!targets.contains(folded(pair.first))) {
            working.append(item);
            continue;
        }

        // 让位：先确认旧文件真的在（否则如实报错，不做无声跳过）
        const QString sourcePath = dir.filePath(pair.first);
        if (!QFileInfo::exists(sourcePath)) {
            result.failed.append(
                QStringLiteral("%1 → %2（源文件已不存在）").arg(pair.first, pair.second));
            continue;
        }
        const QString tempName = uniqueTempName(pair.first, taken, &tempCounter);
        if (!QFile::rename(sourcePath, dir.filePath(tempName))) {
            result.failed.append(QStringLiteral("%1 → %2（暂存改名失败，可能被占用）")
                                     .arg(pair.first, pair.second));
            continue;
        }
        taken.insert(folded(tempName));
        item.currentName = tempName;
        working.append(item);
    }

    // ---------------- 第二阶段：落成最终名 ----------------
    for (const Working &item : working) {
        const QString fromPath = dir.filePath(item.currentName);
        const QString toPath = dir.filePath(item.finalName);

        // 运行时兜底：目标名被"本批之外"的文件占着就拒绝覆盖（计划阶段已查过，
        // 但用户可能在预览之后又往目录里放了东西）
        if (QFileInfo::exists(toPath) && folded(item.finalName) != folded(item.currentName)) {
            // 若它正是本批某个文件的当前名，说明第一阶段没挪成功，这里也不该硬来
            result.failed.append(QStringLiteral("%1 → %2（目标名已存在）")
                                     .arg(item.originalName, item.finalName));
            continue;
        }

        if (QFile::rename(fromPath, toPath)) {
            result.applied.append(qMakePair(item.originalName, item.finalName));
        } else {
            result.failed.append(QStringLiteral("%1 → %2（改名失败，文件可能被占用）")
                                     .arg(item.originalName, item.finalName));
        }
    }

    result.ok = result.failed.isEmpty();
    if (!result.ok) {
        result.error = QStringLiteral("有 %1 项未能完成").arg(result.failed.size());
    }
    return result;
}

} // namespace

// ============================================================================
//  枚举 / 规则的键名与显示名
// ============================================================================

QString caseModeKey(CaseMode mode)
{
    switch (mode) {
    case CaseMode::Lower:
        return QStringLiteral("lower");
    case CaseMode::Upper:
        return QStringLiteral("upper");
    case CaseMode::Capitalize:
        return QStringLiteral("capitalize");
    case CaseMode::Title:
        return QStringLiteral("title");
    case CaseMode::Keep:
        break;
    }
    return QStringLiteral("keep");
}

CaseMode caseModeFromKey(const QString &key, bool *ok)
{
    const QString normalized = key.trimmed().toLower();
    if (ok != nullptr) {
        *ok = true;
    }
    if (normalized == QLatin1String("lower")) {
        return CaseMode::Lower;
    }
    if (normalized == QLatin1String("upper")) {
        return CaseMode::Upper;
    }
    if (normalized == QLatin1String("capitalize")) {
        return CaseMode::Capitalize;
    }
    if (normalized == QLatin1String("title")) {
        return CaseMode::Title;
    }
    if (normalized == QLatin1String("keep") || normalized.isEmpty()) {
        return CaseMode::Keep;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return CaseMode::Keep;
}

QString caseModeDisplayName(CaseMode mode)
{
    switch (mode) {
    case CaseMode::Lower:
        return QStringLiteral("全小写");
    case CaseMode::Upper:
        return QStringLiteral("全大写");
    case CaseMode::Capitalize:
        return QStringLiteral("首字母大写");
    case CaseMode::Title:
        return QStringLiteral("每词首字母大写");
    case CaseMode::Keep:
        break;
    }
    return QStringLiteral("保持原样");
}

QString extensionModeKey(ExtensionMode mode)
{
    switch (mode) {
    case ExtensionMode::LowerCase:
        return QStringLiteral("lower");
    case ExtensionMode::UpperCase:
        return QStringLiteral("upper");
    case ExtensionMode::Replace:
        return QStringLiteral("replace");
    case ExtensionMode::Remove:
        return QStringLiteral("remove");
    case ExtensionMode::Keep:
        break;
    }
    return QStringLiteral("keep");
}

ExtensionMode extensionModeFromKey(const QString &key, bool *ok)
{
    const QString normalized = key.trimmed().toLower();
    if (ok != nullptr) {
        *ok = true;
    }
    if (normalized == QLatin1String("lower")) {
        return ExtensionMode::LowerCase;
    }
    if (normalized == QLatin1String("upper")) {
        return ExtensionMode::UpperCase;
    }
    if (normalized == QLatin1String("replace")) {
        return ExtensionMode::Replace;
    }
    if (normalized == QLatin1String("remove")) {
        return ExtensionMode::Remove;
    }
    if (normalized == QLatin1String("keep") || normalized.isEmpty()) {
        return ExtensionMode::Keep;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return ExtensionMode::Keep;
}

QString extensionModeDisplayName(ExtensionMode mode)
{
    switch (mode) {
    case ExtensionMode::LowerCase:
        return QStringLiteral("扩展名转小写");
    case ExtensionMode::UpperCase:
        return QStringLiteral("扩展名转大写");
    case ExtensionMode::Replace:
        return QStringLiteral("换成指定扩展名");
    case ExtensionMode::Remove:
        return QStringLiteral("去掉扩展名");
    case ExtensionMode::Keep:
        break;
    }
    return QStringLiteral("保持原样");
}

// ============================================================================
//  Item / Plan
// ============================================================================

bool Item::caseOnlyChange() const
{
    return valid() && newName != originalName
        && QString::compare(newName, originalName, Qt::CaseInsensitive) == 0;
}

bool Plan::ok() const
{
    return conflicts.isEmpty();
}

int Plan::changedCount() const
{
    int count = 0;
    for (const Item &item : items) {
        if (item.changed()) {
            ++count;
        }
    }
    return count;
}

int Plan::problemCount() const
{
    int count = 0;
    for (const Item &item : items) {
        if (!item.valid()) {
            ++count;
        }
    }
    return count;
}

QString Plan::summaryText() const
{
    return QStringLiteral("将重命名 %1 项，%2 项有问题，%3 处冲突")
        .arg(changedCount())
        .arg(problemCount())
        .arg(conflicts.size());
}

QString ApplyResult::summaryText() const
{
    if (!ok) {
        return error.isEmpty() ? QStringLiteral("执行失败") : error;
    }
    return QStringLiteral("已重命名 %1 项").arg(applied.size());
}

// ============================================================================
//  规则计算
// ============================================================================

bool isLegalFileName(const QString &name, QString *reason)
{
    const auto fail = [reason](const QString &text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };

    if (name.isEmpty()) {
        return fail(QStringLiteral("文件名为空"));
    }
    if (name.size() > 255) {
        return fail(QStringLiteral("文件名过长（%1 个字符，上限 255）").arg(name.size()));
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        return fail(QStringLiteral("文件名不能包含路径分隔符"));
    }

    static const QString invalidChars = QStringLiteral("\\/:*?\"<>|");
    for (const QChar ch : name) {
        if (invalidChars.contains(ch)) {
            return fail(QStringLiteral("文件名不能包含 \\ / : * ? \" < > | 之一"));
        }
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        return fail(QStringLiteral("文件名不能以点或空格结尾"));
    }

    // Windows 保留名：CON / PRN / AUX / NUL / COM1-9 / LPT1-9（带扩展名也不行）
    static const QStringList reservedNames = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")
    };
    QString stem;
    QString extension;
    splitName(name, &stem, &extension);
    if (reservedNames.contains(stem.toUpper())) {
        return fail(QStringLiteral("「%1」是 Windows 保留名").arg(stem.toUpper()));
    }

    return true;
}

QVector<Item> buildItems(const QStringList &names,
                         const QVector<QDateTime> &timestamps,
                         const Options &options,
                         QString *errorOut)
{
    QVector<Item> items;
    items.reserve(names.size());

    QRegularExpression pattern;
    if (!options.findPattern.isEmpty()) {
        pattern.setPattern(options.findPattern);
        if (!pattern.isValid()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("查找正则无效：%1").arg(pattern.errorString());
            }
            return items;
        }
    }

    QString extensionReplacement = options.newExtension.trimmed();
    while (extensionReplacement.startsWith(QLatin1Char('.'))) {
        extensionReplacement.remove(0, 1);
    }

    for (int index = 0; index < names.size(); ++index) {
        Item item;
        item.originalName = names.at(index);

        QString stem;
        QString extension;
        splitName(item.originalName, &stem, &extension);

        // ①②查找替换 + 捕获组
        QStringList captures;
        captures.reserve(10);
        for (int i = 0; i < 10; ++i) {
            captures.append(QString());
        }

        if (!pattern.pattern().isEmpty()) {
            const QRegularExpressionMatch match = pattern.match(stem);
            if (!match.hasMatch()) {
                item.newName = item.originalName;
                item.problem = QStringLiteral("查找内容未匹配到，本项不重命名");
                items.append(item);
                continue;
            }
            for (int i = 1; i <= 9; ++i) {
                captures[i] = match.captured(i);
            }
            stem.replace(pattern, options.replaceText);
        }

        // ③模板展开
        if (!options.nameTemplate.isEmpty()) {
            QString expanded = options.nameTemplate;
            expanded.replace(QLatin1String("{name}"), stem);
            expanded.replace(QLatin1String("{ext}"), extension);
            const int number = options.startNumber + options.numberStep * index;
            expanded.replace(QLatin1String("{n}"),
                             options.numberPadding > 0
                                 ? QString::number(number).rightJustified(options.numberPadding,
                                                                         QLatin1Char('0'))
                                 : QString::number(number));
            // 时间戳占位符：缺失时展开成空串，绝不静默换成"今天"
            // （批量改名最怕这种兜底：用户看到的名字看着正常，其实是错的日期）
            const QDateTime stamp = index < timestamps.size() ? timestamps.at(index) : QDateTime();
            const bool hasStamp = stamp.isValid();
            expanded.replace(QLatin1String("{datetime}"),
                             hasStamp ? stamp.toString(QStringLiteral("yyyy-MM-dd_HHmmss")) : QString());
            expanded.replace(QLatin1String("{date}"),
                             hasStamp ? stamp.toString(QStringLiteral("yyyy-MM-dd")) : QString());
            expanded.replace(QLatin1String("{time}"),
                             hasStamp ? stamp.toString(QStringLiteral("HHmmss")) : QString());
            for (int i = 1; i <= 9; ++i) {
                expanded.replace(QStringLiteral("{%1}").arg(i), captures.at(i));
            }
            stem = expanded;
        }

        // ④大小写（只管主干，不动用户手打的 prefix/suffix）
        stem = applyCaseMode(stem, options.caseMode);

        // ⑤前后缀
        stem = options.prefix + stem + options.suffix;

        // ⑥扩展名
        switch (options.extensionMode) {
        case ExtensionMode::LowerCase:
            extension = extension.toLower();
            break;
        case ExtensionMode::UpperCase:
            extension = extension.toUpper();
            break;
        case ExtensionMode::Replace:
            extension = extensionReplacement;
            break;
        case ExtensionMode::Remove:
            extension.clear();
            break;
        case ExtensionMode::Keep:
            break;
        }

        item.newName = extension.isEmpty() ? stem : stem + QLatin1Char('.') + extension;

        // ⑦校验
        QString reason;
        if (!isLegalFileName(item.newName, &reason)) {
            item.problem = reason;
        } else if (item.newName == item.originalName) {
            item.problem = QStringLiteral("结果与原名相同（规则对本项无效果）");
        }

        items.append(item);
    }

    return items;
}

Plan plan(const QStringList &names,
          const QStringList &allNamesInDir,
          const QVector<QDateTime> &timestamps,
          const Options &options,
          QString *errorOut)
{
    Plan result;
    result.items = buildItems(names, timestamps, options, errorOut);
    if (result.items.isEmpty()) {
        return result;
    }

    // ---------------- 冲突一：两项要改成同一个名字 ----------------
    QHash<QString, QString> firstOwner; // 折叠名 → 第一个拥有者的原名
    QStringList duplicateTargets;
    for (const Item &item : result.items) {
        if (!item.changed()) {
            continue;
        }
        const QString key = folded(item.newName);
        const auto it = firstOwner.constFind(key);
        if (it == firstOwner.constEnd()) {
            firstOwner.insert(key, item.originalName);
            continue;
        }
        const QString message = QStringLiteral("「%1」与「%2」都要改成「%3」")
                                    .arg(it.value(), item.originalName, item.newName);
        if (!duplicateTargets.contains(message)) {
            duplicateTargets.append(message);
        }
    }

    // ---------------- 冲突二：目标名撞上目录里"不参与本次重命名"的文件 ----------------
    QSet<QString> sourceNames;
    for (const QString &name : names) {
        sourceNames.insert(folded(name));
    }
    QSet<QString> existingNames;
    for (const QString &name : allNamesInDir) {
        existingNames.insert(folded(name));
    }
    QStringList occupiedTargets;
    for (const Item &item : result.items) {
        if (!item.changed()) {
            continue;
        }
        const QString key = folded(item.newName);
        if (existingNames.contains(key) && !sourceNames.contains(key)) {
            const QString message =
                QStringLiteral("目标名「%1」已被目录里的其它文件占用（%2）").arg(item.newName, item.originalName);
            if (!occupiedTargets.contains(message)) {
                occupiedTargets.append(message);
            }
        }
    }

    result.conflicts = occupiedTargets + duplicateTargets;
    return result;
}

// ============================================================================
//  执行 / 撤销
// ============================================================================

ApplyResult apply(const QString &dirPath, const Plan &plan)
{
    if (!plan.ok()) {
        ApplyResult result;
        result.error = QStringLiteral("计划里有 %1 处冲突，已拒绝执行（请先修正规则）")
                           .arg(plan.conflicts.size());
        return result;
    }

    QVector<QPair<QString, QString>> pairs;
    for (const Item &item : plan.items) {
        if (item.changed()) {
            pairs.append(qMakePair(item.originalName, item.newName));
        }
    }
    return applyPairs(dirPath, pairs);
}

ApplyResult undo(const QString &dirPath, const QVector<QPair<QString, QString>> &applied)
{
    // 反向执行：old→new 变成 new→old，并按相反顺序来
    QVector<QPair<QString, QString>> reversed;
    reversed.reserve(applied.size());
    for (int i = applied.size() - 1; i >= 0; --i) {
        reversed.append(qMakePair(applied.at(i).second, applied.at(i).first));
    }
    return applyPairs(dirPath, reversed);
}

} // namespace WinEase::FeaturePlugins::RenameEngine
