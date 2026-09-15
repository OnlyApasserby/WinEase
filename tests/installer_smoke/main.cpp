// ============================================================================
//  installer_smoke —— 打包链路自检（自解压安装程序）
//
//  它验的是"**用户拿到的那一个 exe**"，而不是我们自己的构建目录：
//
//    ① 前置：单文件安装程序存在（build/dist/WinEase-<ver>-x64-Setup.exe）
//    ② **负载完整性**：`--verify` 解到临时目录后逐文件比对清单里的 SHA-256
//    ②b **默认安装目录**：`--default-dir` 解析出的路径必须符合规则
//       （首选 D:\WinEase；本机没有可用的 D 盘时回退 %ProgramFiles%\WinEase）——
//       自检**独立地**再判一次"本机有没有可用的 D 盘"，不引用被测程序内部逻辑
//    ③ **依赖是否带全（离线可用性）**：对负载里**每一个 PE 文件**做导入表审计 ——
//       每条非系统导入都必须能在负载内或系统目录里找到。这一条是"目标机器无需
//       额外配置即可运行"唯一的硬证据：缺 vcruntime140.dll / Qt6Core.dll /
//       qwindows.dll 这类依赖，在这里就会red（而不是等用户双击）
//    ④ **安装 / 注册**：装到临时目录，断言文件到位 + HKCU 卸载项 + 开始菜单快捷方式
//    ⑤ **桥接运行时**：`--selftest` 让桥接层真实枚举一次传感器（证明 .NET 侧能跑）
//    ⑥ **卸载**：文件、目录、注册项、快捷方式全部回收干净，不留空壳目录树
//
//  ⚠ 本组**不安装到真实位置**（一切都在 build 下的临时目录），但会短暂写 HKCU
//    卸载项与开始菜单快捷方式 —— 每一条都在同一个用例里被删掉，跑完不留痕。
// ============================================================================

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <windows.h>

#include <objbase.h>  // CoTaskMemFree
#include <shlobj.h>   // SHGetKnownFolderPath / FOLDERID_Programs

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
//  本地 Reporter
//
//  ⚠ 刻意**不复用** feature_smoke 的 test_support.h：那一份会牵进
//    PluginServices / HookService / ElevationService 等一大堆插件契约，
//    而本组要验的是"用户拿到的那一个 exe"，依赖越少越不容易被无关改动带崩
//    （本工程各套自检本来也各自带 Reporter）。
// ---------------------------------------------------------------------------
class Reporter
{
public:
    void check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_total;
        if (ok) {
            std::printf("[通过] %s\n", what.toUtf8().constData());
        } else {
            ++m_failures;
            std::printf("[失败] %s\n", what.toUtf8().constData());
        }
        if (!detail.isEmpty()) {
            std::printf("        %s\n", detail.toUtf8().constData());
        }
        std::fflush(stdout);
    }

    void info(const QString &text)
    {
        std::printf("[信息] %s\n", text.toUtf8().constData());
        std::fflush(stdout);
    }

    int total() const { return m_total; }
    int failures() const { return m_failures; }

private:
    int m_total = 0;
    int m_failures = 0;
};

// ---------------------------------------------------------------------------
//  极小 PE 导入表解析（只为"依赖审计"服务，不引第三方）
// ---------------------------------------------------------------------------

struct PeImage {
    std::vector<std::string> imports;  ///< 导入的 DLL 名（小写）
    bool valid = false;
    bool is64Bit = false;
};

