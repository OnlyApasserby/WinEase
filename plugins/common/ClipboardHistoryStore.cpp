#include "ClipboardHistoryStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace WinEase::FeaturePlugins::ClipboardHistoryStore {

namespace {

constexpr int kIndexJsonVersion = 1;
/// 搜索正文时最多读多少字节（列表只显示一行预览，正文命中也得能搜到，
/// 但不能为了搜索把几百条正文全读一遍 —— 读头部就够了，且 IO 有上界）
constexpr qint64 kSearchBodyHeadBytes = 8 * 1024;
constexpr const char *kTextSuffix = ".txt";
constexpr const char *kImageSuffix = ".png";
constexpr const char *kFilesSuffix = ".list";

QString hashOfBytes(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString suffixForKind(Kind kind)
{
    switch (kind) {
    case Kind::Image: return QString::fromLatin1(kImageSuffix);
    case Kind::Files: return QString::fromLatin1(kFilesSuffix);
    case Kind::Text:
    default:          return QString::fromLatin1(kTextSuffix);
    }
}

QString errnoText(const QString &what, const QFileDevice &file)
{
    return QStringLiteral("%1：%2").arg(what, file.errorString());
}

} // namespace

quint64 Store::s_sequence = 0;

QString kindKey(Kind kind)
{
    switch (kind) {
    case Kind::Image: return QStringLiteral("image");
    case Kind::Files: return QStringLiteral("files");
    case Kind::Text:
    default:          return QStringLiteral("text");
    }
}

Kind kindFromKey(const QString &key, Kind fallback)
{
    const QString normalized = key.trimmed().toLower();
    if (normalized == QLatin1String("text")) {
        return Kind::Text;
    }
    if (normalized == QLatin1String("image")) {
        return Kind::Image;
    }
    if (normalized == QLatin1String("files")) {
        return Kind::Files;
    }
    return fallback;
}

QString kindDisplayName(Kind kind)
{
    switch (kind) {
    case Kind::Image: return QStringLiteral("图片");
    case Kind::Files: return QStringLiteral("文件");
    case Kind::Text:
    default:          return QStringLiteral("文本");
    }
}

QString addResultText(AddResult result)
{
    switch (result) {
    case AddResult::Added:          return QStringLiteral("已记录");
    case AddResult::Replaced:       return QStringLiteral("同样内容已在历史里，提到最前");
    case AddResult::SkippedEmpty:   return QStringLiteral("内容为空，未记录");
    case AddResult::SkippedTooLarge:return QStringLiteral("超过单条上限，未记录");
    case AddResult::Failed:         return QStringLiteral("写入历史失败");
    }
    return QStringLiteral("未知结果");
}

Store::Store(const QString &directory)
    : m_directory(directory)
{
}

void Store::setDirectory(const QString &directory)
{
    if (m_directory == directory) {
        return;
    }
    m_directory = directory;
    m_entries.clear();
}

QString Store::payloadDirectory() const
{
    return QDir(m_directory).filePath(QStringLiteral("payload"));
}

QString Store::indexPath() const
{
    return QDir(m_directory).filePath(QStringLiteral("index.json"));
}

void Store::setMaxItems(int maxItems)
{
    m_maxItems = maxItems;
}

void Store::setMaxItemBytes(qint64 bytes)
{
    m_maxItemBytes = bytes;
}

// ---------------------------------------------------------------------------
//  索引读写
// ---------------------------------------------------------------------------

void Store::writeEntryJson(const Entry &entry, QJsonObject *json) const
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), entry.id);
    object.insert(QStringLiteral("kind"), kindKey(entry.kind));
    object.insert(QStringLiteral("preview"), entry.preview);
    object.insert(QStringLiteral("hash"), entry.hash);
    object.insert(QStringLiteral("bytes"), static_cast<double>(entry.payloadBytes));
    object.insert(QStringLiteral("createdAt"), entry.createdAt.toString(Qt::ISODate));
    object.insert(QStringLiteral("favorite"), entry.favorite);
    object.insert(QStringLiteral("sourceApp"), entry.sourceApp);
    object.insert(QStringLiteral("payloadFile"), entry.payloadFile);
    object.insert(QStringLiteral("thumbnailOnly"), entry.thumbnailOnly);
    object.insert(QStringLiteral("originalWidth"), entry.originalWidth);
    object.insert(QStringLiteral("originalHeight"), entry.originalHeight);
    *json = object;
}

