#pragma once

// ============================================================================
//  DesktopWallpaper.h —— 桌面壁纸读写
//
//  用途（对应路线图 P1-14）：
//      * 壁纸自动切换（换图、历史回滚、停用还原）
//      * 按显示器分别设置壁纸（多屏用户各屏一张）
//
//  两条实现路径（为什么两条都要）：
//      ① `IDesktopWallpaper`（公开 COM 接口，Win8+）：支持**逐显示器**读写、
//         能拿到真实路径（幻灯片/纯色时为特殊值）、能返回当前显示方式。
//         需要 COM 已初始化（主程序与插件宿主都已初始化 COM 套间）。
//      ② `SystemParametersInfo(SPI_SETDESKWALLPAPER)`：无需 COM 的老接口，
//         在 COM 不可用时兜底（例如安全模式或 COM 初始化失败）。
//         **只支持"所有显示器同一张"**，且取回路径对幻灯片场景不可靠。
//
//  ⚠ 写壁纸属于"改用户系统状态"：调用方必须**先保存原值**，
//    并在功能停用/卸载时还原（本模块只负责读写，不负责记账）。
//
//  无状态：纯函数（见 WinEaseWin32 设计原则 2）。
// ============================================================================

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 一台显示器的壁纸信息
struct WallpaperInfo {
    QString monitorId;    ///< IDesktopWallpaper 的显示器设备路径；SPI 兜底时为空
    QString monitorName;  ///< 展示用名字，如 "显示器 1"
    QRect monitorRect;    ///< 该显示器矩形（物理像素）
    QString path;         ///< 当前壁纸文件路径；幻灯片/纯色时可能为空
    QString position;     ///< fill / fit / stretch / tile / center / span；未知为空
    bool valid = false;
};

/// `IDesktopWallpaper` 是否可用（可用时才有逐显示器能力）。
/// 注意：本函数会尝试初始化 COM，若调用线程尚未初始化 COM 会返回 false
bool isDesktopWallpaperAvailable();

/// 逐显示器壁纸（每台一条，顺序与 Win32::monitors() 一致）。
/// COM 不可用时退化为一条（SPI 取回的全局壁纸）
QList<WallpaperInfo> wallpapers();

/// 主显示器的壁纸路径（取不到返回空串）
QString currentWallpaper();

/// 给**所有**显示器设置同一张壁纸
/// @param imagePath 绝对路径；支持 bmp/jpg/png（Win8+ 的 COM 路径对 png 也有效）
bool setWallpaper(const QString &imagePath, QString *errorOut = nullptr);

/// 只给某一台显示器设置壁纸（monitorIndex 对应 wallpapers() 的顺序）
/// COM 不可用时只能作用于主显示器，且 index 必须为 0
bool setWallpaperForMonitor(int monitorIndex, const QString &imagePath, QString *errorOut = nullptr);

/// 设置壁纸显示方式（对所有显示器生效）
bool setWallpaperPosition(const QString &position, QString *errorOut = nullptr);

/// 可用的显示方式（空串表示不修改）
QStringList wallpaperPositions();

/// 该路径是否是"可作为壁纸的图片文件"（按扩展名判断，供轮换目录扫描用）
bool isSupportedWallpaperFile(const QString &filePath);

} // namespace WinEase::Win32
