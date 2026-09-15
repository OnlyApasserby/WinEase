#include "HelperService.h"

#include "HelperLog.h"
#include "HelperSecurity.h"

#include <QDateTime>

#include <sddl.h>       // ConvertStringSecurityDescriptorToSecurityDescriptorW
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

#include <chrono>
#include <cstring>

Q_LOGGING_CATEGORY(lcHelper, "winease.helper")

namespace WinEase::Helper {

namespace {

constexpr const char *kServerName = "WinEase.Helper.v1";
/// 单条请求上限（hosts 整体替换 1 MiB + JSON 包装），超限按恶意连接处理
constexpr qint64 kMaxRequestBytes = 2 * 1024 * 1024;
/// 一次读取的等待上限：认证过的客户端卡死也不能拖垮服务
constexpr DWORD kReadTimeoutMs = 30000;
/// 等待客户端连接的轮询间隔（期间检查停止标志）
constexpr DWORD kAcceptPollMs = 200;

QString fullPipeName()
{
    return QStringLiteral("\\\\.\\pipe\\") + QString::fromLatin1(kServerName);
}

bool writeAll(HANDLE pipe, const QByteArray &data)
{
    DWORD written = 0;
    return ::WriteFile(pipe, data.constData(), static_cast<DWORD>(data.size()),
                       &written, nullptr)
           && written == static_cast<DWORD>(data.size());
}

QByteArray makeErrorResponse(int id, const QString &error)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), false);
    obj.insert(QStringLiteral("error"), error);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
}

/// 带超时的阻塞读（OVERLAPPED + WaitForSingleObject）
bool readChunk(HANDLE pipe, char *buffer, DWORD size, DWORD *readOut, bool *timedOut)
{
    *timedOut = false;
    OVERLAPPED ov{};
    std::memset(&ov, 0, sizeof(ov));
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        return false;
    }

    const BOOL ok = ::ReadFile(pipe, buffer, size, nullptr, &ov);
    if (ok == FALSE && ::GetLastError() != ERROR_IO_PENDING) {
        ::CloseHandle(ov.hEvent);
        return false;   // 管道已断开等硬错误
    }

    const DWORD waited = ::WaitForSingleObject(ov.hEvent, kReadTimeoutMs);
    if (waited != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe, &ov);
        ::CloseHandle(ov.hEvent);
        *timedOut = (waited == WAIT_TIMEOUT);
        return false;
    }

    DWORD actuallyRead = 0;
    const BOOL done = ::GetOverlappedResult(pipe, &ov, &actuallyRead, FALSE);
    ::CloseHandle(ov.hEvent);
    *readOut = actuallyRead;
    if (done == FALSE) {
        return false;   // ERROR_BROKEN_PIPE = 对端正常断开
    }
    return actuallyRead > 0;
}

} // namespace

HelperService::HelperService(QObject *parent)
    : QObject(parent)
{
}

HelperService::~HelperService()
{
    m_stop = true;
    if (const HANDLE listener = m_listener.load(); listener != nullptr) {
        // 解除 Accept 线程上阻塞的 ConnectNamedPipe/读取（只取消挂起 IO，不关句柄）
        ::CancelIoEx(listener, nullptr);
    }
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
}

bool HelperService::start(QString *errorOut)
{
    m_stop = false;
    m_quitRequested = false;
    m_listenerReady = false;

    m_acceptThread = std::thread([this] { serverLoop(); });

    // 等首个监听实例就绪（正常 <100ms）；失败原因由 serverLoop 写入 m_startError
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_listenerReady.load()) {
        if (!m_startError.isEmpty()
                || std::chrono::steady_clock::now() > deadline) {
            m_stop = true;
            if (const HANDLE listener = m_listener.load(); listener != nullptr) {
                ::CancelIoEx(listener, nullptr);
            }
            if (m_acceptThread.joinable()) {
                m_acceptThread.join();
            }
            if (errorOut != nullptr) {
                *errorOut = m_startError.isEmpty()
                    ? QStringLiteral("监听命名管道超时（5 秒）")
                    : m_startError;
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    qCInfo(lcHelper) << "已开始监听命名管道:" << kServerName;
    return true;
}

HANDLE HelperService::createPipeInstance(bool firstInstance, QString *errorOut)
{
    // DACL：Everyone 可读写（GRGW）；
    // SACL：完整性标签 Medium（ML;;NW;;;ME）——否则普通权限进程会被 MIC 拒绝写入
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GRGW;;;WD)S:(ML;;NW;;;ME)", SDDL_REVISION_1,
                &descriptor, nullptr)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("构造管道安全描述符失败（错误码 %1）").arg(::GetLastError());
        }
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;

    const DWORD access = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED
                         | (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE pipe = ::CreateNamedPipeW(
        reinterpret_cast<const wchar_t *>(fullPipeName().utf16()),
        access,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        4096,   // 出站缓冲（应答远小于此）
        4096,   // 入站缓冲
        0,
        &attributes);

    const DWORD createError = ::GetLastError();
    ::LocalFree(descriptor);

    if (pipe == INVALID_HANDLE_VALUE) {
        if (errorOut != nullptr) {
            if (firstInstance && createError == ERROR_ACCESS_DENIED) {
                *errorOut = QStringLiteral("命名管道已存在（已有 WinEaseHelper 实例在运行）");
            } else {
                *errorOut = QStringLiteral("创建命名管道失败（错误码 %1）").arg(createError);
            }
        }
    }
    return pipe;
}