Entry Store::readEntry(const QJsonObject &json)
{
    Entry entry;
    entry.id = json.value(QStringLiteral("id")).toString();
    entry.kind = kindFromKey(json.value(QStringLiteral("kind")).toString());
    entry.preview = json.value(QStringLiteral("preview")).toString();
    entry.hash = json.value(QStringLiteral("hash")).toString();
    entry.payloadBytes = static_cast<qint64>(json.value(QStringLiteral("bytes")).toDouble());
    entry.createdAt = QDateTime::fromString(json.value(QStringLiteral("createdAt")).toString(),
                                            Qt::ISODate);
    entry.favorite = json.value(QStringLiteral("favorite")).toBool(false);
    entry.sourceApp = json.value(QStringLiteral("sourceApp")).toString();
    entry.payloadFile = json.value(QStringLiteral("payloadFile")).toString();
    entry.thumbnailOnly = json.value(QStringLiteral("thumbnailOnly")).toBool(false);
    entry.originalWidth = json.value(QStringLiteral("originalWidth")).toInt();
    entry.originalHeight = json.value(QStringLiteral("originalHeight")).toInt();
    return entry;
}

bool Store::load(QString *errorOut)
{
    m_entries.clear();
    if (m_directory.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("历史目录为空（插件没配置 storageDir）");
        }
        return false;
    }

    const QString path = indexPath();
    if (!QFile::exists(path)) {
        // 第一次运行：没有索引不是错误
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = errnoText(QStringLiteral("读不了历史索引 %1").arg(path), file);
        }
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("历史索引不是合法 JSON（偏移 %1：%2）")
                            .arg(parseError.offset)
                            .arg(parseError.errorString());
        }
        return false;
    }

    const QJsonArray items = document.object().value(QStringLiteral("items")).toArray();
    m_entries.reserve(items.size());
    for (const QJsonValue &value : items) {
        if (!value.isObject()) {
            continue;
        }
        const Entry entry = readEntry(value.toObject());
        if (!entry.id.isEmpty()) {
            m_entries.append(entry);
        }
    }
    refreshPayloadState();
    return true;
}

bool Store::save(QString *errorOut) const
{
    if (m_directory.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("历史目录为空，无法保存");
        }
        return false;
    }

    QDir().mkpath(m_directory);
    QDir().mkpath(payloadDirectory());

    QJsonArray items;
    for (const Entry &entry : m_entries) {
        QJsonObject object;
        writeEntryJson(entry, &object);
        items.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexJsonVersion);
    root.insert(QStringLiteral("items"), items);

    // QSaveFile：写临时文件 + 落盘后原子替换 —— 掉电/被杀不会留下半个 JSON
    QSaveFile file(indexPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut != nullptr) {
            *errorOut = errnoText(QStringLiteral("写不了历史索引 %1").arg(indexPath()), file);
        }
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size() || !file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = errnoText(QStringLiteral("历史索引落盘失败 %1").arg(indexPath()), file);
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  采集
// ---------------------------------------------------------------------------

QString Store::previewFor(const QString &text, int maxChars)
{
    QString oneLine = text;
    oneLine.replace(QLatin1Char('\r'), QLatin1Char(' '));
    oneLine.replace(QLatin1Char('\n'), QLatin1Char(' '));
    oneLine.replace(QLatin1Char('\t'), QLatin1Char(' '));
    oneLine = oneLine.simplified();
    if (maxChars > 0 && oneLine.size() > maxChars) {
        oneLine = oneLine.left(maxChars) + QStringLiteral("…");
    }
    return oneLine;
}

QString Store::allocateId() const
{
    // 时间戳（毫秒）+ 进程内序号：人肉看目录时是按时间排的，且同毫秒也不会撞
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
    return QStringLiteral("%1_%2").arg(stamp).arg(++s_sequence, 3, 10, QLatin1Char('0'));
}

int Store::indexOf(const QString &id) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

bool Store::contains(const QString &id) const
{
    return indexOf(id) >= 0;
}

int Store::favoriteCount() const
{
    int count = 0;
    for (const Entry &entry : m_entries) {
        if (entry.favorite) {
            ++count;
        }
    }
    return count;
}

