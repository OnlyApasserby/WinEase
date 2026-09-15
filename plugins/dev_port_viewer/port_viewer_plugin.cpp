#include "port_viewer_plugin.h"

#include "sdk/ElevationService.h"
#include "sdk/PluginServices.h"
#include "win32/NetUtils.h"
#include "win32/ProcessUtils.h"

#include "ClipboardTools.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

// ClipboardTools 是命名空间 → 用命名空间别名；EndpointInfo/PortOccupant 是类型 → using 声明
namespace ClipboardTools = WinEase::FeaturePlugins::ClipboardTools;
using WinEase::Win32::EndpointInfo;
using WinEase::Win32::PortOccupant;

namespace {

constexpr int kMaxTableRows = 2000; ///< 全部端点可能上千条，超出就不再显示

void fillTable(QTableWidget *table, const QList<EndpointInfo> &endpoints)
{
    // 一次性灌上千行时逐行刷新会明显卡顿，先冻住绘制
    table->setUpdatesEnabled(false);
    table->setRowCount(0);

    const int rows = static_cast<int>(qMin(endpoints.size(), static_cast<qsizetype>(kMaxTableRows)));
    table->setRowCount(rows);
    for (int row = 0; row < rows; ++row) {
        const EndpointInfo &info = endpoints.at(row);
        const QString processName = WinEase::Win32::processName(info.pid);

        const QStringList cells = {
            info.protocolText(),
            info.localText(),
            info.remoteText(),
            info.stateText(),
            info.pid == 0 ? QStringLiteral("-") : QString::number(info.pid),
            processName.isEmpty() ? QStringLiteral("(无法读取)") : processName,
            WinEase::Win32::processPath(info.pid),
        };
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            if (column == 4) {
                item->setData(Qt::UserRole, info.pid);
            }
            table->setItem(row, column, item);
        }
    }
    table->setUpdatesEnabled(true);
}

} // namespace

PortViewerPlugin::PortViewerPlugin(QObject *parent)
    : WinEase::IFeaturePlugin(parent)
{
}

// ---------------------------------------------------------------------------
//  元信息
// ---------------------------------------------------------------------------

QString PortViewerPlugin::id() const
{
    return QStringLiteral("dev.port_viewer");
}

QString PortViewerPlugin::name() const
{
    return QStringLiteral("端口占用查看");
}

QString PortViewerPlugin::description() const
{
    return QStringLiteral("看端口被哪个进程占用（含完整路径），需要时直接结束它");
}

QIcon PortViewerPlugin::icon() const
{
    return WinEase::Category::icon(WinEase::FeatureCategory::Development);
}

WinEase::FeatureCategory PortViewerPlugin::category() const
{
    return WinEase::FeatureCategory::Development;
}

QStringList PortViewerPlugin::tags() const
{
    return { QStringLiteral("端口"), QStringLiteral("占用"), QStringLiteral("port"),
             QStringLiteral("netstat"), QStringLiteral("监听"), QStringLiteral("pid") };
}

bool PortViewerPlugin::supportsHotkey() const
{
    return true;
}

QKeySequence PortViewerPlugin::defaultHotkey() const
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+P"));
}

// ---------------------------------------------------------------------------
//  生命周期
// ---------------------------------------------------------------------------

bool PortViewerPlugin::initialize()
{
    return true;
}

void PortViewerPlugin::shutdown()
{
}

bool PortViewerPlugin::onEnable()
{
    if (services() == nullptr) {
        setLastError(QStringLiteral("宿主服务未注入，插件无法启用"));
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral("就绪：复制一个端口号再按 %1 即可查占用")
                             .arg(defaultHotkey().toString(QKeySequence::NativeText)));
    return true;
}

void PortViewerPlugin::onDisable()
{
    Q_EMIT statusMessage(QStringLiteral("已停用端口查看"));
}

