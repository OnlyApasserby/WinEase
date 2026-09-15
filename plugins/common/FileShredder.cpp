#include "FileShredder.h"

// ============================================================================
//  FileShredder.cpp —— 覆写 / 校验 / 改名 / 删除的实现
//
//  三个"必须被在代码里说清楚"的点：
//
//   ① **写完之后一定要 FlushFileBuffers**：QFile::flush() 只把 CRT 缓冲区交给
//      操作系统，数据还在系统缓存里。如果紧接着删目录项、进程又立刻退出，
//      缓存里那份覆写数据可能还没落盘 —— "覆写了但没落盘"是最讽刺的一种失败。
//
//   ② **随机块必须写完立刻校验**：随机数据没法在第二遍循环里重新生成，
//      所以"写一块 → 马上回读比对"是这里唯一可行的校验方式（也顺带把随机
//      缓冲区的内存占用压到一块 buffer 的大小）。
//      诚实说明：回读命中的可能只是**系统缓存**，它证明的是"我们的写路径正确"，
//      不等于"磁性介质上的旧痕迹已消失"。
//
//   ③ **改名再删除**：删除目录项后，按原名扫描的恢复工具找不到入口。
//      NTFS 的 MFT 里仍可能留有旧记录，所以这是"提高门槛"，不是"抹除"。
// ============================================================================

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#ifdef Q_OS_WIN
#include <qt_windows.h>

#include <io.h> // _get_osfhandle：把 CRT 文件描述符换成 Win32 句柄
#endif

