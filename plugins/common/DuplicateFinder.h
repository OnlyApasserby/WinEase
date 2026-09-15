#pragma once

// ============================================================================
//  DuplicateFinder.h —— 重复文件查找的四级流水线（P2-03 共用）
//
//  为什么要分四级（也是本功能唯一真正的性能点）：
//      ① 递归枚举：只拿"路径 + 大小"（不读内容）
//      ② 按大小分组：大小不同的文件**一定**不是重复文件 → 一刀砍掉绝大多数
//      ③ 抽样哈希：只读每个文件的前 4KB（默认）→ 再把"头部相同"的组留下
//      ④ 全量 SHA-256：只对第 ③ 步剩下的少数文件做，确认"内容完全一致"
//  自检会断言 `stats.partialHashBytes` 远小于 `stats.totalBytes` ——
//  否则"抽样"就是个摆设（等于全量读盘）。
//
//  ⚠ 本头文件不含 Q_OBJECT（与 TextTools.h 同理）。
//
//  ⚠ 线程约定：扫描与哈希会用**内部线程池**并行，`Progress` 回调**可能在
//    工作线程上被调用**。插件侧必须把它转成排队信号再更新界面，不得直接操作 QWidget。
//
//  ⚠ 删除策略：本模块**只负责算**，不负责删。删除一律由调用方交给
//    `ShellUtils::moveToRecycleBin()`（产品硬约束：重复文件绝不静默硬删）。
// ============================================================================

#include "TaskCancel.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace WinEase::FeaturePlugins::DuplicateFinder {

// ============================================================================
//  数据类型
// ============================================================================

struct Entry
{
    QString path;
    qint64 size = 0;
};

struct ScanOptions
{
    QStringList roots;
    /// 小于该大小的文件不参与（0 字节文件默认被这条挡掉）
    qint64 minSizeBytes = 4096;
    bool includeHidden = false;
    /// 安全上限：超过就停止枚举并如实报告（防止把整个盘扫穿）
    int maxFiles = 200000;
};

/// 各级的中间统计：既是给界面看的诊断，也是自检断言的对象
struct Stats
{
    int scannedFiles = 0;   ///< 枚举到的候选文件数
    int skippedSmall = 0;   ///< 因小于 minSizeBytes 被跳过
    int skippedHidden = 0;  ///< 因隐藏被跳过
    int skippedReparse = 0; ///< 因是符号链接/联接点被跳过（防绕圈）
    qint64 totalBytes = 0;  ///< 候选文件总大小

    int sizeGroups = 0;     ///< ②之后剩下的"候选组"数
    int candidateFiles = 0; ///< ②之后剩下的文件数

    int partialGroups = 0;    ///< ③之后剩下的组数
    int partialHashBytes = 0; ///< ③实际读取的字节数（"抽样"的证明）

    int finalGroups = 0;     ///< ④确认的重复组数
    int duplicateFiles = 0;  ///< 重复文件总数（不含每组保留的那一份）
    qint64 duplicateBytes = 0; ///< 可回收空间

    qint64 elapsedMs = 0;
    bool canceled = false;
    bool truncated = false; ///< 因 maxFiles 上限提前停止

    QString stageText() const;
};

struct Group
{
    qint64 size = 0;
    QString hash;      ///< 全量 SHA-256（小写十六进制）
    QStringList files; ///< 内容完全相同的文件（按字典序，第一个为"保留项"）
};

/// 扁平行：界面表格直接吃这个
struct Row
{
    qint64 size = 0;
    QString hash;
    QString path;
    bool keep = false; ///< 是否建议保留
};

/// 进度回调；(stage, done, total)；total 为 0 表示"总数未知"
///
/// ⚠ 取消标志与进度回调类型来自 `plugins/common/TaskCancel.h`
///   （`WinEase::FeaturePlugins::Cancel` / `Progress`，与粉碎引擎共用一套）。

// ============================================================================
//  四个阶段（各自可单独调用：自检要逐级断言）
// ============================================================================

QVector<Entry> scan(const ScanOptions &options,
                    Cancel *cancel,
                    const Progress &progress,
                    Stats *stats,
                    QString *errorOut);

QVector<QVector<Entry>> groupBySize(const QVector<Entry> &entries, Stats *stats);

QVector<QVector<Entry>> filterBySampleHash(const QVector<QVector<Entry>> &groups,
                                           int sampleBytes,
                                           Cancel *cancel,
                                           const Progress &progress,
                                           Stats *stats,
                                           QString *errorOut);

QVector<Group> confirmByFullHash(const QVector<QVector<Entry>> &groups,
                                 Cancel *cancel,
                                 const Progress &progress,
                                 Stats *stats,
                                 QString *errorOut);

/// 流水线：①→②→③→④ 一把跑完（插件用；自检也可以先逐级跑再比对）
QVector<Group> findDuplicates(const ScanOptions &options,
                              int sampleBytes,
                              Cancel *cancel,
                              const Progress &progress,
                              Stats *stats,
                              QString *errorOut);

// ============================================================================
//  结果加工
// ============================================================================

/// 建议删除项：每组保留第一个（字典序最小的那个），其余为建议删除
QStringList suggestDeletions(const QVector<Group> &groups);

/// 展开成表格行
QVector<Row> flatten(const QVector<Group> &groups);

/// 单个文件的 SHA-256（十六进制小写）；失败返回空串
QString hashFile(const QString &path, int sampleBytes = -1);

/// 把字节数变成"1.2 MB"这类可读文本
QString formatBytes(qint64 bytes);

} // namespace WinEase::FeaturePlugins::DuplicateFinder
