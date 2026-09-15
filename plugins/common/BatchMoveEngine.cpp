#include "BatchMoveEngine.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace WinEase::FeaturePlugins::BatchMove {

namespace {

const char *const kSlash = "/";

/// 文件名里不许出现的字符（Windows 九件套 + 控制字符）
bool hasIllegalCharacters(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return true;
    }
    static const QString illegal = QStringLiteral("\\/:*?\"<>|");
    for (const QChar &ch : name) {
        if (ch.unicode() < 0x20 || illegal.contains(ch)) {
            return true;
        }
    }
    return name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '));
}

/// 把目标目录 + 相对子路径拼成绝对路径，并保证结果**没跑出目标目录**
QString joinGuarded(const QString &root, const QString &subPath, bool *escaped)
{
    const QString cleanRoot = normalizePath(root);
    QString combined = cleanRoot;
    if (!subPath.isEmpty()) {
        combined += QLatin1Char('/') + normalizePath(subPath);
    }
    // QDir::cleanPath 会把 ".." 折叠掉，折叠完再看是否还在 root 里
    const QString cleaned = QDir::cleanPath(combined);
    const bool inside = isWithinDirectory(cleaned, cleanRoot);
    if (escaped != nullptr) {
        *escaped = !inside;
    }
    return inside ? cleaned : cleanRoot;
}

/// 应用命名模板：{name} 原名（无扩展名）、{ext} 扩展名（含点）、{n} 序号
QString applyTemplate(const QString &fileName, const QString &templ, int index)
{
    if (templ.trimmed().isEmpty()) {
        return fileName;
    }
    const QFileInfo info(fileName);
    QString result = templ;
    result.replace(QLatin1String("{name}"), info.completeBaseName());
    result.replace(QLatin1String("{ext}"), info.suffix().isEmpty()
                                               ? QString()
                                               : QLatin1Char('.') + info.suffix());
    result.replace(QLatin1String("{n}"), QString::number(index + 1));
    return result.trimmed();
}

} // namespace

int Plan::readyCount() const
{
    int count = 0;
    for (const PlannedItem &item : items) {
        if (item.status == ItemStatus::Ready) {
            ++count;
        }
    }
    return count;
}

int Plan::skippedCount() const
{
    return items.size() - readyCount();
}

QString ApplyResult::summary() const
{
    if (failed == 0) {
        return QStringLiteral("已移动 %1 个文件").arg(moved);
    }
    return QStringLiteral("已移动 %1 个文件，%2 个失败").arg(moved).arg(failed);
}

QString normalizePath(const QString &path)
{
    QString result = path;
    result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (result.contains(QLatin1String("//"))) {
        result.replace(QLatin1String("//"), QLatin1String("/"));
    }
    if (result.size() > 3 && result.endsWith(QLatin1Char('/'))) {
        result.chop(1);
    }
    return result;
}

bool isWithinDirectory(const QString &path, const QString &root)
{
    const QString cleanedPath = QDir::cleanPath(normalizePath(path));
    const QString cleanedRoot = QDir::cleanPath(normalizePath(root));
    if (cleanedRoot.isEmpty()) {
        return false;
    }
    if (cleanedPath.compare(cleanedRoot, Qt::CaseInsensitive) == 0) {
        return true;
    }
    return cleanedPath.startsWith(cleanedRoot + QLatin1Char('/'), Qt::CaseInsensitive);
}

QString statusText(ItemStatus status)
{
    switch (status) {
    case ItemStatus::Ready:
        return QStringLiteral("待移动");
    case ItemStatus::NotMatched:
        return QStringLiteral("未匹配正则");
    case ItemStatus::InvalidName:
        return QStringLiteral("目标名非法");
    case ItemStatus::SamePlace:
        return QStringLiteral("原地不动");
    case ItemStatus::Conflict:
        return QStringLiteral("目标已存在（按策略跳过）");
    case ItemStatus::DuplicateTarget:
        return QStringLiteral("与同批其它文件撞到同一个目标");
    }
    return QStringLiteral("未知状态");
}

QString autoRename(const QString &fileName, int index)
{
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(index);
    return suffix.isEmpty() ? candidate : candidate + QLatin1Char('.') + suffix;
}

QStringList listFiles(const QString &directory, bool recursive)
{
    QStringList result;
    const QFileInfo root(directory);
    if (!root.isDir()) {
        return result;
    }

    const QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    if (!recursive) {
        const QDir dir(directory);
        for (const QFileInfo &info : dir.entryInfoList(filters, QDir::Name)) {
            result.append(normalizePath(info.absoluteFilePath()));
        }
        return result;
    }

    QDirIterator iterator(directory, filters, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        result.append(normalizePath(iterator.next()));
    }
    return result;
}