namespace WinEase::FeaturePlugins::FileShredder {
namespace {

QString folded(const QString &text)
{
    return text.toCaseFolded();
}

/// 归一化：统一分隔符、去掉结尾分隔符、折成绝对路径
QString normalize(const QString &path)
{
    QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    // 盘根（"C:/"）要保住那个斜杠，否则 "C:" 的语义变成"当前目录"
    if (cleaned.size() == 2 && cleaned.endsWith(QLatin1Char(':'))) {
        cleaned += QLatin1Char('/');
    }
    return cleaned;
}

void flushToDisk(QFile &file)
{
    file.flush();
#ifdef Q_OS_WIN
    const int descriptor = file.handle();
    if (descriptor < 0) {
        return;
    }
    const intptr_t osHandle = ::_get_osfhandle(descriptor);
    if (osHandle == -1) {
        return;
    }
    ::FlushFileBuffers(reinterpret_cast<HANDLE>(osHandle));
#endif
}

QByteArray randomBytes(int length)
{
    QByteArray buffer(length, '\0');
    auto *generator = QRandomGenerator::global();
    int index = 0;
    while (index + 4 <= length) {
        const quint32 value = generator->generate();
        buffer[index] = static_cast<char>(value & 0xFF);
        buffer[index + 1] = static_cast<char>((value >> 8) & 0xFF);
        buffer[index + 2] = static_cast<char>((value >> 16) & 0xFF);
        buffer[index + 3] = static_cast<char>((value >> 24) & 0xFF);
        index += 4;
    }
    while (index < length) {
        buffer[index] = static_cast<char>(generator->generate() & 0xFF);
        ++index;
    }
    return buffer;
}

/// 一遍覆写的数据特征
enum class Pattern { Zero, Ones, Random };

QVector<Pattern> patternSequence(PassMode mode)
{
    switch (mode) {
    case PassMode::ZeroSinglePass:
        return { Pattern::Zero };
    case PassMode::RandomSinglePass:
        return { Pattern::Random };
    case PassMode::RandomThreePass:
        return { Pattern::Random, Pattern::Random, Pattern::Random };
    case PassMode::DodThreePass:
        break;
    }
    // DoD 5220.22-M 的思路：全 0 → 全 1 → 随机
    return { Pattern::Zero, Pattern::Ones, Pattern::Random };
}

QByteArray makeChunk(Pattern pattern, int length)
{
    switch (pattern) {
    case Pattern::Zero:
        return QByteArray(length, '\0');
    case Pattern::Ones:
        return QByteArray(length, static_cast<char>(0xFF));
    case Pattern::Random:
        break;
    }
    return randomBytes(length);
}

QString patternText(Pattern pattern)
{
    switch (pattern) {
    case Pattern::Zero:
        return QStringLiteral("全 0（0x00）");
    case Pattern::Ones:
        return QStringLiteral("全 1（0xFF）");
    case Pattern::Random:
        break;
    }
    return QStringLiteral("随机字节");
}

/// 生成同目录下的随机名字（保留原主干长度，退化时至少 8 个字符）
QString randomNameFor(const QString &originalName)
{
    const int dot = originalName.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? originalName.left(dot) : originalName;
    const int length = qBound(8, stem.size(), 32);
    QString name;
    name.reserve(length);
    const QString alphabet = QStringLiteral("0123456789abcdef");
    for (int i = 0; i < length; ++i) {
        name += alphabet.at(static_cast<int>(QRandomGenerator::global()->bounded(alphabet.size())));
    }
    return name;
}

/// 系统目录 / 盘根 / 用户主目录本身：绝对不许粉碎
struct ProtectedRoot
{
    QString path;
    QString reason;
    bool subtree = true; ///< true = 连同子目录都保护；false = 只保护它自己
};

QVector<ProtectedRoot> protectedRoots()
{
    QVector<ProtectedRoot> roots;
    const auto add = [&roots](const QString &value, const QString &reason, bool subtree) {
        const QString cleaned = normalize(value);
        if (cleaned.isEmpty() || cleaned == QLatin1String("/")) {
            return;
        }
        roots.append(ProtectedRoot{ cleaned, reason, subtree });
    };

    const QString windows = qEnvironmentVariable("SystemRoot");
    if (!windows.isEmpty()) {
        add(windows, QStringLiteral("Windows 系统目录"), true);
    }
    const QString windir = qEnvironmentVariable("windir");
    if (!windir.isEmpty()) {
        add(windir, QStringLiteral("Windows 系统目录"), true);
    }
    add(qEnvironmentVariable("ProgramFiles"), QStringLiteral("程序安装目录"), true);
    add(qEnvironmentVariable("ProgramFiles(x86)"), QStringLiteral("程序安装目录"), true);
    add(qEnvironmentVariable("ProgramData"), QStringLiteral("程序数据目录"), true);
    add(qEnvironmentVariable("CommonProgramFiles"), QStringLiteral("共享程序目录"), true);

    // 用户目录本身、临时目录本身：删掉会让系统/程序立刻出问题
    add(qEnvironmentVariable("USERPROFILE"), QStringLiteral("用户主目录本身"), false);
    add(qEnvironmentVariable("APPDATA"), QStringLiteral("用户配置目录本身"), false);
    add(qEnvironmentVariable("LOCALAPPDATA"), QStringLiteral("用户本地数据目录本身"), false);
    add(QDir::tempPath(), QStringLiteral("临时目录本身"), false);

    return roots;
}

/// 收集目录下的所有文件（含子目录）
void collectFiles(const QString &directory, QVector<QString> *files, QVector<QString> *directories)
{
    QDir dir(directory);
    const QFileInfoList children =
        dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &info : children) {
        const QString path = info.absoluteFilePath();
        if (info.isSymLink()) {
            continue; // 不动符号链接指向的内容（只当普通项处理会被删掉链接本身，这里保守跳过）
        }
        if (info.isDir()) {
            directories->append(path);
            collectFiles(path, files, directories);
            continue;
        }
        if (info.isFile()) {
            files->append(path);
        }
    }
}

} // namespace

// ============================================================================
//  覆写方式
// ============================================================================

QString passModeKey(PassMode mode)
{
    switch (mode) {
    case PassMode::ZeroSinglePass:
        return QStringLiteral("zero1");
    case PassMode::RandomSinglePass:
        return QStringLiteral("random1");
    case PassMode::RandomThreePass:
        return QStringLiteral("random3");
    case PassMode::DodThreePass:
        break;
    }
    return QStringLiteral("dod3");
}

PassMode passModeFromKey(const QString &key, bool *ok)
{
    const QString normalized = key.trimmed().toLower();
    if (ok != nullptr) {
        *ok = true;
    }
    if (normalized == QLatin1String("zero1")) {
        return PassMode::ZeroSinglePass;
    }
    if (normalized == QLatin1String("random1")) {
        return PassMode::RandomSinglePass;
    }
    if (normalized == QLatin1String("random3")) {
        return PassMode::RandomThreePass;
    }
    if (normalized == QLatin1String("dod3") || normalized.isEmpty()) {
        return PassMode::DodThreePass;
    }
    if (ok != nullptr) {
        *ok = false;
    }
    return PassMode::DodThreePass;
}