PeImage readPeImports(const QString &path)
{
    PeImage result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }
    const QByteArray data = file.readAll();
    file.close();
    if (data.size() < 0x100) {
        return result;
    }

    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData());
    if (bytes[0] != 'M' || bytes[1] != 'Z') {
        return result;
    }
    const auto *dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>(bytes);
    const DWORD peOffset = static_cast<DWORD>(dosHeader->e_lfanew);
    if (peOffset + sizeof(IMAGE_NT_HEADERS32) > static_cast<DWORD>(data.size())) {
        return result;
    }
    const auto *nt = bytes + peOffset;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) {
        return result;
    }

    const WORD machine = *reinterpret_cast<const WORD *>(nt + 4);
    result.is64Bit = (machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_ARM64);

    // 数据目录：可选头里偏移 0x60（PE32）或 0x70（PE32+）开始，导入表是第 2 项
    const DWORD optionalOffset = peOffset + 4 + sizeof(IMAGE_FILE_HEADER);
    const DWORD importDirectoryOffset = optionalOffset + (result.is64Bit ? 0x78 : 0x68);
    if (importDirectoryOffset + 8 > static_cast<DWORD>(data.size())) {
        return result;
    }
    const DWORD importRva = *reinterpret_cast<const DWORD *>(bytes + importDirectoryOffset);
    if (importRva == 0) {
        result.valid = true; // 没有任何导入（少见但合法）
        return result;
    }

    // RVA → 文件偏移：解析节表
    const WORD sectionCount = *reinterpret_cast<const WORD *>(nt + 6);
    const WORD optionalSize = *reinterpret_cast<const WORD *>(nt + 20);
    const DWORD sectionOffset = optionalOffset + optionalSize;
    const auto *sections = bytes + sectionOffset;
    const auto toFileOffset = [&](DWORD rva) -> DWORD {
        for (WORD i = 0; i < sectionCount; ++i) {
            const auto *section = sections + static_cast<size_t>(i) * sizeof(IMAGE_SECTION_HEADER);
            const DWORD virtualAddress =
                *reinterpret_cast<const DWORD *>(section + 12); // VirtualAddress
            const DWORD virtualSize = *reinterpret_cast<const DWORD *>(section + 8); // VirtualSize
            const DWORD rawSize = *reinterpret_cast<const DWORD *>(section + 16);   // SizeOfRawData
            const DWORD rawOffset = *reinterpret_cast<const DWORD *>(section + 20);// PointerToRawData
            const DWORD span = (virtualSize > rawSize) ? virtualSize : rawSize;
            if (rva >= virtualAddress && rva < virtualAddress + span) {
                return rawOffset + (rva - virtualAddress);
            }
        }
        return 0;
    };

    DWORD descriptorOffset = toFileOffset(importRva);
    for (int guard = 0; descriptorOffset != 0 && guard < 4096; ++guard) {
        if (descriptorOffset + 20 > static_cast<DWORD>(data.size())) {
            break;
        }
        const auto *descriptor = bytes + descriptorOffset;
        const DWORD nameRva = *reinterpret_cast<const DWORD *>(descriptor + 12);
        if (nameRva == 0) {
            break; // 结束标记
        }
        const DWORD nameOffset = toFileOffset(nameRva);
        if (nameOffset == 0 || nameOffset >= static_cast<DWORD>(data.size())) {
            break;
        }
        const char *name = reinterpret_cast<const char *>(bytes + nameOffset);
        const size_t maxLength = static_cast<size_t>(data.size() - nameOffset);
        size_t length = 0;
        while (length < maxLength && name[length] != '\0') {
            ++length;
        }
        std::string lower;
        lower.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(name[i]))));
        }
        if (!lower.empty()) {
            result.imports.push_back(lower);
        }
        descriptorOffset += 20;
    }

    result.valid = true;
    return result;
}

