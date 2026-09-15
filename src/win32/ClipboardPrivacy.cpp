#include "ClipboardPrivacy.h"

#include "win32/Win32Error.h"

#include <QThread>

#include <windows.h>

namespace WinEase::Win32 {

namespace {

// ---------------------------------------------------------------------------
//  标准（预定义）剪贴板格式名：FormatName 只能查自定义格式（id >= 0xC000），
//  预定义格式必须自己给名字，否则诊断信息里全是"格式 #13"这种没人看得懂的东西
// ---------------------------------------------------------------------------
QString standardFormatName(UINT id)
{
    switch (id) {
    case CF_TEXT:         return QStringLiteral("CF_TEXT");
    case CF_BITMAP:       return QStringLiteral("CF_BITMAP");
    case CF_METAFILEPICT: return QStringLiteral("CF_METAFILEPICT");
    case CF_SYLK:         return QStringLiteral("CF_SYLK");
    case CF_DIF:          return QStringLiteral("CF_DIF");
    case CF_TIFF:         return QStringLiteral("CF_TIFF");
    case CF_OEMTEXT:      return QStringLiteral("CF_OEMTEXT");
    case CF_DIB:          return QStringLiteral("CF_DIB");
    case CF_PALETTE:      return QStringLiteral("CF_PALETTE");
    case CF_PENDATA:      return QStringLiteral("CF_PENDATA");
    case CF_RIFF:         return QStringLiteral("CF_RIFF");
    case CF_WAVE:         return QStringLiteral("CF_WAVE");
    case CF_UNICODETEXT:  return QStringLiteral("CF_UNICODETEXT");
    case CF_ENHMETAFILE:  return QStringLiteral("CF_ENHMETAFILE");
    case CF_HDROP:        return QStringLiteral("CF_HDROP");
    case CF_LOCALE:       return QStringLiteral("CF_LOCALE");
    case CF_DIBV5:        return QStringLiteral("CF_DIBV5");
    default:              return QString();
    }
}

QString formatNameFor(UINT id)
{
    if (id < 0xC000) {
        const QString standard = standardFormatName(id);
        return standard.isEmpty() ? QStringLiteral("标准格式 #%1").arg(id) : standard;
    }
    wchar_t buffer[256] = {};
    const int length = ::GetClipboardFormatNameW(id, buffer, 256);
    if (length <= 0) {
        return QStringLiteral("自定义格式 #%1（名字读取失败）").arg(id);
    }
    return QString::fromWCharArray(buffer, length);
}

/// 注册名 → id；复制的是 Win32 API 语义（同名格式全系统共用一个 id）
UINT formatIdFor(const QString &name)
{
    return ::RegisterClipboardFormatW(reinterpret_cast<const wchar_t *>(name.utf16()));
}

/// 读"值是 DWORD"的自定义格式（CanIncludeInClipboardHistory / CanUploadToCloudClipboard）
/// @return 是否读到；读到则 valueOut 有效
bool readDwordFormat(UINT id, DWORD *valueOut)
{
    HANDLE handle = ::GetClipboardData(id);
    if (handle == nullptr) {
        return false;
    }
    // ⚠ handle 归剪贴板所有：只能 Lock/Unlock，**绝不能** GlobalFree（常见踩坑）
    const void *locked = ::GlobalLock(handle);
    if (locked == nullptr) {
        return false;
    }
    DWORD value = 0;
    const SIZE_T size = ::GlobalSize(handle);
    if (size >= sizeof(DWORD)) {
        value = *static_cast<const DWORD *>(locked);
    }
    ::GlobalUnlock(handle);
    if (size < sizeof(DWORD)) {
        return false;
    }
    *valueOut = value;
    return true;
}

} // namespace

QString clipboardIgnoreFormatName()
{
    return QStringLiteral("Clipboard Viewer Ignore");
}

QString clipboardMonitorExcludeFormatName()
{
    return QStringLiteral("ExcludeClipboardContentFromMonitorProcessing");
}

QString clipboardHistoryOptOutFormatName()
{
    return QStringLiteral("CanIncludeInClipboardHistory");
}

QString clipboardCloudOptOutFormatName()
{
    return QStringLiteral("CanUploadToCloudClipboard");
}

QString ClipboardOriginInfo::reasonText() const
{
    if (!inspected) {
        return error.isEmpty() ? QStringLiteral("剪贴板无法读取") : error;
    }
    if (!ignoreMarker.isEmpty()) {
        return QStringLiteral("来源标记为「不记录」（%1）").arg(ignoreMarker);
    }
    if (historyOptOut) {
        return QStringLiteral("来源声明「不要进剪贴板历史」（%1 = 0）")
            .arg(clipboardHistoryOptOutFormatName());
    }
    return QStringLiteral("来源未声明任何限制");
}

ClipboardOriginInfo inspectClipboardOrigin(int retries, int retryDelayMs)
{
    ClipboardOriginInfo info;

    if (retries < 1) {
        retries = 1;
    }

    bool opened = false;
    for (int attempt = 0; attempt < retries && !opened; ++attempt) {
        if (::OpenClipboard(nullptr) != FALSE) {
            opened = true;
            break;
        }
        if (attempt + 1 < retries && retryDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(retryDelayMs));
        }
    }

    if (!opened) {
        info.inspected = false;
        info.error = describeFailure(QStringLiteral("打开剪贴板"), lastError())
            + QStringLiteral("（可能被其它程序占用）");
        return info;
    }

    const UINT ignoreId = formatIdFor(clipboardIgnoreFormatName());
    const UINT excludeId = formatIdFor(clipboardMonitorExcludeFormatName());
    const UINT historyId = formatIdFor(clipboardHistoryOptOutFormatName());
    const UINT cloudId = formatIdFor(clipboardCloudOptOutFormatName());

    UINT id = 0;
    while ((id = ::EnumClipboardFormats(id)) != 0) {
        info.formats.append(formatNameFor(id));

        if (id == ignoreId) {
            info.ignoreMarker = clipboardIgnoreFormatName();
        } else if (id == excludeId) {
            info.ignoreMarker = clipboardMonitorExcludeFormatName();
        } else if (id == historyId) {
            DWORD value = 1;
            if (readDwordFormat(id, &value) && value == 0) {
                info.historyOptOut = true;
            }
        } else if (id == cloudId) {
            DWORD value = 1;
            if (readDwordFormat(id, &value) && value == 0) {
                info.cloudUploadOptOut = true;
            }
        }
    }

    // ⚠ OpenClipboard 必须配对 CloseClipboard（漏一次就够让别的程序复制失败）
    ::CloseClipboard();

    info.inspected = true;
    return info;
}

unsigned int registerClipboardFormatId(const QString &formatName)
{
    return formatIdFor(formatName);
}

bool clipboardFormatAvailable(unsigned int formatId)
{
    return ::IsClipboardFormatAvailable(formatId) != FALSE;
}

} // namespace WinEase::Win32