Plan buildPlan(const QStringList &files, const Options &options)
{
    Plan plan;

    // ---- 前置校验：任何一条不过就**整个计划作废**，绝不"降级继续搬" ----
    if (options.sourceDirectory.trimmed().isEmpty()
        || options.targetDirectory.trimmed().isEmpty()) {
        plan.error = QStringLiteral("源目录与目标目录都必须指定。");
        return plan;
    }
    if (!QFileInfo(options.sourceDirectory).isDir()) {
        plan.error = QStringLiteral("源目录不存在或不可访问：%1").arg(options.sourceDirectory);
        return plan;
    }

    QRegularExpression regex(options.pattern,
                             options.caseSensitive ? QRegularExpression::NoPatternOption
                                                   : QRegularExpression::CaseInsensitiveOption);
    if (!regex.isValid()) {
        plan.error = QStringLiteral("正则表达式无效：%1（位置 %2）")
                         .arg(regex.errorString())
                         .arg(regex.patternErrorOffset());
        return plan;
    }

    const QString sourceRoot = normalizePath(options.sourceDirectory);
    const QString targetRoot = normalizePath(options.targetDirectory);
    if (sourceRoot.compare(targetRoot, Qt::CaseInsensitive) == 0 && !options.keepStructure
        && options.nameTemplate.trimmed().isEmpty()) {
        plan.error = QStringLiteral("源目录与目标目录相同，且没有改名/保留结构的动作，"
                                    "这次移动没有任何效果。");
        return plan;
    }

    QSet<QString> usedTargets;
    int conflictSkips = 0;
    int index = 0;

    for (const QString &rawFile : files) {
        const QString source = normalizePath(rawFile);
        PlannedItem item;
        item.sourcePath = source;
        item.fileName = QFileInfo(source).fileName();

        const QString subject = options.matchFullPath ? source : item.fileName;
        const bool matched = regex.match(subject).hasMatch();
        if (!matched) {
            item.status = ItemStatus::NotMatched;
            item.targetPath = QString();
            item.note = statusText(item.status);
            plan.items.append(item);
            continue;
        }

        // ---- 目标子路径 ----
        QString relativeDirectory;
        if (options.keepStructure) {
            const QString relative =
                QDir(sourceRoot).relativeFilePath(QFileInfo(source).absolutePath());
            if (relative != QLatin1String(".")) {
                relativeDirectory = relative;
            }
        }

        const QString newName = applyTemplate(item.fileName, options.nameTemplate, index);
        if (hasIllegalCharacters(newName)) {
            item.status = ItemStatus::InvalidName;
            item.note = QStringLiteral("%1（命名模板算出的名字不合法）").arg(statusText(item.status));
            plan.items.append(item);
            ++index;
            continue;
        }

        bool escaped = false;
        const QString directory = joinGuarded(targetRoot, relativeDirectory, &escaped);
        if (escaped) {
            item.status = ItemStatus::InvalidName;
            item.note = QStringLiteral("目标路径越出了目标目录（已拦下）");
            plan.items.append(item);
            ++index;
            continue;
        }

        item.targetPath = normalizePath(directory + QLatin1Char('/') + newName);
        if (!isWithinDirectory(item.targetPath, targetRoot)) {
            item.status = ItemStatus::InvalidName;
            item.note = QStringLiteral("目标路径越出了目标目录（已拦下）");
            plan.items.append(item);
            ++index;
            continue;
        }

        if (item.targetPath.compare(source, Qt::CaseInsensitive) == 0) {
            item.status = ItemStatus::SamePlace;
            item.note = statusText(item.status);
            plan.items.append(item);
            ++index;
            continue;
        }

        // ---- 同批内部撞车 ----
        const QString key = item.targetPath.toCaseFolded();
        if (usedTargets.contains(key)) {
            item.status = ItemStatus::DuplicateTarget;
            item.note = statusText(item.status);
            plan.items.append(item);
            ++index;
            continue;
        }

        // ---- 盘上重名：按策略处理 ----
        if (QFileInfo::exists(item.targetPath)) {
            switch (options.policy) {
            case ConflictPolicy::Skip:
                item.status = ItemStatus::Conflict;
                item.note = statusText(item.status);
                plan.items.append(item);
                ++index;
                ++conflictSkips;
                continue;
            case ConflictPolicy::Rename: {
                int suffix = 1;
                QString candidate = item.targetPath;
                while (QFileInfo::exists(candidate) || usedTargets.contains(candidate.toCaseFolded())) {
                    candidate = normalizePath(
                        directory + QLatin1Char('/') + autoRename(newName, suffix));
                    ++suffix;
                    if (suffix > 9999) {
                        break;
                    }
                }
                item.targetPath = candidate;
                item.note = QStringLiteral("重名，自动改名为 %1")
                                .arg(QFileInfo(candidate).fileName());
                break;
            }
            case ConflictPolicy::Overwrite:
                item.note = QStringLiteral("目标已存在，将**覆盖**");
                break;
            }
        } else {
            item.note = statusText(ItemStatus::Ready);
        }

        item.status = ItemStatus::Ready;
        usedTargets.insert(item.targetPath.toCaseFolded());
        plan.items.append(item);
        ++index;
    }

    if (conflictSkips > 0) {
        plan.warnings.append(
            QStringLiteral("有 %1 个文件因为目标已存在被跳过（策略：跳过）。"
                           "要覆盖或改名请在设置里改策略。")
                .arg(conflictSkips));
    }
    plan.valid = true;
    return plan;
}

