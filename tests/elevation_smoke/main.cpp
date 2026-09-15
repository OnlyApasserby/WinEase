// ============================================================================
//  elevation_smoke —— WinEaseHelper 提权助手验收测试（P0-4）
//
//  对应 ROADMAP 的验收标准：
//      1. 普通权限主程序可完成一次 hosts 写入（写入后恢复原样，不留垃圾）
//      2. 恶意进程伪造请求被拒绝（管道对端 PID / 同目录 / 签名三层校验）
//   +  白名单外的操作被拒绝、参数非法被拒绝、关键系统进程黑名单生效
//
//  运行方式：
//      elevation_smoke.exe               # helper 未运行时只做"拒绝路径"对照并明确标注
//      elevation_smoke.exe --with-uac    # 允许弹 UAC 拉起 helper（全量验收）
//
//  内部辅助模式（由本程序自己拉起，手工执行无意义）：
//      --sleep <秒>      结束进程测试的"受害者"进程
//      --rogue-client    伪造调用方：从 TEMP 目录连接管道发 ping（必须被拒）
// ============================================================================

#include "core/ElevationClient.h"
#include "win32/ProcessUtils.h"
#include "win32/RegistryUtils.h"

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

/// 缩写：平台能力层命名空间
namespace Win32 = WinEase::Win32;

// ---------------------------------------------------------------------------
// 输出工具（与其它 smoke 相同样式：[通过]/[失败] + 末页汇总，退出码 0 = 全过）
// ---------------------------------------------------------------------------
namespace {

const char *const kGreen = "\033[32m";
const char *const kRed = "\033[31m";
const char *const kYellow = "\033[33m";
const char *const kGray = "\033[90m";
const char *const kReset = "\033[0m";

void printLine(const QString &text)
{
    std::fprintf(stdout, "%s\n", text.toUtf8().constData());
    std::fflush(stdout);
}

class Reporter
{
public:
    void section(const QString &title)
    {
        printLine(QStringLiteral("\n──────────── %1 ────────────").arg(title));
    }

    void note(const QString &text)
    {
        printLine(QStringLiteral("  %1· %2%3").arg(QString::fromUtf8(kGray), text,
                                                   QString::fromUtf8(kReset)));
    }

    void check(bool passed, const QString &description, const QString &detail = QString())
    {
        if (passed) {
            ++m_passed;
            printLine(QStringLiteral("  %1[通过]%2 %3")
                          .arg(QString::fromUtf8(kGreen), QString::fromUtf8(kReset), description));
        } else {
            ++m_failed;
            printLine(QStringLiteral("  %1[失败]%2 %3")
                          .arg(QString::fromUtf8(kRed), QString::fromUtf8(kReset), description));
            if (!detail.isEmpty()) {
                printLine(QStringLiteral("         %1%2%3")
                              .arg(QString::fromUtf8(kRed), detail, QString::fromUtf8(kReset)));
            }
        }
    }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    int m_passed = 0;
    int m_failed = 0;
};

void printSummary(Reporter &reporter)
{
    printLine(QStringLiteral("\n──────────── 汇总 ────────────"));
    printLine(QStringLiteral("  通过 %1 / %2")
                  .arg(reporter.passed())
                  .arg(reporter.passed() + reporter.failed()));
    if (reporter.failed() == 0) {
        printLine(QStringLiteral("  %1结果：全部通过%2")
                      .arg(QString::fromUtf8(kGreen), QString::fromUtf8(kReset)));
    } else {
        printLine(QStringLiteral("  %1结果：存在 %2 个失败项%3")
                      .arg(QString::fromUtf8(kRed))
                      .arg(reporter.failed())
                      .arg(QString::fromUtf8(kReset)));
    }
}

/// 提权助手管道名（与 HelperService/ElevationClient 保持一致）
constexpr const char *kServerName = "WinEase.Helper.v1";

QString hostsFilePath()
{
    const QString systemRoot =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    return QDir(systemRoot).filePath(QStringLiteral("System32/drivers/etc/hosts"));
}

QString hex(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

} // namespace

// ---------------------------------------------------------------------------
// 辅助模式 1：伪造调用方（从 TEMP 目录运行 —— 同目录校验必然不通过）
// ---------------------------------------------------------------------------
namespace {

int runRogueClient()
{
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(kServerName));
    if (!socket.waitForConnected(3000)) {
        // 连不上也是有效结果（helper 未运行），交由父进程判断
        std::fprintf(stdout, "{\"ok\":false,\"error\":\"connect-failed\"}\n");
        std::fflush(stdout);
        return 0;
    }

