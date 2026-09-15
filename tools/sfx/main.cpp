// ============================================================================
//  winease-setup —— WinEase 自解压安装器（SFX 启动器）
//
//  【它解决什么问题】
//      用户拿到的应该**只有一个 exe**：双击即装，不需要先解压、不需要联网、
//      不需要单独装 VC++ 运行库、不需要预装 .NET。这个程序就是那个 exe 的"壳"。
//
//  【文件形态】本 exe 的尾部**直接追加**了负载（CAB 压缩包）与一段固定尾部：
//
//      ┌──────────────────────────────┐ 0
//      │ 本程序（SFX 启动器，/MT 静态链接，自身不依赖任何运行库）│
//      ├──────────────────────────────┤ cabOffset
//      │ payload.cab（makecab 产出的 LZX 压缩包，含全部文件）    │
//      ├──────────────────────────────┤
//      │ SfxFooter（96 字节，见下）    │ ← 从文件末尾倒着读 96 字节就能定位
//      └──────────────────────────────┘ EOF
//
//  【为什么用 CAB 而不是自己做压缩格式】
//      解压用 **Windows 自带的 cabinet.dll（FDICopy）**，打包用 **Windows 自带的
//      makecab.exe**：两侧都是操作系统组件，既不用引第三方库，也不用自己写 DEFLATE。
//      这与本工程"能不自造就不自造"的一贯做法一致。
//
//  【为什么本程序用 /MT 静态链接】
//      安装器的职责之一恰恰是"把 VC++ 运行库装到目标机器上" —— 它自己要是还依赖
//      msvcp140.dll 就成了鸡生蛋问题（干净机器上双击没反应，用户完全无从下手）。
//
//  【模式】
//      （默认）      安装到 %LOCALAPPDATA%\Programs\WinEase（免管理员），
//                    写卸载注册表项 + 建开始菜单快捷方式；`--launch` 装完即启动
//      --verify      解到临时目录 → 按清单逐文件校验 SHA-256 → 清掉临时目录
//                    （**只读操作**，不写注册表、不建快捷方式，供自检使用）
//      --selftest    校验桥接运行时是否真的能用：加载 WinEaseLiteMonitorBridge.dll
//                    并让它真实枚举一次硬件传感器（用来证明 .NET 运行库确实带全了）
//      --uninstall   按 <目录>\install.manifest.txt 删除文件与注册表项（可逆）
//      --list        列出负载清单
//      --silent      不打印进度细节（只打印结果）
//      --dir <路径>  指定安装 / 卸载 / 自检目录
//
//  ⚠ 安装器**不联网、不下载、不提权**：所有依赖都在负载里（见 docs/DISTRIBUTION.md）。
// ============================================================================

// WIN32_LEAN_AND_MEAN / NOMINMAX 由 CMakeLists.txt 统一定义（见本目录的说明）

#include <windows.h>

#include <bcrypt.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------------------
//  负载尾部（必须与 scripts/stage_dist.ps1 里的写法逐字节一致）
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct SfxFooter {
    char          magic[8];      // "WESFX/01"
    std::uint64_t cabOffset;     // CAB 在 exe 内的偏移
    std::uint64_t cabSize;       // CAB 字节数
    char          cabSha256[64]; // CAB 的 SHA-256（小写十六进制，NUL 补齐）
    char          reserved[8];
};
#pragma pack(pop)

static_assert(sizeof(SfxFooter) == 96, "尾部布局必须恰好 96 字节（脚本侧按此写）");

namespace {

constexpr const char *kFooterMagic = "WESFX/01";
constexpr const wchar_t *kAppName = L"WinEase";
constexpr const wchar_t *kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinEase";

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------

std::wstring exePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2); // 路径比 MAX_PATH 长时继续要
    }
}

