#pragma once

// ============================================================================
//  TaskCancel.h —— 长任务的"取消标志 + 进度回调"（P2 多个引擎共用）
//
//  为什么单独抽一个头文件：批量重命名 / 重复文件查找 / 粉碎都要"能中断 + 报进度"，
//  三个引擎各写一份 Cancel 会导致插件侧要对着三套类型写代码。
//
//  ⚠ 线程约定：进度回调**可能在工作线程上被调用**（哈希是并行跑的）。
//    插件侧必须排队转到界面线程再碰 QWidget。
// ============================================================================

#include <QString>

#include <atomic>
#include <functional>

namespace WinEase::FeaturePlugins {

/// 协作式取消：调用方 request()，任务在自己的检查点退出
class Cancel
{
public:
    void request() { m_flag.store(true); }
    bool isRequested() const { return m_flag.load(); }
    void reset() { m_flag.store(false); }

private:
    std::atomic_bool m_flag{ false };
};

/// (阶段名, 已完成, 总数)；total <= 0 表示"总数未知"
using Progress = std::function<void(const QString &stage, int done, int total)>;

} // namespace WinEase::FeaturePlugins
