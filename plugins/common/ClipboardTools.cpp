#include "ClipboardTools.h"

#include "win32/InputUtils.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>

namespace WinEase::FeaturePlugins::ClipboardTools {

namespace {

/// 写完立刻读回校验，最多 3 次 —— 剪贴板是全局共享资源，
/// 别的进程正在 OpenClipboard 时写入会静默失败，重试一次基本就过了。
constexpr int kMaxWriteAttempts = 3;
constexpr int kWriteRetryDelayMs = 20;

/// 等待"复制"生效；超时后回落到剪贴板原有内容
constexpr int kSelectionWaitMs = 300;
constexpr int kSelectionPollMs = 10;

} // namespace

QString clipboardText()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    return (clipboard != nullptr) ? clipboard->text() : QString();
}

bool setText(const QString &text)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    for (int attempt = 0; attempt < kMaxWriteAttempts; ++attempt) {
        clipboard->setText(text, QClipboard::Clipboard);
        QCoreApplication::processEvents();
        if (clipboard->text() == text) {
            return true;
        }
        QThread::msleep(kWriteRetryDelayMs);
    }
    return false;
}

AcquireResult acquireText(bool useSelection)
{
    AcquireResult result;
    const QString before = clipboardText();
    result.text = before;
    result.ok = true;

    if (useSelection) {
        // 注入会被 UIPI 丢弃（前台是管理员窗口）时直接跳过，
        // 免得白发一次键盘事件，也免得日志里刷无意义的失败
        if (Win32::canSendInputToForeground() && Win32::sendCopyShortcut()) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < kSelectionWaitMs) {
                QCoreApplication::processEvents();
                const QString now = clipboardText();
                if (!now.isEmpty() && now != before) {
                    // 剪贴板确实被"复制"改写了 → 认定拿到了选中内容
                    result.text = now;
                    result.fromSelection = true;
                    return result;
                }
                QThread::msleep(kSelectionPollMs);
            }
            // 没等到变化：可能压根没有选中内容，也可能是选中的就是剪贴板里那段
            // → 两种情况的正确行为都是"用剪贴板里的内容"，因此继续往下走
        }
    }

    if (result.text.isEmpty()) {
        result.ok = false;
        result.error = QStringLiteral("没有取到文本（没有选中内容，剪贴板里也没有文本）");
    }
    return result;
}

bool pasteBack()
{
    return Win32::sendPasteShortcut();
}

} // namespace WinEase::FeaturePlugins::ClipboardTools