std::wstring directoryOf(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

std::wstring fileNameOf(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::wstring joinPath(const std::wstring &dir, const std::wstring &relative)
{
    if (dir.empty()) {
        return relative;
    }
    std::wstring result = dir;
    if (result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result += relative;
    return result;
}

/// 递归建目录（含中间层）
bool ensureDirectory(const std::wstring &path)
{
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    const std::wstring parent = directoryOf(path);
    if (!parent.empty() && parent != path && !ensureDirectory(parent)) {
        return false;
    }
    if (::CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool fileExists(const std::wstring &path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void out(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

void outLine(const char *text)
{
    out("%s\n", text);
}

// ---------------------------------------------------------------------------
//  SHA-256（走 bcrypt.dll，系统自带）
// ---------------------------------------------------------------------------

bool sha256OfBuffer(const unsigned char *data, size_t size, std::string *hexLower)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    unsigned char digest[32] = {0};
    DWORD digestSize = 0;
    DWORD objectSize = 0;
    std::vector<unsigned char> object;

    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    if (::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &digestSize, 0)
            == 0
        && objectSize > 0) {
        object.resize(objectSize);
        if (::BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0) {
            if (::BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0
                && ::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                static const char *digits = "0123456789abcdef";
                std::string result;
                result.reserve(64);
                for (unsigned char byte : digest) {
                    result.push_back(digits[byte >> 4]);
                    result.push_back(digits[byte & 0x0F]);
                }
                *hexLower = result;
                ok = true;
            }
            ::BCryptDestroyHash(hash);
        }
    }
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool sha256OfFile(const std::wstring &path, std::string *hexLower)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD ignored = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> buffer(256 * 1024);
    unsigned char digest[32] = {0};
    bool ok = false;

    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
        && ::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &ignored, 0)
               == 0
        && objectSize > 0) {
        object.resize(objectSize);
        if (::BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0) {
            ok = true;
            for (;;) {
                DWORD read = 0;
                if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                                nullptr)) {
                    ok = false;
                    break;
                }
                if (read == 0) {
                    break;
                }
                if (::BCryptHashData(hash, buffer.data(), read, 0) != 0) {
                    ok = false;
                    break;
                }
            }
            if (ok && ::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                static const char *digits = "0123456789abcdef";
                hexLower->clear();
                for (unsigned char byte : digest) {
                    hexLower->push_back(digits[byte >> 4]);
                    hexLower->push_back(digits[byte & 0x0F]);
                }
            } else {
                ok = false;
            }
            ::BCryptDestroyHash(hash);
        }
    }
    if (algorithm != nullptr) {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    ::CloseHandle(file);
    return ok;
}

// ---------------------------------------------------------------------------
//  负载：定位 / 落盘 / 校验
// ---------------------------------------------------------------------------

bool readSelfFooter(const std::wstring &self, SfxFooter *footer, std::string *hexActual)
{
    HANDLE file = ::CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // 只读打开即可：安装包这个文件不需要写（原来用 GENERIC_READ|GENERIC_WRITE，
        // 在"文件被别人以只读方式占着"或"目录只读"时会直接失败）
        out("  [失败] 打不开自身文件（错误码 %lu）\n", static_cast<unsigned long>(::GetLastError()));
        return false;
    }

    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(file, &size) != FALSE;
    if (!ok) {
        outLine("  [失败] 读不到自身文件大小（GetFileSizeEx 失败）");
    }
    if (ok && size.QuadPart <= static_cast<LONGLONG>(sizeof(SfxFooter))) {
        outLine("  [失败] 文件太小，不可能带负载");
        ok = false;
    }
    if (ok) {
        LARGE_INTEGER position{};
        position.QuadPart = size.QuadPart - static_cast<LONGLONG>(sizeof(SfxFooter));
        DWORD read = 0;
        ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE
             && ::ReadFile(file, footer, sizeof(SfxFooter), &read, nullptr) != FALSE
             && read == sizeof(SfxFooter);
        if (!ok) {
            out("  [失败] 读取尾部失败（已读 %lu 字节，错误码 %lu）\n",
                static_cast<unsigned long>(read), static_cast<unsigned long>(::GetLastError()));
        }
    }

    if (ok && std::memcmp(footer->magic, kFooterMagic, 8) != 0) {
        outLine("[错误] 这个 exe 里没有 WinEase 负载（尾部标记不匹配）。"
                "请从 build\\dist 目录取安装包，不要手工拼接文件。");
        ok = false;
    }

    // 校验负载自身完整性：先算实际 SHA-256，再和尾部记的比
    if (ok) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(footer->cabOffset);
        ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN);
    }
    if (ok) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectSize = 0;
        DWORD ignored = 0;
        std::vector<unsigned char> object;
        std::vector<unsigned char> buffer(256 * 1024);
        unsigned char digest[32] = {0};
        bool hashed = false;
        if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
            && ::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                                   &ignored, 0)
                   == 0
            && objectSize > 0) {
            object.resize(objectSize);
            if (::BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0) {
                std::uint64_t remaining = footer->cabSize;
                hashed = true;
                while (remaining > 0) {
                    const DWORD want = static_cast<DWORD>(
                        remaining < buffer.size() ? remaining : buffer.size());
                    DWORD read = 0;
                    if (!::ReadFile(file, buffer.data(), want, &read, nullptr) || read == 0) {
                        hashed = false;
                        break;
                    }
                    if (::BCryptHashData(hash, buffer.data(), read, 0) != 0) {
                        hashed = false;
                        break;
                    }
                    remaining -= read;
                }
                if (hashed && ::BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                    static const char *digits = "0123456789abcdef";
                    for (unsigned char byte : digest) {
                        hexActual->push_back(digits[byte >> 4]);
                        hexActual->push_back(digits[byte & 0x0F]);
                    }
                } else {
                    hashed = false;
                }
                ::BCryptDestroyHash(hash);
            }
        }
        if (algorithm != nullptr) {
            ::BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        ok = hashed;
    }

    ::CloseHandle(file);
    return ok;
}

