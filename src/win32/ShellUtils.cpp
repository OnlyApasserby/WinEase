#include "win32/ShellUtils.h"

#include "win32/ComApartment.h"
#include "win32/RegistryUtils.h"
#include "win32/Win32Error.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <vector>

// commctrl.h 必须早于 commoncontrols.h：后者用到 IMAGELISTDRAWPARAMS / IMAGEINFO
// （SHIL_*、IImageList 用于从系统图像列表取高清文件图标）
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

// Microsoft::WRL::ComPtr 来自 Windows SDK（wrl/client.h），不是第三方库。
// 用它把 COM 的 AddRef/Release 样板压到最低。
#include <wrl/client.h>

namespace WinEase::Win32 {

namespace {

using Microsoft::WRL::ComPtr;

/// 把 HBITMAP 转成 QImage。
/// 注意：部分来源的位图没有 alpha 通道（GetDIBits 后 alpha 全 0），
/// 这类图必须把 alpha 补成 255，否则会得到一张全透明的图。
QImage bitmapToImage(HBITMAP bitmap)
{
    if (bitmap == nullptr) {
        return QImage();
    }

    BITMAP native{};
    if (::GetObjectW(bitmap, sizeof(native), &native) == 0) {
        return QImage();
    }

    const int width = native.bmWidth;
    const int height = native.bmHeight;
    if (width <= 0 || height <= 0) {
        return QImage();
    }

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = width;
    header.biHeight = -height; // 负数 = 自上而下，省去后续翻转
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;

    const int stride = width * 4;
    std::vector<BYTE> buffer(static_cast<size_t>(stride) * static_cast<size_t>(height));

    const HDC screenDc = ::GetDC(nullptr);
    const int copied = ::GetDIBits(screenDc,
                                   bitmap,
                                   0,
                                   static_cast<UINT>(height),
                                   buffer.data(),
                                   reinterpret_cast<BITMAPINFO *>(&header),
                                   DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screenDc);

    if (copied == 0) {
        return QImage();
    }

    QImage image(buffer.data(), width, height, stride, QImage::Format_ARGB32);
    image = image.copy(); // 脱离 buffer 生存期

    // alpha 通道整体为 0 说明源位图不带 alpha，补成不透明
    bool hasAlpha = false;
    for (int y = 0; y < height && !hasAlpha; ++y) {
        const auto *line = reinterpret_cast<const uchar *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (line[x * 4 + 3] != 0) {
                hasAlpha = true;
                break;
            }
        }
    }
    if (!hasAlpha) {
        image = image.convertToFormat(QImage::Format_RGB32);
    }

    return image;
}

/// 把 HICON 画到 32bpp DIB 段上再取像素。
/// 这样能正确处理带 alpha 的现代图标，也兼容只有掩码的老式图标，
/// 避免手工解析 ICONINFO 的 hbmColor / hbmMask 规则。
QImage iconToImage(HICON icon, int size)
{
    if (icon == nullptr || size <= 0) {
        return QImage();
    }

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = size;
    header.biHeight = -size;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;

    const HDC screenDc = ::GetDC(nullptr);
    const HDC memoryDc = ::CreateCompatibleDC(screenDc);

    void *bits = nullptr;
    const HBITMAP dib = ::CreateDIBSection(screenDc,
                                           reinterpret_cast<BITMAPINFO *>(&header),
                                           DIB_RGB_COLORS,
                                           &bits,
                                           nullptr,
                                           0);

    QImage image;
    if (memoryDc != nullptr && dib != nullptr && bits != nullptr) {
        HGDIOBJ previous = ::SelectObject(memoryDc, dib);
        if (::DrawIconEx(memoryDc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL) != FALSE) {
            image = QImage(static_cast<const uchar *>(bits), size, size, size * 4, QImage::Format_ARGB32);
            image = image.copy();
        }
        ::SelectObject(memoryDc, previous);
    }

    if (dib != nullptr) {
        ::DeleteObject(dib);
    }
    if (memoryDc != nullptr) {
        ::DeleteDC(memoryDc);
    }
    ::ReleaseDC(nullptr, screenDc);

    return image;
}

/// 通用的 IFileOperation 删除
bool fileOperationDelete(const QStringList &paths, bool toRecycleBin, QString *errorOut)
{
    if (paths.isEmpty()) {
        return true;
    }

    // IFileOperation 要求调用线程已初始化 COM
    if (!isThreadComReady()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("当前线程尚未初始化 COM，无法执行 Shell 文件操作");
        }
        return false;
    }

