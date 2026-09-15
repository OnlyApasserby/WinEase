#include "win32/ScreenCapture.h"

#include "win32/Win32Error.h"

#include <algorithm>
#include <vector>

#include <windows.h>

// 部分 SDK 头未定义 PW_RENDERFULLCONTENT（值为 2），自行兜底
#ifndef PW_RENDERFULLCONTENT
#    define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace WinEase::Win32 {

namespace {

/// 屏幕设备上下文（DISPLAY）与内存 DC 的 RAII 守卫
class ScreenDcGuard
{
public:
    ScreenDcGuard() = default;

    ~ScreenDcGuard()
    {
        // 释放顺序：先把原位图选回 DC，再依次销毁位图与设备上下文，
        // 否则会出现 GDI 对象泄漏（位图仍被 DC 持有）
        if (m_memoryDc != nullptr && m_previousBitmap != nullptr) {
            ::SelectObject(m_memoryDc, m_previousBitmap);
            m_previousBitmap = nullptr;
        }
        if (m_bitmap != nullptr) {
            ::DeleteObject(m_bitmap);
            m_bitmap = nullptr;
        }
        if (m_memoryDc != nullptr) {
            ::DeleteDC(m_memoryDc);
            m_memoryDc = nullptr;
        }
        if (m_screenDc != nullptr) {
            ::DeleteDC(m_screenDc);
            m_screenDc = nullptr;
        }
    }

    ScreenDcGuard(const ScreenDcGuard &) = delete;
    ScreenDcGuard &operator=(const ScreenDcGuard &) = delete;

    /// 创建与屏幕兼容的内存 DC 与位图
    bool prepare(int width, int height)
    {
        if (width <= 0 || height <= 0) {
            return false;
        }

        m_screenDc = ::CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (m_screenDc == nullptr) {
            return false;
        }

        m_memoryDc = ::CreateCompatibleDC(m_screenDc);
        if (m_memoryDc == nullptr) {
            return false;
        }

        m_bitmap = ::CreateCompatibleBitmap(m_screenDc, width, height);
        if (m_bitmap == nullptr) {
            return false;
        }

        m_previousBitmap = ::SelectObject(m_memoryDc, m_bitmap);
        m_width = width;
        m_height = height;
        return true;
    }

    /// 从屏幕指定位置拷贝到内存 DC 的 (0,0)
    bool blitFromScreen(const QRect &sourceRect, bool includeLayeredWindows)
    {
        if (m_memoryDc == nullptr || m_screenDc == nullptr) {
            return false;
        }

        DWORD rop = SRCCOPY;
        if (includeLayeredWindows) {
            rop |= CAPTUREBLT; // 把分层窗口一并抓取
        }

        const BOOL ok = ::BitBlt(m_memoryDc,
                                 0,
                                 0,
                                 m_width,
                                 m_height,
                                 m_screenDc,
                                 sourceRect.x(),
                                 sourceRect.y(),
                                 rop);
        return ok != FALSE;
    }

    /// 用 PrintWindow 渲染指定窗口（可抓被遮挡窗口）
    bool blitFromWindow(HWND hwnd)
    {
        if (m_memoryDc == nullptr) {
            return false;
        }
        return ::PrintWindow(hwnd, m_memoryDc, PW_RENDERFULLCONTENT) != FALSE;
    }

