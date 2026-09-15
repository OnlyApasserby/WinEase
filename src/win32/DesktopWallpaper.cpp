// initguid.h 让 <shobjidl.h> 里的 CLSID/IID **定义**在本编译单元里，
// 从而不需要额外链接 uuid.lib（本模块是静态库，会被编进主程序与每个插件）
#include <initguid.h>

#include "win32/DesktopWallpaper.h"

#include "win32/Win32Error.h"
#include "win32/WindowUtils.h"

#include <shobjidl.h>

#include <QDir>
#include <QFileInfo>

#include <vector>

namespace WinEase::Win32 {

namespace {

constexpr int kMaxPathChars = 1024;

/// 把 IDesktopWallpaper 的 LPWSTR 结果转成 QString 并释放（CoTaskMemFree 是配套约定）。
/// ⚠ 语义是"取走并释放"：调用方拿到 QString 后就**不能再**对这个指针调 CoTaskMemFree
QString takeCoTaskString(LPWSTR text)
{
    if (text == nullptr) {
        return QString();
    }
    const QString result = QString::fromWCharArray(text);
    ::CoTaskMemFree(text);
    return result;
}

/// 创建 IDesktopWallpaper 实例（失败返回 nullptr）。
/// 每次都重新创建：本模块无状态，不能缓存 COM 指针（静态库会被链接进多个模块，
/// 缓存会变成"每个模块一份全局状态"）
IDesktopWallpaper *createDesktopWallpaper()
{
    IDesktopWallpaper *wallpaper = nullptr;
    const HRESULT result = ::CoCreateInstance(CLSID_DesktopWallpaper,
                                              nullptr,
                                              CLSCTX_ALL,
                                              IID_PPV_ARGS(&wallpaper));
    if (FAILED(result)) {
        return nullptr;
    }
    return wallpaper;
}

/// RAII：保证接口指针被释放
struct ComPtrGuard {
    IDesktopWallpaper *ptr = nullptr;
    ~ComPtrGuard()
    {
        if (ptr != nullptr) {
            ptr->Release();
        }
    }
};

/// ⚠ 顺序必须与 DESKTOP_WALLPAPER_POSITION 完全一致：
///   DWPOS_CENTER=0, DWPOS_TILE=1, DWPOS_STRETCH=2, DWPOS_FIT=3, DWPOS_FILL=4, DWPOS_SPAN=5
///   （"填满/适应/平铺/拉伸"写错一个，用户设的就是另一种效果）
const char *const kPositionKeys[] = { "center", "tile", "stretch", "fit", "fill", "span" };
constexpr int kPositionCount = 6;

QString positionToKey(DESKTOP_WALLPAPER_POSITION position)
{
    const int index = static_cast<int>(position);
    if (index >= 0 && index < kPositionCount) {
        return QString::fromLatin1(kPositionKeys[index]);
    }
    return QString();
}

bool keyToPosition(const QString &key, DESKTOP_WALLPAPER_POSITION *out)
{
    for (int index = 0; index < kPositionCount; ++index) {
        if (key.compare(QString::fromLatin1(kPositionKeys[index]), Qt::CaseInsensitive) == 0) {
            *out = static_cast<DESKTOP_WALLPAPER_POSITION>(index);
            return true;
        }
    }
    return false;
}

/// 老接口取当前壁纸（只能取到"所有显示器"那一张）
QString legacyCurrentWallpaper()
{
    std::vector<wchar_t> buffer(kMaxPathChars, L'\0');
    if (::SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                                static_cast<UINT>(buffer.size()),
                                buffer.data(),
                                0)
        == FALSE) {
        return QString();
    }
    return QString::fromWCharArray(buffer.data());
}

} // namespace

bool isDesktopWallpaperAvailable()
{
    ComPtrGuard guard{ createDesktopWallpaper() };
    return guard.ptr != nullptr;
}

QList<WallpaperInfo> wallpapers()
{
    QList<WallpaperInfo> result;

    ComPtrGuard guard{ createDesktopWallpaper() };
    if (guard.ptr == nullptr) {
        // 兜底：老接口只能给出"一张图"，显示器信息用平台层的枚举补上
        WallpaperInfo info;
        info.monitorName = QStringLiteral("主显示器");
        const MonitorInfo primary = WinEase::Win32::primaryMonitor();
        info.monitorRect = primary.geometry;
        info.path = legacyCurrentWallpaper();
        info.valid = true;
        result.append(info);
        return result;
    }

    UINT count = 0;
    if (FAILED(guard.ptr->GetMonitorDevicePathCount(&count)) || count == 0) {
        return result;
    }

    const QList<MonitorInfo> monitorList = monitors();
    for (UINT index = 0; index < count; ++index) {
        LPWSTR monitorId = nullptr;
        if (FAILED(guard.ptr->GetMonitorDevicePathAt(index, &monitorId))) {
            continue;
        }
        const QString id = takeCoTaskString(monitorId);

        WallpaperInfo info;
        info.monitorId = id;
        info.monitorName = QStringLiteral("显示器 %1").arg(index + 1);
        info.valid = true;
        if (static_cast<int>(index) < monitorList.size()) {
            info.monitorRect = monitorList.at(static_cast<int>(index)).geometry;
        }

        RECT rect{ 0, 0, 0, 0 };
        if (SUCCEEDED(guard.ptr->GetMonitorRECT(monitorId, &rect))) {
            info.monitorRect = QRect(rect.left,
                                     rect.top,
                                     rect.right - rect.left,
                                     rect.bottom - rect.top);
        }

        LPWSTR path = nullptr;
        if (SUCCEEDED(guard.ptr->GetWallpaper(monitorId, &path))) {
            info.path = takeCoTaskString(path);
        }

        DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
        if (SUCCEEDED(guard.ptr->GetPosition(&position))) {
            info.position = positionToKey(position);
        }

        result.append(info);
        // ⚠ 不要再 ::CoTaskMemFree(monitorId)：takeCoTaskString() 已经释放过了。
        //    重复释放会直接踩坏堆（实测症状：一进壁纸用例就 STATUS_HEAP_CORRUPTION）
    }

    return result;
}