Entry Store::entry(const QString &id, bool *found) const
{
    const int index = indexOf(id);
    if (found != nullptr) {
        *found = index >= 0;
    }
    return index >= 0 ? m_entries.at(index) : Entry();
}

bool Store::writePayload(const QString &fileName, const QByteArray &bytes, QString *errorOut)
{
    QDir().mkpath(payloadDirectory());
    const QString path = QDir(payloadDirectory()).filePath(fileName);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut != nullptr) {
            *errorOut = errnoText(QStringLiteral("写不了正文文件 %1").arg(path), file);
        }
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = errnoText(QStringLiteral("正文落盘失败 %1").arg(path), file);
        }
        return false;
    }
    return true;
}

bool Store::removePayloadFile(const Entry &entry)
{
    if (entry.payloadFile.isEmpty()) {
        return true;
    }
    const QString path = QDir(payloadDirectory()).filePath(entry.payloadFile);
    if (!QFile::exists(path)) {
        return true;
    }
    return QFile::remove(path);
}

AddResult Store::insert(const Entry &entry, QString *idOut, QString *errorOut)
{
    // ⚠ 调用方（add* 三个入口）**已经**把正文写到了 <storeDir>/payload/<entry.id> 上。
    //   所以本函数一旦不走"用这份 entry"，就必须把那份刚写下去的正文文件删掉：
    //   "淘汰条目时连正文文件一起删"这条纪律对**任何**不落进索引的正文同样成立，
    //   否则磁盘上会攒下一堆再也引用不到的正文（历史文件里最脏的一种垃圾）。
    // ---- 去重：同样内容只留一份，并把已有那条提到最前（不新增、不重写正文）----
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).hash == entry.hash && m_entries.at(i).kind == entry.kind) {
            Entry existing = m_entries.takeAt(i);
            existing.createdAt = entry.createdAt;
            if (!entry.preview.isEmpty()) {
                existing.preview = entry.preview;
            }
            if (!entry.sourceApp.isEmpty()) {
                existing.sourceApp = entry.sourceApp;
            }
            // 索引里留下的仍是 existing 的那份正文 → 刚写下去的那份是多余的一份。
            // 先删再写索引：无论 save 成不成，删掉它都是对的
            // （索引从不引用它，删早了不会丢数据，删晚了才会留垃圾）。
            if (entry.payloadFile != existing.payloadFile) {
                removePayloadFile(entry);
            }
            m_entries.prepend(existing);
            if (!save(errorOut)) {
                return AddResult::Failed;
            }
            if (idOut != nullptr) {
                *idOut = existing.id;
            }
            return AddResult::Replaced;
        }
    }

    m_entries.prepend(entry);
    const int trimmed = trimToLimit();
    if (!save(errorOut)) {
        // 索引没落盘 → 内存也退回这一步，别让"内存里的历史"与"磁盘上的历史"分叉；
        // 刚写下的正文文件同样不留（它没进索引 = 孤儿）。
        // 注意：trimToLimit() 已经删掉的那些正文文件不会因此回来 —— save() 失败意味着
        // 磁盘已经出问题，此时以磁盘上的旧索引为准，重载时那些条目会被如实标成"正文丢了"。
        m_entries.removeFirst();
        removePayloadFile(entry);
        return AddResult::Failed;
    }
    Q_UNUSED(trimmed);
    if (idOut != nullptr) {
        *idOut = entry.id;
    }
    return AddResult::Added;
}

AddResult Store::addText(const QString &text, const QString &sourceApp, QString *idOut, QString *errorOut)
{
    if (text.isEmpty()) {
        return AddResult::SkippedEmpty;
    }
    const QByteArray bytes = text.toUtf8();
    if (m_maxItemBytes > 0 && bytes.size() > m_maxItemBytes) {
        return AddResult::SkippedTooLarge;
    }

    Entry entry;
    entry.id = allocateId();
    entry.kind = Kind::Text;
    entry.preview = previewFor(text);
    entry.hash = hashOfBytes(bytes);
    entry.payloadBytes = bytes.size();
    entry.createdAt = QDateTime::currentDateTime();
    entry.sourceApp = sourceApp;
    entry.payloadFile = entry.id + suffixForKind(Kind::Text);

    if (!writePayload(entry.payloadFile, bytes, errorOut)) {
        return AddResult::Failed;
    }
    return insert(entry, idOut, errorOut);
}

