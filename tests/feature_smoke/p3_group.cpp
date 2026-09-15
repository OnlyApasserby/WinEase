#include "p3_group.h"

#include "UnlockPolicy.h"

#include "win32/RestartManager.h"
#include "win32/StorageDevices.h"
#include "win32/VirtualDesktop.h"

#include <windows.h>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QThread>
#include <QWidget>

namespace FeatureSmoke {

namespace {

using WinEase::Win32::FileLocker;
using WinEase::Win32::RemovalClass;
using WinEase::Win32::StorageDeviceInfo;

const QString kUsbId = QStringLiteral("security.usb_control");
const QString kUnlockId = QStringLiteral("file.unlock");
const QString kVdeskId = QStringLiteral("window.virtual_desktop");

// P3-01 面板控件对象名（收窄后是**只读一览**：桌面列表 + 窗口一览）
const QString kVdeskPanel = QStringLiteral("vdeskPanel");
const QString kVdeskStatusLabel = QStringLiteral("vdeskStatusLabel");
const QString kVdeskDesktopList = QStringLiteral("vdeskDesktopList");
const QString kVdeskWindowList = QStringLiteral("vdeskWindowList");
const QString kVdeskDetailLabel = QStringLiteral("vdeskDetailLabel");
const QString kVdeskRefreshButton = QStringLiteral("vdeskRefreshButton");
const QString kVdeskHintLabel = QStringLiteral("vdeskHintLabel");

// P3-02 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kUnlockPanel = QStringLiteral("unlockPanel");
const QString kUnlockStatusLabel = QStringLiteral("unlockStatusLabel");
const QString kUnlockResultLabel = QStringLiteral("unlockResultLabel");
const QString kUnlockPathEdit = QStringLiteral("unlockPathEdit");
const QString kUnlockLockerList = QStringLiteral("unlockLockerList");
const QString kUnlockDetailLabel = QStringLiteral("unlockDetailLabel");
const QString kUnlockConfirmCheck = QStringLiteral("unlockConfirmCheck");
const QString kUnlockKillButton = QStringLiteral("unlockKillButton");
const QString kUnlockDeleteButton = QStringLiteral("unlockDeleteButton");
const QString kUnlockScanButton = QStringLiteral("unlockScanButton");
const QString kUnlockBrowseButton = QStringLiteral("unlockBrowseButton");
const QString kUnlockPasteButton = QStringLiteral("unlockPasteButton");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kUsbPanelName = QStringLiteral("usbPanel");
const QString kUsbStatusLabel = QStringLiteral("usbStatusLabel");
const QString kUsbDetailLabel = QStringLiteral("usbDetailLabel");
const QString kUsbEjectResultLabel = QStringLiteral("usbEjectResultLabel");
const QString kUsbDeviceList = QStringLiteral("usbDeviceList");
const QString kUsbEjectButton = QStringLiteral("usbEjectButton");
const QString kUsbRefreshButton = QStringLiteral("usbRefreshButton");
const QString kUsbConfirmCheck = QStringLiteral("usbConfirmCheck");

void settle(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

template <typename T>
T *childNamed(QWidget *root, const QString &objectName)
{
    return root != nullptr ? root->findChild<T *>(objectName) : nullptr;
}

QString shorten(const QString &text, int limit = 140)
{
    return text.size() <= limit ? text : text.left(limit) + QStringLiteral("…");
}

/// 某个盘符的"盘符类型"（就是我们**不该**用来判可移动性的那个东西）
UINT driveTypeOf(const QString &driveLetter)
{
    if (driveLetter.isEmpty()) {
        return DRIVE_UNKNOWN;
    }
    const std::wstring root = (driveLetter + QStringLiteral("\\")).toStdWString();
    return ::GetDriveTypeW(root.c_str());
}

// ===========================================================================
//  P3-01 虚拟桌面窗口一览（window.virtual_desktop）
//
//  ★ 收窄后的定位：**只读一览**（2026-09-14 按用户要求）。原来那条"把窗口搬到另一个
//    虚拟桌面"用公开 API 做不到（实测 `MoveWindowToDesktop` 只允许搬本进程自己的窗口），
//    唯一替代路子（系统快捷键 Win+Ctrl+Shift+←/→）要抢用户的前台焦点 —— 已砍掉。
//    因此本段只验两组事：**看得对不对**（桌面构成 / 每个桌面的窗口一览）与
//    **会不会乱动东西**（只读承诺：不搬移、不切桌面、面板上连这类按钮都没有）。
// ===========================================================================

void runVirtualDesktopCase(Reporter &reporter, WinEase::PluginManager &manager,
                           StubServices &services, ProbeWindow &probe)
{
    using WinEase::Win32::DesktopWindowInfo;
    using WinEase::Win32::VirtualDesktopInfo;

    // 只读功能：不经过提权助手，也不需要任何写操作
    Q_UNUSED(services)

    // ---- ① 探测（只读）：全靠"枚举窗口反查 GUID" ----
    QString error;
    const QList<VirtualDesktopInfo> desktops = WinEase::Win32::virtualDesktops(&error);
    reporter.check(error.isEmpty(),
                   QStringLiteral("P3-01 ★ 只用公开接口就能探测虚拟桌面"
                                  "（枚举顶层窗口反查 GUID，免提权、只读）"),
                   error);

    int currentCount = 0;
    int totalWindows = 0;
    for (const VirtualDesktopInfo &info : desktops) {
        currentCount += info.isCurrent ? 1 : 0;
        totalWindows += info.windowCount;
    }
    reporter.check(!desktops.isEmpty() && currentCount == 1,
                   QStringLiteral("P3-01 ★★ 恰好有**一个**桌面被标为「当前桌面」"
                                  "（实测踩过：被 DWM 隐去的窗口会回答「是」，"
                                  "曾出现两个「当前桌面」）"),
                   QStringLiteral("%1 个桌面 / 当前桌面 %2 个")
                       .arg(desktops.size())
                       .arg(currentCount));
    reporter.check(totalWindows > 0,
                   QStringLiteral("P3-01 窗口计数不是空的（能数出每个桌面上有几个窗口）"),
                   QStringLiteral("合计 %1 个窗口").arg(totalWindows));

    {
        QStringList dump;
        for (const VirtualDesktopInfo &info : desktops) {
            // 桌面标识也打出来：多桌面机器上"哪个是哪个"只能靠 GUID 分辨
            dump << QStringLiteral("%1{%2}[%3]")
                        .arg(info.label(), info.id.left(8),
                             info.sampleTitles.join(QChar(0x002F)));
        }
        reporter.info(QStringLiteral("P3-01 探测到的桌面：%1")
                          .arg(dump.isEmpty() ? QStringLiteral("（无）")
                                              : dump.join(QStringLiteral(" | "))));
    }

    // ★ "窗口一览"与"窗口计数"必须来自**同一份数据**：条数对不上就说明界面会骗人
    bool listMatchesCount = true;
    for (const VirtualDesktopInfo &info : desktops) {
        if (info.windows.size() != info.windowCount) {
            listMatchesCount = false;
            break;
        }
    }
    reporter.check(listMatchesCount,
                   QStringLiteral("P3-01 ★ 每个桌面的「窗口一览」条数 == 窗口计数（同一份数据）"));

    const QList<VirtualDesktopInfo> again = WinEase::Win32::virtualDesktops();
    bool stable = (again.size() == desktops.size());
    if (stable) {
        for (int index = 0; index < desktops.size(); ++index) {
            if (again.at(index).id != desktops.at(index).id
                || again.at(index).isCurrent != desktops.at(index).isCurrent) {
                stable = false;
                break;
            }
        }
    }
    reporter.check(stable,
                   QStringLiteral("P3-01 探测结果可复现（排序稳定，界面不会自己跳来跳去）"));

    // ---- ② 只读查询：某个窗口在哪个桌面 / 在不在当前桌面 ----
    QString probeDesktop;
    QString probeError;
    const bool probeDesktopOk =
        WinEase::Win32::windowDesktopId(probe.handle(), &probeDesktop, &probeError);
    reporter.check(probeDesktopOk && !probeDesktop.isEmpty(),
                   QStringLiteral("P3-01 ★ 能查出任意窗口在哪个虚拟桌面"),
                   probeDesktopOk ? probeDesktop : probeError);

    bool onCurrent = false;
    QString currentError;
    const bool onCurrentOk =
        WinEase::Win32::isWindowOnCurrentDesktop(probe.handle(), &onCurrent, &currentError);
    reporter.check(onCurrentOk && onCurrent,
                   QStringLiteral("P3-01 ★ 探针窗口被判定为「在当前桌面上」（只读判断）"),
                   onCurrentOk ? QStringLiteral("onCurrent=%1").arg(onCurrent) : currentError);

    {
        QString shellId;
        QString shellError;
        const bool shellOk =
            WinEase::Win32::windowDesktopId(::GetShellWindow(), &shellId, &shellError);
        reporter.info(QStringLiteral("P3-01 shell 窗口的观测：%1")
                          .arg(shellOk ? QStringLiteral("在桌面 %1").arg(shellId) : shellError));
    }

    // ---- ③ 错误码人话 + 标识校验（纯函数：这类映射不该靠现场碰运气）----
    reporter.check(
        WinEase::Win32::virtualDesktopError(TYPE_E_ELEMENTNOTFOUND)
                .contains(QStringLiteral("不属于任何虚拟桌面"))
            && WinEase::Win32::virtualDesktopError(E_INVALIDARG).contains(QStringLiteral("无效")),
        QStringLiteral("P3-01 ★ 关键失败码都有人话（外壳 / 工具窗口不属于任何虚拟桌面）"),
        WinEase::Win32::virtualDesktopError(TYPE_E_ELEMENTNOTFOUND));

    reporter.check(!WinEase::Win32::isValidDesktopId(QStringLiteral("not-a-guid"))
                       && !WinEase::Win32::isValidDesktopId(
                              QStringLiteral("00000000-0000-0000-0000-000000000000"))
                       && WinEase::Win32::normalizeDesktopId(
                              QStringLiteral("{aa509086-5ca9-4c25-8f95-589d3c07b48a}"))
                              == QStringLiteral("AA509086-5CA9-4C25-8F95-589D3C07B48A"),
                   QStringLiteral("P3-01 桌面标识的校验与归一化（非法串与全零 GUID 都要拒绝）"));

    // ---- ④ ★★ 端到端：那个窗口真的出现在"当前桌面"的窗口一览里 ----
    {
        const VirtualDesktopInfo *current = nullptr;
        for (const VirtualDesktopInfo &info : desktops) {
            if (info.isCurrent) {
                current = &info;
            }
        }
        const QString probeTitle = WinEase::Win32::windowTitle(probe.handle());
        const quint32 probePid = WinEase::Win32::windowProcessId(probe.handle());

        const DesktopWindowInfo *found = nullptr;
        if (current != nullptr) {
            for (const DesktopWindowInfo &window : current->windows) {
                if (window.handle == probe.handle()) {
                    found = &window;
                    break;
                }
            }
        }

        reporter.check(found != nullptr && !probeTitle.isEmpty() && found->title == probeTitle
                           && found->processId == probePid && !found->processName.isEmpty(),
                       QStringLiteral("P3-01 ★★ 「当前桌面」的窗口一览里能查到探针窗口"
                                      "（标题 / PID / 进程名都对得上）"),
                       found != nullptr
                           ? QStringLiteral("%1 · PID %2").arg(found->label()).arg(found->processId)
                           : QStringLiteral("没找到（当前桌面 %1 个窗口）")
                                 .arg(current != nullptr ? current->windows.size() : 0));
    }

    // ---- ⑤ 面板：只读一览 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kVdeskId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P3-01 插件已从插件目录加载（window.virtual_desktop）"));
    if (plugin == nullptr) {
        return;
    }

    reporter.check(!plugin->supportsHotkey(),
                   QStringLiteral("P3-01 ★ 只读一览不再声明快捷键"
                                  "（原来那条 Ctrl+Alt+G 是搬窗口用的，功能已砍）"));
    reporter.check(plugin->detailedDescription().contains(QStringLiteral("只读"))
                       && plugin->detailedDescription().contains(
                           QStringLiteral("MoveWindowToDesktop"))
                       && plugin->detailedDescription().size() > plugin->description().size(),
                   QStringLiteral("P3-01 ★ 插件提供了「关于插件」用的**完整说明**"
                                  "（比卡片上那一行长，且写清为什么只做只读）"),
                   shorten(plugin->detailedDescription()));

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kVdeskPanel,
                   QStringLiteral("P3-01 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kVdeskStatusLabel);
    auto *desktopList = childNamed<QListWidget>(settings, kVdeskDesktopList);
    auto *windowList = childNamed<QListWidget>(settings, kVdeskWindowList);
    auto *detailLabel = childNamed<QLabel>(settings, kVdeskDetailLabel);
    auto *refreshButton = childNamed<QPushButton>(settings, kVdeskRefreshButton);
    reporter.check(statusLabel != nullptr && desktopList != nullptr && windowList != nullptr
                       && detailLabel != nullptr && refreshButton != nullptr
                       && childNamed<QLabel>(settings, kVdeskHintLabel) != nullptr,
                   QStringLiteral("P3-01 面板控件按对象名全部找到"
                                  "（状态 / 桌面列表 / 窗口一览 / 详情 / 刷新 / 说明）"));
    if (statusLabel == nullptr || desktopList == nullptr || windowList == nullptr
        || detailLabel == nullptr || refreshButton == nullptr) {
        delete settings;
        return;
    }

    // ★★ 只读承诺（界面上真的没有那些按钮）：按"不该存在的对象名"逐个查
    {
        const QStringList forbidden = { QStringLiteral("vdeskMoveButton"),
                                        QStringLiteral("vdeskPrevButton"),
                                        QStringLiteral("vdeskNextButton"),
                                        QStringLiteral("vdeskExpPrevButton"),
                                        QStringLiteral("vdeskExpNextButton"),
                                        QStringLiteral("vdeskExperimentalCheck") };
        QStringList stillThere;
        for (const QString &name : forbidden) {
            if (settings->findChild<QWidget *>(name) != nullptr) {
                stillThere << name;
            }
        }
        reporter.check(stillThere.isEmpty(),
                       QStringLiteral("P3-01 ★★ 面板上没有「搬移 / 切桌面 / 实验性」控件的残留"
                                      "（功能收窄到只读一览）"),
                       stillThere.isEmpty() ? QStringLiteral("（都没有）")
                                            : stillThere.join(QStringLiteral(", ")));
    }

    reporter.check(manager.setPluginEnabled(kVdeskId, true),
                   QStringLiteral("P3-01 插件启用成功"), plugin->lastError());
    settle(400);

    refreshButton->click();
    settle(400);
    const QList<VirtualDesktopInfo> live = WinEase::Win32::virtualDesktops();
    reporter.check(desktopList->count() == live.size(),
                   QStringLiteral("P3-01 ★ 面板列表条数 == 平台层探测结果（界面没有自己再筛一遍）"),
                   QStringLiteral("面板 %1 条 / 平台层 %2 条")
                       .arg(desktopList->count())
                       .arg(live.size()));
    reporter.check(statusLabel->text().contains(QStringLiteral("个虚拟桌面")),
                   QStringLiteral("P3-01 状态栏如实报出桌面数量与探测边界"),
                   shorten(statusLabel->text()));
    reporter.check(desktopList->count() > 0 && !desktopList->item(0)->text().isEmpty(),
                   QStringLiteral("P3-01 列表项带上了「当前/其它 + 窗口数 + 标题样例」"),
                   desktopList->count() > 0 ? shorten(desktopList->item(0)->text()) : QString());

    // 选中"当前桌面" → 窗口一览应当列出它的窗口，而且里面有探针窗口
    {
        int currentRow = -1;
        for (int index = 0; index < live.size(); ++index) {
            if (live.at(index).isCurrent) {
                currentRow = index;
                break;
            }
        }
        if (currentRow < 0) {
            reporter.info(QStringLiteral("P3-01 前置不成立：探测结果里没有标出「当前桌面」→ "
                                         "界面那一组无法验证（如实跳过，不算通过）"));
        } else {
            desktopList->setCurrentRow(currentRow);
            settle(250);
            reporter.check(windowList->count() == live.at(currentRow).windows.size(),
                           QStringLiteral("P3-01 ★ 选中一个桌面后，窗口一览条数 == 该桌面的窗口数"),
                           QStringLiteral("列表 %1 条 / 平台层 %2 条")
                               .arg(windowList->count())
                               .arg(live.at(currentRow).windows.size()));

            QStringList listed;
            for (int row = 0; row < windowList->count(); ++row) {
                listed << windowList->item(row)->text();
            }
            const QString probeTitle = WinEase::Win32::windowTitle(probe.handle());
            bool probeListed = false;
            for (const QString &text : listed) {
                if (!probeTitle.isEmpty() && text.contains(probeTitle)) {
                    probeListed = true;
                    break;
                }
            }
            reporter.check(probeListed,
                           QStringLiteral("P3-01 ★★ 界面上的窗口一览里能看到探针窗口"
                                          "（「我那个窗口在哪个桌面」就是这个功能的价值）"),
                           listed.isEmpty() ? QStringLiteral("（列表为空）")
                                            : shorten(listed.join(QStringLiteral(" | "))));
            reporter.check(detailLabel->text().contains(QStringLiteral("正在看")),
                           QStringLiteral("P3-01 详情里说明这个桌面就是**当前**桌面"),
                           shorten(detailLabel->text()));
        }
    }

    // 切到"其它桌面"（多桌面机器上）→ 窗口一览跟着换；只有一个桌面时如实跳过
    {
        int otherRow = -1;
        for (int index = 0; index < live.size(); ++index) {
            if (!live.at(index).isCurrent) {
                otherRow = index;
                break;
            }
        }
        if (otherRow < 0) {
            reporter.info(QStringLiteral("P3-01 本机只探测到 1 个虚拟桌面 → "
                                         "「切到别的桌面看它的窗口」无法验证（如实跳过，不算通过）"));
        } else {
            desktopList->setCurrentRow(otherRow);
            settle(250);
            reporter.check(windowList->count() == live.at(otherRow).windows.size(),
                           QStringLiteral("P3-01 ★★ 换一个桌面，窗口一览跟着换成它的窗口"),
                           QStringLiteral("列表 %1 条 / 平台层 %2 条")
                               .arg(windowList->count())
                               .arg(live.at(otherRow).windows.size()));
            reporter.check(detailLabel->text().contains(QStringLiteral("别的")),
                           QStringLiteral("P3-01 详情里如实说明「它在别的桌面上」"),
                           shorten(detailLabel->text()));
        }
    }

    reporter.check(manager.setPluginEnabled(kVdeskId, false),
                   QStringLiteral("P3-01 插件停用成功"));
    settle(250);
    reporter.check(desktopList->count() == 0 && !refreshButton->isEnabled()
                       && windowList->count() <= 1,
                   QStringLiteral("P3-01 停用后桌面列表清空、按钮禁用（窗口一览只剩占位行）"),
                   QStringLiteral("桌面 %1 条 / 窗口 %2 条")
                       .arg(desktopList->count())
                       .arg(windowList->count()));

    // 只读承诺的收尾断言：整个用例跑完，探针窗口还在它原来的桌面上
    QString finalDesktop;
    if (WinEase::Win32::windowDesktopId(probe.handle(), &finalDesktop) && probeDesktopOk) {
        reporter.check(finalDesktop == probeDesktop,
                       QStringLiteral("P3-01 只读承诺：用例跑完探针窗口仍在原来的虚拟桌面"
                                      "（本功能从头到尾没搬过任何窗口）"),
                       QStringLiteral("读回 %1 / 原 %2").arg(finalDesktop, probeDesktop));
    }

    delete settings;
}

// ===========================================================================
//  P3-02 文件锁定解除
// ===========================================================================

/// 以"独占"方式打开（dwShareMode = 0）——"文件被占用、删不掉"的最小复现
HANDLE openExclusive(const QString &filePath)
{
    const std::wstring native = QDir::toNativeSeparators(filePath).toStdWString();
    return ::CreateFileW(native.c_str(),
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

void runFileUnlockCase(Reporter &reporter, WinEase::PluginManager &manager,
                       StubServices &services)
{
    using WinEase::Common::isDisruptive;
    using WinEase::Common::lockerRoleText;
    using WinEase::Common::needsElevation;
    using WinEase::Common::protectedReason;
    using WinEase::Common::terminationWarning;

    const quint32 selfPid = static_cast<quint32>(QCoreApplication::applicationPid());

    // ---- 素材：自检自己造的一个临时文件（不碰用户的任何文件）----
    const QString tempDir =
        QDir::temp().filePath(QStringLiteral("winease_unlock_smoke_%1").arg(selfPid));
    QDir().mkpath(tempDir);
    const QString target = tempDir + QStringLiteral("/locked.bin");
    {
        QByteArray payload;
        payload.resize(4096);
        payload.fill('x');

        QFile seed(target);
        if (seed.open(QIODevice::WriteOnly)) {
            seed.write(payload);
        }
    }
    reporter.check(QFile::exists(target),
                   QStringLiteral("P3-02 自检素材：已创建临时测试文件（不会碰用户的文件）"), target);

    // ---- ① 平台层：Restart Manager 完整自证（免提权、只读）----
    HANDLE selfLock = openExclusive(target);
    reporter.check(selfLock != INVALID_HANDLE_VALUE,
                   QStringLiteral("P3-02 前置：自检进程自己独占锁住了该文件"
                                  "（这才是「删不掉」的样子：连自检自己再开一次都会被拒）"));

    QString error;
    QList<FileLocker> lockers = WinEase::Win32::findFileLockers(target, &error);
    reporter.check(error.isEmpty(),
                   QStringLiteral("P3-02 ★ Restart Manager 查询成功（免提权、只读、零依赖）"), error);

    const FileLocker *selfLocker = nullptr;
    for (const FileLocker &locker : lockers) {
        if (locker.pid == selfPid) {
            selfLocker = &locker;
            break;
        }
    }

    // 诊断输出：`RM_APP_TYPE` 原值（RmMainWindow=1 / RmOtherWindow=2 / RmService=3 /
    // RmExplorer=4 / RmConsole=5 / RmCritical=1000 / RmUnknownApp=0）
    {
        QStringList dump;
        for (const FileLocker &locker : lockers) {
            dump << QStringLiteral("%1 type=%2 status=%3")
                        .arg(locker.describe())
                        .arg(locker.applicationType)
                        .arg(locker.restartable ? QStringLiteral("可重启")
                                                : QStringLiteral("不可重启"));
        }
        reporter.info(QStringLiteral("P3-02 占用者原始数据：%1")
                          .arg(dump.isEmpty() ? QStringLiteral("（无）")
                                              : dump.join(QStringLiteral(" | "))));
    }
    reporter.check(selfLocker != nullptr,
                   QStringLiteral("P3-02 ★★ 查到的占用者就是自检自己（PID 对得上）"),
                   lockers.isEmpty() ? QStringLiteral("一个占用者都没查到")
                                     : lockers.first().describe());
    if (selfLocker != nullptr) {
        const QString ownExe = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        reporter.check(selfLocker->name.compare(ownExe, Qt::CaseInsensitive) == 0,
                       QStringLiteral("P3-02 应用名与真实映像名一致（用户能在列表里认出是谁）"),
                       QStringLiteral("RM 报 %1 / 实际 %2").arg(selfLocker->name, ownExe));
        reporter.check(!selfLocker->path.isEmpty()
                           && QFileInfo(selfLocker->path).fileName().compare(ownExe,
                                                                            Qt::CaseInsensitive)
                                  == 0,
                       QStringLiteral("P3-02 占用者的完整路径也读到了"), selfLocker->path);
        reporter.check(selfLocker->sameSession,
                       QStringLiteral("P3-02 会话判定正确（自检在自己这个登录会话里）"),
                       QStringLiteral("sameSession=%1").arg(selfLocker->sameSession));
        // ★★ 这一条钉住一个**差点写错的判据**：Restart Manager 把「发起查询的那个程序」
        //    也标成 RmCritical(=1000)，而它的真实含义是"装不下去、得重启"（成因有三种：
        //    确实关键进程 / 没权限关它 / **它就是查询方自己**）。
        //    所以 `RmCritical` **不能**拿来当"系统关键进程"的判据：
        //    真按它禁止结束，普通程序就会变成杀不掉（本机实测：同一份 exe，
        //    作为查询方 type=1000，作为普通子进程 type=5）。
        reporter.check(selfLocker->cannotShutdown && selfLocker->applicationType == 1000,
                       QStringLiteral("P3-02 ★★ 查询方自己会被 RM 标成 RmCritical(=1000) —— "
                                      "这正是「不能拿 RmCritical 当关键进程判据」的证据"),
                       QStringLiteral("type=%1 cannotShutdown=%2")
                           .arg(selfLocker->applicationType)
                           .arg(selfLocker->cannotShutdown));
        reporter.check(protectedReason(*selfLocker, selfPid).contains(QStringLiteral("自己")),
                       QStringLiteral("P3-02 保护自己的理由与 RM 无关（是「不该杀自己」，"
                                      "不是「RM 说关不掉」）"),
                       protectedReason(*selfLocker, selfPid));
    }

    // 查询参数校验：Restart Manager 要求绝对路径，相对路径必须**当场报错**
    // 而不是悄悄给出"没人占用"（那会把用户引到完全错误的方向）
    QString relativeError;
    WinEase::Win32::findFileLockers(QStringLiteral("locked.bin"), &relativeError);
    reporter.check(!relativeError.isEmpty(),
                   QStringLiteral("P3-02 ★ 相对路径被如实拒绝（绝不返回「没人占用」这种假答案）"),
                   relativeError);

    reporter.check(WinEase::Win32::restartManagerError(ERROR_ACCESS_DENIED)
                           .contains(QStringLiteral("权限"))
                       && WinEase::Win32::restartManagerError(ERROR_FILE_NOT_FOUND)
                              .contains(QStringLiteral("不存在")),
                   QStringLiteral("P3-02 错误码中文化（权限不足 / 文件不存在 各有说法）"),
                   WinEase::Win32::restartManagerError(ERROR_ACCESS_DENIED));

    // ---- ② 保护规则与提权判定（纯函数，插件与自检共用同一份实现）----
    FileLocker fakeSelf;
    fakeSelf.pid = selfPid;
    fakeSelf.name = QStringLiteral("feature_smoke.exe");
    reporter.check(!protectedReason(fakeSelf, selfPid).isEmpty()
                       && lockerRoleText(fakeSelf, selfPid).contains(QStringLiteral("自身")),
                   QStringLiteral("P3-02 ★ 规则：WinEase 自身**不允许结束**，并说明理由"),
                   protectedReason(fakeSelf, selfPid));

    // system-owned 的进程：依据是**名单**（受保护进程），不是 RM 的返回值
    FileLocker systemProc;
    systemProc.pid = 4;
    systemProc.name = QStringLiteral("System");
    reporter.check(protectedReason(systemProc, selfPid).contains(QStringLiteral("操作系统"))
                       && WinEase::Common::isSystemProcessName(QStringLiteral("csrss.exe"))
                       && WinEase::Common::isSystemProcessName(QStringLiteral("lsass.exe"))
                       && !WinEase::Common::isSystemProcessName(QStringLiteral("notepad.exe")),
                   QStringLiteral("P3-02 ★ 规则：操作系统的组成部分不允许结束"
                                  "（System/csrss/lsass…，且不误伤 notepad）"),
                   protectedReason(systemProc, selfPid));

    // ⚠ 反向断言：`RmCritical` **不**构成保护理由 ——
    //   否则"系统认为关不掉"会被翻译成"系统关键进程"，普通程序就变得杀不掉
    FileLocker cannotShutdown;
    cannotShutdown.pid = 500;
    cannotShutdown.name = QStringLiteral("someapp.exe");
    cannotShutdown.cannotShutdown = true;
    reporter.check(protectedReason(cannotShutdown, selfPid).isEmpty()
                       && WinEase::Common::systemSaysCannotShutDown(cannotShutdown)
                       && !lockerRoleText(cannotShutdown, selfPid).contains(QStringLiteral("关键")),
                   QStringLiteral("P3-02 ★★ 规则：RM 的「关不掉」只当提示，"
                                  "**不作为**「不允许结束」的依据（否则普通程序会杀不掉）"),
                   lockerRoleText(cannotShutdown, selfPid));

    FileLocker elevated;
    elevated.pid = 100;
    elevated.name = QStringLiteral("admin_tool.exe");
    elevated.isElevated = true;

    FileLocker otherSession;
    otherSession.pid = 101;
    otherSession.name = QStringLiteral("other_user.exe");
    otherSession.sameSession = false;

    FileLocker plain;
    plain.pid = 200;
    plain.name = QStringLiteral("notepad.exe");

    reporter.check(protectedReason(elevated, selfPid).isEmpty() && needsElevation(elevated)
                       && protectedReason(otherSession, selfPid).isEmpty()
                       && needsElevation(otherSession) && !needsElevation(plain)
                       && protectedReason(plain, selfPid).isEmpty(),
                   QStringLiteral("P3-02 规则：管理员进程 / 其它会话的进程走提权助手，"
                                  "普通进程本地结束（保护规则不误伤正常人）"));

    QStringList roles;
    for (const FileLocker &locker : { fakeSelf, systemProc, elevated, otherSession, plain }) {
        roles << lockerRoleText(locker, selfPid);
    }
    roles.removeDuplicates();
    reporter.check(roles.size() == 5,
                   QStringLiteral("P3-02 五种身份标签互不重复（列表里一眼看得出这是什么进程）"),
                   roles.join(QStringLiteral(" / ")));

    FileLocker explorer;
    explorer.pid = 300;
    explorer.name = QStringLiteral("explorer.exe");
    explorer.isExplorer = true;
    reporter.check(isDisruptive(explorer) && terminationWarning({ explorer })
                                                   .contains(QStringLiteral("资源管理器"))
                       && terminationWarning({ plain }).contains(QStringLiteral("未保存")),
                   QStringLiteral("P3-02 ★ 二次确认的文案说到点子上（资源管理器会消失 / "
                                  "没保存的内容会丢）"),
                   terminationWarning({ explorer }));

    // ---- ③ 面板：查得到、看得见、受保护的那一行杀不掉 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kUnlockId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P3-02 插件已从插件目录加载（file.unlock）"));
    if (plugin == nullptr) {
        if (selfLock != INVALID_HANDLE_VALUE) {
            ::CloseHandle(selfLock);
        }
        QDir(tempDir).removeRecursively();
        return;
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kUnlockPanel,
                   QStringLiteral("P3-02 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        if (selfLock != INVALID_HANDLE_VALUE) {
            ::CloseHandle(selfLock);
        }
        QDir(tempDir).removeRecursively();
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kUnlockStatusLabel);
    auto *resultLabel = childNamed<QLabel>(settings, kUnlockResultLabel);
    auto *pathEdit = childNamed<QLineEdit>(settings, kUnlockPathEdit);
    auto *list = childNamed<QListWidget>(settings, kUnlockLockerList);
    auto *detailLabel = childNamed<QLabel>(settings, kUnlockDetailLabel);
    auto *confirmCheck = childNamed<QCheckBox>(settings, kUnlockConfirmCheck);
    auto *killButton = childNamed<QPushButton>(settings, kUnlockKillButton);
    auto *deleteButton = childNamed<QPushButton>(settings, kUnlockDeleteButton);
    auto *scanButton = childNamed<QPushButton>(settings, kUnlockScanButton);

    reporter.check(statusLabel != nullptr && resultLabel != nullptr && pathEdit != nullptr
                       && list != nullptr && detailLabel != nullptr && confirmCheck != nullptr
                       && killButton != nullptr && deleteButton != nullptr
                       && scanButton != nullptr
                       && childNamed<QPushButton>(settings, kUnlockBrowseButton) != nullptr
                       && childNamed<QPushButton>(settings, kUnlockPasteButton) != nullptr,
                   QStringLiteral("P3-02 面板控件按对象名全部找到"));
    if (statusLabel == nullptr || resultLabel == nullptr || pathEdit == nullptr || list == nullptr
        || detailLabel == nullptr || confirmCheck == nullptr || killButton == nullptr
        || deleteButton == nullptr || scanButton == nullptr) {
        delete settings;
        if (selfLock != INVALID_HANDLE_VALUE) {
            ::CloseHandle(selfLock);
        }
        QDir(tempDir).removeRecursively();
        return;
    }

    reporter.check(manager.setPluginEnabled(kUnlockId, true),
                   QStringLiteral("P3-02 插件启用成功"), plugin->lastError());
    settle(300);

    services.elevation()->clearCalls();

    pathEdit->setText(QDir::toNativeSeparators(target));
    scanButton->click();
    settle(500);

    reporter.check(list->count() == lockers.size(),
                   QStringLiteral("P3-02 ★ 面板列出的条数 == 平台层查询结果（界面没有自己再筛一遍）"),
                   QStringLiteral("面板 %1 条 / 平台层 %2 条")
                       .arg(list->count())
                       .arg(lockers.size()));
    reporter.check(statusLabel->text().contains(QStringLiteral("%1 个").arg(lockers.size())),
                   QStringLiteral("P3-02 状态栏如实报出占用者数量"), shorten(statusLabel->text()));

    if (selfLocker != nullptr) {
        list->setCurrentRow(0);
        settle(200);
        reporter.check(detailLabel->text().contains(QStringLiteral("不允许结束")),
                       QStringLiteral("P3-02 ★★ 详情里明确写出「不允许结束」及理由"
                                      "（而不是等用户点了才报错）"),
                       shorten(detailLabel->text()));

        confirmCheck->setChecked(true);
        settle(200);
        reporter.check(!killButton->isEnabled(),
                       QStringLiteral("P3-02 ★★ 受保护的那一行，**即使勾上确认**「结束」按钮仍然禁用"));

        confirmCheck->setChecked(false);
        settle(150);
        reporter.check(!killButton->isEnabled(),
                       QStringLiteral("P3-02 没勾选确认时「结束选中的进程」是禁用的（二次确认是硬门槛）"));
    } else {
        reporter.info(QStringLiteral("P3-02 前置不成立（自检没能把自己写进占用者列表），"
                                     "「受保护的行杀不掉」这一条无法断言"));
    }

    // 关掉自己的句柄 → 占用者消失（证明查询结果跟着真实句柄走，不是缓存）
    if (selfLock != INVALID_HANDLE_VALUE) {
        ::CloseHandle(selfLock);
        selfLock = INVALID_HANDLE_VALUE;
    }
    bool stillSelf = false;
    for (const FileLocker &locker : WinEase::Win32::findFileLockers(target)) {
        stillSelf = stillSelf || locker.pid == selfPid;
    }
    reporter.check(!stillSelf,
                   QStringLiteral("P3-02 ★ 关掉句柄后自己立刻从占用者列表里消失（不是缓存结果）"));

    // ---- ④ 真实占用者（子进程）→ 结束它 → 占用解除 → 能删掉 ----
    FileLockProbe probe;
    QString probeError;
    reporter.check(probe.start(target, &probeError),
                   QStringLiteral("P3-02 占用源就绪：另一个进程独占锁住了该文件（P3-02 的真实场景）"),
                   probeError);
    reporter.check(probe.isRunning() && probe.pid() != 0,
                   QStringLiteral("P3-02 占用源进程存活"), QStringLiteral("PID %1").arg(probe.pid()));

    // 前置：此刻文件**真的**打不开/删不掉（自检自己也开不了）
    const HANDLE probeCheck = openExclusive(target);
    reporter.check(probeCheck == INVALID_HANDLE_VALUE,
                   QStringLiteral("P3-02 前置：此刻该文件被独占（这就是「被占用、删不掉」的硬证据）"));
    if (probeCheck != INVALID_HANDLE_VALUE) {
        ::CloseHandle(probeCheck);
    }

    scanButton->click();
    settle(500);
    const QList<FileLocker> childLockers = WinEase::Win32::findFileLockers(target);
    {
        QStringList dump;
        for (const FileLocker &locker : childLockers) {
            dump << QStringLiteral("%1 type=%2").arg(locker.describe()).arg(locker.applicationType);
        }
        reporter.info(QStringLiteral("P3-02 子进程占用者的原始数据：%1")
                          .arg(dump.join(QStringLiteral(" | "))));
    }
    reporter.check(childLockers.size() == 1 && childLockers.first().applicationType == 5
                       && !childLockers.first().cannotShutdown,
                   QStringLiteral("P3-02 ★ 同一份 exe 作为**普通子进程**时是 RmConsole(=5)、"
                                  "不是 RmCritical —— 与上面那条一起证明 RmCritical 不是"
                                  "「关键进程」判据"),
                   childLockers.isEmpty()
                       ? QStringLiteral("没查到")
                       : QStringLiteral("type=%1").arg(childLockers.first().applicationType));
    reporter.check(list->count() == 1,
                   QStringLiteral("P3-02 面板只列出那一个真实占用者"),
                   QStringLiteral("%1 条：%2")
                       .arg(list->count())
                       .arg(list->count() > 0 ? list->item(0)->text() : QString()));
    if (list->count() == 1) {
        reporter.check(list->item(0)->text().contains(QString::number(probe.pid())),
                       QStringLiteral("P3-02 列表项里带着占用者的 PID"),
                       shorten(list->item(0)->text()));
        list->setCurrentRow(0);
        settle(200);
        reporter.check(detailLabel->text().contains(QStringLiteral("普通程序")),
                       QStringLiteral("P3-02 普通进程被如实标成「普通程序」（没有被当成危险对象）"),
                       shorten(detailLabel->text()));
    }

    // ⚠ 还没解除占用就删：**必须失败**，而且要说出还剩谁在占用
    //    （"删不掉"是用户进来时的处境，插件不能假装成功）
    deleteButton->click();
    settle(1000);
    reporter.check(QFile::exists(target),
                   QStringLiteral("P3-02 ★★ 还没解除占用时「重试删除」如实失败 —— 文件仍在原处"
                                  "（绝不静默硬删，也绝不假装成功）"));
    reporter.check(resultLabel->text().contains(QStringLiteral("失败"))
                       && resultLabel->text().contains(QStringLiteral("仍被占用")),
                   QStringLiteral("P3-02 ★ 失败时会说出「还剩谁在占用」，而不是把用户丢回原点"),
                   shorten(resultLabel->text(), 200));

    // 真正结束它
    confirmCheck->setChecked(true);
    settle(200);
    reporter.check(killButton->isEnabled(),
                   QStringLiteral("P3-02 普通占用者 + 已勾选确认 → 「结束选中的进程」可点"));
    killButton->click();

    const bool killed = probe.waitFinished(6000);
    reporter.check(killed,
                   QStringLiteral("P3-02 ★★ 点「结束选中的进程」后，占用者进程真的退出了"),
                   QStringLiteral("PID %1").arg(probe.pid()));
    if (!killed) {
        probe.stop(); // 兜底：别把子进程留到用例之外
    }

    reporter.check(waitFor([&target] { return WinEase::Win32::findFileLockers(target).isEmpty(); },
                           5000),
                   QStringLiteral("P3-02 ★★ 占用真的解除了（Restart Manager 再也查不到占用者）"));
    reporter.check(waitFor([&list] { return list->count() == 0; }, 5000),
                   QStringLiteral("P3-02 面板列表跟着刷新到 0 条（不需要用户手点刷新）"),
                   QStringLiteral("%1 条").arg(list->count()));

    reporter.check(services.elevation()->callCount(QStringLiteral("killProcess")) == 0,
                   QStringLiteral("P3-02 ★ 普通进程由本地结束，**没有惊动提权助手**"
                                  "（能本地做的绝不弹 UAC）"),
                   QStringLiteral("killProcess 调用 %1 次")
                       .arg(services.elevation()->callCount(QStringLiteral("killProcess"))));
    reporter.info(QStringLiteral("P3-02 说明：需要提权的分支（管理员进程 / 其它登录会话）"
                                 "由上面的纯函数断言覆盖 —— 本机没有一种"
                                 "「可安全制造、又能被提权结束」的占用者，端到端留手测"));

    // 现在能删了：走回收站（可还原）
    confirmCheck->setChecked(false);
    deleteButton->click();
    settle(1200);
    reporter.check(!QFile::exists(target),
                   QStringLiteral("P3-02 ★★ 解除占用之后「重试删除」成功（文件已移出原位置）"));
    reporter.check(resultLabel->text().contains(QStringLiteral("回收站")),
                   QStringLiteral("P3-02 删除走的是**回收站**（可还原），不是永久删除"),
                   shorten(resultLabel->text()));

    // ---- ⑤ 停用：列表清空、按钮禁用、如实说明 ----
    reporter.check(manager.setPluginEnabled(kUnlockId, false),
                   QStringLiteral("P3-02 插件停用成功"));
    settle(300);
    reporter.check(list->count() == 0 && !killButton->isEnabled() && !deleteButton->isEnabled(),
                   QStringLiteral("P3-02 停用后列表清空、结束与删除按钮都禁用"));
    reporter.check(statusLabel->text().contains(QStringLiteral("已停用")),
                   QStringLiteral("P3-02 停用后如实说明「没有结束任何进程，也没有删除任何文件」"),
                   shorten(statusLabel->text()));

    delete settings;
    probe.stop();
    QFile::remove(target); // 若上一步的删除没成功，别把残留留在临时目录里
    QDir(tempDir).removeRecursively();
}

// ===========================================================================
//  P3-14 USB 设备管控
// ===========================================================================

void runUsbControlCase(Reporter &reporter, WinEase::PluginManager &manager,
                       StubServices &services)
{
    Q_UNUSED(services)

    // ---- ① 平台层判据（纯数据，不需要面板）----
    QString error;
    const QList<StorageDeviceInfo> all = WinEase::Win32::allStorageDevices(&error);
    const QList<StorageDeviceInfo> removable = WinEase::Win32::removableStorageDevices(&error);

    reporter.check(!all.isEmpty() && error.isEmpty(),
                   QStringLiteral("P3-14 平台层能枚举存储设备（SetupAPI + IOCTL，只读）"), error);

    bool allDescribed = true;
    for (const StorageDeviceInfo &device : all) {
        // ⚠ 分隔符写 QChar(0x00B7)（= '·'）而不是 QLatin1Char('·')：
        //    后者是**多字节字符常量**（int），传给 QLatin1Char 会被截断，
        //    MSVC 报 C4305/C4309（截断后的值恰好还是 0xB7，但警告必须消掉）
        if (device.busText.isEmpty() || !device.describe().contains(QChar(0x00B7))) {
            allDescribed = false;
        }
    }
    reporter.check(allDescribed,
                   QStringLiteral("P3-14 每个设备都有总线/可移除性描述（界面不会出现空白项）"));

    bool removableConsistent = true;
    for (const StorageDeviceInfo &device : removable) {
        if (!device.removable()) {
            removableConsistent = false;
        }
    }
    reporter.check(removableConsistent,
                   QStringLiteral("P3-14 可移除列表里每一项的 `removable()` 都为真"));

    // ★★ 本组最重要的一条：**按盘符类型筛会漏掉移动硬盘**
    int fixedButRemovable = 0;
    QStringList fixedButRemovableNames;
    for (const StorageDeviceInfo &device : removable) {
        const UINT type = driveTypeOf(device.driveLetter);
        if (type == DRIVE_FIXED) {
            ++fixedButRemovable;
            fixedButRemovableNames << device.describe();
        }
    }
    if (fixedButRemovable > 0) {
        reporter.check(true,
                       QStringLiteral("P3-14 ★★ 存在「盘符类型是固定盘、但实际可安全移除」的设备，"
                                      "而它**没有被漏掉**（这正是移动硬盘的情形，"
                                      "也是预研修正的错误判据）"),
                       fixedButRemovableNames.join(QStringLiteral(" | ")));
    } else {
        reporter.info(QStringLiteral("P3-14 本机当前没有「固定盘类型但是可移除」的设备"
                                     "（没接移动硬盘时就是这种情况）—— "
                                     "该判据的有效性由下面那条纯函数断言兜底"));
    }

    // 纯函数：容量格式化（边界）
    reporter.check(WinEase::Win32::humanizeBytes(0) == QStringLiteral("0 字节")
                       && WinEase::Win32::humanizeBytes(1024) == QStringLiteral("1 KB")
                       && WinEase::Win32::humanizeBytes(1024ULL * 1024 * 1024)
                              == QStringLiteral("1.0 GB")
                       && WinEase::Win32::humanizeBytes(1024ULL * 1024 * 1024 * 1024)
                              == QStringLiteral("1.00 TB"),
                   QStringLiteral("P3-14 容量格式化（字节/KB/GB/TB 边界）"),
                   QStringLiteral("%1 / %2 / %3 / %4")
                       .arg(WinEase::Win32::humanizeBytes(0),
                            WinEase::Win32::humanizeBytes(1024),
                            WinEase::Win32::humanizeBytes(1024ULL * 1024 * 1024),
                            WinEase::Win32::humanizeBytes(1024ULL * 1024 * 1024 * 1024)));

    // 纯函数：弹出失败原因中文化（"说清为什么"是这个功能的验收要求）
    reporter.check(WinEase::Win32::ejectFailureReason(ERROR_DEVICE_IN_USE)
                           .contains(QStringLiteral("正在使用"))
                       && WinEase::Win32::ejectFailureReason(ERROR_ACCESS_DENIED)
                              .contains(QStringLiteral("权限"))
                       && WinEase::Win32::ejectFailureReason(ERROR_NOT_SUPPORTED)
                              .contains(QStringLiteral("不支持")),
                   QStringLiteral("P3-14 弹出失败原因中文化（占用/权限/不支持 各有说法）"),
                   WinEase::Win32::ejectFailureReason(ERROR_DEVICE_IN_USE));

    reporter.check(WinEase::Win32::removalClassText(RemovalClass::SurpriseRemoval)
                           != WinEase::Win32::removalClassText(RemovalClass::OrderlyRemoval)
                       && WinEase::Win32::removalClassText(RemovalClass::NoRemoval)
                              .contains(QStringLiteral("固定")),
                   QStringLiteral("P3-14 三档可移除性各有人话（可随时拔出 / 需安全弹出 / 固定盘）"));

    // ---- ② 插件与面板 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kUsbId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P3-14 插件已从插件目录加载（security.usb_control）"));
    if (plugin == nullptr) {
        return;
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kUsbPanelName,
                   QStringLiteral("P3-14 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        return;
    }

    auto *statusLabel = childNamed<QLabel>(settings, kUsbStatusLabel);
    auto *detailLabel = childNamed<QLabel>(settings, kUsbDetailLabel);
    auto *list = childNamed<QListWidget>(settings, kUsbDeviceList);
    auto *ejectButton = childNamed<QPushButton>(settings, kUsbEjectButton);
    auto *refreshButton = childNamed<QPushButton>(settings, kUsbRefreshButton);
    auto *confirmCheck = childNamed<QCheckBox>(settings, kUsbConfirmCheck);

    reporter.check(statusLabel != nullptr && detailLabel != nullptr && list != nullptr
                       && ejectButton != nullptr && refreshButton != nullptr
                       && confirmCheck != nullptr,
                   QStringLiteral("P3-14 面板控件按对象名全部找到"));
    if (statusLabel == nullptr || detailLabel == nullptr || list == nullptr
        || ejectButton == nullptr || refreshButton == nullptr || confirmCheck == nullptr) {
        delete settings;
        return;
    }

    reporter.check(manager.setPluginEnabled(kUsbId, true),
                   QStringLiteral("P3-14 插件启用成功"), plugin->lastError());
    settle(300);

    reporter.check(list->count() == removable.size(),
                   QStringLiteral("P3-14 ★ 面板列表条数 == 平台层枚举结果（界面没有自己再筛一遍）"),
                   QStringLiteral("面板 %1 条 / 平台层 %2 条")
                       .arg(list->count())
                       .arg(removable.size()));

    if (removable.isEmpty()) {
        reporter.info(QStringLiteral("P3-14 本机当前没有可安全移除的设备 → "
                                     "下面几条端到端断言无从触发（如实跳过，不是通过）"));
        delete settings;
        return;
    }

    reporter.check(statusLabel->text().contains(QStringLiteral("%1 个").arg(removable.size())),
                   QStringLiteral("P3-14 状态栏如实报出设备数量"), shorten(statusLabel->text()));

    list->setCurrentRow(0);
    settle(200);
    const StorageDeviceInfo first = removable.first();
    reporter.check(detailLabel->text().contains(first.busText),
                   QStringLiteral("P3-14 详情里写明总线类型（USB/NVMe…）"),
                   shorten(detailLabel->text()));
    reporter.check(!first.label.isEmpty()
                       ? detailLabel->text().contains(first.label)
                       : detailLabel->text().contains(QStringLiteral("文件系统")),
                   QStringLiteral("P3-14 详情里带上卷标与文件系统"), shorten(detailLabel->text()));

    // ---- ③ 二次确认：这是自检能验证的那一半（另一半是真正的弹出，留手测）----
    confirmCheck->setChecked(false);
    settle(150);
    reporter.check(!ejectButton->isEnabled(),
                   QStringLiteral("P3-14 ★ 没勾选确认时「安全弹出」是**禁用**的"
                                  "（弹出会摘掉设备，正在写入时可能丢数据）"));

    confirmCheck->setChecked(true);
    settle(150);
    reporter.check(ejectButton->isEnabled(),
                   QStringLiteral("P3-14 勾选确认后「安全弹出」才可用"));

    // ⚠⚠ **自检到此为止，绝不点这个按钮**：用户的移动硬盘可能正在备份，
    //     一次"测试性弹出"就是灾难。真正的弹出动作由用户手测（插件里有二次确认与 veto 提示）。
    reporter.info(QStringLiteral("P3-14 说明：「安全弹出」的实际执行**不在自检里做**"
                                 "（设备上可能是用户的真实数据）—— 自检覆盖的是判据、"
                                 "列表一致性与二次确认拦截；弹出本身留手测"));

    reporter.check(list->count() == removable.size(),
                   QStringLiteral("P3-14 ★ 走到这里设备仍在列表里（自检没有弹掉任何东西）"),
                   QStringLiteral("列表 %1 条").arg(list->count()));

    // ---- ④ 刷新与停用 ----
    refreshButton->click();
    settle(250);
    reporter.check(list->count() == removable.size(),
                   QStringLiteral("P3-14 点「刷新列表」后条数不变（枚举结果稳定可复现）"),
                   QStringLiteral("%1 条").arg(list->count()));

    reporter.check(manager.setPluginEnabled(kUsbId, false),
                   QStringLiteral("P3-14 插件停用成功"));
    settle(200);
    reporter.check(!confirmCheck->isChecked() && !ejectButton->isEnabled(),
                   QStringLiteral("P3-14 停用后确认框被清空、弹出按钮禁用"));
    reporter.check(statusLabel->text().contains(QStringLiteral("已停用")),
                   QStringLiteral("P3-14 停用后状态如实说明（没有弹出任何设备）"),
                   shorten(statusLabel->text()));

    delete settings;
}

} // namespace

int runP3GroupTests(Reporter &reporter,
                    WinEase::PluginManager &manager,
                    StubServices &services,
                    ProbeWindow &probe)
{
    const int failuresAtStart = reporter.failures();

    reporter.info(QStringLiteral("说明：P3-01 是**只读一览**（不搬移、不切桌面，用例只验"
                                 "「看得对不对」与「会不会乱动东西」）；"
                                 "P3-02 锁的是**自检自己造的临时文件**，占用源是自检自己起的"
                                 "子进程（真能被结束，结束了也不心疼）；P3-14 **绝不执行设备弹出**"
                                 "（用户接的移动硬盘上有真实数据）"));

    runVirtualDesktopCase(reporter, manager, services, probe);
    runFileUnlockCase(reporter, manager, services);
    runUsbControlCase(reporter, manager, services);

    reporter.info(QStringLiteral("P3 组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