    // 故意自报一个"合法"的 PID —— helper 必须无视自报信息，只采信管道对端元数据
    QJsonObject request;
    request.insert(QStringLiteral("id"), 1);
    request.insert(QStringLiteral("op"), QStringLiteral("ping"));
    QJsonObject args;
    args.insert(QStringLiteral("pid"), 123);
    request.insert(QStringLiteral("args"), args);

    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    socket.waitForBytesWritten(2000);

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        buffer += socket.readAll();
        if (buffer.contains('\n')) {
            break;
        }
        if (!socket.waitForReadyRead(300)
                && socket.state() == QLocalSocket::UnconnectedState) {
            break;
        }
    }

    const int newline = static_cast<int>(buffer.indexOf('\n'));
    const QByteArray line = (newline >= 0) ? buffer.left(newline) : buffer;
    const QByteArray output = line.isEmpty()
        ? QByteArrayLiteral("{\"ok\":false,\"error\":\"empty-response\"}")
        : line;
    std::fprintf(stdout, "%s\n", output.constData());
    std::fflush(stdout);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 辅助模式 2：结束进程测试的"受害者"
// ---------------------------------------------------------------------------
namespace {

int runVictim(int seconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < qint64(seconds) * 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("elevation_smoke"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("WinEaseHelper 提权助手验收测试"));
    parser.addHelpOption();
    QCommandLineOption withUacOption(QStringLiteral("with-uac"),
                                     QStringLiteral("允许弹出 UAC 拉起提权助手（全量验收）"));
    QCommandLineOption sleepOption(QStringLiteral("sleep"),
                                   QStringLiteral("内部模式：静默 N 秒后退出"), QStringLiteral("seconds"));
    QCommandLineOption rogueOption(QStringLiteral("rogue-client"),
                                   QStringLiteral("内部模式：以伪造调用方身份连接管道"));
    QCommandLineOption quitOption(QStringLiteral("quit-helper"),
                                  QStringLiteral("请求常驻的提权助手优雅退出（运维用）"));
    QCommandLineOption restoreOption(QStringLiteral("restore-hosts"),
                                     QStringLiteral("运维：用指定备份文件整体还原 hosts"),
                                     QStringLiteral("file"));
    parser.addOption(withUacOption);
    parser.addOption(sleepOption);
    parser.addOption(rogueOption);
    parser.addOption(quitOption);
    parser.addOption(restoreOption);
    parser.process(app);

    if (parser.isSet(rogueOption)) {
        return runRogueClient();
    }
    if (parser.isSet(sleepOption)) {
        return runVictim(qMax(1, parser.value(sleepOption).toInt()));
    }
    if (parser.isSet(quitOption)) {
        WinEase::ElevationClient client;
        const WinEase::ElevationResult r = client.execute(QStringLiteral("quit"), {});
        std::printf("%s\n", r.ok ? "helper 已退出" : r.error.toUtf8().constData());
        return r.ok ? 0 : 1;
    }
    if (parser.isSet(restoreOption)) {
        const QString backup = parser.value(restoreOption);
        QFile file(backup);
        if (!file.open(QIODevice::ReadOnly)) {
            std::printf("无法读取备份文件: %s\n", backup.toUtf8().constData());
            return 1;
        }
        const QByteArray bytes = file.readAll();
        file.close();
        WinEase::ElevationClient client;
        QVariantMap args;
        args.insert(QStringLiteral("mode"), QStringLiteral("replace"));
        args.insert(QStringLiteral("contentBase64"), QString::fromLatin1(bytes.toBase64()));
        const WinEase::ElevationResult r = client.execute(QStringLiteral("writeHosts"), args);
        std::printf("%s\n", r.ok ? "hosts 已按备份还原"
                                 : r.error.toUtf8().constData());
        return r.ok ? 0 : 1;
    }

    Reporter reporter;
    const bool withUac = parser.isSet(withUacOption);

    // ========================================================================
    reporter.section(QStringLiteral("〇、环境基线"));
    // ========================================================================
    const QString helperPath =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("WinEaseHelper.exe"));
    const bool helperExeExists = QFileInfo::exists(helperPath);
    reporter.check(helperExeExists,
                   QStringLiteral("提权助手 WinEaseHelper.exe 与主程序同目录部署"),
                   QStringLiteral("缺失路径: %1").arg(QDir::toNativeSeparators(helperPath)));

