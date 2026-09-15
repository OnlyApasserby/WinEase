#include "core/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <cstdio>

namespace WinEase::Logging {

Q_LOGGING_CATEGORY(lcApp, "winease.app")
Q_LOGGING_CATEGORY(lcConfig, "winease.config")
Q_LOGGING_CATEGORY(lcPlugin, "winease.plugin")
Q_LOGGING_CATEGORY(lcHotkey, "winease.hotkey")
Q_LOGGING_CATEGORY(lcTray, "winease.tray")
Q_LOGGING_CATEGORY(lcUi, "winease.ui")

namespace {

/// 单个日志文件上限（超过后滚动）
constexpr qint64 kMaxLogFileSize = 8 * 1024 * 1024;

QMutex g_logMutex;
QFile *g_logFile = nullptr;
QTextStream *g_logStream = nullptr;
QString g_logDirectory;
QString g_logFilePath;
QtMsgType g_minLevel = QtDebugMsg;
QtMessageHandler g_previousHandler = nullptr;
bool g_installed = false;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO ";
    case QtWarningMsg:
        return "WARN ";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    }
    return "UNKWN";
}

/// 返回当天的日志文件路径
QString makeLogFilePath(const QString &directory)
{
    const QString name = QStringLiteral("winease-%1.log")
                             .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    return QDir(directory).filePath(name);
}

/// 调用者必须已持有 g_logMutex
void openLogFileLocked(const QString &directory)
{
    if (g_logFile) {
        return;
    }
    QDir().mkpath(directory);
    g_logFilePath = makeLogFilePath(directory);

    g_logFile = new QFile(g_logFilePath);
    if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete g_logFile;
        g_logFile = nullptr;
        return;
    }
    g_logStream = new QTextStream(g_logFile);
    g_logStream->setEncoding(QStringConverter::Utf8);
}

void closeLogFileLocked()
{
    if (g_logStream) {
        g_logStream->flush();
        delete g_logStream;
        g_logStream = nullptr;
    }
    if (g_logFile) {
        g_logFile->close();
        delete g_logFile;
        g_logFile = nullptr;
    }
    g_logFilePath.clear();
}

/// 日志滚动：把当前文件改名为带时间戳的备份，并清理历史
void rotateLocked()
{
    if (!g_logFile || g_logDirectory.isEmpty()) {
        return;
    }
    g_logStream->flush();
    g_logFile->close();

    const QString backup = QDir(g_logDirectory)
                               .filePath(QStringLiteral("winease-%1.log")
                                             .arg(QDateTime::currentDateTime().toString(
                                                 QStringLiteral("yyyyMMdd-hhmmss"))));
    QFile::remove(backup);
    QFile::rename(g_logFilePath, backup);

    g_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type < g_minLevel) {
        return;
    }

    const QString line = QStringLiteral("[%1][%2][%3] %4")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")),
                                  QString::fromLatin1(levelName(type)),
                                  QString::fromLatin1(context.category ? context.category : "default"),
                                  message);

    {
        QMutexLocker locker(&g_logMutex);
        if (g_logFile && g_logStream) {
            *g_logStream << line << '\n';
            g_logStream->flush();
            if (g_logFile->size() > kMaxLogFileSize) {
                rotateLocked();
            }
        }
    }

    // 调试构建下同时输出到调试器/控制台，便于开发定位
#ifdef QT_DEBUG
    std::fputs(qPrintable(line), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
#endif

    if (g_previousHandler) {
        g_previousHandler(type, context, message);
    }
}

} // namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------

bool install(const QString &logDirectoryPath, QtMsgType minLevel)
{
    QString openedPath;
    {
        // 注意：日志写入回调同样会加锁，此处必须在解锁后再输出日志，避免自死锁
        QMutexLocker locker(&g_logMutex);
        if (g_installed) {
            return true;
        }

        g_logDirectory = logDirectoryPath;
        QDir().mkpath(g_logDirectory);
        g_minLevel = minLevel;

        openLogFileLocked(g_logDirectory);
        if (!g_logFile) {
            return false;
        }

        openedPath = g_logFilePath;
        g_previousHandler = qInstallMessageHandler(messageHandler);
        g_installed = true;
    }

    qCInfo(lcApp) << "日志系统已启动:" << openedPath;
    return true;
}

void uninstall()
{
    QMutexLocker locker(&g_logMutex);
    if (!g_installed) {
        return;
    }

    qInstallMessageHandler(g_previousHandler);
    closeLogFileLocked();

    g_installed = false;
    g_previousHandler = nullptr;
    g_logDirectory.clear();
}

QString currentLogFilePath()
{
    QMutexLocker locker(&g_logMutex);
    return g_logFilePath;
}

QString logDirectory()
{
    QMutexLocker locker(&g_logMutex);
    return g_logDirectory;
}

void setMinimumLevel(QtMsgType level)
{
    QMutexLocker locker(&g_logMutex);
    g_minLevel = level;
}

QtMsgType minimumLevel()
{
    QMutexLocker locker(&g_logMutex);
    return g_minLevel;
}

void pruneOldLogs(int keepDays)
{
    QString directory;
    {
        QMutexLocker locker(&g_logMutex);
        directory = g_logDirectory;
    }
    if (directory.isEmpty() || keepDays <= 0) {
        return;
    }

    const QDate deadline = QDate::currentDate().addDays(-keepDays);
    const QDir dir(directory);
    const QFileInfoList files = dir.entryInfoList({ QStringLiteral("winease-*.log") }, QDir::Files);
    for (const QFileInfo &info : files) {
        if (info.lastModified().date() < deadline) {
            QFile::remove(info.absoluteFilePath());
        }
    }
}

QtMsgType toQtMsgType(int pluginLogLevel)
{
    switch (pluginLogLevel) {
    case 0:
        return QtDebugMsg;
    case 1:
        return QtInfoMsg;
    case 2:
        return QtWarningMsg;
    case 3:
        return QtCriticalMsg;
    default:
        return QtInfoMsg;
    }
}

} // namespace WinEase::Logging
