#include "DuplicateFinder.h"

// ============================================================================
//  DuplicateFinder.cpp —— 四级流水线的实现
//
//  两个"很容易写错、写错了也不报错"的点：
//    ① **抽样哈希必须带上头部实际长度**：只读前 4KB 时，短文件的头部很容易
//       一模一样 —— 组内大小已经相同所以不会串，但为了让 hashFile() 单独调用
//       时也可信，把"实际读到的长度"混进哈希输入。
//    ② **必须跳过符号链接/联接点**：否则 `C:\Users\me\Application Data`
//       指回自己，枚举无限绕圈；或 `D:\link` 指到整个 D 盘，白扫一遍。
//       目录层面的剪枝只能手写递归做（QDirIterator 没有"不下去"的开关）。
//
//  ⚠ 删除策略不在本文件：产品硬约束是"重复文件只能进回收站"，由调用方执行。
// ============================================================================

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRunnable>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace WinEase::FeaturePlugins::DuplicateFinder {
namespace {

#ifdef Q_OS_WIN
bool isReparsePoint(const QString &path)
{
    const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<const wchar_t *>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
bool isReparsePoint(const QString &)
{
    return false;
}
#endif

QString folded(const QString &text)
{
    return text.toCaseFolded();
}

/// 单文件哈希：sampleBytes > 0 时只读头部（抽样），否则全量读。
/// 失败（打不开/读不动）返回空串 —— 调用方把它当作"本次无法比较"，而不是"不重复"。
QString hashFileInternal(const QString &path, int sampleBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (sampleBytes > 0) {
        const QByteArray head = file.read(sampleBytes);
        if (head.isEmpty() && file.size() > 0) {
            return QString();
        }
        hash.addData(head);
        hash.addData(QByteArray::number(head.size()));
    } else {
        constexpr qint64 kChunk = 1024 * 1024;
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(kChunk);
            if (chunk.isEmpty()) {
                break;
            }
            hash.addData(chunk);
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

/// 并行哈希：结果按原下标写回预分配的 QVector。
/// 每个下标只被一个线程写 → 无数据竞争，不需要锁。
QVector<QString> parallelHash(const QStringList &paths, int sampleBytes, Cancel *cancel)
{
    QVector<QString> results(paths.size());
    if (paths.isEmpty()) {
        return results;
    }

    const int threads = qBound(1, QThread::idealThreadCount() - 1, 8);
    if (paths.size() < 4 || threads <= 1) {
        for (int i = 0; i < paths.size(); ++i) {
            if (cancel != nullptr && cancel->isRequested()) {
                break;
            }
            results[i] = hashFileInternal(paths.at(i), sampleBytes);
        }
        return results;
    }

    // 私有线程池（不用 globalInstance()：那是全进程共享的，waitForDone() 会连
    // 别人的任务一起等，也可能把界面线程的 QtConcurrent 任务拖住）
    QThreadPool pool;
    pool.setMaxThreadCount(threads);
    const int chunkSize = (paths.size() + threads - 1) / threads;
    for (int begin = 0; begin < paths.size(); begin += chunkSize) {
        const int end = qMin(begin + chunkSize, paths.size());
        pool.start(QRunnable::create([&results, &paths, sampleBytes, cancel, begin, end] {
            for (int i = begin; i < end; ++i) {
                if (cancel != nullptr && cancel->isRequested()) {
                    break;
                }
                results[i] = hashFileInternal(paths.at(i), sampleBytes);
            }
        }));
    }
    pool.waitForDone();
    return results;
}

/// 哈希相同的子组（保留 key：全量校验那一步直接拿它当组指纹，不用再读一遍文件）
struct Bucket
{
    QString key;
    QVector<Entry> entries;
};

QVector<Bucket> regroup(const QVector<Entry> &entries, const QVector<QString> &keys)
{
    QHash<QString, QVector<Entry>> buckets;
    QStringList order;
    for (int i = 0; i < entries.size(); ++i) {
        const QString &key = keys.at(i);
        if (key.isEmpty()) {
            continue; // 读不了的文件不参与比较
        }
        if (!buckets.contains(key)) {
            order.append(key);
        }
        buckets[key].append(entries.at(i));
    }

    QVector<Bucket> result;
    for (const QString &key : order) {
        const QVector<Entry> &bucket = buckets.value(key);
        if (bucket.size() >= 2) {
            result.append(Bucket{ key, bucket });
        }
    }
    return result;
}

/// 递归枚举（手写递归而不是 QDirIterator：需要在**目录**层面剪掉联接点）
void scanDirectory(const QString &directory,
                   const ScanOptions &options,
                   Cancel *cancel,
                   const Progress &progress,
                   Stats *stats,
                   QVector<Entry> *out,
                   QSet<QString> *seen,
                   bool *stopped)
{
    QDir dir(directory);
    const QFileInfoList children = dir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &info : children) {
        if (cancel != nullptr && cancel->isRequested()) {
            stats->canceled = true;
            *stopped = true;
            return;
        }

        const QString path = info.absoluteFilePath();
        if (info.isSymLink() || isReparsePoint(path)) {
            ++stats->skippedReparse;
            continue;
        }

        if (info.isDir()) {
            scanDirectory(path, options, cancel, progress, stats, out, seen, stopped);
            if (*stopped) {
                return;
            }
            continue;
        }
        if (!info.isFile()) {
            continue;
        }
        if (!options.includeHidden && info.isHidden()) {
            ++stats->skippedHidden;
            continue;
        }
        if (info.size() < options.minSizeBytes) {
            ++stats->skippedSmall;
            continue;
        }

        // 父子目录都被添加时同一个文件会出现两次：按路径去重
        const QString key = folded(path);
        if (seen->contains(key)) {
            continue;
        }
        seen->insert(key);

        Entry entry;
        entry.path = path;
        entry.size = info.size();
        out->append(entry);
        ++stats->scannedFiles;
        stats->totalBytes += entry.size;

        if ((stats->scannedFiles % 512) == 0 && progress) {
            progress(QStringLiteral("枚举文件"), stats->scannedFiles, 0);
        }
        if (out->size() >= options.maxFiles) {
            stats->truncated = true;
            *stopped = true;
            return;
        }
    }
}

} // namespace

// ============================================================================
//  Stats
// ============================================================================

QString Stats::stageText() const
{
    QString text =
        QStringLiteral("枚举 %1 个文件（共 %2）→ 大小分组 %3 组 → 抽样读取 %4 → 确认 %5 组重复")
            .arg(scannedFiles)
            .arg(formatBytes(totalBytes))
            .arg(sizeGroups)
            .arg(formatBytes(partialHashBytes))
            .arg(finalGroups);
    if (canceled) {
        text += QStringLiteral("（已中断）");
    }
    if (truncated) {
        text += QStringLiteral("（达到文件数上限）");
    }
    return text;
}

// ============================================================================
//  阶段①：递归枚举
// ============================================================================

QVector<Entry> scan(const ScanOptions &options,
                    Cancel *cancel,
                    const Progress &progress,
                    Stats *stats,
                    QString *errorOut)
{
    QVector<Entry> entries;
    if (stats == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("内部错误：stats 为空");
        }
        return entries;
    }

    QStringList roots;
    QSet<QString> seenRoots;
    for (const QString &root : options.roots) {
        const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(root.trimmed()));
        if (cleaned.isEmpty()) {
            continue;
        }
        const QString key = folded(cleaned);
        if (seenRoots.contains(key)) {
            continue; // 同一目录加了两次：只扫一次
        }
        const QFileInfo rootInfo(cleaned);
        if (!rootInfo.exists() || !rootInfo.isDir()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("目录不存在或不是目录：%1").arg(cleaned);
            }
            return entries;
        }
        seenRoots.insert(key);
        roots.append(cleaned);
    }
    if (roots.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("没有添加任何要扫描的目录");
        }
        return entries;
    }

    QElapsedTimer timer;
    timer.start();

    QSet<QString> seen;
    bool stopped = false;
    for (const QString &root : roots) {
        scanDirectory(root, options, cancel, progress, stats, &entries, &seen, &stopped);
        if (stopped) {
            break;
        }
    }

    stats->elapsedMs += timer.elapsed();
    if (entries.isEmpty() && !stats->canceled && errorOut != nullptr) {
        *errorOut = QStringLiteral(
            "没有找到符合条件的文件（默认跳过小于 4 KB 的文件与隐藏文件，也跳过符号链接目录）");
    }
    return entries;
}