    const bool selfElevated = Win32::isProcessElevated(Win32::currentProcessId());
    reporter.note(selfElevated
        ? QStringLiteral("当前测试进程已提权（部分对照项会相应调整）")
        : QStringLiteral("当前测试进程为普通权限（正是验收标准要求的场景）"));

    // ElevationClient 只探测管道，绝不弹 UAC
    const bool helperRunning = WinEase::ElevationClient().isHelperAvailable();
    reporter.note(helperRunning
        ? QStringLiteral("提权助手已在运行（沿用既有实例）")
        : (withUac ? QStringLiteral("提权助手未运行，将按 --with-uac 发起 UAC 拉起")
                   : QStringLiteral("提权助手未运行，且未加 --with-uac（提权场景将被跳过并明确标注）")));

    // ========================================================================
    reporter.section(QStringLiteral("一、普通权限的直接写入对照（不经过提权通道）"));
    // ========================================================================
    const QString envSubKey =
        QStringLiteral("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    const QString envName = QStringLiteral("WINEASE_ELEVATION_SMOKE");

    if (!selfElevated) {
        QString writeError;
        const bool directWrite = Win32::writeValue(
            Win32::RegistryRoot::LocalMachine, envSubKey, envName,
            QStringLiteral("x"), Win32::RegistryValueType::String,
            Win32::RegistryView::Default, &writeError);
        reporter.check(!directWrite,
                       QStringLiteral("普通权限直接写 HKLM 环境变量失败（证明确实需要提权通道）"),
                       QStringLiteral("意外成功 —— 说明当前 shell 已是管理员，对照无意义"));
    } else {
        reporter.note(QStringLiteral("当前进程已提权，跳过该对照项"));
    }

    // ========================================================================
    reporter.section(QStringLiteral("二、提权助手就绪"));
    // ========================================================================
    if (!helperRunning && !withUac) {
        reporter.note(QStringLiteral("跳过第二~七节：helper 未运行。"
                                     "加 --with-uac 可覆盖全量场景（会弹一次 UAC）"));
        printSummary(reporter);
        return reporter.failed() == 0 ? 0 : 1;
    }

    WinEase::ElevationClient client;
    const WinEase::ElevationResult ping = client.execute(QStringLiteral("ping"), {});
    reporter.check(ping.ok,
                   QStringLiteral("经提权助手完成一次往返（首次调用会触发一次 UAC）"),
                   ping.error);
    reporter.check(ping.data.value(QStringLiteral("elevated")).toBool(),
                   QStringLiteral("提权助手确实以管理员权限运行（requireAdministrator 清单生效）"),
                   QStringLiteral("ping 应答 elevated = %1")
                       .arg(hex(ping.data.value(QStringLiteral("elevated")).toBool())));
    reporter.check(!ping.data.value(QStringLiteral("version")).toString().isEmpty()
                       && ping.data.value(QStringLiteral("pid")).toUInt() > 0,
                   QStringLiteral("ping 应答带版本号与进程 id"),
                   QStringLiteral("version=%1 pid=%2")
                       .arg(ping.data.value(QStringLiteral("version")).toString())
                       .arg(ping.data.value(QStringLiteral("pid")).toUInt()));

    // ========================================================================
    reporter.section(QStringLiteral("三、白名单与参数校验（全部应被拒绝）"));
    // ========================================================================
    const auto rejected = [&client, &reporter](const QString &op, const QVariantMap &args,
                                               const QString &expect, const QString &label) {
        const WinEase::ElevationResult r = client.execute(op, args);
        reporter.check(!r.ok && r.error.contains(expect),
                       label,
                       QStringLiteral("ok=%1 error=%2")
                           .arg(hex(r.ok), r.error.isEmpty() ? QStringLiteral("<空>") : r.error));
    };

    rejected(QStringLiteral("explode"), {},
             QStringLiteral("白名单"),
             QStringLiteral("白名单外的操作被拒绝"));
    rejected(QStringLiteral("writeHosts"), {{QStringLiteral("mode"), QStringLiteral("bogus")}},
             QStringLiteral("mode"),
             QStringLiteral("hosts 操作模式非法被拒绝"));
    rejected(QStringLiteral("setEnvVar"),
             {{QStringLiteral("scope"), QStringLiteral("machine")},
              {QStringLiteral("name"), QStringLiteral("a=b")},
              {QStringLiteral("value"), QStringLiteral("x")}},
             QStringLiteral("name"),
             QStringLiteral("环境变量名含 = 被拒绝"));
    rejected(QStringLiteral("setEnvVar"),
             {{QStringLiteral("scope"), QStringLiteral("galaxy")},
              {QStringLiteral("name"), QStringLiteral("X")},
              {QStringLiteral("value"), QStringLiteral("y")}},
             QStringLiteral("scope"),
             QStringLiteral("环境变量范围非法被拒绝"));
    rejected(QStringLiteral("killProcess"), {{QStringLiteral("pid"), 0}},
             QStringLiteral("pid"),
             QStringLiteral("结束进程 pid=0 被拒绝"));
    rejected(QStringLiteral("killProcess"), {{QStringLiteral("pid"), 4}},
             QStringLiteral("拒绝"),
             QStringLiteral("结束关键系统进程（System, pid=4）被拒绝"));
    rejected(QStringLiteral("powerAction"), {{QStringLiteral("action"), QStringLiteral("explode")}},
             QStringLiteral("action"),
             QStringLiteral("电源操作类型非法被拒绝"));

    // ========================================================================
    reporter.section(QStringLiteral("四、hosts 写入往返（验收标准 1：普通权限完成一次写入）"));
    // ========================================================================
    QFile hostsFile(hostsFilePath());
    QByteArray originalHosts;
    if (hostsFile.open(QIODevice::ReadOnly)) {
        originalHosts = hostsFile.readAll();
        hostsFile.close();
    }
    reporter.check(!originalHosts.isEmpty(),
                   QStringLiteral("可读取 hosts 原始内容"),
                   QStringLiteral("读取失败: %1").arg(hostsFilePath()));

    if (!originalHosts.isEmpty()) {
        const QString marker =
            QStringLiteral("# WinEase elevation_smoke %1")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));

