#pragma once

// ============================================================================
//  ShellUtils.h —— Windows Shell 能力
//
//  用途（对应路线图中多个功能）：
//      * 快速文件预览（P2-02，缩略图兜底方案）
//      * 重复文件查找（P2-03，删除走回收站）
//      * 右键菜单扩展（P1-05，调用系统默认程序）
//      * 批量重命名（P2-01，定位到资源管理器）
//
//  设计要点：
//      1. 缩略图走 `IShellItemImageFactory`——由系统缩略图提供程序实现，
//         因此图片/视频/PDF/Office 文档都能拿到图，且**零额外依赖**
//      2. 图标转换统一用"32bpp DIB 段 + DrawIconEx"方式，
//         避免了手工解析图标掩码（HICON 的 alpha 规则很繁琐）
//      3. 删除一律**默认进回收站**，永久删除必须显式调用另一个接口
// ============================================================================

#include <QImage>
#include <QString>
#include <QStringList>

namespace WinEase::Win32 {

/// 常用系统文件夹（避免让每个插件各自写 FOLDERID 常量）
enum class KnownFolder {
    Desktop,
    Documents,
    Downloads,
    Pictures,
    Music,
    Videos,
    StartMenu,        ///< 当前用户开始菜单
    CommonStartMenu,  ///< 所有用户开始菜单
    Startup,          ///< 当前用户启动项
    CommonStartup,    ///< 所有用户启动项
    ProgramFiles,
    ProgramFilesX86,
    Windows,
    System32,
    Temp,
    RoamingAppData,
    LocalAppData,
    PublicDesktop,
    PublicDocuments
};

/// 系统文件夹路径；失败返回空串
QString knownFolderPath(KnownFolder folder);

// ============================================================================
//  缩略图与图标
// ============================================================================

struct ThumbnailResult {
    QImage image;
    bool fromShellIcon = false; ///< true 表示是文件类型图标而非内容缩略图
    QString error;
    bool isValid() const { return !image.isNull(); }
};

/// 取 Shell 内容缩略图（仅当系统能生成缩略图时成功）
/// @param size 期望边长（像素）；系统可能返回其他尺寸
ThumbnailResult fileThumbnail(const QString &path, int size = 256);

/// 取文件类型图标（一定会成功，除非路径非法）
ThumbnailResult fileIcon(const QString &path, int size = 256);

/// 先试缩略图，失败自动回退图标；上层无需关心差异
ThumbnailResult filePreview(const QString &path, int size = 256);

// ============================================================================
//  文件操作
// ============================================================================

/// 删除到回收站（用户可还原）。使用 IFileOperation（现代 API，支持长路径）
bool moveToRecycleBin(const QStringList &paths, QString *errorOut = nullptr);

/// 永久删除（**不进回收站，不可恢复**，调用方必须做过二次确认）
bool deletePermanently(const QStringList &paths, QString *errorOut = nullptr);

/// 在资源管理器中定位并选中该文件/文件夹
bool revealInExplorer(const QString &path);

/// 用系统默认程序打开（verb 为 "open"/"edit"/"runas" 等）
bool openWithShell(const QString &path, const QString &verb = QStringLiteral("open"));

// ============================================================================
//  文件类型信息
// ============================================================================

/// 系统对该类型的描述，例如 "文本文档"
QString fileTypeDescription(const QString &path);

/// 注册表中登记的 Content Type，例如 "text/plain"；未登记返回空串
QString fileContentType(const QString &path);

} // namespace WinEase::Win32