// ============================================================================
//  阶段②：按大小分组
// ============================================================================

QVector<QVector<Entry>> groupBySize(const QVector<Entry> &entries, Stats *stats)
{
    QHash<qint64, QVector<Entry>> buckets;
    QList<qint64> sizes;
    for (const Entry &entry : entries) {
        if (!buckets.contains(entry.size)) {
            sizes.append(entry.size);
        }
        buckets[entry.size].append(entry);
    }
    // 大的排前面：用户通常更关心"几个 G 的重复大文件"
    std::sort(sizes.begin(), sizes.end(), std::greater<qint64>());

    QVector<QVector<Entry>> groups;
    for (const qint64 size : sizes) {
        const QVector<Entry> &bucket = buckets.value(size);
        if (bucket.size() < 2) {
            continue; // 大小唯一 → 一定不重复
        }
        groups.append(bucket);
        if (stats != nullptr) {
            ++stats->sizeGroups;
            stats->candidateFiles += static_cast<int>(bucket.size());
        }
    }
    return groups;
}

// ============================================================================
//  阶段③：抽样哈希
// ============================================================================

QVector<QVector<Entry>> filterBySampleHash(const QVector<QVector<Entry>> &groups,
                                           int sampleBytes,
                                           Cancel *cancel,
                                           const Progress &progress,
                                           Stats *stats,
                                           QString *errorOut)
{
    Q_UNUSED(errorOut)
    QVector<QVector<Entry>> result;
    const int total = groups.size();

    for (int i = 0; i < total; ++i) {
        if (cancel != nullptr && cancel->isRequested()) {
            if (stats != nullptr) {
                stats->canceled = true;
            }
            return result;
        }

        const QVector<Entry> &group = groups.at(i);
        QStringList paths;
        paths.reserve(group.size());
        for (const Entry &entry : group) {
            paths.append(entry.path);
        }

        const QVector<QString> keys = parallelHash(paths, sampleBytes, cancel);
        const QVector<Bucket> subGroups = regroup(group, keys);
        for (const Bucket &sub : subGroups) {
            result.append(sub.entries);
            if (stats != nullptr) {
                ++stats->partialGroups;
                // 抽样实际读了多少字节：只读头部，短文件按实际长度算
                for (const Entry &entry : sub.entries) {
                    stats->partialHashBytes += sampleBytes <= 0
                        ? static_cast<int>(entry.size)
                        : static_cast<int>(qMin<qint64>(sampleBytes, entry.size));
                }
            }
        }

        if (progress) {
            progress(QStringLiteral("抽样比对"), i + 1, total);
        }
    }
    return result;
}