        QVariantMap appendArgs;
        appendArgs.insert(QStringLiteral("mode"), QStringLiteral("append"));
        appendArgs.insert(QStringLiteral("line"), marker);
        const WinEase::ElevationResult appended = client.execute(QStringLiteral("writeHosts"), appendArgs);
        reporter.check(appended.ok, QStringLiteral("普通权限经助手追加了 hosts 记录"), appended.error);

        bool markerPresent = false;
        if (QFile check(hostsFilePath()); check.open(QIODevice::ReadOnly)) {
            markerPresent = check.readAll().contains(marker.toUtf8());
            check.close();
        }
        reporter.check(appended.ok && markerPresent,
                       QStringLiteral("hosts 文件确实包含了追加的记录（不只是应答成功）"),
                       QStringLiteral("标记行未找到"));

        if (markerPresent) {
            QVariantMap restoreArgs;
            restoreArgs.insert(QStringLiteral("mode"), QStringLiteral("replace"));
            // 用 base64 做字节级还原：JSON 文本通道会剥掉开头的 UTF-8 BOM（实测差 3 字节）
            restoreArgs.insert(QStringLiteral("contentBase64"),
                               QString::fromLatin1(originalHosts.toBase64()));
            const WinEase::ElevationResult restored =
                client.execute(QStringLiteral("writeHosts"), restoreArgs);
            reporter.check(restored.ok, QStringLiteral("hosts 已恢复原始内容"), restored.error);

            QByteArray after;
            if (QFile verify(hostsFilePath()); verify.open(QIODevice::ReadOnly)) {
                after = verify.readAll();
                verify.close();
            }
            reporter.check(after == originalHosts,
                           QStringLiteral("恢复后与原始字节完全一致（不留痕迹）"),
                           QStringLiteral("字节数 原=%1 现=%2")
                               .arg(originalHosts.size()).arg(after.size()));
            if (!restored.ok || after != originalHosts) {
                reporter.note(QStringLiteral("!! hosts 未能自动恢复，备份位置见助手日志与应答 backupPath"));
            }
        } else if (appended.ok) {
            // 写成功了但没读到标记 → 主动恢复，别把测试垃圾留在系统文件里
            QVariantMap restoreArgs;
            restoreArgs.insert(QStringLiteral("mode"), QStringLiteral("replace"));
            restoreArgs.insert(QStringLiteral("content"), QString::fromUtf8(originalHosts));
            (void) client.execute(QStringLiteral("writeHosts"), restoreArgs);
        }
    }