/// 把负载（CAB）写到临时文件，交给 cabinet.dll 解压
bool writePayloadToTempFile(const std::wstring &self, const SfxFooter &footer, std::wstring *cabPath,
                            std::string *error)
{
    wchar_t tempDirectory[MAX_PATH] = {0};
    if (::GetTempPathW(MAX_PATH, tempDirectory) == 0) {
        *error = "拿不到临时目录（GetTempPath 失败）";
        return false;
    }
    wchar_t tempFile[MAX_PATH] = {0};
    if (::GetTempFileNameW(tempDirectory, L"wes", 0, tempFile) == 0) {
        *error = "无法在临时目录创建文件（磁盘满或权限不足）";
        return false;
    }

    HANDLE source = ::CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE target = ::CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(footer.cabOffset);

    bool ok = source != INVALID_HANDLE_VALUE && target != INVALID_HANDLE_VALUE
              && ::SetFilePointerEx(source, position, nullptr, FILE_BEGIN);
    std::vector<unsigned char> buffer(256 * 1024);
    std::uint64_t remaining = ok ? footer.cabSize : 0;
    while (ok && remaining > 0) {
        const DWORD want =
            static_cast<DWORD>(remaining < buffer.size() ? remaining : buffer.size());
        DWORD read = 0;
        DWORD written = 0;
        ok = ::ReadFile(source, buffer.data(), want, &read, nullptr) && read > 0
             && ::WriteFile(target, buffer.data(), read, &written, nullptr) && written == read;
        remaining -= read;
    }

    if (source != INVALID_HANDLE_VALUE) {
        ::CloseHandle(source);
    }
    if (target != INVALID_HANDLE_VALUE) {
        ::CloseHandle(target);
    }
    if (!ok) {
        ::DeleteFileW(tempFile);
        *error = "写入临时 CAB 失败（磁盘空间不足？）";
        return false;
    }
    *cabPath = tempFile;
    return true;
}

// ---------------------------------------------------------------------------
//  cabinet.dll 解压（FDICopy）
// ---------------------------------------------------------------------------

struct ExtractContext {
    std::wstring destinationRoot;
    bool         silent = false;
    int          fileCount = 0;
    std::uint64_t totalBytes = 0;
    std::wstring currentPath; // 当前正在写的文件（回调里临时用）
};

// ---------------------------------------------------------------------------
//  cabinet 遍历回调（setupapi）
//
//  ⚠ 为什么用 SetupIterateCabinetW 而不是 cabinet.dll 的 FDI 那套：
//    FDICreate 在本机直接返回 NULL，而且**连一次内存分配回调都没调用**
//    （说明它在参数校验阶段就拒了），错误码是上一次调用残留的 183，
//    排查价值极低；SetupIterateCabinetW 是同一批系统组件里**文档齐全**的
//    "遍历 CAB" 接口，回调语义明确（改 FullTargetName 即改落点）。
//
//  一次回调 = CAB 里的一个文件（或一条通知），返回 FILEOP_DOIT 表示"就解到
//  我改过的 FullTargetName 去"，返回 FILEOP_SKIP 表示"这个跳过"。
// ---------------------------------------------------------------------------
UINT CALLBACK sfxCabinetCallback(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR param2)
{
    auto *extract = static_cast<ExtractContext *>(context);
    switch (notification) {
    case SPFILENOTIFY_FILEINCABINET: {
        auto *info = reinterpret_cast<FILE_IN_CABINET_INFO_W *>(param1);
        if (info == nullptr || info->NameInCabinet == nullptr) {
            return FILEOP_SKIP;
        }
        const std::wstring relative = info->NameInCabinet;
        const std::wstring target = joinPath(extract->destinationRoot, relative);
        if (target.size() + 1 > MAX_PATH) {
            // 超长路径：⛔ 不静默截断（截断会把文件写到别的地方去），如实报错
            out("  [失败] 路径过长，无法解出：%ls\n", relative.c_str());
            return FILEOP_ABORT;
        }
        if (!ensureDirectory(directoryOf(target))) {
            out("  [失败] 建不出目录：%ls\n", directoryOf(target).c_str());
            return FILEOP_ABORT;
        }
        std::wcsncpy(info->FullTargetName, target.c_str(), MAX_PATH - 1);
        info->FullTargetName[MAX_PATH - 1] = L'\0';

        ++extract->fileCount;
        extract->totalBytes += static_cast<std::uint64_t>(info->FileSize);
        if (!extract->silent) {
            out("  + %ls\n", relative.c_str());
        }
        return FILEOP_DOIT;
    }

    case SPFILENOTIFY_NEEDNEWCABINET:
        // 负载是单卷 CAB：出现这个通知说明包结构不对（或被拆过），必须报错
        out("  [失败] 负载被要求换下一卷 CAB（本包是单卷）\n");
        return FILEOP_ABORT;

    default:
        return NO_ERROR; // 其余通知放行（setupapi 的 0 即成功）
    }
}