void PortViewerPlugin::onHotkey(const QString &hotkeyId)
{
    if (!isEnabled()) {
        return;
    }
    // 本插件只有一个动作，任何分发都按"查剪贴板里的端口"处理
    Q_UNUSED(hotkeyId);

    const QString text = ClipboardTools::clipboardText();
    const int port = extractPort(text);
    if (port <= 0) {
        Q_EMIT statusMessage(QStringLiteral("剪贴板里没有找到端口号（先复制一个数字，如 8080）"));
        return;
    }
    queryPort(static_cast<quint16>(port));
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

int PortViewerPlugin::extractPort(const QString &text)
{
    // 端口最多 5 位；取第一个落在合法范围内的数字
    static const QRegularExpression numberPattern(QStringLiteral("(\\d{1,5})"));
    auto matches = numberPattern.globalMatch(text);
    while (matches.hasNext()) {
        const int value = matches.next().captured(1).toInt();
        if (value > 0 && value <= 65535) {
            return value;
        }
    }
    return -1;
}

bool PortViewerPlugin::queryPort(quint16 port)
{
    const QList<PortOccupant> occupants = WinEase::Win32::portOccupants(port);

    QStringList lines;
    if (occupants.isEmpty()) {
        lines.append(QStringLiteral("端口 %1（TCP/UDP）没有发现监听中的进程").arg(port));
    } else {
        for (const PortOccupant &occupant : occupants) {
            lines.append(occupant.describe());
            lines.append(occupant.processPath.isEmpty()
                             ? QStringLiteral("  路径：无权限读取（系统进程请用管理员身份查看）")
                             : QStringLiteral("  路径：%1").arg(occupant.processPath));
        }
    }

    const QString output = lines.join(QLatin1Char('\n'));
    const bool copied = ClipboardTools::setText(output);
    if (!copied) {
        setLastError(QStringLiteral("写入剪贴板失败（可能被其它程序占用）"));
    }

    if (occupants.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("端口 %1 没有被占用").arg(port));
        return false;
    }

    const PortOccupant &first = occupants.first();
    Q_EMIT statusMessage(QStringLiteral("端口 %1 被 %2 占用%3")
                             .arg(port)
                             .arg(first.processName.isEmpty()
                                      ? QStringLiteral("PID %1").arg(first.pid)
                                      : QStringLiteral("%1（PID %2）").arg(first.processName).arg(first.pid))
                             .arg(occupants.size() > 1
                                      ? QStringLiteral("（共 %1 条记录）").arg(occupants.size())
                                      : QString()));
    return true;
}

bool PortViewerPlugin::terminateByElevation(quint32 pid, QString *messageOut)
{
    WinEase::PluginServices *svc = services();
    if (svc == nullptr) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("宿主服务不可用");
        }
        return false;
    }

    WinEase::ElevationService *elevation = svc->elevationService();
    if (elevation == nullptr) {
        if (messageOut != nullptr) {
            *messageOut = QStringLiteral("提权通道不可用（安全模式下不提供）");
        }
        return false;
    }

    QVariantMap arguments;
    arguments.insert(QStringLiteral("pid"), static_cast<uint>(pid));
    const WinEase::ElevationResult result = elevation->execute(QStringLiteral("killProcess"), arguments);

    if (messageOut != nullptr) {
        *messageOut = result.ok
                          ? QStringLiteral("已结束进程 PID %1").arg(pid)
                          : QStringLiteral("结束进程失败：%1").arg(result.error);
    }
    return result.ok;
}

// ---------------------------------------------------------------------------
//  设置面板
// ---------------------------------------------------------------------------

