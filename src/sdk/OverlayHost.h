#pragma once

// ============================================================================
//  OverlayHost.h —— 悬浮层宿主（OverlayKit 的生命周期管理器）
//
//  为什么必须有一个宿主统一管理悬浮层（而不是让插件各自 new/delete）：
//    1. 悬浮层是**置顶透明窗口**，忘记销毁会一直挡在用户桌面上；
//       插件崩溃、被停用、被卸载时若没人兜底回收，就是"残影"与泄漏
//    2. 跨 DPI 多屏下必须"每显示器一个悬浮层"，这个分配策略应由宿主统一提供，
//       否则每个插件都会各写一遍且多半写错
//    3. 插件 DLL 与主程序各有一份 Qt 全局状态，
//       窗口的创建/销毁必须集中在主程序侧，才谈得上"可完整卸载"
//
//  ---------------------------------------------------------------------------
//  为什么本文件在 **sdk/** 而不是 app/core/（P1-C 修正）
//
//    OverlayHost 原本放在主程序内部，测试自检也把它编进自己的目标；
//    但 P1-C 是**第一个真正用 OverlayKit 的插件**，立刻就撞上了结构性问题：
//    插件 DLL 需要调用宿主的方法（createOverlaysForAllMonitors / 回收…），
//    而主程序的符号对 DLL 不可见（MSVC 下 .exe 默认不导出符号），
//    插件若各自编一份 OverlayHost.cpp，就会变成"每个插件一份实现"——
//    这正是本工程反复强调要避免的多份状态。
//
//    → 结论：OverlayHost 是 OverlayKit 生命周期契约的一部分，放进 SDK，
//      随 WinEaseSdk 静态链接进主程序与所有插件（与 OverlayWindow 一致），
//      **实现只有一份**。它自身没有全局状态，实例仍由主程序持有
//      （`PluginServices::overlayHost()` 注入，插件不得接管其生命周期）。
//  ---------------------------------------------------------------------------
//
//  归属（ownerId）约定：一律用插件 id()。插件停用/卸载/崩溃时宿主按 ownerId 批量回收。
//
//  用法（插件侧，两条路都行）：
//      ① 宿主代建（普通悬浮层）：
//          OverlayHost *host = services()->overlayHost();
//          OverlayWindow *overlay = host->createOverlay(id());   // 所有权在宿主
//          overlay->coverMonitor(0);
//          overlay->show();
//      ② 插件自带子类（要重写 paintOverlay() 时，P1-07/P1-08 就是这条）：
//          auto *overlay = new MyRulerOverlay();                // 自己 new
//          host->adoptOverlay(id(), overlay);                   // 交给宿主托管
//          overlay->coverMonitor(0);
//          overlay->show();
//      // 停用时（onDisable）：host->closeOverlaysOfOwner(id());
// ============================================================================

#include "sdk/OverlayWindow.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

namespace WinEase {

class OverlayHost : public QObject
{
    Q_OBJECT

public:
    explicit OverlayHost(QObject *parent = nullptr);
    ~OverlayHost() override;

    OverlayHost(const OverlayHost &) = delete;
    OverlayHost &operator=(const OverlayHost &) = delete;

    // ---------------- 创建 ----------------

    /// 创建一个归属 ownerId 的悬浮层。
    /// @return 悬浮层指针；**所有权归 OverlayHost**，调用方不得 delete，
    ///         回收请用 closeOverlay() / closeOverlaysOfOwner()
    OverlayWindow *createOverlay(const QString &ownerId);

    /// 为 ownerId 在**每一个显示器**各创建一个铺满该显示器的悬浮层。
    /// 跨不同 DPI 显示器时这是唯一能保证处处原生清晰的方案
    /// （原因见 OverlayWindow.h 的"跨 DPI 的硬限制"）
    QList<OverlayWindow *> createOverlaysForAllMonitors(const QString &ownerId);

    /// 接管一个**由调用方创建**的悬浮层（用于插件自己的 OverlayWindow 子类）。
    /// 接管后所有权移交宿主：插件不得再 delete，回收走 closeOverlay()/
    /// closeOverlaysOfOwner()，插件停用/卸载/崩溃时由主程序兜底回收。
    /// 重复接管同一实例时只更新归属者，不会重复登记。
    /// @return 传入的 overlay（便于链式写法）；overlay 为 nullptr 时返回 nullptr
    OverlayWindow *adoptOverlay(const QString &ownerId, OverlayWindow *overlay);

    // ---------------- 回收 ----------------

    /// 关闭并销毁单个悬浮层
    void closeOverlay(OverlayWindow *overlay);

    /// 关闭并销毁某归属者的全部悬浮层，返回关闭数量。
    /// 插件停用、卸载、崩溃后由主程序兜底调用
    int closeOverlaysOfOwner(const QString &ownerId);

    /// 关闭全部悬浮层，返回关闭数量（程序退出时调用）
    int closeAll();

    // ---------------- 查询 ----------------

    int overlayCount() const;
    QList<OverlayWindow *> overlays() const;
    QList<OverlayWindow *> overlaysOfOwner(const QString &ownerId) const;

Q_SIGNALS:
    void overlayCreated(WinEase::OverlayWindow *overlay, const QString &ownerId);
    void overlayClosed(const QString &ownerId);
    void overlayCountChanged(int count);

private:
    struct Entry {
        QPointer<OverlayWindow> overlay;
        QString ownerId;
    };

    /// 把 overlay 登记到 ownerId 名下（登记表 + destroyed 兜底 + 两个信号）
    void registerEntry(const QString &ownerId, OverlayWindow *overlay);
    /// 从登记表移除，返回其归属者（未找到返回空串）
    QString takeEntry(OverlayWindow *overlay);
    int takeEntriesOfOwner(const QString &ownerId);

    QList<Entry> m_entries;
};

} // namespace WinEase