bool extractPayload(const std::wstring &cabPath,
                    const std::wstring &destinationRoot,
                    bool silent,
                    ExtractContext *result,
                    std::string *error)
{
    ExtractContext context;
    context.destinationRoot = destinationRoot;
    context.silent = silent;

    context.fileCount = 0;
    context.totalBytes = 0;

    const BOOL ok = ::SetupIterateCabinetW(cabPath.c_str(), 0, sfxCabinetCallback, &context);
    if (!ok) {
        const DWORD code = ::GetLastError();
        char buffer[256] = {0};
        std::snprintf(buffer, sizeof(buffer),
                      "解压负载失败（错误码 %lu）。包可能损坏，请重新获取安装程序。",
                      static_cast<unsigned long>(code));
        *error = buffer;
    }

    if (result != nullptr) {
        *result = context;
    }
    return ok == TRUE;
}

bool extractTo(const std::wstring &self, const SfxFooter &footer, const std::wstring &destination,
               bool silent, ExtractContext *result)
{
    if (!ensureDirectory(destination)) {
        out("  [失败] 无法创建目录：%ls\n", destination.c_str());
        return false;
    }
    std::wstring cabPath;
    std::string error;
    if (!writePayloadToTempFile(self, footer, &cabPath, &error)) {
        out("  [失败] %s\n", error.c_str());
        return false;
    }
    const bool ok = extractPayload(cabPath, destination, silent, result, &error);
    ::DeleteFileW(cabPath.c_str());
    if (!ok) {
        out("  [失败] %s\n", error.c_str());
    }
    return ok;
}

// ---------------------------------------------------------------------------
//  清单（安装目录内 install.manifest.txt；负载里带 payload.manifest.txt）
// ---------------------------------------------------------------------------

struct ManifestEntry {
    std::string hash; // 小写十六进制
    std::string path; // 相对路径（反斜杠）
    std::uint64_t size = 0;
};

std::vector<ManifestEntry> readManifest(const std::wstring &file, std::string *error)
{
    std::vector<ManifestEntry> entries;
    HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = "清单文件读不到";
        return entries;
    }
    std::string content;
    std::vector<char> buffer(64 * 1024);
    DWORD read = 0;
    while (::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)
           && read > 0) {
        content.append(buffer.data(), read);
    }
    ::CloseHandle(handle);

    size_t position = 0;
    while (position < content.size()) {
        const size_t end = content.find('\n', position);
        std::string line = content.substr(position, (end == std::string::npos) ? std::string::npos
                                                                              : end - position);
        position = (end == std::string::npos) ? content.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue; // 注释行
        }
        // 格式：<sha256><空白><size><空白><相对路径>
        // ⚠ 空白按"一个或多个空格"处理：脚本生成时用的是**两个**空格，
        //   按单个空格切会把 size 当成路径的一部分（表现是"所有文件都缺失"，
        //   而且打印出来的"文件名"前面挂着一串数字 —— 这个 bug 就是这么露头的）。
        const size_t firstSpace = line.find(' ');
        if (firstSpace != 64) {
            continue;
        }
        ManifestEntry entry;
        entry.hash = line.substr(0, 64);

        size_t position = line.find_first_not_of(' ', firstSpace);
        if (position == std::string::npos) {
            continue;
        }
        const size_t sizeEnd = line.find(' ', position);
        entry.size = std::strtoull(line.substr(position, sizeEnd - position).c_str(), nullptr, 10);

        // 路径里不会有空格（本工程全部文件名都不带空格），所以剩下的整段就是路径
        position = (sizeEnd == std::string::npos) ? std::string::npos
                                                  : line.find_first_not_of(' ', sizeEnd);
        if (position == std::string::npos) {
            continue;
        }
        entry.path = line.substr(position);
        if (!entry.path.empty()) {
            entries.push_back(entry);
        }
    }
    return entries;
}

/// 从清单头部读 `# app-version=1.0.0`（找不到就返回空，调用方给个兜底）
std::string readManifestVersion(const std::wstring &manifestPath)
{
    HANDLE handle = ::CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    char buffer[512] = {0};
    DWORD read = 0;
    ::ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr);
    ::CloseHandle(handle);

    const std::string content(buffer, read);
    const std::string key = "# app-version=";
    const size_t position = content.find(key);
    if (position == std::string::npos) {
        return std::string();
    }
    const size_t start = position + key.size();
    const size_t end = content.find_first_of("\r\n", start);
    return content.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
}

bool verifyDirectory(const std::wstring &root, const std::vector<ManifestEntry> &entries,
                     bool silent)
{
    int mismatched = 0;
    int missing = 0;
    for (const ManifestEntry &entry : entries) {
        const std::wstring path =
            joinPath(root, std::wstring(entry.path.begin(), entry.path.end()));
        if (!fileExists(path)) {
            out("  [缺失] %s\n", entry.path.c_str());
            ++missing;
            continue;
        }
        std::string actual;
        if (!sha256OfFile(path, &actual) || actual != entry.hash) {
            out("  [不一致] %s\n", entry.path.c_str());
            ++mismatched;
            continue;
        }
        if (!silent) {
            out("  [OK] %s\n", entry.path.c_str());
        }
    }
    out("  校验完成：共 %zu 项，缺失 %d，内容不符 %d\n", entries.size(), missing, mismatched);
    return missing == 0 && mismatched == 0;
}