QString passModeDisplayName(PassMode mode)
{
    switch (mode) {
    case PassMode::ZeroSinglePass:
        return QStringLiteral("覆写 1 遍：全 0（最快）");
    case PassMode::RandomSinglePass:
        return QStringLiteral("覆写 1 遍：随机");
    case PassMode::RandomThreePass:
        return QStringLiteral("覆写 3 遍：随机 ×3");
    case PassMode::DodThreePass:
        break;
    }
    return QStringLiteral("覆写 3 遍：全 0 → 全 1 → 随机（推荐）");
}

int passCount(PassMode mode)
{
    return patternSequence(mode).size();
}

QStringList passDescriptions(PassMode mode)
{
    QStringList descriptions;
    const QVector<Pattern> patterns = patternSequence(mode);
    for (int i = 0; i < patterns.size(); ++i) {
        descriptions.append(QStringLiteral("第 %1 遍：%2").arg(i + 1).arg(patternText(patterns.at(i))));
    }
    return descriptions;
}

// ============================================================================
//  Stats
// ============================================================================

QString Stats::summaryText() const
{
    QString text = QStringLiteral("粉碎 %1 个文件（覆写 %2 遍，写入 %3，回读校验 %4 遍通过）")
                       .arg(filesShredded)
                       .arg(passesPerformed)
                       .arg(bytesOverwritten < 1024 * 1024
                                ? QStringLiteral("%1 KB").arg(bytesOverwritten / 1024.0, 0, 'f', 1)
                                : QStringLiteral("%1 MB")
                                      .arg(bytesOverwritten / (1024.0 * 1024.0), 0, 'f', 1))
                       .arg(verifiedPasses);
    if (directoriesRemoved > 0) {
        text += QStringLiteral("，删除 %1 个空目录").arg(directoriesRemoved);
    }
    text += QStringLiteral("，耗时 %1 毫秒").arg(elapsedMs);
    if (!skipped.isEmpty()) {
        text += QStringLiteral("，%1 项被拒").arg(skipped.size());
    }
    if (!failures.isEmpty()) {
        text += QStringLiteral("，%1 项失败").arg(failures.size());
    }
    if (canceled) {
        text += QStringLiteral("（已中断）");
    }
    return text;
}

// ============================================================================
//  保护策略
// ============================================================================

bool isProtectedPath(const QString &path, QString *reason)
{
    const QString target = normalize(path);
    if (target.isEmpty()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("路径为空");
        }
        return true;
    }

    // 盘根（C:\ / D:\ …）：整盘粉碎没有意义，而且几乎总是误操作
    if ((target.size() <= 3 && target.endsWith(QLatin1Char('/')) && target.at(1) == QLatin1Char(':'))
        || QDir(target).isRoot()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("盘根目录");
        }
        return true;
    }

    const QVector<ProtectedRoot> roots = protectedRoots();
    for (const ProtectedRoot &entry : roots) {
        const QString protectedPath = folded(entry.path);
        const QString candidate = folded(target);
        if (candidate == protectedPath) {
            if (reason != nullptr) {
                *reason = entry.reason;
            }
            return true;
        }
        if (entry.subtree && candidate.startsWith(protectedPath + QLatin1Char('/'))) {
            if (reason != nullptr) {
                *reason = entry.reason;
            }
            return true;
        }
    }
    return false;
}

// ============================================================================
//  覆写 + 校验（自检直接调用它验证覆写实现本身）
// ============================================================================

