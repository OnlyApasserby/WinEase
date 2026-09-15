#include "sdk/OverlayHost.h"

#include "win32/WindowUtils.h"

#include <utility>

namespace WinEase {

OverlayHost::OverlayHost(QObject *parent)
    : QObject(parent)
{
}

OverlayHost::~OverlayHost()
{
    // 析构时不依赖事件循环：直接 hide + delete。
    // （closeAll() 走的是 hide → processEvents → deleteLater 的路径，
    //   那需要事件循环还活着；退出阶段不能假设这一点）
    const QList<Entry> entries = std::exchange(m_entries, QList<Entry>());
    for (const Entry &entry : entries) {
        if (entry.overlay != nullptr) {
            entry.overlay->hide();
            delete entry.overlay;
        }
    }
}

// ---------------------------------------------------------------------------
//  创建
// ---------------------------------------------------------------------------

OverlayWindow *OverlayHost::createOverlay(const QString &ownerId)
{
    // 顶层窗口：不设父对象，否则会被父窗口的层级与裁剪约束住
    auto *overlay = new OverlayWindow();
    registerEntry(ownerId, overlay);
    return overlay;
}

QList<OverlayWindow *> OverlayHost::createOverlaysForAllMonitors(const QString &ownerId)
{
    QList<OverlayWindow *> created;
    const QList<Win32::MonitorInfo> all = Win32::monitors();
    for (int index = 0; index < all.size(); ++index) {
        if (!all.at(index).valid) {
            continue;
        }
        OverlayWindow *overlay = createOverlay(ownerId);
        if (overlay->coverMonitor(index)) {
            created.append(overlay);
        }
    }
    return created;
}

OverlayWindow *OverlayHost::adoptOverlay(const QString &ownerId, OverlayWindow *overlay)
{
    if (overlay == nullptr) {
        return nullptr;
    }

    // 重复接管同一实例：只改归属，不重复登记、不重复连接 destroyed
    for (Entry &entry : m_entries) {
        if (entry.overlay.data() == overlay) {
            if (entry.ownerId != ownerId) {
                entry.ownerId = ownerId;
                Q_EMIT overlayCreated(overlay, ownerId);
            }
            return overlay;
        }
    }

    registerEntry(ownerId, overlay);
    return overlay;
}

// ---------------------------------------------------------------------------
//  回收
// ---------------------------------------------------------------------------

void OverlayHost::closeOverlay(OverlayWindow *overlay)
{
    if (overlay == nullptr) {
        return;
    }
    const QString owner = takeEntry(overlay);
    overlay->closeOverlay();
    if (!owner.isNull()) {
        Q_EMIT overlayClosed(owner);
        Q_EMIT overlayCountChanged(m_entries.size());
    }
}

int OverlayHost::closeOverlaysOfOwner(const QString &ownerId)
{
    const int count = takeEntriesOfOwner(ownerId);
    if (count == 0) {
        return 0;
    }
    Q_EMIT overlayClosed(ownerId);
    Q_EMIT overlayCountChanged(m_entries.size());
    return count;
}

int OverlayHost::closeAll()
{
    if (m_entries.isEmpty()) {
        return 0;
    }

    const int count = m_entries.size();
    const QList<Entry> entries = std::exchange(m_entries, QList<Entry>());
    for (const Entry &entry : entries) {
        if (entry.overlay != nullptr) {
            entry.overlay->closeOverlay();
        }
    }

    Q_EMIT overlayClosed(QString());
    Q_EMIT overlayCountChanged(0);
    return count;
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

int OverlayHost::overlayCount() const
{
    return m_entries.size();
}

QList<OverlayWindow *> OverlayHost::overlays() const
{
    QList<OverlayWindow *> result;
    result.reserve(m_entries.size());
    for (const Entry &entry : m_entries) {
        if (entry.overlay != nullptr) {
            result.append(entry.overlay);
        }
    }
    return result;
}

QList<OverlayWindow *> OverlayHost::overlaysOfOwner(const QString &ownerId) const
{
    QList<OverlayWindow *> result;
    for (const Entry &entry : m_entries) {
        if (entry.ownerId == ownerId && entry.overlay != nullptr) {
            result.append(entry.overlay);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
//  登记表
// ---------------------------------------------------------------------------

void OverlayHost::registerEntry(const QString &ownerId, OverlayWindow *overlay)
{
    // 被外部直接 delete（含"插件自己 new 的那条路"）时也要能把登记表清干净。
    // 只比较指针值，不解引用，因此这里不存在悬空访问
    connect(overlay, &QObject::destroyed, this, [this, overlay]() {
        const QString owner = takeEntry(overlay);
        if (!owner.isNull()) {
            Q_EMIT overlayClosed(owner);
            Q_EMIT overlayCountChanged(m_entries.size());
        }
    });

    m_entries.append(Entry{overlay, ownerId});

    Q_EMIT overlayCreated(overlay, ownerId);
    Q_EMIT overlayCountChanged(m_entries.size());
}

QString OverlayHost::takeEntry(OverlayWindow *overlay)
{
    for (int index = 0; index < m_entries.size(); ++index) {
        if (m_entries.at(index).overlay.data() != overlay) {
            continue;
        }
        const QString owner = m_entries.at(index).ownerId;
        m_entries.removeAt(index);
        return owner;
    }
    return QString();
}

int OverlayHost::takeEntriesOfOwner(const QString &ownerId)
{
    int closed = 0;
    for (int index = m_entries.size() - 1; index >= 0; --index) {
        if (m_entries.at(index).ownerId != ownerId) {
            continue;
        }
        OverlayWindow *overlay = m_entries.at(index).overlay.data();
        m_entries.removeAt(index);
        ++closed;
        if (overlay != nullptr) {
            overlay->closeOverlay();
        }
    }
    return closed;
}

} // namespace WinEase