AddResult Store::addImage(const QImage &image,
                          bool thumbnailOnly,
                          const QString &sourceApp,
                          QString *idOut,
                          QString *errorOut)
{
    if (image.isNull()) {
        return AddResult::SkippedEmpty;
    }

    // ⚠ 只缩不放：QImage::scaled(Qt::KeepAspectRatio) 在目标比原图大时**会把图放大**，
    //   所以这里自己判"是否真的需要缩"（踩坑 #43 的教训）
    QImage stored = image;
    if (thumbnailOnly) {
        const int longest = qMax(image.width(), image.height());
        if (longest > kThumbnailMaxEdge) {
            stored = image.scaled(kThumbnailMaxEdge, kThumbnailMaxEdge,
                                  Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    QByteArray bytes;
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!stored.save(&buffer, "PNG")) {
            if (errorOut != nullptr) {
                *errorOut = QStringLiteral("图片编码为 PNG 失败（内存不足或图片过大）");
            }
            return AddResult::Failed;
        }
    }

    if (m_maxItemBytes > 0 && bytes.size() > m_maxItemBytes) {
        return AddResult::SkippedTooLarge;
    }

    Entry entry;
    entry.id = allocateId();
    entry.kind = Kind::Image;
    entry.hash = hashOfBytes(bytes);
    entry.payloadBytes = bytes.size();
    entry.createdAt = QDateTime::currentDateTime();
    entry.sourceApp = sourceApp;
    entry.payloadFile = entry.id + suffixForKind(Kind::Image);
    entry.thumbnailOnly = thumbnailOnly;
    entry.originalWidth = image.width();
    entry.originalHeight = image.height();

    if (thumbnailOnly && (stored.width() != image.width() || stored.height() != image.height())) {
        entry.preview = QStringLiteral("图片 %1×%2（只存了缩略图 %3×%4，原图未保存）")
                            .arg(image.width())
                            .arg(image.height())
                            .arg(stored.width())
                            .arg(stored.height());
    } else if (thumbnailOnly) {
        entry.preview = QStringLiteral("图片 %1×%2（小于缩略图上限，按原图保存）")
                            .arg(image.width())
                            .arg(image.height());
    } else {
        entry.preview = QStringLiteral("图片 %1×%2").arg(image.width()).arg(image.height());
    }

    if (!writePayload(entry.payloadFile, bytes, errorOut)) {
        return AddResult::Failed;
    }
    return insert(entry, idOut, errorOut);
}

AddResult Store::addFiles(const QStringList &paths, const QString &sourceApp, QString *idOut, QString *errorOut)
{
    if (paths.isEmpty()) {
        return AddResult::SkippedEmpty;
    }
    const QByteArray bytes = paths.join(QLatin1Char('\n')).toUtf8();
    if (m_maxItemBytes > 0 && bytes.size() > m_maxItemBytes) {
        return AddResult::SkippedTooLarge;
    }

    Entry entry;
    entry.id = allocateId();
    entry.kind = Kind::Files;
    entry.preview = paths.size() == 1
        ? QFileInfo(paths.first()).fileName()
        : QStringLiteral("%1 等 %2 项").arg(QFileInfo(paths.first()).fileName()).arg(paths.size());
    entry.hash = hashOfBytes(bytes);
    entry.payloadBytes = bytes.size();
    entry.createdAt = QDateTime::currentDateTime();
    entry.sourceApp = sourceApp;
    entry.payloadFile = entry.id + suffixForKind(Kind::Files);

    if (!writePayload(entry.payloadFile, bytes, errorOut)) {
        return AddResult::Failed;
    }
    return insert(entry, idOut, errorOut);
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------

bool Store::readText(const QString &id, QString *textOut, QString *errorOut) const
{
    const Entry existing = entry(id);
    if (existing.id.isEmpty()) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("历史里没有这条记录");
        }
        return false;
    }
    if (existing.kind != Kind::Text) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("这条不是文本（是%1）").arg(kindDisplayName(existing.kind));
        }
        return false;
    }
    const QString path = payloadPath(existing);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = errnoText(QStringLiteral("读不了正文文件 %1").arg(path), file);
        }
        return false;
    }
    if (textOut != nullptr) {
        *textOut = QString::fromUtf8(file.readAll());
    }
    return true;
}