bool overwriteAndVerify(const QString &path,
                        PassMode mode,
                        int bufferBytes,
                        bool verify,
                        Cancel *cancel,
                        qint64 *bytesWritten,
                        int *passesDone,
                        int *verifiedPasses,
                        QString *errorOut)
{
    const auto fail = [errorOut](const QString &text) {
        if (errorOut != nullptr) {
            *errorOut = text;
        }
        return false;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return fail(QStringLiteral("无法打开文件（%1）").arg(file.errorString()));
    }

    const qint64 size = file.size();
    if (size == 0) {
        // 空文件没有内容可覆写：重命名 + 删除就是全部处理
        return true;
    }

    const int chunk = qBound(4096, bufferBytes, 64 * 1024 * 1024);
    const QVector<Pattern> patterns = patternSequence(mode);

    for (const Pattern pattern : patterns) {
        if (cancel != nullptr && cancel->isRequested()) {
            return fail(QStringLiteral("已中断"));
        }
        qint64 offset = 0;
        while (offset < size) {
            const int length = static_cast<int>(qMin<qint64>(chunk, size - offset));
            const QByteArray data = makeChunk(pattern, length);

            if (!file.seek(offset)) {
                return fail(QStringLiteral("定位失败（%1）").arg(file.errorString()));
            }
            if (file.write(data) != length) {
                return fail(QStringLiteral("覆写失败（%1）").arg(file.errorString()));
            }
            if (bytesWritten != nullptr) {
                *bytesWritten += length;
            }

            // 随机块无法在别处重现，所以校验必须紧跟这次写入
            if (verify) {
                // ⚠ write() 之后文件位置停在 offset+length 上，必须显式 seek 回来，
                //   否则读的是"下一块"（拿新写的块去比对的下一块），校验必然失败
                if (!file.seek(offset)) {
                    return fail(QStringLiteral("回读定位失败（%1）").arg(file.errorString()));
                }
                const QByteArray readBack = file.read(length);
                if (readBack != data) {
                    return fail(QStringLiteral("回读校验不一致（偏移 %1）").arg(offset));
                }
                if (verifiedPasses != nullptr && offset + length >= size) {
                    // 只在整个文件都读回对得上时才算"这一遍校验通过"
                    ++(*verifiedPasses);
                }
            }

            offset += length;
            if (cancel != nullptr && cancel->isRequested()) {
                return fail(QStringLiteral("已中断"));
            }
        }
        // 一遍写完就落到磁盘（不留"覆写了但还在缓存里"的窗口）
        flushToDisk(file);
        if (passesDone != nullptr) {
            ++(*passesDone);
        }
    }

    return true;
}

// ============================================================================
//  粉碎
// ============================================================================

namespace {

bool shredSingleFile(const QString &path,
                     const Options &options,
                     Cancel *cancel,
                     Stats *stats,
                     QString *errorOut)
{
    const QFileInfo info(path);

    if (!overwriteAndVerify(path, options.mode, options.bufferBytes, options.verifyEachPass, cancel,
                            &stats->bytesOverwritten, &stats->passesPerformed, &stats->verifiedPasses,
                            errorOut)) {
        return false;
    }

    if (options.renameBeforeDelete) {
        const QString newName = randomNameFor(info.fileName());
        const QString newPath = QDir(info.absolutePath()).filePath(newName);
        if (QFile::rename(path, newPath)) {
            ++stats->renamedFiles;
            if (!QFile::remove(newPath)) {
                if (errorOut != nullptr) {
                    *errorOut = QStringLiteral("改名后删除失败（改名成了 %1）").arg(newName);
                }
                return false;
            }
            return true;
        }
        // 改名失败不是致命问题（可能被占用/权限）：继续按原名删除
        if (cancel != nullptr && cancel->isRequested()) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("已中断");
            }
            return false;
        }
    }

    if (!QFile::remove(path)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("删除失败（文件可能被别的程序占用）");
        }
        return false;
    }
    return true;
}

} // namespace