// ---------------------------------------------------------------------------
//  注册表（卸载项）与开始菜单快捷方式
// ---------------------------------------------------------------------------

bool writeRegistryString(HKEY root, const std::wstring &subKey, const wchar_t *name,
                         const std::wstring &value)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr)
        != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = ::RegSetValueExW(key, name, 0, REG_SZ,
                                            reinterpret_cast<const BYTE *>(value.c_str()), bytes);
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool writeRegistryDword(HKEY root, const std::wstring &subKey, const wchar_t *name, DWORD value)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr)
        != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = ::RegSetValueExW(key, name, 0, REG_DWORD,
                                            reinterpret_cast<const BYTE *>(&value), sizeof(value));
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

/// 写 HKCU 的"应用和功能"卸载项。⚠ 一律 HKCU：本安装器**不提权**
bool registerUninstallEntry(const std::wstring &installDirectory, const std::string &version)
{
    const std::wstring exe = joinPath(installDirectory, L"WinEase.exe");
    const std::wstring setup = exePath();
    const std::wstring versionWide(version.begin(), version.end());

    const bool ok =
        writeRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", L"WinEase 易用性增强工具集")
        && writeRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", versionWide)
        && writeRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", L"WinEase")
        && writeRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", installDirectory)
        && writeRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", exe)
        && writeRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString",
                               L"\"" + setup + L"\" --uninstall --dir \"" + installDirectory + L"\"")
        && writeRegistryDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1)
        && writeRegistryDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);
    // 说明：⚠ 这里**不注册任何 COM / 服务 / 驱动**。WinEase 的功能扩展（右键菜单
    // 等）由插件在启用时自己写 HKCU，卸载时自己清 —— 安装器只管"文件落盘"。
    return ok;
}

bool unregisterUninstallEntry()
{
    const LSTATUS status = ::RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

std::wstring startMenuShortcutPath()
{
    PWSTR folder = nullptr;
    if (::SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &folder) != S_OK || folder == nullptr) {
        return std::wstring();
    }
    std::wstring path = folder;
    ::CoTaskMemFree(folder);
    return joinPath(path, L"WinEase.lnk");
}

bool createStartMenuShortcut(const std::wstring &installDirectory)
{
    const std::wstring linkPath = startMenuShortcutPath();
    if (linkPath.empty()) {
        return false;
    }
    // ⚠ IShellLink 是 COM 组件：不初始化 COM 时 CoCreateInstance 直接失败
    //   （表现是"快捷方式创建失败"，其它一切正常 —— 很容易被当成权限问题）
    const HRESULT comStatus = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comReady = SUCCEEDED(comStatus);
    if (!comReady) {
        out("  [提示] COM 初始化失败（0x%08lX），跳过快捷方式\n",
            static_cast<unsigned long>(comStatus));
        return false;
    }

    IShellLinkW *shellLink = nullptr;
    if (::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                           reinterpret_cast<void **>(&shellLink))
        != S_OK) {
        return false;
    }
    bool ok = false;
    IPersistFile *persist = nullptr;
    if (shellLink->SetPath(joinPath(installDirectory, L"WinEase.exe").c_str()) == S_OK
        && shellLink->SetWorkingDirectory(installDirectory.c_str()) == S_OK
        && shellLink->SetDescription(L"WinEase 易用性增强工具集") == S_OK
        && shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist)) == S_OK) {
        ok = persist->Save(linkPath.c_str(), TRUE) == S_OK;
        persist->Release();
    }
    shellLink->Release();
    ::CoUninitialize();
    return ok;
}

bool removeStartMenuShortcut()
{
    const std::wstring linkPath = startMenuShortcutPath();
    if (linkPath.empty()) {
        return false;
    }
    return ::DeleteFileW(linkPath.c_str()) != FALSE || ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

// ---------------------------------------------------------------------------
//  安装 / 卸载
// ---------------------------------------------------------------------------

/// 目标机上有没有 .NET 8 运行时？
///
/// 为什么查文件而不是查注册表：`%ProgramFiles%\dotnet\shared\Microsoft.NETCore.App\8.x.y`
/// 是同一个事实的**更直接**的证据（注册表项在不同安装形态下会缺），而且不需要解析
/// MULTI_SZ。这里只负责"有没有 8.x"，具体版本号读目录名 —— 供安装时如实报出来。
bool detectDotNet8(std::wstring *version)
{
    wchar_t programFiles[MAX_PATH] = {0};
    if (::GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) == 0) {
        return false;
    }
    const std::wstring root =
        joinPath(joinPath(joinPath(programFiles, L"dotnet"), L"shared"), L"Microsoft.NETCore.App");
    const std::wstring pattern = joinPath(root, L"8.*");
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (version != nullptr) {
                *version = data.cFileName;
            }
            found = true;
            break;
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return found;
}