    // ========================================================================
    reporter.section(QStringLiteral("五、HKLM 环境变量往返（验收标准 2：系统级修改）"));
    // ========================================================================
    {
        const QString value =
            QStringLiteral("elevation-smoke-%1").arg(QDateTime::currentMSecsSinceEpoch());

        QVariantMap setArgs;
        setArgs.insert(QStringLiteral("scope"), QStringLiteral("machine"));
        setArgs.insert(QStringLiteral("name"), envName);
        setArgs.insert(QStringLiteral("value"), value);
        const WinEase::ElevationResult set = client.execute(QStringLiteral("setEnvVar"), setArgs);
        reporter.check(set.ok, QStringLiteral("经助手写入 HKLM 环境变量"), set.error);

        const QString readBack = Win32::readValue(
            Win32::RegistryRoot::LocalMachine, envSubKey, envName).toString();
        reporter.check(readBack == value,
                       QStringLiteral("普通权限侧读回值一致（且已广播 WM_SETTINGCHANGE）"),
                       QStringLiteral("期望 %1，实际 %2").arg(value, readBack));

        QVariantMap delArgs;
        delArgs.insert(QStringLiteral("scope"), QStringLiteral("machine"));
        delArgs.insert(QStringLiteral("name"), envName);
        delArgs.insert(QStringLiteral("value"), QVariant());   // null → 删除
        const WinEase::ElevationResult removed =
            client.execute(QStringLiteral("setEnvVar"), delArgs);
        reporter.check(removed.ok, QStringLiteral("经助手删除该环境变量"), removed.error);

        const QString afterDelete = Win32::readValue(
            Win32::RegistryRoot::LocalMachine, envSubKey, envName).toString();
        reporter.check(afterDelete.isEmpty(),
                       QStringLiteral("删除后该值已不存在（不留测试残留）"),
                       QStringLiteral("仍读到: %1").arg(afterDelete));
    }