QString Store::payloadPath(const Entry &entry) const
{
    if (entry.payloadFile.isEmpty()) {
        return QString();
    }
    return QDir(payloadDirectory()).filePath(entry.payloadFile);
}

void Store::refreshPayloadState()
{
    for (Entry &entry : m_entries) {
        entry.payloadMissing = !entry.payloadFile.isEmpty()
            && !QFile::exists(payloadPath(entry));
    }
}

QVector<Entry> Store::search(const QString &keyword, const QString &filterKey) const
{
    const QString filter = filterKey.trimmed().toLower();
    const QString needle = keyword.trimmed();

    QVector<Entry> result;
    for (const Entry &entry : m_entries) {
        if (filter == QLatin1String("favorite")) {
            if (!entry.favorite) {
                continue;
            }
        } else if (filter == QLatin1String("text") || filter == QLatin1String("image")
                   || filter == QLatin1String("files")) {
            if (kindKey(entry.kind) != filter) {
                continue;
            }
        }

        if (!needle.isEmpty()) {
            bool hit = entry.preview.contains(needle, Qt::CaseInsensitive)
                || entry.sourceApp.contains(needle, Qt::CaseInsensitive)
                || kindDisplayName(entry.kind).contains(needle);
            if (!hit && entry.kind == Kind::Text && !entry.payloadFile.isEmpty()) {
                QFile file(payloadPath(entry));
                if (file.open(QIODevice::ReadOnly)) {
                    const QString head = QString::fromUtf8(file.read(kSearchBodyHeadBytes));
                    hit = head.contains(needle, Qt::CaseInsensitive);
                }
            }
            if (!hit) {
                continue;
            }
        }
        result.append(entry);
    }
    return result;
}

// ---------------------------------------------------------------------------
//  修改
// ---------------------------------------------------------------------------

bool Store::setFavorite(const QString &id, bool favorite)
{
    const int index = indexOf(id);
    if (index < 0) {
        return false;
    }
    if (m_entries.at(index).favorite == favorite) {
        return true;
    }
    m_entries[index].favorite = favorite;
    trimToLimit();
    return save();
}

bool Store::remove(const QString &id, QString *errorOut)
{
    const int index = indexOf(id);
    if (index < 0) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("要删除的条目不存在");
        }
        return false;
    }
    const Entry entry = m_entries.at(index);
    m_entries.remove(index);
    if (!removePayloadFile(entry)) {
        if (errorOut != nullptr) {
            *errorOut = QStringLiteral("条目已从索引里删除，但正文文件删不掉：%1")
                            .arg(payloadPath(entry));
        }
        save();
        return false;
    }
    return save(errorOut);
}

int Store::clear(QString *errorOut)
{
    const int removed = m_entries.size();
    for (const Entry &entry : m_entries) {
        removePayloadFile(entry);
    }
    m_entries.clear();
    if (!m_directory.isEmpty()) {
        // payload 目录本身留着（空目录无害），但里面应该一个文件都不剩
        save(errorOut);
    }
    return removed;
}

int Store::trimToLimit()
{
    if (m_maxItems <= 0) {
        return 0;
    }

    QVector<int> removable; // 非收藏条目在 m_entries 里的下标（新 → 旧，末尾最旧）
    for (int i = 0; i < m_entries.size(); ++i) {
        if (!m_entries.at(i).favorite) {
            removable.append(i);
        }
    }

    int trimCount = removable.size() - m_maxItems;
    if (trimCount <= 0) {
        return 0;
    }

    // 从最旧的非收藏条目开始删（列表尾部 = 最新在前的尾部）
    QVector<int> doomed;
    for (int i = m_entries.size() - 1; i >= 0 && trimCount > 0; --i) {
        if (!m_entries.at(i).favorite) {
            doomed.append(i);
            --trimCount;
        }
    }
    std::sort(doomed.begin(), doomed.end());

    for (int i = doomed.size() - 1; i >= 0; --i) {
        const int index = doomed.at(i);
        removePayloadFile(m_entries.at(index));
        m_entries.remove(index);
    }
    return doomed.size();
}

} // namespace WinEase::FeaturePlugins::ClipboardHistoryStore