    ComPtr<IFileOperation> operation;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOperation,
                                    nullptr,
                                    CLSCTX_ALL,
                                    IID_PPV_ARGS(&operation));
    if (FAILED(hr) || !operation) {
        if (errorOut != nullptr) {
            *errorOut = describeHresultFailure(QStringLiteral("创建 Shell 文件操作对象"), hr);
        }
        return false;
    }

    // FOF_NO_UI = 静默 + 不确认 + 不弹错误框；FOF_ALLOWUNDO 表示保留撤销信息（进回收站）
    DWORD flags = FOF_NO_UI;
    if (toRecycleBin) {
        flags |= FOF_ALLOWUNDO;
    }
    operation->SetOperationFlags(flags);
    operation->SetOwnerWindow(nullptr);

    int failedItems = 0;
    for (const QString &path : paths) {
        const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        ComPtr<IShellItem> item;
        hr = ::SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                                           nullptr,
                                           IID_PPV_ARGS(&item));
        if (FAILED(hr) || !item) {
            ++failedItems;
            continue;
        }
        if (FAILED(operation->DeleteItem(item.Get(), nullptr))) {
            ++failedItems;
        }
    }

    hr = operation->PerformOperations();

    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);

    if (failedItems > 0 || FAILED(hr) || aborted != FALSE) {
        if (errorOut != nullptr) {
            if (failedItems > 0) {
                *errorOut = QStringLiteral("有 %1 个项目无法处理（可能被占用或权限不足）").arg(failedItems);
            } else if (aborted != FALSE) {
                *errorOut = QStringLiteral("操作被中止");
            } else {
                *errorOut = describeHresultFailure(QStringLiteral("执行 Shell 文件操作"), hr);
            }
        }
        return false;
    }
    return true;
}

/// 取文件属性（用于图标/类型查询）
DWORD fileAttributesOf(const QString &path, bool *existsOut)
{
    const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<LPCWSTR>(
        QDir::toNativeSeparators(path).utf16()));
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (existsOut != nullptr) {
        *existsOut = exists;
    }
    return exists ? attributes : FILE_ATTRIBUTE_NORMAL;
}

/// 映射到系统已知文件夹 ID。
/// 注意：FOLDERID_* 是常量 GUID 对象，不存在名为 FOLDERID 的类型，
/// 因此返回类型必须写 KNOWNFOLDERID 的引用。
const KNOWNFOLDERID &folderIdOf(KnownFolder folder)
{
    switch (folder) {
    case KnownFolder::Desktop:         return FOLDERID_Desktop;
    case KnownFolder::Documents:       return FOLDERID_Documents;
    case KnownFolder::Downloads:       return FOLDERID_Downloads;
    case KnownFolder::Pictures:        return FOLDERID_Pictures;
    case KnownFolder::Music:           return FOLDERID_Music;
    case KnownFolder::Videos:          return FOLDERID_Videos;
    case KnownFolder::StartMenu:       return FOLDERID_Programs;
    case KnownFolder::CommonStartMenu: return FOLDERID_CommonPrograms;
    case KnownFolder::Startup:         return FOLDERID_Startup;
    case KnownFolder::CommonStartup:   return FOLDERID_CommonStartup;
    case KnownFolder::ProgramFiles:    return FOLDERID_ProgramFiles;
    case KnownFolder::ProgramFilesX86: return FOLDERID_ProgramFilesX86;
    case KnownFolder::Windows:         return FOLDERID_Windows;
    case KnownFolder::System32:        return FOLDERID_System;
    case KnownFolder::Temp:            return FOLDERID_LocalAppData;
    case KnownFolder::RoamingAppData:  return FOLDERID_RoamingAppData;
    case KnownFolder::LocalAppData:    return FOLDERID_LocalAppData;
    case KnownFolder::PublicDesktop:   return FOLDERID_PublicDesktop;
    case KnownFolder::PublicDocuments: return FOLDERID_PublicDocuments;
    }
    return FOLDERID_Desktop;
}

} // namespace

// ============================================================================
//  系统文件夹
// ============================================================================

QString knownFolderPath(KnownFolder folder)
{
    const KNOWNFOLDERID &id = folderIdOf(folder);

    PWSTR rawPath = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &rawPath);
    if (FAILED(hr) || rawPath == nullptr) {
        if (rawPath != nullptr) {
            ::CoTaskMemFree(rawPath);
        }
        return QString();
    }

    QString path = QString::fromWCharArray(rawPath);
    ::CoTaskMemFree(rawPath);
    return QDir::cleanPath(path);
}

// ============================================================================
//  缩略图与图标
// ============================================================================

ThumbnailResult fileThumbnail(const QString &path, int size)
{
    ThumbnailResult result;
    if (path.isEmpty()) {
        result.error = QStringLiteral("路径为空");
        return result;
    }
    if (size <= 0) {
        size = 256;
    }

    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());

    ComPtr<IShellItemImageFactory> factory;
    HRESULT hr = ::SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                                               nullptr,
                                               IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        result.error = describeHresultFailure(QStringLiteral("打开 Shell 项目"), hr);
        return result;
    }

    SIZE requested{ size, size };
    HBITMAP bitmap = nullptr;
    // SIIGBF_THUMBNAILONLY：没有缩略图时直接失败，交给上层回退到图标
    hr = factory->GetImage(requested, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bitmap);
    if (FAILED(hr) || bitmap == nullptr) {
        result.error = describeHresultFailure(QStringLiteral("获取文件缩略图"), hr);
        return result;
    }

    result.image = bitmapToImage(bitmap);
    ::DeleteObject(bitmap);

    if (result.image.isNull()) {
        result.error = QStringLiteral("缩略图转换为图像失败");
    }
    return result;
}