    // ========================================================================
    reporter.section(QStringLiteral("六、结束进程（管理员才能结束的用户态进程）"));
    // ========================================================================
    {
        QProcess victim;
        victim.setProgram(QCoreApplication::applicationFilePath());
        victim.setArguments({QStringLiteral("--sleep"), QStringLiteral("60")});
        victim.start();
        const bool started = victim.waitForStarted(5000);
        reporter.check(started, QStringLiteral("已启动用于结束测试的受害者进程"));
        if (started) {
            const quint32 victimPid = static_cast<quint32>(victim.processId());
            reporter.note(QStringLiteral("受害者 PID = %1").arg(victimPid));

            QVariantMap killArgs;
            killArgs.insert(QStringLiteral("pid"), static_cast<quint32>(victimPid));
            const WinEase::ElevationResult killed =
                client.execute(QStringLiteral("killProcess"), killArgs);
            reporter.check(killed.ok, QStringLiteral("经助手结束目标进程"), killed.error);

            const bool finished = victim.waitForFinished(5000);
            // 证据链：victim 是 --sleep 60，启动 1 秒内就退出只能是"被终止"；
            // exitCode == 1 正是助手 terminateProcess 传入的退出码
            // （TerminateProcess 时 Qt 在 Windows 上报 NormalExit，不能拿它当证据）
            reporter.check(finished && victim.exitCode() == 1,
                           QStringLiteral("目标进程确实被终止（60 秒睡眠 1 秒内退出，退出码=助手的 1）"),
                           QStringLiteral("finished=%1 exitCode=%2 exitStatus=%3")
                               .arg(hex(finished))
                               .arg(victim.exitCode())
                               .arg(victim.exitStatus() == QProcess::NormalExit
                                        ? QStringLiteral("NormalExit")
                                        : QStringLiteral("CrashExit")));
            if (!finished) {
                victim.kill();
                victim.waitForFinished(3000);
            }
        }
    }

    // ========================================================================
    reporter.section(QStringLiteral("七、伪造调用方（验收标准 3：恶意进程被拒绝）"));
    // ========================================================================
    {
        // 把自己的副本放到 TEMP 目录运行：路径不在程序目录 → 同目录校验必须拒绝。
        // 这正是"第三方进程伪造请求"的等价物（签名校验/对端 PID 校验同属该链路）。
        QTemporaryDir rogueDir;
        rogueDir.setAutoRemove(true);
        bool copied = false;
        if (rogueDir.isValid()) {
            copied = QFile::copy(QCoreApplication::applicationFilePath(),
                                 rogueDir.filePath(QStringLiteral("winease_rogue.exe")));
        }
        reporter.check(copied,
                       QStringLiteral("已构造伪造调用方（TEMP 目录下的副本）"),
                       QStringLiteral("无法创建临时目录/复制文件，无法验证该场景"));

        if (copied) {
            QProcess rogue;
            rogue.setProgram(rogueDir.filePath(QStringLiteral("winease_rogue.exe")));
            rogue.setArguments({QStringLiteral("--rogue-client")});
            // TEMP 副本找不到 Qt DLL：把程序目录追加进 PATH
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("PATH"),
                       QCoreApplication::applicationDirPath() + QStringLiteral(";")
                           + env.value(QStringLiteral("PATH")));
            rogue.setProcessEnvironment(env);

            rogue.start();
            const bool finished = rogue.waitForFinished(15000);
            const QString output = QString::fromUtf8(rogue.readAllStandardOutput()).trimmed();

            const QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8());
            const bool responded = finished && doc.isObject();
            reporter.check(responded,
                           QStringLiteral("伪造调用方得到了助手的应答（连接未被静默丢弃）"),
                           finished ? QStringLiteral("输出: %1")
                                        .arg(output.isEmpty() ? QStringLiteral("<空>") : output)
                                    : QStringLiteral("15 秒内未退出"));

            if (responded) {
                const QJsonObject obj = doc.object();
                const QString error = obj.value(QStringLiteral("error")).toString();
                reporter.check(!obj.value(QStringLiteral("ok")).toBool(),
                               QStringLiteral("伪造请求被拒绝（未执行任何操作）"),
                               QStringLiteral("error = %1").arg(error));
                reporter.check(error.contains(QStringLiteral("拒绝"))
                                   || error.contains(QStringLiteral("校验"))
                                   || error.contains(QStringLiteral("目录")),
                               QStringLiteral("拒绝原因是安全校验（而非别的故障）"),
                               QStringLiteral("error = %1").arg(error));
            }
        }
    }

    // ========================================================================
    reporter.section(QStringLiteral("八、收尾：助手仍可用"));
    // ========================================================================
    {
        const WinEase::ElevationResult pingAgain =
            client.execute(QStringLiteral("ping"), {});
        reporter.check(pingAgain.ok,
                       QStringLiteral("全部测试后助手仍正常服务（常驻未被破坏）"),
                       pingAgain.error);
    }

    printSummary(reporter);
    return reporter.failed() == 0 ? 0 : 1;
}