Result shredPath(const QString &path, const Options &options, Cancel *cancel, const Progress &progress)
{
    Result result;
    QElapsedTimer timer;
    timer.start();

    const QString target = normalize(path);
    QString protectedReason;
    if (isProtectedPath(target, &protectedReason)) {
        result.stats.skipped.append(QStringLiteral("%1（受保护：%2）").arg(target, protectedReason));
        result.error = QStringLiteral("拒绝粉碎受保护路径：%1（%2）").arg(target, protectedReason);
        result.stats.elapsedMs = timer.elapsed();
        return result;
    }

    const QFileInfo info(target);
    if (!info.exists()) {
        result.error = QStringLiteral("路径不存在：%1").arg(target);
        result.stats.elapsedMs = timer.elapsed();
        return result;
    }

    if (info.isDir()) {
        QVector<QString> files;
        QVector<QString> directories;
        collectFiles(target, &files, &directories);

        const int total = files.size();
        for (int i = 0; i < total; ++i) {
            if (cancel != nullptr && cancel->isRequested()) {
                result.stats.canceled = true;
                break;
            }
            QString failure;
            if (shredSingleFile(files.at(i), options, cancel, &result.stats, &failure)) {
                ++result.stats.filesShredded;
            } else {
                ++result.stats.failedCount;
                result.stats.failures.append(QStringLiteral("%1（%2）").arg(files.at(i), failure));
                if (cancel != nullptr && cancel->isRequested()) {
                    result.stats.canceled = true;
                    break;
                }
            }
            if (progress) {
                progress(QStringLiteral("粉碎文件"), i + 1, total);
            }
        }

        // 自底向上删空目录（子目录先删，父目录才可能变空）
        for (int i = directories.size() - 1; i >= 0; --i) {
            if (QDir(directories.at(i)).rmdir(directories.at(i))) {
                ++result.stats.directoriesRemoved;
            }
        }
        if (QDir(target).rmdir(target)) {
            ++result.stats.directoriesRemoved;
        }
    } else if (info.isFile()) {
        if (progress) {
            progress(QStringLiteral("粉碎文件"), 0, 1);
        }
        QString failure;
        if (shredSingleFile(target, options, cancel, &result.stats, &failure)) {
            result.stats.filesShredded = 1;
        } else {
            result.stats.failedCount = 1;
            result.stats.failures.append(QStringLiteral("%1（%2）").arg(target, failure));
            if (cancel != nullptr && cancel->isRequested()) {
                result.stats.canceled = true;
            }
        }
        if (progress) {
            progress(QStringLiteral("粉碎文件"), 1, 1);
        }
    } else {
        result.error = QStringLiteral("不支持的路径类型（既不是文件也不是目录）：%1").arg(target);
        result.stats.elapsedMs = timer.elapsed();
        return result;
    }

    result.stats.elapsedMs = timer.elapsed();
    if (result.stats.canceled) {
        // 中断要如实说清"已完成的部分不可回滚"
        result.error = QStringLiteral("已中断：已粉碎的文件无法恢复，未处理的部分保持原样");
        result.ok = false;
        return result;
    }
    result.ok = result.stats.failures.isEmpty();
    if (!result.ok) {
        result.error = QStringLiteral("%1 项粉碎失败").arg(result.stats.failures.size());
    }
    return result;
}

Result shredPaths(const QStringList &paths,
                  const Options &options,
                  Cancel *cancel,
                  const Progress &progress)
{
    Result total;
    QElapsedTimer timer;
    timer.start();

    const int count = paths.size();
    for (int i = 0; i < count; ++i) {
        if (cancel != nullptr && cancel->isRequested()) {
            total.stats.canceled = true;
            break;
        }
        const Result one = shredPath(paths.at(i), options, cancel, progress);
        total.stats.filesShredded += one.stats.filesShredded;
        total.stats.directoriesRemoved += one.stats.directoriesRemoved;
        total.stats.bytesOverwritten += one.stats.bytesOverwritten;
        total.stats.passesPerformed += one.stats.passesPerformed;
        total.stats.verifiedPasses += one.stats.verifiedPasses;
        total.stats.renamedFiles += one.stats.renamedFiles;
        total.stats.failedCount += one.stats.failedCount;
        total.stats.failures += one.stats.failures;
        total.stats.skipped += one.stats.skipped;
        if (!one.ok && !one.error.isEmpty() && one.stats.failures.isEmpty()
            && one.stats.skipped.isEmpty()) {
            total.stats.failedCount += 1;
            total.stats.failures.append(QStringLiteral("%1（%2）").arg(paths.at(i), one.error));
        }
    }

    total.stats.elapsedMs = timer.elapsed();
    if (total.stats.canceled) {
        total.error = QStringLiteral("已中断：已粉碎的文件无法恢复，未处理的部分保持原样");
        return total;
    }
    total.ok = total.stats.failures.isEmpty();
    if (!total.ok) {
        total.error = QStringLiteral("%1 项粉碎失败").arg(total.stats.failures.size());
    }
    return total;
}

} // namespace WinEase::FeaturePlugins::FileShredder