ThumbnailResult fileIcon(const QString &path, int size)
{
    ThumbnailResult result;
    if (path.isEmpty()) {
        result.error = QStringLiteral("路径为空");
        return result;
    }
    if (size <= 0) {
        size = 256;
    }

    bool exists = false;
    const DWORD attributes = fileAttributesOf(path, &exists);

    // 文件不存在时用 USEFILEATTRIBUTES，按扩展名给图标（预览已删除文件时会用到）
    UINT infoFlags = SHGFI_SYSICONINDEX;
    if (!exists) {
        infoFlags |= SHGFI_USEFILEATTRIBUTES;
    }

    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());

    SHFILEINFOW info{};
    if (::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                         attributes,
                         &info,
                         sizeof(info),
                         infoFlags) == 0) {
        result.error = QStringLiteral("获取文件图标信息失败");
        return result;
    }

    // 优先用系统图像列表拿到高清图标（JUMBO 为 256px）
    const int listType = (size >= 256) ? SHIL_JUMBO : ((size >= 48) ? SHIL_EXTRALARGE : SHIL_LARGE);
    IImageList *imageList = nullptr;
    if (SUCCEEDED(::SHGetImageList(listType, IID_PPV_ARGS(&imageList))) && imageList != nullptr) {
        HICON icon = nullptr;
        if (SUCCEEDED(imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) && icon != nullptr) {
            result.image = iconToImage(icon, size);
            ::DestroyIcon(icon);
        }
        imageList->Release();
    }

    if (result.image.isNull()) {
        // 回退：直接取 32x32 图标
        SHFILEINFOW fallback{};
        UINT fallbackFlags = SHGFI_ICON | SHGFI_LARGEICON;
        if (!exists) {
            fallbackFlags |= SHGFI_USEFILEATTRIBUTES;
        }
        if (::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                             attributes,
                             &fallback,
                             sizeof(fallback),
                             fallbackFlags) != 0
            && fallback.hIcon != nullptr) {
            result.image = iconToImage(fallback.hIcon, 48);
            ::DestroyIcon(fallback.hIcon);
        }
    }

    if (result.image.isNull()) {
        result.error = QStringLiteral("无法获取文件图标");
        return result;
    }

    result.fromShellIcon = true;
    return result;
}

ThumbnailResult filePreview(const QString &path, int size)
{
    // 先要内容缩略图（有内容就有信息量），拿不到再退回类型图标
    ThumbnailResult thumbnail = fileThumbnail(path, size);
    if (thumbnail.isValid()) {
        return thumbnail;
    }

    ThumbnailResult icon = fileIcon(path, size);
    if (icon.isValid()) {
        return icon;
    }

    // 两者都失败时，把缩略图的错误作为主因返回（更有诊断价值）
    return thumbnail;
}

// ============================================================================
//  文件操作
// ============================================================================

bool moveToRecycleBin(const QStringList &paths, QString *errorOut)
{
    return fileOperationDelete(paths, true, errorOut);
}

bool deletePermanently(const QStringList &paths, QString *errorOut)
{
    return fileOperationDelete(paths, false, errorOut);
}

bool revealInExplorer(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return false;
    }

    // explorer /select,<path> 需要把开关和路径放在同一个参数里
    const QString argument = QStringLiteral("/select,") + QDir::toNativeSeparators(info.absoluteFilePath());
    return QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList{ argument });
}

bool openWithShell(const QString &path, const QString &verb)
{
    const QString nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const HINSTANCE instance = ::ShellExecuteW(nullptr,
                                               reinterpret_cast<LPCWSTR>(verb.utf16()),
                                               reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                                               nullptr,
                                               nullptr,
                                               SW_SHOWNORMAL);
    // ShellExecute 的返回值 <= 32 表示失败（历史约定）
    return reinterpret_cast<INT_PTR>(instance) > 32;
}

// ============================================================================
//  文件类型信息
// ============================================================================

QString fileTypeDescription(const QString &path)
{
    bool exists = false;
    const DWORD attributes = fileAttributesOf(path, &exists);

    UINT flags = SHGFI_TYPENAME;
    if (!exists) {
        flags |= SHGFI_USEFILEATTRIBUTES;
    }

    SHFILEINFOW info{};
    if (::SHGetFileInfoW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                         attributes,
                         &info,
                         sizeof(info),
                         flags) == 0) {
        return QString();
    }
    return QString::fromWCharArray(info.szTypeName);
}

QString fileContentType(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.isEmpty()) {
        return QString();
    }

    const QString subKey = QStringLiteral("Software\\Classes\\.") + suffix;
    // HKCU 优先（用户级覆盖），再退到 HKLM
    QString contentType = readValue(RegistryRoot::CurrentUser, subKey, QStringLiteral("Content Type")).toString();
    if (contentType.isEmpty()) {
        contentType = readValue(RegistryRoot::LocalMachine, subKey, QStringLiteral("Content Type")).toString();
    }
    return contentType;
}

} // namespace WinEase::Win32
