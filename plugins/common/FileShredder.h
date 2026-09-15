#pragma once

// ============================================================================
//  FileShredder.h —— 敏感文件粉碎：多遍覆写 → 改名 → 永久删除（P2-12 共用）
//
//  ★ 先把话说清楚（这段也必须原样出现在插件的界面上，不能只写在代码里）：
//    粉碎能防的是"**用恢复软件按常规手段**扫出旧数据"，防不了：
//      · SSD 的磨损均衡 / TRIM：写下去的覆写块可能被映射到别处，
//        原物理块由固件回收 —— 这是**硬件层面**的事，用户态软件无法保证；
//      · 快照 / 卷影副本（VSS）、文件历史、云同步的其它副本；
//      · 已经产生的碎片备份、其它机器上的副本。
//    所以"粉碎"是**提高门槛**，不是"物理保证"。SSD 上的正确做法是
//    全盘加密 + 丢弃密钥（本工具的说明里直接这么告诉用户）。
//
//  另一个必须写明的语义：**已粉碎的文件无法恢复，且本工具不提供撤销**
//  （与"批量重命名"刻意相反 —— 那个有完整撤销）。
//
//  ⚠ 本头文件不含 Q_OBJECT。
// ============================================================================

#include "TaskCancel.h"

#include <QString>
#include <QStringList>

namespace WinEase::FeaturePlugins::FileShredder {

// ============================================================================
//  覆写方式
// ============================================================================

enum class PassMode {
    ZeroSinglePass = 0, ///< 覆写 1 遍 0x00（最快，防常规恢复）
    RandomSinglePass,   ///< 覆写 1 遍随机字节
    RandomThreePass,    ///< 覆写 3 遍随机字节
    DodThreePass        ///< 覆写 3 遍：0x00 → 0xFF → 随机（对应 DoD 5220.22-M 的思路）
};

QString passModeKey(PassMode mode);
PassMode passModeFromKey(const QString &key, bool *ok = nullptr);
QString passModeDisplayName(PassMode mode);
int passCount(PassMode mode);
/// 每一遍的数据特征，供界面展示："第 1 遍：全 0（0x00）"
QStringList passDescriptions(PassMode mode);

// ============================================================================
//  参数与报告
// ============================================================================

struct Options
{
    PassMode mode = PassMode::DodThreePass;
    /// 覆写完先改名再删（让"按原文件名恢复目录项"也失效）
    bool renameBeforeDelete = true;
    /// 每遍覆写后回读校验（慢一倍，但能证明"确实写进去了"；自检正是靠它做断言）
    bool verifyEachPass = true;
    /// 覆写缓冲区大小
    int bufferBytes = 1 << 20;
};

struct Stats
{
    int filesShredded = 0;      ///< 成功粉碎的文件数
    int directoriesRemoved = 0; ///< 删除的空目录数
    int failedCount = 0;        ///< 失败项数
    qint64 bytesOverwritten = 0;///< 累计写入字节数（= 文件大小 × 遍数，含校验读不算）
    int passesPerformed = 0;    ///< 实际执行的覆写遍数
    int verifiedPasses = 0;     ///< 回读校验通过的遍数
    int renamedFiles = 0;       ///< 成功改名的文件数
    qint64 elapsedMs = 0;
    bool canceled = false;
    QStringList failures; ///< "path（原因）"
    QStringList skipped;  ///< "path（被保护策略拒绝：原因）"

    QString summaryText() const;
};

struct Result
{
    bool ok = false;
    Stats stats;
    QString error;

    QString summaryText() const { return error.isEmpty() ? stats.summaryText() : error; }
};

// ============================================================================
//  接口
// ============================================================================

/// 粉碎一个路径：文件 → 多遍覆写 + 改名 + 永久删除；目录 → 递归粉碎其中所有文件后删除空目录。
/// 被保护的路径（系统目录/盘根）会被拒绝并记进 stats.skipped，不影响其它项。
Result shredPath(const QString &path, const Options &options, Cancel *cancel, const Progress &progress);

/// 批量（按顺序逐项执行；单项失败不中断后面的）
Result shredPaths(const QStringList &paths,
                  const Options &options,
                  Cancel *cancel,
                  const Progress &progress);

/// 这段路径是否属于"绝对不许粉碎"的范围（系统目录、盘根）。
/// 注意这是**产品安全阀**，不是权限检查：真正的权限由文件系统说话。
bool isProtectedPath(const QString &path, QString *reason = nullptr);

/// 覆盖写入（+ 可选回读校验）一个文件；自检直接用它验证"覆写实现本身"是否正确
bool overwriteAndVerify(const QString &path,
                        PassMode mode,
                        int bufferBytes,
                        bool verify,
                        Cancel *cancel,
                        qint64 *bytesWritten,
                        int *passesDone,
                        int *verifiedPasses,
                        QString *errorOut);

} // namespace WinEase::FeaturePlugins::FileShredder