    /// 把内存位图取成 QImage（32bpp，含拷贝，可安全脱离 GDI 生命周期）
    QImage toImage() const
    {
        if (m_memoryDc == nullptr || m_bitmap == nullptr) {
            return QImage();
        }

        BITMAPINFOHEADER header{};
        header.biSize = sizeof(BITMAPINFOHEADER);
        header.biWidth = m_width;
        header.biHeight = -m_height; // 负值 = 自上而下，避免后续垂直翻转
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;

        const int bytesPerLine = m_width * 4;
        std::vector<BYTE> buffer(static_cast<size_t>(bytesPerLine) * static_cast<size_t>(m_height));

        const int copied = ::GetDIBits(m_memoryDc,
                                       m_bitmap,
                                       0,
                                       static_cast<UINT>(m_height),
                                       buffer.data(),
                                       reinterpret_cast<BITMAPINFO *>(&header),
                                       DIB_RGB_COLORS);
        if (copied == 0) {
            return QImage();
        }

        // copy() 让 QImage 拥有自己的内存，不依赖 buffer 的生存期
        return QImage(buffer.data(), m_width, m_height, bytesPerLine, QImage::Format_RGB32).copy();
    }

private:
    HDC m_screenDc = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previousBitmap = nullptr;
    int m_width = 0;
    int m_height = 0;
};

/// 抓取指定矩形并返回结果
CaptureResult captureRectInternal(const QRect &sourceRect,
                                  bool includeLayeredWindows,
                                  HWND windowFallback)
{
    CaptureResult result;
    result.sourceRect = sourceRect;

    if (!sourceRect.isValid() || sourceRect.width() <= 0 || sourceRect.height() <= 0) {
        result.error = QStringLiteral("抓取区域无效");
        return result;
    }

    ScreenDcGuard dc;
    if (!dc.prepare(sourceRect.width(), sourceRect.height())) {
        result.error = describeFailure(QStringLiteral("创建抓屏设备上下文"), lastError());
        return result;
    }

    if (!dc.blitFromScreen(sourceRect, includeLayeredWindows)) {
        // 屏幕抓取失败时，若给了窗口句柄则尝试 PrintWindow 兜底
        if (windowFallback == nullptr || !dc.blitFromWindow(windowFallback)) {
            result.error = describeFailure(QStringLiteral("从屏幕拷贝像素"), lastError());
            return result;
        }
    }

    result.image = dc.toImage();
    if (result.image.isNull()) {
        result.error = QStringLiteral("无法把抓屏结果转换为图像");
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
//  抓取
// ---------------------------------------------------------------------------

CaptureResult captureScreenRect(const QRect &physicalScreenRect, bool includeLayeredWindows)
{
    return captureRectInternal(physicalScreenRect, includeLayeredWindows, nullptr);
}

CaptureResult captureMonitor(const MonitorInfo &monitor, bool includeLayeredWindows)
{
    if (!monitor.valid) {
        CaptureResult result;
        result.error = QStringLiteral("显示器信息无效");
        return result;
    }
    return captureRectInternal(monitor.geometry, includeLayeredWindows, nullptr);
}

CaptureResult captureMonitorAt(int monitorIndex, bool includeLayeredWindows)
{
    const QList<MonitorInfo> all = monitors();
    if (monitorIndex < 0 || monitorIndex >= all.size()) {
        CaptureResult result;
        result.error = QStringLiteral("显示器索引越界（共 %1 个显示器）").arg(all.size());
        return result;
    }
    return captureMonitor(all.at(monitorIndex), includeLayeredWindows);
}

CaptureResult captureWindow(WindowHandle hwnd, bool includeLayeredWindows)
{
    CaptureResult result;
    if (!isValidWindow(hwnd)) {
        result.error = QStringLiteral("窗口句柄无效");
        return result;
    }

    const QRect bounds = visualWindowRect(hwnd);
    result.sourceRect = bounds;

    if (bounds.width() <= 0 || bounds.height() <= 0) {
        result.error = QStringLiteral("窗口尺寸为零，无法抓取");
        return result;
    }

    // 先试 PrintWindow：即使窗口被遮挡或部分在屏外也能拿到内容
    if (ScreenDcGuard dc; dc.prepare(bounds.width(), bounds.height()) && dc.blitFromWindow(hwnd)) {
        result.image = dc.toImage();
        if (!result.image.isNull()) {
            return result;
        }
    }

    // 退回屏幕抓取
    return captureRectInternal(bounds, includeLayeredWindows, hwnd);
}

CaptureResult captureVirtualDesktop(bool includeLayeredWindows)
{
    return captureRectInternal(virtualDesktopRect(), includeLayeredWindows, nullptr);
}

QRect virtualDesktopRect()
{
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (width <= 0 || height <= 0) {
        return QRect();
    }
    return QRect(left, top, width, height);
}

// ---------------------------------------------------------------------------
//  取色
// ---------------------------------------------------------------------------

QColor colorAt(const QPoint &physicalScreenPoint, bool *ok)
{
    if (ok != nullptr) {
        *ok = false;
    }

    // 用 1x1 的 BitBlt 而非 GetPixel：
    // GetPixel 在 DWM 合成与分层窗口场景下经常返回过期/错误颜色
    const CaptureResult captured = captureScreenRect(QRect(physicalScreenPoint, QSize(1, 1)));
    if (!captured.isValid()) {
        return QColor();
    }

    const QColor color = captured.image.pixelColor(0, 0);
    if (ok != nullptr && color.isValid()) {
        *ok = true;
    }
    return color;
}

PixelProbe probePixel(const QPoint &physicalScreenPoint, int sampleRadius)
{
    PixelProbe probe;

    const int radius = std::clamp(sampleRadius, 0, 64);
    const QRect sampleRect(physicalScreenPoint.x() - radius,
                           physicalScreenPoint.y() - radius,
                           radius * 2 + 1,
                           radius * 2 + 1);
    probe.rect = sampleRect;

    const CaptureResult captured = captureScreenRect(sampleRect);
    if (!captured.isValid()) {
        probe.error = captured.error;
        return probe;
    }

    probe.patch = captured.image;
    probe.color = captured.image.pixelColor(radius, radius);
    probe.valid = probe.color.isValid();
    if (!probe.valid) {
        probe.error = QStringLiteral("无法读取该位置的颜色");
    }
    return probe;
}

} // namespace WinEase::Win32