// ============================================================================
//  阶段④：全量 SHA-256 确认
// ============================================================================

QVector<Group> confirmByFullHash(const QVector<QVector<Entry>> &groups,
                                 Cancel *cancel,
                                 const Progress &progress,
                                 Stats *stats,
                                 QString *errorOut)
{
    Q_UNUSED(errorOut)
    QVector<Group> result;
    const int total = groups.size();

    for (int i = 0; i < total; ++i) {
        if (cancel != nullptr && cancel->isRequested()) {
            if (stats != nullptr) {
                stats->canceled = true;
            }
            return result;
        }

        const QVector<Entry> &group = groups.at(i);
        QStringList paths;
        paths.reserve(group.size());
        for (const Entry &entry : group) {
            paths.append(entry.path);
        }

        const QVector<QString> keys = parallelHash(paths, -1, cancel);
        const QVector<Bucket> subGroups = regroup(group, keys);
        for (const Bucket &sub : subGroups) {
            Group confirmed;
            confirmed.size = sub.entries.first().size;
            confirmed.hash = sub.key; // 就是全量 SHA-256，不用再读一遍文件
            for (const Entry &entry : sub.entries) {
                confirmed.files.append(entry.path);
            }
            confirmed.files.sort(Qt::CaseInsensitive);
            result.append(confirmed);
            if (stats != nullptr) {
                ++stats->finalGroups;
                stats->duplicateFiles += static_cast<int>(confirmed.files.size()) - 1;
                stats->duplicateBytes += confirmed.size * (confirmed.files.size() - 1);
            }
        }

        if (progress) {
            progress(QStringLiteral("全量校验"), i + 1, total);
        }
    }
    return result;
}