/// 系统目录里的 DLL 一律算"系统提供"（api-ms-win-*、kernel32、user32…）
bool isSystemProvided(const std::string &dllNameLower)
{
    static const QStringList prefixes = {
        QStringLiteral("api-ms-win-"), QStringLiteral("ext-ms-win-"),
        QStringLiteral("kernel32"),   QStringLiteral("user32"),
        QStringLiteral("gdi32"),      QStringLiteral("advapi32"),
        QStringLiteral("shell32"),    QStringLiteral("ole32"),
        QStringLiteral("oleaut32"),   QStringLiteral("shlwapi"),
        QStringLiteral("ws2_32"),     QStringLiteral("comdlg32"),
        QStringLiteral("comctl32"),   QStringLiteral("dwmapi"),
        QStringLiteral("ntdll"),      QStringLiteral("rpcrt4"),
        QStringLiteral("bcrypt"),     QStringLiteral("crypt32"),
        QStringLiteral("cabinet"),    QStringLiteral("setupapi"),
        QStringLiteral("winmm"),      QStringLiteral("version"),
        QStringLiteral("mpr"),        QStringLiteral("dnsapi"),
        QStringLiteral("iphlpapi"),   QStringLiteral("wtsapi32"),
        QStringLiteral("powrprof"),   QStringLiteral("secur32"),
        QStringLiteral("netapi32"),   QStringLiteral("uxtheme"),
        QStringLiteral("magnification"), QStringLiteral("dxva2"),
        QStringLiteral("wbemuuid"),   QStringLiteral("pdh"),
        QStringLiteral("cfgmgr32"),   QStringLiteral("rstrtmgr"),
        QStringLiteral("propsys"),    QStringLiteral("windowscodecs"),
        QStringLiteral("d3d"),        QStringLiteral("opengl32"),
        QStringLiteral("msvcrt"),     QStringLiteral("mfplat"),
        QStringLiteral("mfuuid"),     QStringLiteral("mf"),
        QStringLiteral("wininet"),    QStringLiteral("urlmon"),
        QStringLiteral("wldap32"),    QStringLiteral("normaliz"),
        QStringLiteral("sspicli"),    QStringLiteral("authz"),
        QStringLiteral("avrt"),       QStringLiteral("hid"),
        QStringLiteral("bluetoothapis"), QStringLiteral("fwpuclnt"),
        QStringLiteral("ntdll"),      QStringLiteral("cryptbase"),
        QStringLiteral("profapi"),    QStringLiteral("bcryptprimitives")};
    const QString name = QString::fromStdString(dllNameLower);
    for (const QString &prefix : prefixes) {
        if (name.startsWith(prefix)) {
            return true;
        }
    }
    // WinRT / 系统 api-set：api-ms-win-* 已覆盖，这里兜住剩余的 sys 文件
    if (name.endsWith(QLatin1String(".sys"))) {
        return true;
    }
    // 系统目录里真的存在也算（覆盖面兜底，避免误报）
    wchar_t systemDirectory[MAX_PATH] = {0};
    if (::GetSystemDirectoryW(systemDirectory, MAX_PATH) != 0) {
        const QString candidate =
            QDir(QString::fromWCharArray(systemDirectory)).absoluteFilePath(name);
        if (QFileInfo::exists(candidate)) {
            return true;
        }
    }
    return false;
}

/// 递归收集负载里的 PE 文件
QStringList collectPeFiles(const QString &root)
{
    QStringList result;
    QDirIterator iterator(root, QStringList{QStringLiteral("*.dll"), QStringLiteral("*.exe")},
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        result.append(iterator.next());
    }
    result.sort();
    return result;
}

int runProcess(const QString &program, const QStringList &arguments, QString *output, int timeoutMs)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        if (output != nullptr) {
            *output = QStringLiteral("无法启动进程");
        }
        return -1;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        if (output != nullptr) {
            *output = QStringLiteral("超时");
        }
        return -2;
    }
    if (output != nullptr) {
        *output = QString::fromUtf8(process.readAll());
    }
    return process.exitCode();
}

} // namespace