std::wstring defaultInstallDirectory()
{
    PWSTR folder = nullptr;
    if (::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder) != S_OK || folder == nullptr) {
        return std::wstring();
    }
    std::wstring path = folder;
    ::CoTaskMemFree(folder);
    // 装到 %LOCALAPPDATA%\Programs\WinEase：与"应用以普通权限运行"的定位一致，
    // 全程不需要 UAC（提权助手 WinEaseHelper 只在用户主动用到需管理员的动作时才拉起）
    return joinPath(joinPath(path, L"Programs"), kAppName);
}

int doInstall(const std::wstring &self, const SfxFooter &footer, const std::wstring &directory,
              bool silent, bool launch)
{
    out("正在解压到 %ls …\n", directory.c_str());
    ExtractContext context;
    if (!extractTo(self, footer, directory, silent, &context)) {
        return 2;
    }
    out("已解压 %d 个文件（%llu 字节）\n", context.fileCount,
        static_cast<unsigned long long>(context.totalBytes));

    // 落地清单：卸载时按它删文件（不去猜目录里有什么）
    {
        const std::wstring manifestInPayload = joinPath(directory, L"payload.manifest.txt");
        const std::wstring manifestCopy = joinPath(directory, L"install.manifest.txt");
        ::CopyFileW(manifestInPayload.c_str(), manifestCopy.c_str(), FALSE);
    }

    // 版本号取自负载清单头部（`# app-version=…`），写进注册表供"应用和功能"显示
    std::string version = readManifestVersion(joinPath(directory, L"payload.manifest.txt"));
    if (version.empty()) {
        version = "1.0.0";
    }

    // 关键依赖自检：这几样缺了就是"装了也用不了"，必须在装完的那一刻告诉用户
    struct Requirement {
        const wchar_t *file;
        const char    *why;
    };
    const Requirement requirements[] = {
        {L"WinEase.exe", "主程序"},
        {L"vcruntime140.dll", "VC++ 运行库（应用本地部署）"},
        {L"msvcp140.dll", "VC++ 标准库"},
        {L"platforms\\qwindows.dll", "Qt Windows 平台插件"},
    };
    int missingCritical = 0;
    for (const Requirement &requirement : requirements) {
        if (!fileExists(joinPath(directory, requirement.file))) {
            out("  [警告] 缺少 %ls（%s）—— 程序可能无法启动\n", requirement.file, requirement.why);
            ++missingCritical;
        }
    }

    const bool hasBridge = fileExists(joinPath(directory, L"WinEaseLiteMonitorBridge.dll"));
    const bool hasBundledRuntime = fileExists(joinPath(directory, L"coreclr.dll"));
    if (hasBridge) {
        // 桥接（硬件监控的温度类指标）需要 .NET 8。这里**主动查清楚再说话**：
        // 装了就说版本，没装就说清楚"少了什么功能"，而不是等用户点开面板看见"桥接组件不可用"。
        if (hasBundledRuntime) {
            outLine("[依赖] 硬件监控桥接：已随包附带 .NET 运行库（无需预装）");
        } else {
            std::wstring runtimeVersion;
            if (detectDotNet8(&runtimeVersion)) {
                out(" [依赖] 硬件监控桥接：检测到本机 .NET 运行时 %ls（温度类指标可用）\n",
                    runtimeVersion.c_str());
            } else {
                outLine("[依赖] 硬件监控桥接：**本机未检测到 .NET 8 运行时**。");
                outLine("       影响范围：只有「硬件监控」里的 CPU/主板/GPU 温度与风扇转速不可用"
                        "（面板会如实写原因）；");
                outLine("       其它功能（窗口/文件/输入/媒体/局域网传输等）完全不受影响。");
                outLine("       需要温度读数时：安装 .NET 8 桌面/控制台运行时"
                        "（https://dotnet.microsoft.com/download/dotnet/8.0），装完重启 WinEase 即可。");
            }
        }
    }

    const bool registered = registerUninstallEntry(directory, version);
    const bool shortcut = createStartMenuShortcut(directory);
    out("[注册] 卸载项：%s　开始菜单快捷方式：%s\n", registered ? "已写入（HKCU）" : "写入失败",
        shortcut ? "已创建" : "创建失败");

    if (missingCritical == 0) {
        outLine("安装完成。");
    } else {
        out("安装完成，但有 %d 项关键文件缺失（见上方警告）。\n", missingCritical);
    }

    if (launch) {
        const std::wstring exe = joinPath(directory, L"WinEase.exe");
        ::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, directory.c_str(), SW_SHOWNORMAL);
        outLine("已请求启动 WinEase。");
    }
    return missingCritical == 0 ? 0 : 3;
}