QString currentWallpaper()
{
    const QList<WallpaperInfo> all = wallpapers();
    if (all.isEmpty()) {
        return QString();
    }
    for (const WallpaperInfo &info : all) {
        if (!info.path.isEmpty()) {
            return info.path;
        }
    }
    return all.first().path;
}

bool setWallpaper(const QString &imagePath, QString *errorOut)
{
    if (imagePath.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("壁纸路径为空");
        }
        return false;
    }
    const QFileInfo file(imagePath);
    if (!file.exists()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("壁纸文件不存在：%1").arg(imagePath);
        }
        return false;
    }
    const QString nativePath = QDir::toNativeSeparators(file.absoluteFilePath());

    // 优先走 IDesktopWallpaper：它能处理 png/webp，且会立刻刷新桌面
    {
        ComPtrGuard guard{ createDesktopWallpaper() };
        if (guard.ptr != nullptr) {
            const HRESULT result = guard.ptr->SetWallpaper(
                nullptr,
                reinterpret_cast<const wchar_t *>(nativePath.utf16()));
            if (SUCCEEDED(result)) {
                return true;
            }
            if (errorOut != nullptr) {
                *errorOut = describeHresultFailure(QStringLiteral("设置壁纸"),
                                                   result);
            }
        }
    }

    // 兜底：老接口（需要绝对路径、可写缓冲区；bmp/jpg 一定支持）
    std::vector<wchar_t> buffer(nativePath.size() + 1, L'\0');
    nativePath.toWCharArray(buffer.data());
    if (::SystemParametersInfoW(SPI_SETDESKWALLPAPER,
                                0,
                                buffer.data(),
                                SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)
        == FALSE) {
        if (errorOut != nullptr) {
            *errorOut = describeFailure(QStringLiteral("设置壁纸（SPI_SETDESKWALLPAPER）"),
                                        ::GetLastError());
        }
        return false;
    }
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    return true;
}

bool setWallpaperForMonitor(int monitorIndex, const QString &imagePath, QString *errorOut)
{
    if (monitorIndex < 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("显示器序号非法");
        }
        return false;
    }
    if (monitorIndex == 0 && !isDesktopWallpaperAvailable()) {
        // COM 不可用：只有"主显示器"能设，而这恰好等价于"设置全部"
        return setWallpaper(imagePath, errorOut);
    }

    const QFileInfo file(imagePath);
    if (!file.exists()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("壁纸文件不存在：%1").arg(imagePath);
        }
        return false;
    }

    ComPtrGuard guard{ createDesktopWallpaper() };
    if (guard.ptr == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("IDesktopWallpaper 不可用（COM 未初始化？）");
        }
        return false;
    }

    LPWSTR monitorId = nullptr;
    if (FAILED(guard.ptr->GetMonitorDevicePathAt(static_cast<UINT>(monitorIndex), &monitorId))) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("取不到第 %1 台显示器").arg(monitorIndex + 1);
        }
        return false;
    }

    const QString nativePath = QDir::toNativeSeparators(file.absoluteFilePath());
    const HRESULT result = guard.ptr->SetWallpaper(
        monitorId,
        reinterpret_cast<const wchar_t *>(nativePath.utf16()));
    ::CoTaskMemFree(monitorId);

    if (FAILED(result)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(
                QStringLiteral("给第 %1 台显示器设置壁纸").arg(monitorIndex + 1), result);
        }
        return false;
    }
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    return true;
}

bool setWallpaperPosition(const QString &position, QString *errorOut)
{
    DESKTOP_WALLPAPER_POSITION value = DWPOS_FILL;
    if (!keyToPosition(position, &value)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("不支持的显示方式：%1（可选 %2）")
                            .arg(position, wallpaperPositions().join(QStringLiteral("/")));
        }
        return false;
    }

    ComPtrGuard guard{ createDesktopWallpaper() };
    if (guard.ptr == nullptr) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("IDesktopWallpaper 不可用（显示方式只能改注册表，暂未实现）");
        }
        return false;
    }
    const HRESULT result = guard.ptr->SetPosition(value);
    if (FAILED(result)) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("设置壁纸显示方式"), result);
        }
        return false;
    }
    return true;
}

QStringList wallpaperPositions()
{
    QStringList result;
    for (int index = 0; index < kPositionCount; ++index) {
        result.append(QString::fromLatin1(kPositionKeys[index]));
    }
    return result;
}

bool isSupportedWallpaperFile(const QString &filePath)
{
    static const QStringList extensions = { QStringLiteral("bmp"),  QStringLiteral("jpg"),
                                            QStringLiteral("jpeg"), QStringLiteral("png"),
                                            QStringLiteral("webp"), QStringLiteral("gif") };
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    return !suffix.isEmpty() && extensions.contains(suffix);
}

} // namespace WinEase::Win32