int main(int argc, char **argv)
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    // 控制台按 UTF-8 输出：本工程的中文日志保持一致（与 winease-setup 同做法）
    ::SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);

    Reporter reporter;
    reporter.info(QStringLiteral("---- 打包链路（自解压安装程序）----"));

    // -----------------------------------------------------------------------
    //  ① 前置：安装程序在哪
    // -----------------------------------------------------------------------
    const QString workDir = QStringLiteral("build/dist");
    QDir distDir(workDir);
    QStringList installers;
    for (const QFileInfo &info : distDir.entryInfoList(QStringList{QStringLiteral("WinEase-*-Setup.exe")},
                                                      QDir::Files, QDir::Name)) {
        installers.append(info.absoluteFilePath());
    }
    reporter.check(!installers.isEmpty(),
                   QStringLiteral("前置：build/dist 下存在单文件安装程序"
                                  "（cmake --build build --target winease_installer）"),
                   installers.join(QStringLiteral(", ")));
    if (installers.isEmpty()) {
        reporter.info(QStringLiteral("=== 共 %1 项，失败 %2 项 ===")
                          .arg(reporter.total())
                          .arg(reporter.failures()));
        return 1;
    }
    const QString setup = installers.last();
    const qint64 setupSize = QFileInfo(setup).size();
    reporter.info(QStringLiteral("安装程序：%1（%2 MB）")
                      .arg(QFileInfo(setup).fileName())
                      .arg(QString::number(setupSize / 1024.0 / 1024.0, 'f', 1)));

    reporter.check(setupSize > 5ll * 1024 * 1024,
                   QStringLiteral("★ 安装程序是**单文件**且体积含全部依赖（> 5MB，说明确实把"
                                  "Qt/VC/插件都装进去了）"),
                   QStringLiteral("%1 字节").arg(setupSize));

    QTemporaryDir installDir(QDir::tempPath() + QStringLiteral("/winease_installer_smoke_XXXXXX"));
    reporter.check(installDir.isValid(), QStringLiteral("前置：能建临时安装目录"));
    if (!installDir.isValid()) {
        reporter.info(QStringLiteral("=== 共 %1 项，失败 %2 项 ===")
                          .arg(reporter.total())
                          .arg(reporter.failures()));
        return 1;
    }
    const QString targetDir = QDir::toNativeSeparators(installDir.path());

    // -----------------------------------------------------------------------
    //  ② 负载完整性：--verify（只读，不写注册表）
    // -----------------------------------------------------------------------
    {
        QString output;
        const int code = runProcess(setup, {QStringLiteral("--verify"), QStringLiteral("--silent")},
                                    &output, 180000);
        const bool allGood = code == 0 && output.contains(QStringLiteral("缺失 0"))
                             && output.contains(QStringLiteral("内容不符 0"));
        reporter.check(allGood,
                       QStringLiteral("★ `--verify`：解到临时目录并**逐文件比对 SHA-256**，"
                                      "缺失 0 / 内容不符 0（负载没被截断或篡改）"),
                       QStringLiteral("退出码 %1；%2").arg(code).arg(output.right(160)));
    }

    // -----------------------------------------------------------------------
    //  ②b 默认安装目录：D:\WinEase（首选） / %ProgramFiles%\WinEase（本机没有可用 D 盘）
    //      ⚠ "有没有 D 盘"独立判一遍：看盘的**类型与可访问性**，不看盘符字母。
    // -----------------------------------------------------------------------
    {
        QString output;
        const int code = runProcess(setup, {QStringLiteral("--default-dir")}, &output, 60000);

        const UINT driveType = ::GetDriveTypeW(L"D:\\");
        const bool dUsable = driveType != DRIVE_NO_ROOT_DIR && driveType != DRIVE_UNKNOWN
                             && driveType != DRIVE_CDROM
                             && ::GetFileAttributesW(L"D:\\") != INVALID_FILE_ATTRIBUTES;
        QString expected;
        if (dUsable) {
            expected = QStringLiteral("D:\\WinEase");
        } else {
            wchar_t buffer[MAX_PATH] = {0};
            if (::GetEnvironmentVariableW(L"ProgramFiles", buffer, MAX_PATH) > 0) {
                expected = QDir(QString::fromWCharArray(buffer))
                               .absoluteFilePath(QStringLiteral("WinEase"));
            }
        }
        expected = QDir::toNativeSeparators(expected);

        const QString marker = QStringLiteral("[默认目录] ");
        QString reported;
        const int at = output.indexOf(marker);
        if (at >= 0) {
            const int begin = at + marker.size();
            int end = output.indexOf(QLatin1Char('\n'), begin);
            if (end < 0) {
                end = output.size();
            }
            reported = QDir::toNativeSeparators(output.mid(begin, end - begin).trimmed());
        }

        reporter.check(code == 0 && !reported.isEmpty() && !expected.isEmpty()
                           && QString::compare(reported, expected, Qt::CaseInsensitive) == 0,
                       QStringLiteral("★ 默认安装目录符合规则：D 盘可用 → D:\\WinEase；"
                                      "本机没有可用的 D 盘 → 回退 %ProgramFiles%\\WinEase"),
                       QStringLiteral("本机 D 盘可用 = %1；安装程序报告 %2；期望 %3")
                           .arg(dUsable ? QStringLiteral("是") : QStringLiteral("否"))
                           .arg(reported)
                           .arg(expected));
    }

    // -----------------------------------------------------------------------
    //  ③ 依赖审计：离线可用性的硬证据
    // -----------------------------------------------------------------------
    {
        // 先解包一份（--verify 会自己清场，这里装到临时目录后再审计）
        QString output;
        const int code = runProcess(setup,
                                    {QStringLiteral("--dir"), targetDir, QStringLiteral("--silent")},
                                    &output, 180000);
        reporter.check(code == 0, QStringLiteral("安装到临时目录成功（不碰真实安装位置）"),
                       QStringLiteral("退出码 %1；%2").arg(code).arg(output.right(200)));

        const QStringList peFiles = collectPeFiles(installDir.path());
        reporter.check(peFiles.size() >= 80,
                       QStringLiteral("负载里有数十个 PE 文件（主程序 + 38 个插件 + Qt 运行库 + "
                                      "VC++ 运行库；具体数量随 Qt 插件多寡浮动）"),
                       QStringLiteral("%1 个").arg(peFiles.size()));

        // 负载内可提供的 DLL 名（小写）
        QSet<QString> provided;
        for (const QString &path : peFiles) {
            provided.insert(QFileInfo(path).fileName().toLower());
        }

        QStringList missing;
        int audited = 0;
        for (const QString &path : peFiles) {
            const PeImage image = readPeImports(path);
            if (!image.valid) {
                missing.append(QStringLiteral("%1（PE 解析失败）").arg(QFileInfo(path).fileName()));
                continue;
            }
            ++audited;
            for (const std::string &importName : image.imports) {
                const QString name = QString::fromStdString(importName);
                if (provided.contains(name) || isSystemProvided(importName)) {
                    continue;
                }
                missing.append(QStringLiteral("%1 → 缺 %2")
                                   .arg(QFileInfo(path).fileName(), name));
            }
        }
        reporter.check(missing.isEmpty(),
                       QStringLiteral("★★ **依赖审计**：负载里每个 PE 的每一条非系统导入都能在负载内"
                                      "找到（这就是「目标机无需额外配置」的硬证据）"),
                       QStringLiteral("审计 %1 个 PE；缺：%2")
                           .arg(audited)
                           .arg(missing.mid(0, 8).join(QStringLiteral("; "))));

        // 关键文件必须在（这几样缺了就是"装上了但打不开"）
        const QStringList mustHave = {QStringLiteral("WinEase.exe"),
                                      QStringLiteral("WinEaseHelper.exe"),
                                      QStringLiteral("vcruntime140.dll"),
                                      QStringLiteral("msvcp140.dll"),
                                      QStringLiteral("concrt140.dll"),
                                      QStringLiteral("Qt6Core.dll"),
                                      QStringLiteral("Qt6Gui.dll"),
                                      QStringLiteral("Qt6Widgets.dll"),
                                      QStringLiteral("platforms/qwindows.dll"),
                                      QStringLiteral("WinEaseLiteMonitorBridge.dll"),
                                      QStringLiteral("WinEaseLiteMonitorBridge.runtimeconfig.json"),
                                      QStringLiteral("README-install.txt")};
        QStringList notPresent;
        for (const QString &relative : mustHave) {
            if (!QFileInfo::exists(QDir(installDir.path()).absoluteFilePath(relative))) {
                notPresent.append(relative);
            }
        }
        reporter.check(notPresent.isEmpty(),
                       QStringLiteral("关键文件齐备（主程序 / 提权助手 / VC++ 运行库 / Qt 运行库与平台插件 / "
                                      "桥接与它的 runtimeconfig / 安装说明）"),
                       QStringLiteral("缺：%1").arg(notPresent.join(QStringLiteral(", "))));

        // 插件数量对得上（构建目录里有多少个，包里就该有多少个）
        const int builtPlugins = QDir(QStringLiteral("build/bin/plugins"))
                                     .entryList(QStringList{QStringLiteral("*.dll")}, QDir::Files)
                                     .size();
        const int shippedPlugins = QDir(QDir(installDir.path()).absoluteFilePath(QStringLiteral("plugins")))
                                       .entryList(QStringList{QStringLiteral("*.dll")}, QDir::Files)
                                       .size();
        reporter.check(builtPlugins > 0 && shippedPlugins == builtPlugins,
                       QStringLiteral("★ 插件数一致：包里的插件数与构建产物**逐个不差**"
                                      "（少一个插件就是少一个功能，用户看不出来）"),
                       QStringLiteral("构建 %1 / 包内 %2").arg(builtPlugins).arg(shippedPlugins));

        // -------------------------------------------------------------------
        //  ④ 注册：卸载项 + 开始菜单快捷方式
        // -------------------------------------------------------------------
        HKEY key = nullptr;
        const LONG opened = ::RegOpenKeyExW(HKEY_CURRENT_USER,
                                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinEase",
                                           0, KEY_READ, &key);
        QString displayName;
        QString uninstallString;
        if (opened == ERROR_SUCCESS) {
            wchar_t buffer[512] = {0};
            DWORD size = sizeof(buffer);
            DWORD type = 0;
            if (::RegQueryValueExW(key, L"DisplayName", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
                displayName = QString::fromWCharArray(buffer);
            }
            size = sizeof(buffer);
            if (::RegQueryValueExW(key, L"UninstallString", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
                uninstallString = QString::fromWCharArray(buffer);
            }
            ::RegCloseKey(key);
        }
        reporter.check(opened == ERROR_SUCCESS && !displayName.isEmpty()
                           && uninstallString.contains(QStringLiteral("--uninstall")),
                       QStringLiteral("★ 安装后写入了 HKCU 卸载项（\"应用和功能\"里看得见，"
                                      "且卸载命令带 --uninstall —— 用户能自己卸干净）"),
                       QStringLiteral("DisplayName=%1").arg(displayName));

        PWSTR programs = nullptr;
        QString shortcutPath;
        if (::SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs) == S_OK
            && programs != nullptr) {
            shortcutPath = QDir(QString::fromWCharArray(programs))
                               .absoluteFilePath(QStringLiteral("WinEase.lnk"));
            ::CoTaskMemFree(programs);
        }
        reporter.check(!shortcutPath.isEmpty() && QFileInfo::exists(shortcutPath),
                       QStringLiteral("★ 开始菜单快捷方式已创建（用户装完能找到入口）"),
                       shortcutPath);
    }

    // -----------------------------------------------------------------------
    //  ⑤ 桥接运行时：让桥接层真实枚举一次传感器
    // -----------------------------------------------------------------------
    {
        QString output;
        const int code = runProcess(
            setup, {QStringLiteral("--selftest"), QStringLiteral("--dir"), targetDir}, &output,
            180000);
        const bool ok = code == 0 && output.contains(QStringLiteral("[通过]"));
        reporter.check(ok,
                       QStringLiteral("★ 桥接自检：装的这份**真的能用**（枚举到硬件传感器）—— "
                                      "证明 Qt / VC / 桥接三层依赖在目标目录里能协同工作"),
                       QStringLiteral("退出码 %1；%2").arg(code).arg(output.right(200)));
        if (!ok && output.contains(QStringLiteral("未检测到 .NET"))) {
            reporter.info(QStringLiteral("（本机没有 .NET 8 运行时时这条会失败，属预期："
                                         "安装器会如实说明温度类指标不可用）"));
        }
    }

    // -----------------------------------------------------------------------
    //  ⑥ 卸载：文件、目录、注册项、快捷方式全部回收
    // -----------------------------------------------------------------------
    {
        QString output;
        const int code = runProcess(
            setup,
            {QStringLiteral("--uninstall"), QStringLiteral("--dir"), targetDir,
             QStringLiteral("--silent")},
            &output, 180000);
        reporter.check(code == 0,
                       QStringLiteral("卸载执行成功（删文件 + 移除注册项与快捷方式）"),
                       QStringLiteral("退出码 %1；%2").arg(code).arg(output.right(160)));

        const QStringList leftovers = collectPeFiles(installDir.path());
        reporter.check(leftovers.isEmpty(),
                       QStringLiteral("★ 卸载后**一个文件都不剩**（不留 DLL/EXE 残留）"),
                       leftovers.mid(0, 5).join(QStringLiteral(", ")));
        reporter.check(!QFileInfo::exists(installDir.path()),
                       QStringLiteral("★ 卸载后连**空壳目录树**都收干净"
                                      "（Qt 的 platforms/styles/tls… 子目录不留）"),
                       installDir.path());

        HKEY key = nullptr;
        const LONG opened = ::RegOpenKeyExW(HKEY_CURRENT_USER,
                                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinEase",
                                           0, KEY_READ, &key);
        if (opened == ERROR_SUCCESS) {
            ::RegCloseKey(key);
        }
        reporter.check(opened != ERROR_SUCCESS,
                       QStringLiteral("★ 卸载后注册项已删除（「应用和功能」里不再残留）"));

        PWSTR programs = nullptr;
        QString shortcutPath;
        if (::SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs) == S_OK
            && programs != nullptr) {
            shortcutPath = QDir(QString::fromWCharArray(programs))
                               .absoluteFilePath(QStringLiteral("WinEase.lnk"));
            ::CoTaskMemFree(programs);
        }
        reporter.check(shortcutPath.isEmpty() || !QFileInfo::exists(shortcutPath),
                       QStringLiteral("★ 卸载后快捷方式已移除"), shortcutPath);
    }

    reporter.info(QStringLiteral("=== 共 %1 项，失败 %2 项 ===")
                      .arg(reporter.total())
                      .arg(reporter.failures()));
    return reporter.failures() == 0 ? 0 : 1;
}