QWidget *PortViewerPlugin::createSettingsWidget(QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);

    auto *queryRow = new QHBoxLayout();
    queryRow->addWidget(new QLabel(QStringLiteral("端口 / PID / 进程名："), widget));
    auto *queryEdit = new QLineEdit(widget);
    queryEdit->setPlaceholderText(QStringLiteral("8080"));
    queryRow->addWidget(queryEdit);
    auto *listeningOnly = new QCheckBox(QStringLiteral("只看监听"), widget);
    listeningOnly->setChecked(false);
    queryRow->addWidget(listeningOnly);
    auto *queryButton = new QPushButton(QStringLiteral("查询"), widget);
    queryRow->addWidget(queryButton);
    auto *allButton = new QPushButton(QStringLiteral("列出全部端点"), widget);
    queryRow->addWidget(allButton);
    layout->addLayout(queryRow);

    auto *table = new QTableWidget(widget);
    table->setColumnCount(7);
    table->setHorizontalHeaderLabels({ QStringLiteral("协议"), QStringLiteral("本地地址"),
                                       QStringLiteral("远端地址"), QStringLiteral("状态"),
                                       QStringLiteral("PID"), QStringLiteral("进程"),
                                       QStringLiteral("完整路径") });
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setMinimumHeight(220);
    layout->addWidget(table);

    auto *statusLabel = new QLabel(widget);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto *actionRow = new QHBoxLayout();
    auto *killButton = new QPushButton(QStringLiteral("结束选中行的进程"), widget);
    actionRow->addWidget(killButton);
    auto *copyButton = new QPushButton(QStringLiteral("复制选中行"), widget);
    actionRow->addWidget(copyButton);
    actionRow->addStretch(1);
    layout->addLayout(actionRow);

    auto *hint = new QLabel(QStringLiteral("快捷键 %1：直接查剪贴板里的端口号，结果写回剪贴板。\n"
                                          "枚举端口只需普通权限；结束**系统进程**会走提权助手（首次会弹一次 UAC）。")
                                .arg(defaultHotkey().toString(QKeySequence::NativeText)),
                            widget);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch(1);

    const auto runQuery = [queryEdit, table, statusLabel, listeningOnly] {
        const QString keyword = queryEdit->text().trimmed();
        QList<EndpointInfo> endpoints;
        QString scope;

        bool numeric = false;
        const quint32 number = keyword.toUInt(&numeric);
        if (numeric && number > 0 && number <= 65535) {
            // 端口号与 PID 的范围重叠，先按端口查；查不到再按 PID 查
            endpoints = WinEase::Win32::endpointsOnPort(static_cast<quint16>(number), true, true,
                                                       listeningOnly->isChecked());
            scope = QStringLiteral("端口 %1").arg(number);
            if (endpoints.isEmpty()) {
                endpoints = WinEase::Win32::endpointsOfProcess(number);
                scope = QStringLiteral("进程 %1").arg(number);
            }
        } else {
            endpoints = WinEase::Win32::allEndpoints(true, true);
            scope = QStringLiteral("全部端点");
            if (!keyword.isEmpty()) {
                // 按进程名过滤：先找出匹配的 PID，再筛端点
                const QList<WinEase::Win32::ProcessInfo> processes =
                    WinEase::Win32::findProcessesByName(keyword);
                QList<quint32> pids;
                pids.reserve(processes.size());
                for (const WinEase::Win32::ProcessInfo &process : processes) {
                    pids.append(process.pid);
                }
                QList<EndpointInfo> filtered;
                for (const EndpointInfo &info : endpoints) {
                    if (pids.contains(info.pid)) {
                        filtered.append(info);
                    }
                }
                endpoints = filtered;
                scope = QStringLiteral("进程名含「%1」").arg(keyword);
            }
        }

        if (listeningOnly->isChecked()) {
            QList<EndpointInfo> onlyListening;
            for (const EndpointInfo &info : endpoints) {
                if (info.isListening()) {
                    onlyListening.append(info);
                }
            }
            endpoints = onlyListening;
        }

        fillTable(table, endpoints);
        statusLabel->setText(QStringLiteral("%1：共 %2 条%3")
                                 .arg(scope)
                                 .arg(endpoints.size())
                                 .arg(endpoints.size() > kMaxTableRows
                                          ? QStringLiteral("（只显示前 %1 条）").arg(kMaxTableRows)
                                          : QString()));
    };

    QObject::connect(queryButton, &QPushButton::clicked, widget, runQuery);
    QObject::connect(queryEdit, &QLineEdit::returnPressed, widget, runQuery);
    QObject::connect(allButton, &QPushButton::clicked, widget, [table, statusLabel, runQuery, queryEdit] {
        queryEdit->clear();
        runQuery();
        statusLabel->setText(QStringLiteral("全部端点：共 %1 条").arg(table->rowCount()));
    });

    QObject::connect(copyButton, &QPushButton::clicked, widget, [table] {
        const int row = table->currentRow();
        if (row < 0) {
            return;
        }
        QStringList cells;
        for (int column = 0; column < table->columnCount(); ++column) {
            const QTableWidgetItem *item = table->item(row, column);
            cells.append(item != nullptr ? item->text() : QString());
        }
        ClipboardTools::setText(cells.join(QLatin1Char('\t')));
    });

    QObject::connect(killButton, &QPushButton::clicked, widget, [this, widget, table, statusLabel] {
        const int row = table->currentRow();
        if (row < 0) {
            statusLabel->setText(QStringLiteral("先在上面选中一行"));
            return;
        }
        const QTableWidgetItem *pidItem = table->item(row, 4);
        const quint32 pid = (pidItem != nullptr) ? pidItem->data(Qt::UserRole).toUInt() : 0;
        if (pid == 0) {
            statusLabel->setText(QStringLiteral("这一行没有可用的 PID"));
            return;
        }

        QString message;
        const bool ok = terminateByElevation(pid, &message);
        statusLabel->setText(message);
        Q_EMIT statusMessage(message);
        if (!ok) {
            // 失败原因（例如"该进程受系统保护"）值得完整展示，状态栏放不下
            QMessageBox::warning(widget, name(), message);
        }
    });

    runQuery();
    return widget;
}
