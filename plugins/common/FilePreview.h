#pragma once

// ============================================================================
//  FilePreview.h —— 文件预览的"取值"引擎（P2-02 快速文件预览共用）
//
//  为什么把预览拆成一个纯函数引擎（而不是写在插件的窗口类里）：
//    ① 端到端自检要**直接断言引擎的数字**，例如"100MB 的文本只读了 512KB"
//       （`TextResult::bytesRead`），这比隔着窗口去数控件可靠得多；
//    ② "取什么样的内容"和"怎么画出来"是两件事，混在一起后加一种文件类型
//       就要动界面代码。
//
//  ★ 本模块的三条硬指标（也是自检的断言点）：
//    ① 文本：只读文件**头部** maxBytes，绝不整文件载入内存；
//       → `TextResult::bytesRead` 与 `fileSize` 一起报出来，谁都能核对
//    ② 图片：**按目标尺寸解码**（QImageReader::setScaledSize），不做"先全解码再缩小"
//    ③ PDF：按需渲染**单页**（WinRT Windows.Data.Pdf），不整本载入
//
//  ⚠ 本头文件不含 Q_OBJECT；线程约定：本模块的函数**不保证线程安全**，
//    但除"Shell 缩略图 / WinRT PDF"外都是纯计算。这两项要求调用线程已初始化 COM，
//    因此插件一律在界面线程上调用（见 quick_look_plugin.cpp 里的说明）。
// ============================================================================

#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QString>

namespace WinEase::FeaturePlugins::FilePreview {

// ============================================================================
//  类型判定
// ============================================================================

enum class Kind {
    Unsupported = 0,
    Text,    ///< 纯文本 / 代码 / 日志 / 配置文件
    Image,   ///< Qt 能解码的位图，或系统能出缩略图的图片
    Pdf,     ///< PDF（走 WinRT 单页渲染，失败回退系统缩略图）
    Media,   ///< 视频 / 音频（取系统缩略图 = 首帧）
    Archive, ///< 压缩包（只用系统缩略图/图标，不解压）
    Other    ///< 其它（系统缩略图或类型图标）
};

QString kindDisplayName(Kind kind);

/// 按扩展名分类（纯函数，自检直接喂扩展名断言）
Kind classifyByExtension(const QString &extension);

/// 文件信息（预览窗口顶部那行文字就是 summary()）
struct Info
{
    QString path;
    QString fileName;
    QString extension; ///< 小写、不含点
    qint64 sizeBytes = 0;
    QDateTime modified;
    Kind kind = Kind::Unsupported;
    QString typeDescription; ///< 系统登记的类型描述，例如 "文本文档"
    bool exists = false;
    bool isDirectory = false;

    /// "PNG 图片 · 1.4 MB · 2026-09-14 10:20"
    QString summary() const;
};

Info describe(const QString &path);

// ============================================================================
//  文本预览
// ============================================================================

struct TextResult
{
    bool ok = false;
    QString text;        ///< 交给界面的内容（文本，或二进制时的十六进制转储）
    qint64 bytesRead = 0;///< ★ 实际从磁盘读到的字节数（"没有全量载入"的证据）
    qint64 fileSize = 0; ///< 文件真实大小
    bool truncated = false;  ///< 因为超过 maxBytes 被截断
    bool binary = false;     ///< 判定为二进制（不做乱码式"文本"展示）
    qint64 textBytes = 0;    ///< 按文本解读的部分有多少字节
    QString encoding;        ///< "UTF-8" / "非 UTF-8（按系统代码页解读）" / "二进制"
    QString error;

    /// "已显示前 512 KB / 共 128.0 MB"
    QString sizeText() const;
};

/// 读文件头部做预览。@param maxBytes 最多读多少字节（默认 512 KB）
TextResult readText(const QString &path, qint64 maxBytes = 512 * 1024);

/// 二进制内容的十六进制转储（自检直接断言它，不用真去读文件）
QString hexDump(const QByteArray &data, int maxBytes = 4096);

// ============================================================================
//  位图预览（图片 / PDF 页 / 系统缩略图）
// ============================================================================

struct BitmapResult
{
    bool ok = false;
    QImage image;
    QSize originalSize; ///< 原始尺寸；PDF 为渲染尺寸，缩略图与 image 相同
    bool scaled = false;///< 是否按 maxSize 缩小解码过
    QString source;     ///< "图片解码" / "PDF 第 1 页" / "系统缩略图"
    QString error;

    QString describe() const; ///< "1200×800 → 显示 800×533（按窗口缩放解码）"
};

/// 图片：按目标尺寸**缩放解码**（JPEG 交给解码器直接出小图；PNG 只分配目标大小的缓冲）
BitmapResult loadImage(const QString &path, const QSize &maxSize);

/// 系统缩略图（视频首帧 / Office / 压缩包等都靠它）
BitmapResult loadShellThumbnail(const QString &path, const QSize &maxSize);

/// 当前系统是否具备 PDF 渲染能力（探测 Windows.Data.Pdf，探测失败自动回退缩略图）
bool pdfAvailable(QString *errorOut = nullptr);

/// PDF 总页数；失败返回 -1
int pdfPageCount(const QString &path, QString *errorOut = nullptr);

/// 按需渲染**单页**（默认第 1 页），不整本载入
BitmapResult renderPdfPage(const QString &path, int pageIndex, const QSize &maxSize);

/// 统一入口：按 kind 选一条路（图片 → PDF → 缩略图），插件只调这一个
BitmapResult loadBitmap(const QString &path, Kind kind, const QSize &maxSize);

// ============================================================================
//  小工具
// ============================================================================

/// "1.4 MB" / "812 B" / "2.3 GB"
QString formatBytes(qint64 bytes);

} // namespace WinEase::FeaturePlugins::FilePreview
