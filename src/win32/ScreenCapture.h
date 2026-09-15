#pragma once

// ============================================================================
//  ScreenCapture.h —— 屏幕抓取与取色
//
//  坐标系：全部使用 **物理像素 + 屏幕坐标**（与 WindowUtils 一致）。
//          多显示器时虚拟桌面原点可能为负值，本模块已正确处理。
//
//  实现说明：
//      使用 GDI 的 CreateDCW(L"DISPLAY") + BitBlt，并带 CAPTUREBLT 标志，
//      以便把分层窗口（Layered Window，如异形窗口、部分悬浮层）一并抓进来。
//
//  性能提示：
//      * 抓取整块 4K 屏幕约需数十毫秒，**不要放在 UI 线程的高频循环里**
//      * 只需要取色时用 colorAt()/probePixel()，避免整屏抓取
//
//  后续演进（P3-08）：
//      录屏与高频抓屏会改用 DXGI Desktop Duplication，
//      届时新增实现并保持本头文件接口不变（本层已按接口隔离设计）。
// ============================================================================

#include "win32/WindowUtils.h"

#include <QColor>
#include <QImage>
#include <QRect>

namespace WinEase::Win32 {

/// 一次抓取的结果
struct CaptureResult {
    QImage image;        ///< 抓到的图像；失败时为空
    QRect sourceRect;    ///< 抓取区域的屏幕坐标（物理像素）
    QString error;       ///< 失败原因（中文）；成功时为空

    bool isValid() const { return !image.isNull(); }
};

/// 抓取屏幕上的任意矩形区域（物理像素、屏幕坐标）
/// @param includeLayeredWindows 是否包含分层窗口（CAPTUREBLT），默认包含
CaptureResult captureScreenRect(const QRect &physicalScreenRect, bool includeLayeredWindows = true);

/// 抓取指定显示器
CaptureResult captureMonitor(const MonitorInfo &monitor, bool includeLayeredWindows = true);

/// 抓取指定显示器（按索引，越界返回空结果）
CaptureResult captureMonitorAt(int monitorIndex, bool includeLayeredWindows = true);

/// 抓取窗口
/// 优先使用 PrintWindow(PW_RENDERFULLCONTENT)，即使窗口被遮挡也能拿到内容；
/// 失败时退回从屏幕抓取其视觉边界。
CaptureResult captureWindow(WindowHandle hwnd, bool includeLayeredWindows = true);

/// 抓取整个虚拟桌面（所有显示器的并集区域）
CaptureResult captureVirtualDesktop(bool includeLayeredWindows = true);

/// 全部显示器的并集区域（物理像素，可能含负坐标）
QRect virtualDesktopRect();

/// 单点取色（物理像素、屏幕坐标）
/// @param ok 可选输出，失败时为 false
QColor colorAt(const QPoint &physicalScreenPoint, bool *ok = nullptr);

/// 取色探测结果：颜色 + 周边像素块（供取色器的放大预览使用）
struct PixelProbe {
    QColor color;      ///< 中心像素颜色
    QImage patch;      ///< 中心周围的像素块（放大后可做预览）
    QRect rect;        ///< patch 对应的屏幕区域（物理像素）
    bool valid = false;
    QString error;
};

/// 取色并附带周边像素块
/// @param sampleRadius 采样半径（像素），例如 8 表示取 17x17 的块
PixelProbe probePixel(const QPoint &physicalScreenPoint, int sampleRadius = 8);

} // namespace WinEase::Win32