ApplyResult applyPlan(const Plan &plan, bool dryRun)
{
    ApplyResult result;
    if (!plan.valid) {
        result.failed = 1;
        result.errors.append(plan.error.isEmpty() ? QStringLiteral("计划无效。") : plan.error);
        return result;
    }

    for (const PlannedItem &item : plan.items) {
        if (item.status != ItemStatus::Ready) {
            continue;
        }
        if (dryRun) {
            ++result.moved;
            continue;
        }

        const QFileInfo sourceInfo(item.sourcePath);
        if (!sourceInfo.exists()) {
            ++result.failed;
            result.errors.append(QStringLiteral("%1：源文件已不存在").arg(item.fileName));
            continue;
        }

        // 目标目录可能不存在（保留结构时），先建出来
        const QString targetDirectory = QFileInfo(item.targetPath).absolutePath();
        if (!QDir().mkpath(targetDirectory)) {
            ++result.failed;
            result.errors.append(
                QStringLiteral("%1：无法创建目标目录 %2").arg(item.fileName, targetDirectory));
            continue;
        }

        QFile::remove(item.targetPath); // 策略为覆盖时先删掉旧的（跳过/改名分支不会走到这）
        if (QFile::rename(item.sourcePath, item.targetPath)) {
            ++result.moved;
            result.records.append({item.sourcePath, item.targetPath});
            continue;
        }

        // 跨卷时 rename 会失败（ERROR_NOT_SAME_DEVICE）→ 退化成"复制 + 删源"
        if (QFile::copy(item.sourcePath, item.targetPath)) {
            if (QFile::remove(item.sourcePath)) {
                ++result.moved;
                result.records.append({item.sourcePath, item.targetPath});
                continue;
            }
            // 复制成功但源删不掉 → 回滚目标，如实报错（绝不留下"两份都不知道哪份是新的"）
            QFile::remove(item.targetPath);
            ++result.failed;
            result.errors.append(QStringLiteral("%1：已复制到目标，但源文件删不掉（可能被占用）")
                                     .arg(item.fileName));
            continue;
        }

        ++result.failed;
        result.errors.append(
            QStringLiteral("%1：移动失败（目标可能被占用或没有权限）").arg(item.fileName));
    }

    return result;
}

ApplyResult undoMoves(const QList<MoveRecord> &records)
{
    ApplyResult result;
    // 反向执行：后搬的先退回，避免目录里出现"空壳"顺序问题
    for (int i = records.size() - 1; i >= 0; --i) {
        const MoveRecord &record = records.at(i);
        const QFileInfo targetInfo(record.targetPath);
        if (!targetInfo.exists()) {
            ++result.failed;
            result.errors.append(
                QStringLiteral("%1：撤销失败，文件已不在原位").arg(targetInfo.fileName()));
            continue;
        }
        if (!QDir().mkpath(QFileInfo(record.sourcePath).absolutePath())) {
            ++result.failed;
            result.errors.append(QStringLiteral("%1：无法重建原目录")
                                     .arg(QFileInfo(record.sourcePath).fileName()));
            continue;
        }
        if (QFile::rename(record.targetPath, record.sourcePath)) {
            ++result.moved;
            continue;
        }
        ++result.failed;
        result.errors.append(
            QStringLiteral("%1：撤销失败（原位置可能已有同名文件）").arg(targetInfo.fileName()));
    }
    return result;
}

QString serializeMoveRecords(const QList<MoveRecord> &records)
{
    QStringList lines;
    for (const MoveRecord &record : records) {
        // 制表符分隔：路径里不会出现制表符（Windows 文件名不允许控制字符）
        lines.append(record.sourcePath + QLatin1Char('\t') + record.targetPath);
    }
    return lines.join(QLatin1Char('\n'));
}

QList<MoveRecord> parseMoveRecords(const QString &text)
{
    QList<MoveRecord> records;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab <= 0) {
            continue; // 非法行直接跳过：撤销日志宁可少做一条，也不能猜着搬
        }
        MoveRecord record;
        record.sourcePath = normalizePath(line.left(tab).trimmed());
        record.targetPath = normalizePath(line.mid(tab + 1).trimmed());
        if (record.sourcePath.isEmpty() || record.targetPath.isEmpty()) {
            continue;
        }
        records.append(record);
    }
    return records;
}

} // namespace WinEase::FeaturePlugins::BatchMove
