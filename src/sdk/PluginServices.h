#pragma once

// ============================================================================
//  PluginServices.h —— 宿主服务接口（插件反向调用主程序的唯一通道）
//
//  设计原则：
//    插件动态库不允许直接依赖主程序的单例（否则会出现两份全局状态），
//    所有需要主程序配合的能力（配置、日志、通知、全局快捷键）都通过
//    本接口由主程序注入。
//
//  获取方式：
//      PluginServices *services = this->services();   // IFeaturePlugin 提供
// ============================================================================

#include <QKeySequence>
#include <QString>
#include <QVariant>

class QWidget;

namespace WinEase {

class ElevationService;
class HookService;
class OverlayHost;

/// 插件日志级别（内部会映射到 QLoggingCategory）
enum class PluginLogLevel {
    Debug = 0,
    Info,
    Warning,
    Critical
};

class PluginServices
{
public:
    PluginServices() = default;
    virtual ~PluginServices() = default;

    PluginServices(const PluginServices &) = delete;
    PluginServices &operator=(const PluginServices &) = delete;

    // ---------------- 配置持久化（自动归入 [Plugins/<pluginId>] 段）--------
    virtual QVariant configValue(const QString &pluginId,
                                 const QString &key,
                                 const QVariant &defaultValue = QVariant()) const = 0;
    virtual void setConfigValue(const QString &pluginId, const QString &key, const QVariant &value) = 0;
    virtual void syncConfig() = 0;

    // ---------------- 日志 ----------------
    virtual void log(const QString &pluginId, PluginLogLevel level, const QString &message) = 0;

    // ---------------- 系统通知（气泡 / 托盘）----------------
    virtual void notify(const QString &title, const QString &body) = 0;

    // ---------------- 托盘状态徽标 ----------------
    /// 在宿主托盘图标上挂一个"状态徽标"（典型用途：麦克风已静音）。
    ///
    /// 为什么要有这条通道：像"一键闭麦"这种**全局开关**，用户按完快捷键需要一个
    /// **常驻可见**的状态反馈 —— 气泡通知会自己消失，而"我现在到底是不是静音"这件事
    /// 必须一眼可见。但插件**不应该**各自建托盘图标：一个插件一个图标会把托盘撑爆，
    /// 而且插件卸载/崩溃后那个图标可能永远留在那里（没人回收）。
    ///
    /// @param text     徽标文字（1~3 个字，如「静」）；**空字符串表示清除徽标**
    /// @param colorHex 徽标底色，如 "#E5484D"；空则用默认警示色
    /// @param tooltip  托盘悬停提示的附加说明（可为空）
    ///
    /// 绘制与回收都由主程序负责：插件停用/卸载/崩溃隔离时主程序按 pluginId 清除徽标，
    /// 插件**不需要**（也做不到）自己回收。
    virtual void setTrayBadge(const QString &pluginId,
                              const QString &text,
                              const QString &colorHex,
                              const QString &tooltip) = 0;

    // ---------------- 全局快捷键 ----------------
    /// action 为插件内动作名，最终键为 "<pluginId>::<action>"
    virtual bool registerHotkey(const QString &pluginId,
                                const QString &action,
                                const QKeySequence &sequence,
                                const QString &description) = 0;
    virtual void unregisterHotkey(const QString &pluginId, const QString &action) = 0;

    // ---------------- 全局输入钩子（P0-2）----------------
    /// 全局键盘/鼠标钩子服务；由主程序统一安装，插件只订阅。
    /// 返回的指针由主程序持有，插件**不得**接管其生命周期。
    /// 正常运行时不会为 nullptr；但仍建议判空（例如主程序以安全模式启动时）。
    virtual HookService *hookService() const = 0;

    // ---------------- 悬浮层宿主（P0-3 OverlayKit）----------------
    /// 悬浮层（OverlayKit）的统一创建/回收入口；由主程序持有。
    /// 通过它创建的悬浮层所有权归宿主，插件**不得** delete；
    /// 插件停用或卸载时主程序会按 ownerId（即插件 id）兜底回收，避免残留置顶窗口。
    virtual OverlayHost *overlayHost() const = 0;

    // ---------------- 提权操作通道（P0-4）----------------
    /// 白名单提权操作（写 hosts、结束系统进程、改 HKLM 环境变量、电源操作…）
    /// 的统一入口；由主程序经 WinEaseHelper.exe 执行，白名单与调用方校验都在
    /// helper 端完成。指针由主程序持有，插件**不得**接管其生命周期。
    virtual ElevationService *elevationService() const = 0;

    // ---------------- 环境信息 ----------------
    virtual QString appVersion() const = 0;
    virtual bool isElevated() const = 0;
    virtual QString configFilePath() const = 0;
    virtual QString logDirectory() const = 0;
    /// 主窗口指针（可能为 nullptr，插件不得接管其生命周期）
    virtual QWidget *mainWindow() const = 0;
};

} // namespace WinEase

Q_DECLARE_METATYPE(WinEase::PluginLogLevel)
