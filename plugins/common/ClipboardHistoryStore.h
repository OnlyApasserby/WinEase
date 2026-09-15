#pragma once

// ============================================================================
//  ClipboardHistoryStore.h —— 剪贴板历史的存储引擎（P2-04）
//
//  为什么是"分片文件"而不是 SQLite：
//      本工程对插件有一条硬约束（架构约定 6）——**多引一个 Qt 模块就等于每个插件
//      多背一份 DLL 依赖**。Qt6Sql 在目标机上并不保证存在，为了几百条历史引入它
//      不划算；而"索引 + 每条正文一个文件"这种结构在这个量级上完全够用：
//
//          <storeDir>/index.json        条目索引（有序：新 → 旧）
//          <storeDir>/payload/<id>.txt  文本正文
//          <storeDir>/payload/<id>.png  图片（原图或缩略图）
//          <storeDir>/payload/<id>.list 文件路径清单（每行一个）
//
//      两个直接好处：
//        ① 正文不塞进索引 → 每次复制一条只重写索引（几百字节），不必重写全部历史；
//        ② 索引与正文分离 → 索引坏了也能靠 payload 目录看出"丢了什么"，
//           而且单条正文文件丢了只影响那一条（`payloadMissing` 如实标出，不装作有）。
//
//  落盘纪律：
//      * 每次**变更**都立刻写索引（不依赖退出时的收尾）；写索引走"临时文件 + 改名"，
//        避免掉电/被杀留下半个 JSON（半个 JSON 等于整份历史读不出来）；
//      * 淘汰条目时**连正文文件一起删**（否则磁盘上会留一堆再也引用不到的正文）。
//
//  上限策略（有意为之）：
//      条数上限只约束**非收藏**条目；用户明确点过"收藏"的东西被自动剪掉属于数据丢失。
//      界面上会把这件事说明白（"上限 N 条，收藏不计入"）。
//
//  ⚠ 本头文件不含 Q_OBJECT：它是纯数据 + 文件 IO，可被插件与自检**共用同一份源码**。
// ============================================================================

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

class QJsonObject;

namespace WinEase::FeaturePlugins::ClipboardHistoryStore {

/// 条目类型
enum class Kind {
    Text = 0, ///< 纯文本
    Image,    ///< 图片
    Files     ///< 文件（资源管理器里复制的文件/文件夹）
};

/// 配置文件里用的稳定键名（发布后不可改）
QString kindKey(Kind kind);
Kind kindFromKey(const QString &key, Kind fallback = Kind::Text);
/// 界面显示名（"文本" / "图片" / "文件"）
QString kindDisplayName(Kind kind);

/// 一条历史
struct Entry {
    QString id;               ///< 唯一 id（同时是正文文件名的主体）
    Kind kind = Kind::Text;
    QString preview;          ///< 单行预览（列表显示用，已截断）
    QString hash;             ///< 正文指纹（去重用）
    qint64 payloadBytes = 0;  ///< 正文占用的字节数
    QDateTime createdAt;      ///< 记录时间（本地时区）
    bool favorite = false;    ///< 用户收藏（不参与自动淘汰）
    QString sourceApp;        ///< 复制那一刻的前台进程名（可能为空）
    QString payloadFile;      ///< 相对 <storeDir>/payload 的文件名；空表示这条没有正文

    /// 图片：只存了缩略图（粘贴回去的是缩略图，不是原图 —— 界面必须说明）
    bool thumbnailOnly = false;
    /// 图片原始尺寸（thumbnailOnly 时用于说明"原图 N×M 没存"）
    int originalWidth = 0;
    int originalHeight = 0;

    /// 索引里有这条，但正文文件已经不在了（用户手动删过 / 磁盘出错）——如实展示
    bool payloadMissing = false;
};

/// add*() 的结果（"没记下来"也必须能说清是为什么）
enum class AddResult {
    Added,          ///< 新增
    Replaced,       ///< 同样内容已存在 → 提到最前，不新增
    SkippedEmpty,   ///< 空内容（剪贴板被清空等）
    SkippedTooLarge,///< 超过单条上限
    Failed          ///< 落盘失败
};

QString addResultText(AddResult result);

class Store
{
public:
    Store() = default;
    explicit Store(const QString &directory);

