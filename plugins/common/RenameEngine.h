#pragma once

// ============================================================================
//  RenameEngine.h —— 批量重命名规则引擎（P2-01 共用）
//
//  为什么把规则计算从插件里拆出来：
//    批量重命名最容易出的事故是"把用户 1000 个文件改成不认识的样子"，
//    所以产品上必须做到**先预览、可撤销**。而"预览出来的名字"与"真正落盘的名字"
//    必须是同一份计算结果 —— 引擎是纯函数，插件只负责把结果画到表格上、
//    用户点"应用"时再交给 apply()。自检也因此能脱离界面直接跑：
//      ① 造 1000 个文件 → ② buildItems/plan 出预期 → ③ apply → ④ 回读磁盘比对
//      → ⑤ undo → ⑥ 断言逐字节回到原样
//
//  ⚠ 本头文件不含 Q_OBJECT（与 TextTools.h / WindowFeatureState.h 同理）。
//
//  ---------------------------------------------------------------------------
//  规则的处理顺序（顺序即语义，写死在这里以免"预览与落盘不一致"）：
//      1. 主干 = 原文件名去掉扩展名
//      2. 查找替换：正则作用于**主干**（有意不含扩展名，扩展名交给扩展名规则）
//         正则不匹配 → 该项标记为"不参与重命名"（不是把整批拒掉）
//      3. 模板展开：空模板 = 沿用第 2 步的结果；非空时按占位符生成
//         {name}=第 2 步结果 {ext}=扩展名 {n}=序号 {1}..{9}=正则捕获组
//         {date}/{time}/{datetime}=该项的时间戳（yyy-MM-dd / HHmmss / yyyy-MM-dd_HHmmss）
//         时间戳由调用方给出（照片可给 EXIF 拍摄时间，其余给文件修改时间）；
//         缺失时展开成空串，而不是偷偷用"今天"——批量改名最怕这种静默兜底
//      4. 大小写转换（只作用于主干，**不含用户手打的 prefix/suffix**）
//      5. 加前缀 / 后缀
//      6. 扩展名规则（保持 / 转小写 / 转大写 / 换成新扩展名 / 去掉）
//      7. 合法性校验
//  序号按"该项在列表中的下标"计算（start + step * index），与该项是否有效无关 ——
//  这样"第 5 个文件永远是 5 号"，用户改一条规则时其它行的编号不会整体漂移。
// ============================================================================

#include <QDateTime>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace WinEase::FeaturePlugins::RenameEngine {

// ============================================================================
//  规则
// ============================================================================

enum class CaseMode {
    Keep = 0,   ///< 保持原样
    Lower,      ///< 全小写
    Upper,      ///< 全大写
    Capitalize, ///< 首字母大写
    Title       ///< 每个单词首字母大写
};

enum class ExtensionMode {
    Keep = 0, ///< 保持
    LowerCase,///< 转小写
    UpperCase,///< 转大写
    Replace,  ///< 换成 newExtension
    Remove    ///< 去掉扩展名
};

QString caseModeKey(CaseMode mode);
CaseMode caseModeFromKey(const QString &key, bool *ok = nullptr);
QString caseModeDisplayName(CaseMode mode);

QString extensionModeKey(ExtensionMode mode);
ExtensionMode extensionModeFromKey(const QString &key, bool *ok = nullptr);
QString extensionModeDisplayName(ExtensionMode mode);

struct Options
{
    QString findPattern;   ///< 正则；空 = 不做查找替换
    QString replaceText;   ///< 替换文本，支持 \1..\9 反向引用
    QString nameTemplate;  ///< 空 = 沿用查找替换后的原名
    int startNumber = 1;   ///< 序号起始
    int numberStep = 1;    ///< 序号步长
    int numberPadding = 3; ///< 序号补零位数（0 = 不补零）
    QString prefix;
    QString suffix;
    CaseMode caseMode = CaseMode::Keep;
    ExtensionMode extensionMode = ExtensionMode::Keep;
    QString newExtension; ///< ExtensionMode::Replace 时使用（可带点，内部会归一化）

    bool hasTemplating() const
    {
        return !nameTemplate.isEmpty() || !prefix.isEmpty() || !suffix.isEmpty();
    }
};

// ============================================================================
//  预览项
// ============================================================================

struct Item
{
    QString originalName; ///< 原文件名（不含路径）
    QString newName;      ///< 计划的新文件名（problem 非空时可能等于原名）
    QString problem;      ///< 非空 = 该项不参与重命名，内容是中文原因

    bool valid() const { return problem.isEmpty(); }
    bool changed() const { return valid() && newName != originalName; }
    /// 仅大小写变化（Windows 下需要两阶段改名，见 apply()）
    bool caseOnlyChange() const;
};

struct Plan
{
    QVector<Item> items;
    QStringList conflicts; ///< 目标名冲突（组内重复 / 磁盘上已有别的文件）

    bool ok() const;
    int changedCount() const;
    int problemCount() const;
    QString summaryText() const; ///< "将重命名 8 项，2 项有问题，1 处冲突"
};

/// 逐项计算新名（不做冲突检测）。errorOut 只在规则本身非法时非空（例如正则写错）。
/// @param timestamps 与 names **同序**的时间戳（可短于 names / 为空：缺失项按"没有时间"处理），
///        只在模板用到 {date}/{time}/{datetime} 时才需要
QVector<Item> buildItems(const QStringList &names,
                         const QVector<QDateTime> &timestamps,
                         const Options &options,
                         QString *errorOut = nullptr);

/// 生成完整计划：@param names 要处理的文件名，@param allNamesInDir 目录里**全部**文件名
/// （含不参与本次重命名的），用于检测"目标名撞到别的文件"。
Plan plan(const QStringList &names,
          const QStringList &allNamesInDir,
          const QVector<QDateTime> &timestamps,
          const Options &options,
          QString *errorOut = nullptr);

// ============================================================================
//  执行与撤销
// ============================================================================

struct ApplyResult
{
    bool ok = false;
    /// 实际完成的映射（old → new），按执行顺序；撤销就是拿它反着来。
    /// 部分失败时也只包含**确实改成功**的那部分。
    QVector<QPair<QString, QString>> applied;
    QStringList failed; ///< 失败项："a.txt → b.txt（原因）"
    QString error;

    QString summaryText() const;
};

/// 落盘执行。目录不存在 / 计划有冲突时直接拒绝（返回 ok=false）。
ApplyResult apply(const QString &dirPath, const Plan &plan);

/// 撤销：把 applied 里的映射反向执行一遍。
/// 反向执行时同样会遇到"目标名被本批其它文件占着"（a→b, b→a），
/// 因此复用与 apply 完全相同的两阶段算法。
ApplyResult undo(const QString &dirPath, const QVector<QPair<QString, QString>> &applied);

// ============================================================================
//  文件名校验（Windows 规则）
// ============================================================================

/// 文件名是否合法；不合法时 reason 给出中文原因
bool isLegalFileName(const QString &name, QString *reason = nullptr);

} // namespace WinEase::FeaturePlugins::RenameEngine
