// ============================================================================
//  WinEaseHelper —— WinEase 提权助手（P0-4）
//
//  角色：常驻后台的独立进程，requireAdministrator 清单使其以管理员权限运行
//  （UAC 只在主程序首次用到提权操作时弹一次）。
//
//  安全要点：
//    * 只接受白名单操作（HelperOps），白名单外一律拒绝
//    * 只接受与本程序同目录且签名有效的调用方（HelperSecurity）
//    * 危险动作全部有护栏：hosts 写前备份、关键系统进程黑名单、
//      禁止结束自身/调用方、参数大小硬上限
//
//  日志：%APPDATA%/WinEase/logs/helper.log（>2 MiB 自动轮转为 .old）
// ============================================================================

#include "HelperLog.h"
#include "HelperService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

namespace {

/// 简易文件日志：helper 是 WIN32 子系统程序（无控制台），
/// 提权后写文件是唯一可靠的诊断手段
void fileMessageHandler(QtMsgType type,
                        const QMessageLogContext &context,
                        const QString &message)
{
    static QFile *logFile = nullptr;   // 进程级单例，handler 只装一次

    const QString basePath =
        QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"))
        + QStringLiteral("/WinEase/logs");
    if (logFile == nullptr) {
        QDir().mkpath(basePath);
        const QString path = basePath + QStringLiteral("/helper.log");

        // 轮转：超过 2 MiB 就挪开，避免无限增长
        QFileInfo info(path);
        if (info.exists() && info.size() > 2 * 1024 * 1024) {
            QFile::remove(path + QStringLiteral(".old"));
            QFile::rename(path, path + QStringLiteral(".old"));
        }

        logFile = new QFile(path);
        logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }

    // 源码已用 /utf-8 编译，窄字面量即 UTF-8 字节，直接 fromUtf8 即可
    const char *level = "调试";
    switch (type) {
    case QtInfoMsg:     level = "信息"; break;
    case QtWarningMsg:  level = "警告"; break;
    case QtCriticalMsg: level = "严重"; break;
    case QtFatalMsg:    level = "致命"; break;
    case QtDebugMsg:    break;
    }

    QString line = QDateTime::currentDateTime().toString(
                       QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                   + QStringLiteral(" [") + QString::fromUtf8(level) + QStringLiteral("]")
                   + QStringLiteral(" [") + QString::fromUtf8(context.category) + QStringLiteral("] ")
                   + message + QStringLiteral("\n");

    if (logFile != nullptr && logFile->isOpen()) {
        logFile->write(line.toUtf8());
        logFile->flush();
    }
    std::fprintf(stderr, "%s", qPrintable(line));
    std::fflush(stderr);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("WinEase"));
    QCoreApplication::setApplicationName(QStringLiteral("WinEaseHelper"));
    QCoreApplication::setApplicationVersion(QStringLiteral(WINEASE_APP_VERSION));

    qInstallMessageHandler(fileMessageHandler);
    qCInfo(lcHelper) << "WinEaseHelper 启动, 版本:"
                     << QCoreApplication::applicationVersion()
                     << "PID:" << QCoreApplication::applicationPid();

    WinEase::Helper::HelperService service;
    QString error;
    if (!service.start(&error)) {
        // 已有实例在运行属正常情况（UAC 常驻），不算错误
        qCInfo(lcHelper) << "未启动:" << error;
        return 0;
    }

    return app.exec();
}
