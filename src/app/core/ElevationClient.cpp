#include "core/ElevationClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QThread>

#include <windows.h>
#include <shellapi.h>

static Q_LOGGING_CATEGORY(lcElevation, "winease.elevation")

namespace WinEase {

namespace {

constexpr const char *kServerName = "WinEase.Helper.v1";
constexpr int kProbeTimeoutMs = 500;        // 探测管道存在性
constexpr int kConnectTimeoutMs = 3000;     // 请求连接超时
constexpr int kResponseTimeoutMs = 30000;   // 应答超时（hosts/杀进程类操作可能偏慢）
constexpr int kLaunchWaitMs = 30000;        // 等待 UAC 授权 + helper 就绪的上限
constexpr int kLaunchFailGraceMs = 10000;   // 拉起失败后的冷静期，防止循环弹 UAC

/// helper.exe 与主程序同目录部署
QString helperExecutablePath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("WinEaseHelper.exe"));
}

ElevationResult failure(const QString &error)
{
    ElevationResult r;
    r.ok = false;
    r.error = error;
    return r;
}

} // namespace

ElevationClient::ElevationClient(QObject *parent)
    : QObject(parent)
{
}

ElevationClient::~ElevationClient() = default;

bool ElevationClient::isHelperAvailable()
{
    // 只探测管道存在性，绝不发起 UAC
    QLocalSocket probe;
    probe.connectToServer(QLatin1String(kServerName));
    const bool connected = probe.waitForConnected(kProbeTimeoutMs);
    if (connected) {
        probe.disconnectFromServer();
    }
    return connected;
}

QString ElevationClient::statusText()
{
    if (isHelperAvailable()) {
        return QStringLiteral("提权助手运行中（管理员权限）");
    }
    if (QFileInfo::exists(helperExecutablePath())) {
        return QStringLiteral("提权助手未运行（首次使用提权功能时会请求管理员授权）");
    }
    return QStringLiteral("提权助手缺失（安装不完整，提权功能不可用）");
}

bool ElevationClient::ensureHelperRunning(QString *errorOut)
{
    if (isHelperAvailable()) {
        return true;
    }

    // 冷静期：拉起刚失败时快速失败，避免调用方循环重试反复弹 UAC
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastLaunchFailMs != 0 && now - m_lastLaunchFailMs < kLaunchFailGraceMs) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("上一次拉起提权助手未成功（等待 %1 秒），请稍后再试")
                            .arg(kLaunchWaitMs / 1000);
        }
        return false;
    }

    const QString helperPath = helperExecutablePath();
    if (!QFileInfo::exists(helperPath)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("找不到提权助手 %1（安装不完整）")
                            .arg(QDir::toNativeSeparators(helperPath));
        }
        m_lastLaunchFailMs = now;
        return false;
    }

    // ShellExecuteExW + runas：requireAdministrator 清单会触发 UAC。
    // 已提权环境下该 verb 也能正常工作（不再弹窗）。
    const std::wstring file = QDir::toNativeSeparators(helperPath).toStdWString();
    const std::wstring params = QStringLiteral("--service").toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;   // 失败要自己报，别弹系统错误框
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;

    if (::ShellExecuteExW(&info) == FALSE) {
        const DWORD error = ::GetLastError();
        m_lastLaunchFailMs = now;
        if (errorOut != nullptr) {
            *errorOut = (error == ERROR_CANCELLED)
                ? QStringLiteral("用户取消了管理员授权（UAC）")
                : QStringLiteral("无法启动提权助手（错误码 %1）").arg(error);
        }
        qCWarning(lcElevation) << "提权助手启动失败, 错误码:" << error;
        return false;
    }

    // 等管道出现。期间持续处理事件：UAC 弹窗期间界面不能冻结
    qCInfo(lcElevation) << "已发起提权助手启动请求，等待其就绪…";
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kLaunchWaitMs) {
        if (isHelperAvailable()) {
            qCInfo(lcElevation) << "提权助手已就绪（等待" << timer.elapsed() << "ms）";
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(50);
    }

    m_lastLaunchFailMs = QDateTime::currentMSecsSinceEpoch();
    if (errorOut != nullptr) {
        *errorOut = QStringLiteral("等待提权助手就绪超时（%1 秒）。"
                                   "若 UAC 弹窗尚未确认，请确认后重试")
                        .arg(kLaunchWaitMs / 1000);
    }
    return false;
}

ElevationResult ElevationClient::sendRequest(const QString &operation,
                                             const QVariantMap &arguments,
                                             QString *errorOut)
{
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(kServerName));
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("无法连接提权助手（可能尚未启动）");
        }
        return failure(errorOut != nullptr ? *errorOut : QString());
    }

    QJsonObject request;
    request.insert(QStringLiteral("id"), 1);
    request.insert(QStringLiteral("op"), operation);
    request.insert(QStringLiteral("args"), QJsonObject::fromVariantMap(arguments));
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(3000)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("请求发送失败（提权助手无响应）");
        }
        return failure(errorOut != nullptr ? *errorOut : QString());
    }

    // 读到第一行（以 \n 结束）即视为应答完成
    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kResponseTimeoutMs) {
        buffer += socket.readAll();
        if (buffer.contains('\n')) {
            break;
        }
        if (!socket.waitForReadyRead(500)
                && socket.state() == QLocalSocket::UnconnectedState) {
            break;   // 对端断开：缓冲里可能已有数据，继续走解析
        }
    }

    const int newline = static_cast<int>(buffer.indexOf('\n'));
    const QByteArray line = (newline >= 0) ? buffer.left(newline) : buffer;

    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("提权助手应答格式异常");
        }
        return failure(errorOut != nullptr ? *errorOut : QString());
    }

    const QJsonObject obj = doc.object();
    ElevationResult result;
    result.ok = obj.value(QStringLiteral("ok")).toBool(false);
    result.error = obj.value(QStringLiteral("error")).toString();
    result.data = obj.value(QStringLiteral("data")).toObject().toVariantMap();

    if (!result.ok && result.error.isEmpty()) {
        result.error = QStringLiteral("提权操作被拒绝（未说明原因）");
    }
    if (!result.ok) {
        qCWarning(lcElevation) << "提权操作被拒绝, op:" << operation << "原因:" << result.error;
    } else {
        qCInfo(lcElevation) << "提权操作完成, op:" << operation;
    }
    return result;
}

ElevationResult ElevationClient::execute(const QString &operation, const QVariantMap &arguments)
{
    if (operation.isEmpty() || operation.size() > 64) {
        return failure(QStringLiteral("提权操作名非法"));
    }

    QString error;
    if (!ensureHelperRunning(&error)) {
        return failure(error);
    }
    return sendRequest(operation, arguments, &error);
}

} // namespace WinEase