QVector<Group> findDuplicates(const ScanOptions &options,
                              int sampleBytes,
                              Cancel *cancel,
                              const Progress &progress,
                              Stats *stats,
                              QString *errorOut)
{
    Stats local;
    Stats *target = stats != nullptr ? stats : &local;

    QElapsedTimer timer;
    timer.start();

    const QVector<Entry> entries = scan(options, cancel, progress, target, errorOut);
    if (entries.isEmpty()) {
        target->elapsedMs += timer.elapsed();
        return {};
    }

    const QVector<QVector<Entry>> sizeGroups = groupBySize(entries, target);
    if (sizeGroups.isEmpty()) {
        target->elapsedMs += timer.elapsed();
        return {};
    }

    const QVector<QVector<Entry>> partial =
        filterBySampleHash(sizeGroups, sampleBytes, cancel, progress, target, errorOut);
    if (target->canceled || partial.isEmpty()) {
        target->elapsedMs += timer.elapsed();
        return {};
    }

    const QVector<Group> groups = confirmByFullHash(partial, cancel, progress, target, errorOut);
    target->elapsedMs += timer.elapsed();
    return groups;
}

// ============================================================================
//  结果加工
// ============================================================================

QStringList suggestDeletions(const QVector<Group> &groups)
{
    QStringList deletions;
    for (const Group &group : groups) {
        // 保留第一个（files 已按字典序排序 → 结果稳定可复现），其余全删
        for (int i = 1; i < group.files.size(); ++i) {
            deletions.append(group.files.at(i));
        }
    }
    return deletions;
}

QVector<Row> flatten(const QVector<Group> &groups)
{
    QVector<Row> rows;
    for (const Group &group : groups) {
        for (int i = 0; i < group.files.size(); ++i) {
            Row row;
            row.size = group.size;
            row.hash = group.hash;
            row.path = group.files.at(i);
            row.keep = (i == 0);
            rows.append(row);
        }
    }
    return rows;
}

QString hashFile(const QString &path, int sampleBytes)
{
    return hashFileInternal(path, sampleBytes);
}

QString formatBytes(qint64 bytes)
{
    constexpr double kKb = 1024.0;
    constexpr double kMb = kKb * 1024.0;
    constexpr double kGb = kMb * 1024.0;
    const double value = static_cast<double>(bytes);
    if (value < kKb) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (value < kMb) {
        return QStringLiteral("%1 KB").arg(value / kKb, 0, 'f', 1);
    }
    if (value < kGb) {
        return QStringLiteral("%1 MB").arg(value / kMb, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(value / kGb, 0, 'f', 2);
}

} // namespace WinEase::FeaturePlugins::DuplicateFinder