    // ---------------- 位置与上限 ----------------

    void setDirectory(const QString &directory);
    QString directory() const { return m_directory; }
    QString payloadDirectory() const;
    QString indexPath() const;

    /// 条数上限（只约束非收藏条目）；<= 0 表示不限
    void setMaxItems(int maxItems);
    int maxItems() const { return m_maxItems; }

    /// 单条正文上限（字节）；<= 0 表示不限
    void setMaxItemBytes(qint64 bytes);
    qint64 maxItemBytes() const { return m_maxItemBytes; }

    // ---------------- 读写 ----------------

    /// 读索引（目录不存在时视为空历史，返回 true）
    bool load(QString *errorOut = nullptr);
    /// 写索引（临时文件 + 改名，失败时给出中文原因）
    bool save(QString *errorOut = nullptr) const;

    // ---------------- 采集 ----------------

    AddResult addText(const QString &text,
                      const QString &sourceApp,
                      QString *idOut = nullptr,
                      QString *errorOut = nullptr);

    /// @param thumbnailOnly true = 只存缩略图（`kThumbnailMaxEdge` 为长边上限）
    AddResult addImage(const QImage &image,
                       bool thumbnailOnly,
                       const QString &sourceApp,
                       QString *idOut = nullptr,
                       QString *errorOut = nullptr);

    AddResult addFiles(const QStringList &paths,
                       const QString &sourceApp,
                       QString *idOut = nullptr,
                       QString *errorOut = nullptr);

    // ---------------- 查询 ----------------

    /// 全部条目（新 → 旧）
    const QVector<Entry> &entries() const { return m_entries; }
    int count() const { return m_entries.size(); }
    int favoriteCount() const;
    bool contains(const QString &id) const;
    /// 取一条；找不到时返回默认构造的 Entry（found 给出结果）
    Entry entry(const QString &id, bool *found = nullptr) const;

    /// 过滤 + 关键字搜索。
    /// @param keyword 空表示不筛关键字；同时匹配预览、来源应用名与**文本正文的前 8 KB**
    ///        （列表只显示一行预览，正文命中也得搜得到；只读头部保证一次搜索的 IO 有上界）
    /// @param filterKey "all" / "text" / "image" / "files" / "favorite"（空等同 all）
    QVector<Entry> search(const QString &keyword, const QString &filterKey) const;

    // ---------------- 修改 ----------------

    bool setFavorite(const QString &id, bool favorite);
    /// 删除一条（连正文文件）
    bool remove(const QString &id, QString *errorOut = nullptr);
    /// 清空全部（连全部正文文件）；返回删除的条数
    int clear(QString *errorOut = nullptr);
    /// 按上限裁掉最旧的非收藏条目；返回裁掉的条数
    int trimToLimit();

    // ---------------- 正文 ----------------

    QString payloadPath(const Entry &entry) const;
    /// 读回文本正文；条目不是文本 / 正文丢了 → false + 中文原因
    bool readText(const QString &id, QString *textOut, QString *errorOut = nullptr) const;
    /// 重新核对正文文件是否还在（刷新界面时调用）
    void refreshPayloadState();

    /// 单行预览（换行/制表符压成空格，超长截断）
    static QString previewFor(const QString &text, int maxChars = 120);
    /// 图片缩略图长边上限（像素）
    static constexpr int kThumbnailMaxEdge = 320;

private:
    int indexOf(const QString &id) const;
    QString allocateId() const;
    bool writePayload(const QString &fileName, const QByteArray &bytes, QString *errorOut);
    bool removePayloadFile(const Entry &entry);
    /// 去重 + 插入到最前 + 裁剪 + 落盘（add* 三个入口共用）
    AddResult insert(const Entry &entry, QString *idOut, QString *errorOut);
    void writeEntryJson(const Entry &entry, QJsonObject *json) const;
    static Entry readEntry(const QJsonObject &json);

    QString m_directory;
    QVector<Entry> m_entries; ///< 新 → 旧
    int m_maxItems = 200;
    qint64 m_maxItemBytes = 256 * 1024;
    /// 单进程内的序号（保证同一毫秒内连造两条 id 也不撞）
    static quint64 s_sequence;
};

} // namespace WinEase::FeaturePlugins::ClipboardHistoryStore