int doUninstall(const std::wstring &directory, bool silent)
{
    const std::wstring manifest = joinPath(directory, L"install.manifest.txt");
    std::string error;
    std::vector<ManifestEntry> entries = readManifest(manifest, &error);
    if (entries.empty()) {
        // 没有清单也要尽力清理：至少把注册项与快捷方式撤掉
        out("  [提示] %s（%ls）—— 无法按清单删除文件\n", error.c_str(), manifest.c_str());
    }

    int removed = 0;
    int failed = 0;
    for (const ManifestEntry &entry : entries) {
        const std::wstring path = joinPath(directory, std::wstring(entry.path.begin(), entry.path.end()));
        if (::DeleteFileW(path.c_str())) {
            ++removed;
        } else if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
            ++failed;
            if (!silent) {
                out("  [占用] 删不掉：%s\n", entry.path.c_str());
            }
        }
    }
    ::DeleteFileW(manifest.c_str());
    ::DeleteFileW(joinPath(directory, L"payload.manifest.txt").c_str());

    // 目录：从每个文件所在目录**由深到浅**逐级尝试删除。
    // ⚠ 只删 <目录>\plugins 和根目录是不够的：Qt 的插件目录（platforms / styles /
    //   imageformats / tls / iconengines / translations …）会整片留成空壳目录树
    //   —— 文件清干净了但装着"看起来没卸载"的一堆空文件夹。
    //   RemoveDirectory 对非空目录会失败，所以这里不用排序也不会误删。
    for (const ManifestEntry &entry : entries) {
        const std::wstring path =
            joinPath(directory, std::wstring(entry.path.begin(), entry.path.end()));
        std::wstring parent = directoryOf(path);
        while (parent.size() > directory.size()) {
            ::RemoveDirectoryW(parent.c_str());
            parent = directoryOf(parent);
        }
    }
    ::RemoveDirectoryW(joinPath(directory, L"plugins").c_str());
    ::RemoveDirectoryW(directory.c_str());

    const bool registry = unregisterUninstallEntry();
    const bool shortcut = removeStartMenuShortcut();
    out("已删除 %d 个文件（%d 个删不掉，可能正在运行），注册项 %s，快捷方式 %s\n", removed, failed,
        registry ? "已移除" : "移除失败", shortcut ? "已移除" : "移除失败");
    if (failed > 0) {
        outLine("⚠ 请先退出 WinEase 再重试卸载（被占用的文件删不掉）。");
    }
    return failed == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
//  --selftest：证明"桥接 + .NET 运行库"在目标机上真的能用
// ---------------------------------------------------------------------------

using FnOpen = void *(*)();
using FnClose = void (*)(void *);
using FnRefresh = int (*)(void *);
using FnCount = int (*)(void *);
using FnRuntimeInfo = int (*)(wchar_t *, int);

int doSelfTest(const std::wstring &directory)
{
    const std::wstring bridge = joinPath(directory, L"WinEaseLiteMonitorBridge.dll");
    if (!fileExists(bridge)) {
        out("  [失败] 找不到 %ls\n", bridge.c_str());
        return 2;
    }

    // ⚠ 用 LoadLibraryEx + LOAD_WITH_ALTERED_SEARCH_PATH：让桥接 DLL 从它自己所在目录
    //   去找托管依赖与 .NET 运行库（自包含部署时运行库就在旁边）
    HMODULE module = ::LoadLibraryExW(bridge.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        out("  [失败] 桥接 DLL 加载失败（错误码 %lu）。多半是 .NET 8 运行库没带全。\n",
            static_cast<unsigned long>(::GetLastError()));
        return 2;
    }

    auto open = reinterpret_cast<FnOpen>(::GetProcAddress(module, "we_lm_open"));
    auto close = reinterpret_cast<FnClose>(::GetProcAddress(module, "we_lm_close"));
    auto refresh = reinterpret_cast<FnRefresh>(::GetProcAddress(module, "we_lm_refresh"));
    auto count = reinterpret_cast<FnCount>(::GetProcAddress(module, "we_lm_sensor_count"));
    auto info = reinterpret_cast<FnRuntimeInfo>(::GetProcAddress(module, "we_lm_runtime_info"));
    if (open == nullptr || close == nullptr || refresh == nullptr || count == nullptr) {
        outLine("  [失败] 桥接 DLL 缺少导出函数（版本不匹配）");
        ::FreeLibrary(module);
        return 2;
    }

    wchar_t buffer[512] = {0};
    if (info != nullptr) {
        info(buffer, 512);
        out("  桥接运行时：%ls\n", buffer);
    }
    void *session = open();
    if (session == nullptr) {
        outLine("  [失败] 无法建立硬件会话（.NET 运行库不完整）");
        ::FreeLibrary(module);
        return 2;
    }
    const int sensors = refresh(session);
    const int total = count(session);
    close(session);
    ::FreeLibrary(module);

    if (sensors < 0 || total <= 0) {
        out("  [失败] 桥接层没枚举到任何传感器（refresh=%d，count=%d）\n", sensors, total);
        return 2;
    }
    out("  [通过] 桥接层可用，枚举到 %d 个传感器\n", total);
    return 0;
}

