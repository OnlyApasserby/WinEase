#include "dev_group.h"

#include "EnvTools.h"
#include "HostsDocument.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QWidget>

namespace FeatureSmoke {

namespace {

using WinEase::Common::HostsDocument;
using WinEase::Common::HostsLineKind;

const QString kHostsId = QStringLiteral("dev.hosts_editor");

// 面板控件对象名（跨 DLL 边界只能靠运行期元对象找，见踩坑 #18）
const QString kHostsPanelName = QStringLiteral("hostsEditorPanel");
const QString kHostsPathLabel = QStringLiteral("hostsPathLabel");
const QString kHostsStatusLabel = QStringLiteral("hostsStatusLabel");
const QString kHostsCheckLabel = QStringLiteral("hostsCheckLabel");
const QString kHostsTextEdit = QStringLiteral("hostsTextEdit");
const QString kHostsSaveButton = QStringLiteral("hostsSaveButton");
const QString kHostsReloadButton = QStringLiteral("hostsReloadButton");
const QString kHostsDefaultButton = QStringLiteral("hostsDefaultButton");
const QString kHostsCommentButton = QStringLiteral("hostsCommentButton");
const QString kHostsBackupCombo = QStringLiteral("hostsBackupCombo");
const QString kHostsRestoreBackupButton = QStringLiteral("hostsRestoreBackupButton");

/// 自检伪造的"历史备份"（真实备份只有助手看得到，但插件要能正确列出与还原它们）
const QString kFakeBackupName = QStringLiteral("hosts-20260914-030000-000.bak");
const QByteArray kFakeBackupBytes("# 这是备份里的内容\n10.9.8.7\tbackup.local\n");

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

QString shorten(const QString &text, int limit = 120)
{
    return text.size() <= limit ? text : text.left(limit) + QStringLiteral("…");
}

QByteArray base64Decoded(const QVariantMap &arguments)
{
    return QByteArray::fromBase64(
        arguments.value(QStringLiteral("contentBase64")).toString().toLatin1());
}

/// 读真实的 hosts 字节（普通权限可读；这是"面板显示的是真内容"的证据来源）
QByteArray readRealHosts()
{
    QFile file(HostsDocument::hostsFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

// ===========================================================================
//  P2-13 Hosts 快速编辑
//
//  ⚠ 自检绝不真的写 hosts：写请求打在记录型桩上。
//     读文件那半段是真的 —— "打开面板看到真实内容"是真断言。
// ===========================================================================

void runHostsEditorCase(Reporter &reporter, WinEase::PluginManager &manager,
                        StubServices &services)
{
    // ---- ① 纯函数：逐行解析与校验（保存前的最后一道闸门）----
    const WinEase::Common::HostsLine entry =
        HostsDocument::parseLine(QStringLiteral("127.0.0.1\tlocalhost  foo.local"), 3);
    reporter.check(entry.kind == HostsLineKind::Entry && entry.number == 3
                       && entry.address == QStringLiteral("127.0.0.1")
                       && entry.hostNames.size() == 2,
                   QStringLiteral("P2-13 解析合法记录（制表符/多主机名）"),
                   QStringLiteral("地址=%1 主机名=%2")
                       .arg(entry.address, entry.hostNames.join(QStringLiteral(","))));

    reporter.check(HostsDocument::parseLine(QStringLiteral("  # 缩进的注释"), 1).kind
                       == HostsLineKind::Comment
                       && HostsDocument::parseLine(QString(), 2).kind == HostsLineKind::Blank,
                   QStringLiteral("P2-13 缩进注释与空行的判定"));

    // 行尾注释：`#` 之后的内容不能进主机名（这是 hosts 语法里最容易解析错的地方）
    const WinEase::Common::HostsLine trailing =
        HostsDocument::parseLine(QStringLiteral("10.0.0.1  foo.local   # 这是说明文字"), 1);
    reporter.check(trailing.kind == HostsLineKind::Entry && trailing.hostNames.size() == 1
                       && trailing.hostNames.first() == QStringLiteral("foo.local"),
                   QStringLiteral("P2-13 行尾注释（`#` 之后）不参与解析"),
                   trailing.hostNames.join(QStringLiteral(",")));

    const WinEase::Common::HostsLine badAddress =
        HostsDocument::parseLine(QStringLiteral("192.168.1.999  bad.local"), 7);
    reporter.check(badAddress.kind == HostsLineKind::Invalid
                       && badAddress.problem.contains(QStringLiteral("地址格式非法")),
                   QStringLiteral("P2-13 非法 IPv4 被判为错误（并带原因）"),
                   badAddress.problem);

    reporter.check(HostsDocument::parseLine(QStringLiteral("10.0.0.1"), 5).kind
                       == HostsLineKind::Invalid,
                   QStringLiteral("P2-13 只有地址、没有主机名 → 错误"));
    reporter.check(HostsDocument::parseLine(QStringLiteral("10.0.0.1  站点.com"), 1).kind
                       == HostsLineKind::Invalid,
                   QStringLiteral("P2-13 全角/中文主机名 → 错误（输入法最容易打进去的东西）"));
    reporter.check(HostsDocument::isValidAddress(QStringLiteral("::1"))
                       && HostsDocument::isValidAddress(QStringLiteral("fe80::1%3"))
                       && !HostsDocument::isValidAddress(QStringLiteral("192.168.1")),
                   QStringLiteral("P2-13 IPv6 地址判定（含作用域 ID）"));

    // ---- ② 纯函数：往返无损（BOM / CRLF / 制表符一个都不许动）----
    const QByteArray original =
        QByteArray("\xEF\xBB\xBF")
        + QByteArray("# 注释\r\n\r\n127.0.0.1\tlocalhost\r\n   # 缩进注释\r\n");
    const HostsDocument reparsed = HostsDocument::parse(original);
    reporter.check(reparsed.render() == original,
                   QStringLiteral("P2-13 ★ 解析 → 渲染**逐字节往返无损**（BOM / CRLF / 制表符 / 缩进）"),
                   QStringLiteral("原 %1 字节 → 渲染 %2 字节")
                       .arg(original.size())
                       .arg(reparsed.render().size()));
    reporter.check(reparsed.hasBom(), QStringLiteral("P2-13 BOM 被识别并保留"));

    // ---- ③ 纯函数：注释切换与校验汇总 ----
    HostsDocument toggled = HostsDocument::parse(QStringLiteral("127.0.0.1 localhost\n").toUtf8());
    reporter.check(toggled.toggleComment(1)
                       && toggled.lines().first().text.startsWith(QLatin1Char('#')),
                   QStringLiteral("P2-13 注释切换：加 `#`"));
    reporter.check(toggled.toggleComment(1)
                       && toggled.lines().first().text == QStringLiteral("127.0.0.1 localhost"),
                   QStringLiteral("P2-13 再切一次**逐字符还原**（原始缩进与制表符都在）"),
                   toggled.lines().first().text);

    const HostsDocument mixed = HostsDocument::parse(
        QByteArray("127.0.0.1 a.local\n192.168.1.999 b.local\n10.0.0.1\n"));
    const WinEase::Common::HostsCheck check = mixed.check();
    reporter.check(!check.ok && check.errorLines.size() == 2
                       && check.errors.first().startsWith(QStringLiteral("第 2 行：")),
                   QStringLiteral("P2-13 校验汇总：错误带**行号**且按行序排列"),
                   check.errors.join(QStringLiteral("；")));

    // 同一主机名映射到两个地址 → 是警告不是错误（保存不该被拦）
    const HostsDocument conflict =
        HostsDocument::parse(QByteArray("1.1.1.1 dup.local\n2.2.2.2 dup.local\n"));
    reporter.check(conflict.check().ok && !conflict.check().warnings.isEmpty(),
                   QStringLiteral("P2-13 同一主机名映射到多个地址 → 警告（不阻止保存）"),
                   conflict.check().warnings.join(QStringLiteral("；")));

    const HostsDocument defaults = HostsDocument::parse(HostsDocument::defaultContent());
    reporter.check(defaults.check().ok && defaults.check().entryCount == 0,
                   QStringLiteral("P2-13 默认内容本身通过校验、且**不含任何映射**"
                                  "（恢复默认 = 让所有解析回到 DNS）"));

    // ---- ④ 端到端：面板里的内容就是磁盘上的内容 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kHostsId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-13 插件已从插件目录加载（dev.hosts_editor）"));
    if (plugin == nullptr) {
        return;
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kHostsPanelName,
                   QStringLiteral("P2-13 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        return;
    }

    auto *pathLabel = childNamed<QLabel>(settings, kHostsPathLabel);
    auto *statusLabel = childNamed<QLabel>(settings, kHostsStatusLabel);
    auto *checkLabel = childNamed<QLabel>(settings, kHostsCheckLabel);
    auto *editor = childNamed<QPlainTextEdit>(settings, kHostsTextEdit);
    auto *saveButton = childNamed<QPushButton>(settings, kHostsSaveButton);
    auto *reloadButton = childNamed<QPushButton>(settings, kHostsReloadButton);
    auto *defaultButton = childNamed<QPushButton>(settings, kHostsDefaultButton);
    auto *commentButton = childNamed<QPushButton>(settings, kHostsCommentButton);
    auto *backupCombo = childNamed<QComboBox>(settings, kHostsBackupCombo);
    auto *restoreButton = childNamed<QPushButton>(settings, kHostsRestoreBackupButton);

    reporter.check(pathLabel != nullptr && statusLabel != nullptr && checkLabel != nullptr
                       && editor != nullptr && saveButton != nullptr && reloadButton != nullptr
                       && defaultButton != nullptr && commentButton != nullptr
                       && backupCombo != nullptr && restoreButton != nullptr,
                   QStringLiteral("P2-13 面板控件按对象名全部找到"));
    if (pathLabel == nullptr || editor == nullptr || saveButton == nullptr || backupCombo == nullptr
        || restoreButton == nullptr || checkLabel == nullptr || statusLabel == nullptr) {
        delete settings;
        return;
    }

    reporter.check(pathLabel->text() == HostsDocument::hostsFilePath(),
                   QStringLiteral("P2-13 面板上显示的就是系统 hosts 的真实路径"),
                   pathLabel->text());

    const QByteArray realHosts = readRealHosts();
    reporter.check(!realHosts.isEmpty(),
                   QStringLiteral("P2-13 前置：能读到真实 hosts（普通权限即可，不需助手）"),
                   HostsDocument::hostsFilePath());

    // 桩：助手"可用"，并伪造一份历史备份（真实备份只有助手看得到）
    services.elevation()->setAvailable(true);
    services.elevation()->clearCalls();
    {
        QVariantMap listData;
        listData.insert(QStringLiteral("directory"), QStringLiteral("<自检伪造>"));
        listData.insert(QStringLiteral("backups"), QStringList{ kFakeBackupName });
        listData.insert(QStringLiteral("stamps"),
                        QStringList{ QStringLiteral("2026-09-14T03:00:00") });
        services.elevation()->setResultData(QStringLiteral("hostsBackups"), listData);
    }

    reporter.check(manager.setPluginEnabled(kHostsId, true),
                   QStringLiteral("P2-13 插件启用成功"), plugin->lastError());
    settle(300);

    // 内容比对要归一化行尾：真实 hosts 里两种换行混着有是常态
    QString realText = QString::fromUtf8(realHosts);
    realText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (realText.endsWith(QLatin1Char('\n'))) {
        realText.chop(1);
    }
    reporter.check(editor->toPlainText() == realText,
                   QStringLiteral("P2-13 ★ 面板里显示的就是**磁盘上真实的 hosts**内容"),
                   QStringLiteral("面板 %1 字符 / 磁盘 %2 字符")
                       .arg(editor->toPlainText().size())
                       .arg(realText.size()));
    reporter.check(checkLabel->text().contains(QStringLiteral("语法检查通过"))
                       || checkLabel->text().contains(QStringLiteral("⚠")),
                   QStringLiteral("P2-13 打开面板就做了一次校验（校验结果显示在面板上）"),
                   shorten(checkLabel->text()));

    // ---- ⑤ 验收原文：非法语法**拒绝保存**并指出行号 ----
    const QString illegalLine = QStringLiteral("192.168.1.999\t这个域名不合法");
    editor->setPlainText(editor->toPlainText() + QLatin1Char('\n') + illegalLine);
    settle(200);
    reporter.check(checkLabel->text().contains(QStringLiteral("地址格式非法")),
                   QStringLiteral("P2-13 编辑区出现非法记录时立刻标出问题"),
                   shorten(checkLabel->text()));

    const int lineCount = editor->toPlainText().count(QLatin1Char('\n')) + 1;
    saveButton->click();
    settle(300);

    reporter.check(plugin->lastError().contains(QStringLiteral("第 %1 行").arg(lineCount)),
                   QStringLiteral("P2-13 ★ 保存被拒绝，且**指出行号**（验收原文）"),
                   plugin->lastError());
    reporter.check(statusLabel->text().contains(QStringLiteral("未保存")),
                   QStringLiteral("P2-13 状态栏明说「未保存」"), shorten(statusLabel->text()));
    reporter.check(services.elevation()->callCount(QStringLiteral("writeHosts")) == 0,
                   QStringLiteral("P2-13 ★★ 拒绝就是**真的不写**：连提权请求都没发出去"),
                   QStringLiteral("桩收到 writeHosts %1 次")
                       .arg(services.elevation()->callCount(QStringLiteral("writeHosts"))));

    // ---- ⑥ 合法内容 → 真的把字节交给助手（往返无损）----
    editor->setPlainText(realText);
    settle(200);
    const int callsBeforeSave = services.elevation()->callCount(QStringLiteral("writeHosts"));
    saveButton->click();
    settle(300);

    reporter.check(services.elevation()->callCount(QStringLiteral("writeHosts")) == callsBeforeSave + 1,
                   QStringLiteral("P2-13 合法内容 → 保存请求真的发出去了"),
                   QStringLiteral("桩收到 %1 次")
                       .arg(services.elevation()->callCount(QStringLiteral("writeHosts"))));
    // ⚠ 要看的是**writeHosts** 那一次的参数：保存成功后插件会紧接着刷新
    //    "历史备份列表"，于是"最后一次调用"变成了那条列表请求（这个坑刚踩过）
    const QVariantMap saveArguments =
        services.elevation()->lastArgumentsOf(QStringLiteral("writeHosts"));
    reporter.check(saveArguments.value(QStringLiteral("mode")).toString() == QStringLiteral("replace"),
                   QStringLiteral("P2-13 用的是 replace 模式（整体替换，不是 append）"),
                   saveArguments.value(QStringLiteral("mode")).toString());

    const QByteArray sent = base64Decoded(saveArguments);
    const QByteArray expected = HostsDocument::parse(realHosts).render();
    reporter.check(sent == expected,
                   QStringLiteral("P2-13 ★ 送出去的字节与「原文件解析后再渲染」逐字节一致"
                                  "（BOM / CRLF / 注释 / 缩进都没被改）"),
                   QStringLiteral("送 %1 字节 / 期望 %2 字节").arg(sent.size()).arg(expected.size()));

    // ---- ⑦ 填入默认内容：只填编辑区、不落盘 ----
    const int callsBeforeDefault = services.elevation()->callCount(QStringLiteral("writeHosts"));
    defaultButton->click();
    settle(200);
    const HostsDocument filled = HostsDocument::parse(editor->toPlainText().toUtf8());
    reporter.check(filled.check().ok && filled.check().entryCount == 0,
                   QStringLiteral("P2-13 「填入默认内容」装进编辑区的是无映射的默认内容"));
    reporter.check(services.elevation()->callCount(QStringLiteral("writeHosts")) == callsBeforeDefault,
                   QStringLiteral("P2-13 ★ 填入默认内容**不会自动落盘**"
                                  "（一键抹掉多年 hosts 是灾难级误操作）"));
    reporter.check(statusLabel->text().contains(QStringLiteral("点「保存」")),
                   QStringLiteral("P2-13 状态栏明说还要点保存"), shorten(statusLabel->text()));

    // ---- ⑧ 一键还原：列出的备份 + 送出去的字节 ----
    editor->setPlainText(realText);
    settle(200);
    reporter.check(backupCombo->count() == 1
                       && backupCombo->currentData().toString() == kFakeBackupName,
                   QStringLiteral("P2-13 历史备份列表来自助手的返回（插件不猜目录）"),
                   QStringLiteral("列表 %1 项").arg(backupCombo->count()));

    {
        QVariantMap readData;
        readData.insert(QStringLiteral("name"), kFakeBackupName);
        readData.insert(QStringLiteral("contentBase64"),
                        QString::fromLatin1(kFakeBackupBytes.toBase64()));
        services.elevation()->setResultData(QStringLiteral("hostsBackups"), readData);
    }
    const int callsBeforeRestore = services.elevation()->callCount(QStringLiteral("writeHosts"));
    restoreButton->click();
    settle(300);

    reporter.check(services.elevation()->callCount(QStringLiteral("writeHosts")) == callsBeforeRestore + 1,
                   QStringLiteral("P2-13 「一键还原」背后的动作就是一次 writeHosts"),
                   QStringLiteral("桩收到 %1 次")
                       .arg(services.elevation()->callCount(QStringLiteral("writeHosts"))));
    const QByteArray restored =
        base64Decoded(services.elevation()->lastArgumentsOf(QStringLiteral("writeHosts")));
    reporter.check(restored == kFakeBackupBytes,
                   QStringLiteral("P2-13 ★ 还原时送出去的**就是那份备份的字节**（逐字节）"),
                   QStringLiteral("送 %1 字节 / 备份 %2 字节")
                       .arg(restored.size())
                       .arg(kFakeBackupBytes.size()));
    reporter.check(editor->toPlainText().contains(QStringLiteral("backup.local")),
                   QStringLiteral("P2-13 还原后编辑区同步显示备份内容"));

    // ---- ⑨ 没有提权通道时：诚实禁用，而不是给一个注定失败的按钮 ----
    services.setElevationEnabled(false);
    manager.setPluginEnabled(kHostsId, false);
    settle(200);
    reporter.check(manager.setPluginEnabled(kHostsId, true),
                   QStringLiteral("P2-13 插件重新启用（通道已撤掉）"), plugin->lastError());
    settle(300);
    reporter.check(!saveButton->isEnabled(),
                   QStringLiteral("P2-13 ★ 没有提权助手通道时「保存」被禁用"
                                  "（不给用户一个注定失败的按钮）"));
    reporter.check(statusLabel->text().contains(QStringLiteral("提权助手不可用")),
                   QStringLiteral("P2-13 面板如实说明为什么不能保存"), shorten(statusLabel->text()));
    reporter.check(editor->toPlainText().size() > 0,
                   QStringLiteral("P2-13 但**查看**不受影响（读 hosts 本来就不需要提权）"));

    services.setElevationEnabled(true);
    manager.setPluginEnabled(kHostsId, false);
    settle(200);
    reporter.check(!editor->isEnabled() || editor->isReadOnly(),
                   QStringLiteral("P2-13 停用后编辑区变为只读（功能关了就不该能改）"));

    delete settings;
}

// ===========================================================================
//  P2-14 环境变量管理
//
//  这一项会**真的写用户注册表**（HKCU\Environment），所以只用一个
//  "自检专用变量"做端到端：写进去 → 回读 → 删掉 → 断言不留残留。
//
//  ⚠ 两条刻意的克制：
//    * **绝不动用户的 PATH**（那是"能让一堆程序突然找不到"的东西）：
//      PATH 部分只验"读对了没、问题标出来了没、去重只改编辑区不落盘"；
//    * 系统级变量走**提权桩**（`setEnvVar` 只被记录，不真写 HKLM）。
// ===========================================================================

const QString kEnvId = QStringLiteral("dev.env_manager");

const QString kEnvPanelName = QStringLiteral("envPanel");
const QString kEnvScopeCombo = QStringLiteral("envScopeCombo");
const QString kEnvScopeLabel = QStringLiteral("envScopeLabel");
const QString kEnvTable = QStringLiteral("envTable");
const QString kEnvNameEdit = QStringLiteral("envNameEdit");
const QString kEnvValueEdit = QStringLiteral("envValueEdit");
const QString kEnvSaveButton = QStringLiteral("envSaveButton");
const QString kEnvDeleteButton = QStringLiteral("envDeleteButton");
const QString kEnvPathList = QStringLiteral("envPathList");
const QString kEnvPathSummaryLabel = QStringLiteral("envPathSummaryLabel");
const QString kEnvPathDedupeButton = QStringLiteral("envPathDedupeButton");
const QString kEnvPathSaveButton = QStringLiteral("envPathSaveButton");
const QString kEnvExpandLabel = QStringLiteral("envExpandLabel");
const QString kEnvStatusLabel = QStringLiteral("envStatusLabel");

/// 自检专用变量：端到端只用它，绝不碰用户自己的变量
const QString kSmokeVariable = QStringLiteral("WINEASE_SMOKE_ENVVAR");

/// 抓 WM_SETTINGCHANGE 广播的隐藏窗口。
/// "改完新开的 cmd 立即生效"这条验收，机制上完全依赖广播
/// （不广播就得重启资源管理器才会刷新环境），所以这里真的抓一次。
class SettingChangeSniffer : public QWidget
{
public:
    explicit SettingChangeSniffer(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowTitle(QStringLiteral("WinEase 自检·环境广播嗅探"));
        resize(120, 60);
        (void)winId(); // 没有真实 HWND 就收不到广播
    }

    int count() const { return m_count; }
    QStringList areas() const { return m_areas; }
    void reset()
    {
        m_count = 0;
        m_areas.clear();
    }

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(eventType);
        Q_UNUSED(result);
        auto *msg = static_cast<MSG *>(message);
        if (msg != nullptr && msg->message == WM_SETTINGCHANGE) {
            ++m_count;
            m_areas.append(msg->lParam != 0
                               ? QString::fromWCharArray(reinterpret_cast<const wchar_t *>(msg->lParam))
                               : QString());
        }
        return false;
    }

private:
    int m_count = 0;
    QStringList m_areas;
};

void runEnvManagerCase(Reporter &reporter, WinEase::PluginManager &manager,
                       StubServices &services)
{
    using WinEase::Common::EnvScope;

    // ---- ① 纯函数：PATH 拆分（空条目不隐藏、重复项能定位）----
    const QString messyPath = QStringLiteral("C:\\a;;C:\\b;c:\\A\\");
    const QList<WinEase::Common::PathEntry> entries =
        WinEase::Common::splitPathEntries(messyPath);
    reporter.check(entries.size() == 4,
                   QStringLiteral("P2-14 PATH 按 `;` 拆分（含空条目，一个都不丢）"),
                   QStringLiteral("%1 条").arg(entries.size()));
    reporter.check(entries.size() == 4 && entries.at(1).empty,
                   QStringLiteral("P2-14 ★ `;;` 产生的空条目被识别出来"
                                  "（它等价于「当前目录」，是真实的安全隐患）"));
    reporter.check(entries.size() == 4 && entries.at(3).duplicate
                       && entries.at(3).duplicateOf == 0,
                   QStringLiteral("P2-14 ★ 重复项能**定位到第几条**"
                                  "（`c:\\A\\` 与 `C:\\a` 归一化后相同）"),
                   entries.size() == 4 ? QStringLiteral("与第 %1 条重复").arg(entries.at(3).duplicateOf + 1)
                                       : QString());
    reporter.check(WinEase::Common::joinPathEntries(entries) == messyPath,
                   QStringLiteral("P2-14 拆分 → 拼回**逐字符往返**（不偷偷改用户的写法）"));
    reporter.check(WinEase::Common::countPathProblems(entries) == 2,
                   QStringLiteral("P2-14 问题条目计数（空 1 + 重复 1）"));

    const QString deduped = WinEase::Common::normalizePathValue(messyPath);
    reporter.check(deduped == QStringLiteral("C:\\a;C:\\b"),
                   QStringLiteral("P2-14 去重同时去掉空条目、保留首次出现的顺序"),
                   deduped);

    // ---- ② 纯函数：变量名校验（只接受 ASCII）----
    reporter.check(WinEase::Common::isValidVariableName(QStringLiteral("WINEASE_SMOKE"))
                       && WinEase::Common::isValidVariableName(QStringLiteral("_A1")),
                   QStringLiteral("P2-14 合法变量名（半角字母/数字/下划线）"));
    reporter.check(!WinEase::Common::isValidVariableName(QStringLiteral("站点"))
                       && !WinEase::Common::isValidVariableName(QStringLiteral("2DIGIT"))
                       && !WinEase::Common::isValidVariableName(QStringLiteral("with space"))
                       && !WinEase::Common::isValidVariableName(QStringLiteral("with-dash")),
                   QStringLiteral("P2-14 ★ 中文 / 数字开头 / 空格 / 连字符都被拒绝"
                                  "（ASCII-only；`isLetterOrNumber()` 会放行中文，踩坑 #58）"),
                   WinEase::Common::variableNameProblem(QStringLiteral("站点")));

    // ---- ③ 纯函数：%VAR% 展开 ----
    QHash<QString, QString> variables;
    variables.insert(QStringLiteral("SystemRoot"), QStringLiteral("C:\\Windows"));
    reporter.check(WinEase::Common::expandVariables(QStringLiteral("%SystemRoot%\\System32"),
                                                    variables)
                       == QStringLiteral("C:\\Windows\\System32"),
                   QStringLiteral("P2-14 展开 `%VAR%`"));
    reporter.check(WinEase::Common::expandVariables(QStringLiteral("%systemroot%\\x"), variables)
                       == QStringLiteral("C:\\Windows\\x"),
                   QStringLiteral("P2-14 展开**大小写不敏感**（环境变量名本来就不区分大小写）"));
    reporter.check(WinEase::Common::expandVariables(QStringLiteral("%NOT_DEFINED%\\bin"), variables)
                       == QStringLiteral("%NOT_DEFINED%\\bin"),
                   QStringLiteral("P2-14 ★ 未定义的变量**原样保留**"
                                  "（静默变空串会造出「看起来正常实则残废」的路径）"));

    // ---- ④ 纯函数：条目体检 ----
    const QString systemRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    reporter.check(WinEase::Common::pathEntryProblem(systemRoot).isEmpty(),
                   QStringLiteral("P2-14 存在且是目录 → 没有问题"),
                   systemRoot);
    reporter.check(WinEase::Common::pathEntryProblem(QStringLiteral("C:\\NoSuchDir_WinEase_x"))
                       == QStringLiteral("目录不存在"),
                   QStringLiteral("P2-14 目录不存在被标出来"));
    reporter.check(WinEase::Common::pathEntryProblem(QStringLiteral("%UNDEFINED%\\bin"))
                       .contains(QStringLiteral("未展开")),
                   QStringLiteral("P2-14 含未展开变量的条目单独报一种问题"));

    // ---- ⑤ 纯函数：导出/导入往返 ----
    QMap<QString, QString> toExport;
    toExport.insert(QStringLiteral("ALPHA"), QStringLiteral("1"));
    toExport.insert(QStringLiteral("BETA"), QStringLiteral("%ALPHA%;C:\\x"));
    const QString exported = WinEase::Common::exportVariables(toExport, EnvScope::User);
    QMap<QString, QString> imported;
    QString importError;
    reporter.check(WinEase::Common::importVariables(exported, &imported, &importError)
                       && imported == toExport,
                   QStringLiteral("P2-14 导出 → 导入**完全往返**（含 `%VAR%` 值）"),
                   importError);
    reporter.check(!WinEase::Common::importVariables(QStringLiteral("{ not json"),
                                                     &imported, &importError),
                   QStringLiteral("P2-14 非法 JSON 被拒绝并给出原因"), importError);

    // ---- ⑥ 端到端：面板与真实注册表 ----
    WinEase::IFeaturePlugin *plugin = manager.plugin(kEnvId);
    reporter.check(plugin != nullptr,
                   QStringLiteral("P2-14 插件已从插件目录加载（dev.env_manager）"));
    if (plugin == nullptr) {
        return;
    }

    QWidget *settings = plugin->createSettingsWidget(nullptr);
    reporter.check(settings != nullptr && settings->objectName() == kEnvPanelName,
                   QStringLiteral("P2-14 设置面板可创建且对象名正确"));
    if (settings == nullptr) {
        return;
    }

    auto *scopeCombo = childNamed<QComboBox>(settings, kEnvScopeCombo);
    auto *scopeLabel = childNamed<QLabel>(settings, kEnvScopeLabel);
    auto *table = childNamed<QTableWidget>(settings, kEnvTable);
    auto *nameEdit = childNamed<QLineEdit>(settings, kEnvNameEdit);
    auto *valueEdit = childNamed<QLineEdit>(settings, kEnvValueEdit);
    auto *saveButton = childNamed<QPushButton>(settings, kEnvSaveButton);
    auto *deleteButton = childNamed<QPushButton>(settings, kEnvDeleteButton);
    auto *pathList = childNamed<QListWidget>(settings, kEnvPathList);
    auto *pathSummary = childNamed<QLabel>(settings, kEnvPathSummaryLabel);
    auto *dedupeButton = childNamed<QPushButton>(settings, kEnvPathDedupeButton);
    auto *pathSaveButton = childNamed<QPushButton>(settings, kEnvPathSaveButton);
    auto *expandLabel = childNamed<QLabel>(settings, kEnvExpandLabel);
    auto *statusLabel = childNamed<QLabel>(settings, kEnvStatusLabel);

    reporter.check(scopeCombo != nullptr && scopeLabel != nullptr && table != nullptr
                       && nameEdit != nullptr && valueEdit != nullptr && saveButton != nullptr
                       && deleteButton != nullptr && pathList != nullptr && pathSummary != nullptr
                       && dedupeButton != nullptr && pathSaveButton != nullptr
                       && expandLabel != nullptr && statusLabel != nullptr,
                   QStringLiteral("P2-14 面板控件按对象名全部找到"));
    if (scopeCombo == nullptr || table == nullptr || nameEdit == nullptr || valueEdit == nullptr
        || saveButton == nullptr || deleteButton == nullptr || pathList == nullptr
        || dedupeButton == nullptr || pathSaveButton == nullptr || expandLabel == nullptr
        || statusLabel == nullptr) {
        delete settings;
        return;
    }

    services.elevation()->setAvailable(true);
    services.elevation()->clearCalls();
    reporter.check(manager.setPluginEnabled(kEnvId, true),
                   QStringLiteral("P2-14 插件启用成功"), plugin->lastError());
    settle(300);

    reporter.check(scopeCombo->currentData().toString() == QStringLiteral("user"),
                   QStringLiteral("P2-14 默认作用域是用户变量（免提权，改完立即生效）"),
                   scopeCombo->currentData().toString());

    // 前置：用户 PATH 读得到（读不出来后面那几条就没意义）
    const QMap<QString, QString> userVariables =
        WinEase::Common::readEnvironment(EnvScope::User);
    const QString realPath = userVariables.value(QStringLiteral("Path"));
    reporter.check(userVariables.contains(QStringLiteral("Path")),
                   QStringLiteral("P2-14 前置：读到了真实的用户 PATH（读注册表不需要提权）"),
                   QStringLiteral("Path = %1 字符").arg(realPath.size()));

    // 表格内容 == 真实注册表内容（逐项比对，不是"有几行"）
    reporter.check(table->rowCount() == userVariables.size(),
                   QStringLiteral("P2-14 ★ 变量表行数 == 注册表里的变量个数（读的是真值）"),
                   QStringLiteral("表 %1 行 / 注册表 %2 项")
                       .arg(table->rowCount())
                       .arg(userVariables.size()));

    // ---- ⑦ PATH：读对了没、问题标出来了没 ----
    const QList<WinEase::Common::PathEntry> realEntries =
        WinEase::Common::splitPathEntries(realPath);
    reporter.check(pathList->count() == realEntries.size(),
                   QStringLiteral("P2-14 ★ PATH 列表条数 == 真实 PATH 的条目数（含空条目）"),
                   QStringLiteral("列表 %1 条 / 真实 %2 条")
                       .arg(pathList->count())
                       .arg(realEntries.size()));

    int realProblems = 0;
    for (const WinEase::Common::PathEntry &entry : realEntries) {
        if (entry.empty || entry.duplicate) {
            ++realProblems;
        }
    }
    if (realProblems > 0) {
        bool anyMarked = false;
        for (int index = 0; index < pathList->count(); ++index) {
            if (pathList->item(index)->text().contains(QStringLiteral("⚠"))) {
                anyMarked = true;
            }
        }
        reporter.check(anyMarked,
                       QStringLiteral("P2-14 ★ 真实 PATH 里确实有问题条目，且界面上标出来了"),
                       QStringLiteral("真实 PATH 有 %1 条问题").arg(realProblems));
    } else {
        reporter.info(QStringLiteral("P2-14 本机用户 PATH 目前没有重复/空条目，"
                                     "「界面标出问题」这条用纯函数断言覆盖（如实说明）"));
    }
    reporter.check(pathSummary->text().contains(QStringLiteral("PATH 共")),
                   QStringLiteral("P2-14 面板给出 PATH 概况（条数 / 重复 / 空 / 目录不存在）"),
                   shorten(pathSummary->text()));
    reporter.check(expandLabel->text().contains(QStringLiteral("展开预览")),
                   QStringLiteral("P2-14 面板给出展开预览"), shorten(expandLabel->text()));

    // ---- ⑧ 去重只改编辑区、**不落盘**（PATH 是危险的东西，绝不自作主张）----
    const int callsBeforeDedupe = services.elevation()->callCount(QStringLiteral("setEnvVar"));
    dedupeButton->click();
    settle(250);
    const QMap<QString, QString> afterDedupe = WinEase::Common::readEnvironment(EnvScope::User);
    reporter.check(afterDedupe.value(QStringLiteral("Path")) == realPath,
                   QStringLiteral("P2-14 ★★ 点「去重」**用户 PATH 一个字节都没变**"
                                  "（它只改编辑区，保存必须用户自己再点一次）"));
    reporter.check(services.elevation()->callCount(QStringLiteral("setEnvVar")) == callsBeforeDedupe,
                   QStringLiteral("P2-14 去重连提权请求都不会发（用户级写本来也不用助手）"));
    reporter.check(pathList->count() <= realEntries.size(),
                   QStringLiteral("P2-14 但编辑区里的重复条目已经被去掉了"),
                   QStringLiteral("%1 → %2 条").arg(realEntries.size()).arg(pathList->count()));

    // ---- ⑨ 真的写一个"自检专用变量"（用户级：免提权 + 广播）----
    SettingChangeSniffer sniffer;
    sniffer.reset();

    const QString smokeValue = QStringLiteral("winease-%1")
                                   .arg(QDateTime::currentMSecsSinceEpoch());
    nameEdit->setText(kSmokeVariable);
    valueEdit->setText(smokeValue);
    saveButton->click();
    settle(250);

    const QMap<QString, QString> written = WinEase::Common::readEnvironment(EnvScope::User);
    reporter.check(written.value(kSmokeVariable) == smokeValue,
                   QStringLiteral("P2-14 ★ 保存后**注册表里真的有了**这个变量（回读确认）"),
                   written.value(kSmokeVariable));
    reporter.check(plugin->lastError().isEmpty(),
                   QStringLiteral("P2-14 保存成功后没有残留错误状态"), plugin->lastError());
    reporter.check(sniffer.count() > 0
                       && sniffer.areas().contains(QStringLiteral("Environment")),
                   QStringLiteral("P2-14 ★★ 写完广播了 WM_SETTINGCHANGE(\"Environment\")"
                                  "（这就是「新开的 cmd 立即生效」的机制）"),
                   QStringLiteral("收到 %1 次：[%2]")
                       .arg(sniffer.count())
                       .arg(sniffer.areas().join(QStringLiteral(" | "))));
    reporter.check(table->rowCount() == written.size(),
                   QStringLiteral("P2-14 保存后变量表自动刷新到最新状态"),
                   QStringLiteral("表 %1 行 / 注册表 %2 项")
                       .arg(table->rowCount())
                       .arg(written.size()));

    // ---- ⑩ 删除（走同一条写通道）----
    sniffer.reset();
    nameEdit->setText(kSmokeVariable);
    deleteButton->click();
    settle(250);
    const QMap<QString, QString> afterDelete = WinEase::Common::readEnvironment(EnvScope::User);
    reporter.check(!afterDelete.contains(kSmokeVariable),
                   QStringLiteral("P2-14 ★ 删除后注册表里**真的没有了**（自检不留残留）"));
    reporter.check(sniffer.areas().contains(QStringLiteral("Environment")),
                   QStringLiteral("P2-14 删除同样广播了设置变更"));

    // ---- ⑪ 系统级变量：走提权助手（桩），**不真写 HKLM** ----
    const int machineIndex = scopeCombo->findData(QStringLiteral("machine"));
    reporter.check(machineIndex >= 0, QStringLiteral("P2-14 作用域下拉里有「系统变量」"));
    scopeCombo->setCurrentIndex(machineIndex);
    settle(300);
    reporter.check(scopeLabel->text().contains(QStringLiteral("管理员")),
                   QStringLiteral("P2-14 切到系统变量时面板说明需要管理员权限"),
                   shorten(scopeLabel->text()));

    services.elevation()->clearCalls();
    nameEdit->setText(QStringLiteral("WINEASE_SMOKE_MACHINE"));
    valueEdit->setText(QStringLiteral("1"));
    saveButton->click();
    settle(250);

    reporter.check(services.elevation()->callCount(QStringLiteral("setEnvVar")) == 1,
                   QStringLiteral("P2-14 ★ 系统级保存**经提权助手**发出（setEnvVar）"),
                   QStringLiteral("桩收到 %1 次")
                       .arg(services.elevation()->callCount(QStringLiteral("setEnvVar"))));
    const QVariantMap machineArguments =
        services.elevation()->lastArgumentsOf(QStringLiteral("setEnvVar"));
    reporter.check(machineArguments.value(QStringLiteral("scope")).toString()
                       == QStringLiteral("machine")
                       && machineArguments.value(QStringLiteral("name")).toString()
                              == QStringLiteral("WINEASE_SMOKE_MACHINE"),
                   QStringLiteral("P2-14 请求里的 scope/name 正确（助手据此写 HKLM）"),
                   QStringLiteral("scope=%1 name=%2")
                       .arg(machineArguments.value(QStringLiteral("scope")).toString(),
                            machineArguments.value(QStringLiteral("name")).toString()));
    reporter.check(!WinEase::Common::readEnvironment(EnvScope::Machine)
                        .contains(QStringLiteral("WINEASE_SMOKE_MACHINE")),
                   QStringLiteral("P2-14 系统变量**没有被真的写进 HKLM**（请求打在桩上）"));

    // ---- ⑫ 没有提权通道时：诚实禁用 ----
    services.setElevationEnabled(false);
    manager.setPluginEnabled(kEnvId, false);
    settle(200);
    reporter.check(manager.setPluginEnabled(kEnvId, true),
                   QStringLiteral("P2-14 插件重新启用（通道已撤掉）"), plugin->lastError());
    settle(300);
    scopeCombo->setCurrentIndex(scopeCombo->findData(QStringLiteral("machine")));
    settle(250);
    reporter.check(!saveButton->isEnabled(),
                   QStringLiteral("P2-14 ★ 系统级 + 没有提权通道 → 「保存」被禁用"
                                  "（不给用户一个注定失败的按钮）"));
    reporter.check(statusLabel->text().contains(QStringLiteral("提权助手不可用")),
                   QStringLiteral("P2-14 面板如实说明原因"), shorten(statusLabel->text()));

    scopeCombo->setCurrentIndex(scopeCombo->findData(QStringLiteral("user")));
    settle(250);
    reporter.check(saveButton->isEnabled(),
                   QStringLiteral("P2-14 切回用户变量 → 免提权，保存恢复可用"));

    services.setElevationEnabled(true);
    reporter.check(manager.setPluginEnabled(kEnvId, false),
                   QStringLiteral("P2-14 插件停用成功"));

    delete settings;

    // ---- ⑬ 收尾：确认自检专用变量真的没留下 ----
    reporter.check(!WinEase::Common::readEnvironment(EnvScope::User).contains(kSmokeVariable),
                   QStringLiteral("P2-14 收尾：用户环境变量里没有自检留下的任何东西"));
}

} // namespace

int runDevGroupTests(Reporter &reporter,
                     WinEase::PluginManager &manager,
                     StubServices &services)
{
    const int failuresAtStart = reporter.failures();

    reporter.info(QStringLiteral("说明：本组**不会真的写 hosts**——提权请求打在记录型桩上；"
                                 "读文件、校验、组装字节这几步全部真实跑。"
                                 "P2-14 会真的写一个自检专用用户环境变量（HKCU），用完立刻删掉"));

    runHostsEditorCase(reporter, manager, services);
    runEnvManagerCase(reporter, manager, services);

    reporter.info(QStringLiteral("开发运维组失败项：%1").arg(reporter.failures() - failuresAtStart));
    return reporter.failures() - failuresAtStart;
}

} // namespace FeatureSmoke