void HelperService::serverLoop()
{
    QString error;
    HANDLE listener = createPipeInstance(true, &error);
    if (listener == INVALID_HANDLE_VALUE) {
        m_startError = error;
        qCWarning(lcHelper) << "监听失败:" << error;
        return;
    }
    m_listener = listener;
    m_listenerReady = true;

    while (!m_stop.load()) {
        // ---- 等待客户端连接（可被停止信号打断）----
        OVERLAPPED ov{};
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        const BOOL connected = ::ConnectNamedPipe(listener, &ov);
        BOOL haveClient = (connected == TRUE);
        if (!haveClient && ::GetLastError() == ERROR_IO_PENDING) {
            while (!m_stop.load()) {
                const DWORD waited = ::WaitForSingleObject(ov.hEvent, kAcceptPollMs);
                if (waited == WAIT_OBJECT_0) {
                    haveClient = TRUE;
                    break;
                }
                if (waited != WAIT_TIMEOUT) {
                    break;
                }
            }
        } else if (!haveClient && ::GetLastError() == ERROR_PIPE_CONNECTED) {
            haveClient = TRUE;   // 客户端在 CreateNamedPipe 与 ConnectNamedPipe 之间挤了进来
        }
        ::CloseHandle(ov.hEvent);

        if (m_stop.load()) {
            ::DisconnectNamedPipe(listener);
            ::CloseHandle(listener);
            m_listener = nullptr;
            return;
        }
        if (!haveClient) {
            // 监听实例损坏：换一个再来（不放弃服务）
            ::DisconnectNamedPipe(listener);
            ::CloseHandle(listener);
            listener = createPipeInstance(false, &error);
            if (listener == INVALID_HANDLE_VALUE) {
                qCWarning(lcHelper) << "重建监听实例失败:" << error;
                m_listener = nullptr;
                return;
            }
            m_listener = listener;
            continue;
        }

        // ---- 客户端已连上：独立线程处理会话，立刻准备下一个监听实例 ----
        HANDLE client = listener;
        std::thread([this, client] { handleClient(client); }).detach();

        listener = createPipeInstance(false, &error);
        if (listener == INVALID_HANDLE_VALUE) {
            qCWarning(lcHelper) << "创建后续监听实例失败:" << error;
            m_listener = nullptr;
            return;
        }
        m_listener = listener;
    }
}

void HelperService::handleClient(HANDLE pipe)
{
    // ---- 调用方校验：fail-closed，先于读取任何业务数据 ----
    quint32 callerPid = 0;
    QString error;
    if (verifyCaller(pipe, &callerPid, &error) != CallerCheckResult::Accepted) {
        qCWarning(lcHelper) << "已拒绝调用方:" << error;
        writeAll(pipe, makeErrorResponse(0, error));
        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
        return;
    }

    qCInfo(lcHelper) << "调用方已通过校验, PID =" << callerPid;

    QByteArray buffer;
    while (!m_stop.load() && !m_quitRequested.load()) {
        char chunk[4096];
        DWORD read = 0;
        bool timedOut = false;
        if (!readChunk(pipe, chunk, sizeof(chunk), &read, &timedOut)) {
            if (timedOut) {
                qCWarning(lcHelper) << "客户端 30 秒无数据，断开, PID =" << callerPid;
            }
            break;   // 对端断开 / 出错 / 超时
        }
        buffer.append(chunk, static_cast<int>(read));
        if (buffer.size() > kMaxRequestBytes) {
            qCWarning(lcHelper) << "请求超出大小上限，断开, PID =" << callerPid;
            break;
        }

        int newline = 0;
        while ((newline = static_cast<int>(buffer.indexOf('\n'))) >= 0) {
            const QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            processRequest(pipe, callerPid, line);
            if (m_quitRequested.load()) {
                break;
            }
        }
    }

    ::FlushFileBuffers(pipe);
    ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);
}

void HelperService::processRequest(HANDLE pipe, quint32 callerPid, const QByteArray &line)
{
    const QJsonDocument request = QJsonDocument::fromJson(line);
    if (!request.isObject()) {
        writeAll(pipe, makeErrorResponse(0, QStringLiteral("请求不是合法的 JSON 对象")));
        return;
    }

    const QJsonObject obj = request.object();
    const int id = obj.value(QStringLiteral("id")).toInt(0);
    const QString op = obj.value(QStringLiteral("op")).toString();
    const QVariantMap args = obj.value(QStringLiteral("args")).toObject().toVariantMap();

    if (op.isEmpty()) {
        writeAll(pipe, makeErrorResponse(id, QStringLiteral("请求缺少 op 字段")));
        return;
    }

    qCDebug(lcHelper) << "处理请求, id:" << id << "op:" << op;

    const OpResult result = m_ops.dispatch(op, args, callerPid);
    if (op == QStringLiteral("quit") && result.ok) {
        m_quitRequested = true;   // 本会话响应完就关闭；主线程经队列消息退出事件循环
    }

    QJsonObject response;
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("ok"), result.ok);
    if (!result.ok) {
        response.insert(QStringLiteral("error"), result.error);
    }
    if (!result.data.isEmpty()) {
        response.insert(QStringLiteral("data"), QJsonObject::fromVariantMap(result.data));
    }
    writeAll(pipe, QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
}

} // namespace WinEase::Helper