void printUsage()
{
    outLine("WinEase 自解压安装器");
    outLine("");
    outLine("  winease-setup.exe                          安装到 %LOCALAPPDATA%\\Programs\\WinEase");
    outLine("  winease-setup.exe --dir <目录> [--launch]  指定安装目录，可选装完即启动");
    outLine("  winease-setup.exe --verify                 解到临时目录并按清单校验 SHA-256（只读）");
    outLine("  winease-setup.exe --selftest --dir <目录>  校验桥接与 .NET 运行库是否可用");
    outLine("  winease-setup.exe --list                   列出负载清单");
    outLine("  winease-setup.exe --uninstall --dir <目录> 卸载（删文件 + 注册项 + 快捷方式）");
    outLine("  --silent                                   安静模式（不逐文件打印）");
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    ::SetConsoleOutputCP(CP_UTF8); // 源码是 UTF-8：设了它中文才不会变乱码

    bool verify = false;
    bool selfTest = false;
    bool uninstall = false;
    bool list = false;
    bool silent = false;
    bool launch = false;
    std::wstring directory;

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--verify") {
            verify = true;
        } else if (argument == L"--selftest") {
            selfTest = true;
        } else if (argument == L"--uninstall") {
            uninstall = true;
        } else if (argument == L"--list") {
            list = true;
        } else if (argument == L"--silent") {
            silent = true;
        } else if (argument == L"--launch") {
            launch = true;
        } else if (argument == L"--dir" && i + 1 < argc) {
            directory = argv[++i];
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            printUsage();
            return 0;
        } else {
            out("未知参数：%ls\n", argument.c_str());
            printUsage();
            return 1;
        }
    }

    const std::wstring self = exePath();
    if (self.empty()) {
        outLine("[错误] 取不到自身路径");
        return 1;
    }

    SfxFooter footer{};
    std::string cabHash;
    if (!readSelfFooter(self, &footer, &cabHash)) {
        outLine("[错误] 无法读取负载信息");
        return 1;
    }
    const std::string expectedHash(footer.cabSha256, 64);
    if (!cabHash.empty() && cabHash != expectedHash) {
        out("  [错误] 负载校验失败！\n    期望 %s\n    实际 %s\n", expectedHash.c_str(),
            cabHash.c_str());
        outLine("  安装程序可能损坏（下载不完整 / 被篡改 / 被解压工具改写过），请重新获取。");
        return 1;
    }
    out("负载校验通过（SHA-256 %s…，%llu 字节）\n", expectedHash.substr(0, 16).c_str(),
        static_cast<unsigned long long>(footer.cabSize));

    // ---- 只读模式：--list / --verify ----
    if (list || verify) {
        const std::wstring temporary = joinPath(
            [&] {
                wchar_t tempDirectory[MAX_PATH] = {0};
                ::GetTempPathW(MAX_PATH, tempDirectory);
                return std::wstring(tempDirectory);
            }(),
            L"WinEaseSfxVerify");
        ::RemoveDirectoryW(temporary.c_str()); // 上一次的残留
        out("解到临时目录：%ls\n", temporary.c_str());
        ExtractContext context;
        if (!extractTo(self, footer, temporary, silent, &context)) {
            return 2;
        }
        std::string error;
        const std::vector<ManifestEntry> entries =
            readManifest(joinPath(temporary, L"payload.manifest.txt"), &error);
        if (entries.empty()) {
            out("  [失败] 负载里没有清单文件：%s\n", error.c_str());
            return 2;
        }
        if (list) {
            for (const ManifestEntry &entry : entries) {
                out("%12llu  %s  %s\n", static_cast<unsigned long long>(entry.size),
                    entry.hash.substr(0, 16).c_str(), entry.path.c_str());
            }
            out("共 %zu 项\n", entries.size());
        }
        int code = 0;
        if (verify) {
            code = verifyDirectory(temporary, entries, silent) ? 0 : 2;
        }
        // 清场：先删文件再删目录（目录树按清单里的路径逐层删）
        for (const ManifestEntry &entry : entries) {
            const std::wstring path = joinPath(temporary, std::wstring(entry.path.begin(), entry.path.end()));
            ::DeleteFileW(path.c_str());
            std::wstring parent = directoryOf(path);
            while (parent.size() > temporary.size()) {
                ::RemoveDirectoryW(parent.c_str());
                parent = directoryOf(parent);
            }
        }
        ::DeleteFileW(joinPath(temporary, L"payload.manifest.txt").c_str());
        ::RemoveDirectoryW(temporary.c_str());
        return code;
    }

    // ---- 自检：证明桥接运行时可用 ----
    if (selfTest) {
        if (directory.empty()) {
            directory = defaultInstallDirectory();
        }
        out("桥接自检目录：%ls\n", directory.c_str());
        return doSelfTest(directory);
    }

    if (directory.empty()) {
        directory = defaultInstallDirectory();
        if (directory.empty()) {
            outLine("[错误] 取不到默认安装目录（%LOCALAPPDATA% 不可用）");
            return 1;
        }
    }

    if (uninstall) {
        return doUninstall(directory, silent);
    }
    return doInstall(self, footer, directory, silent, launch);
}
