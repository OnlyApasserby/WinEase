#include "sdk/FeatureCategory.h"

#include <QStringList>

#include <iterator>

namespace WinEase::Category {

namespace {

/// 分类静态描述表 —— 下标必须与 FeatureCategory 枚举值严格一致
struct CategoryInfo {
    const char *key;         // 稳定键名
    const char *displayName; // 中文名
    const char *description; // 说明
    const char *iconFile;    // 资源文件名
};

constexpr CategoryInfo kCategoryTable[] = {
    { "window",   "窗口管理", "窗口置顶、透明、吸附布局与多桌面管理",     "window.svg"      },
    { "file",     "文件增强", "右键菜单、批量重命名、快速复制路径",       "file.svg"        },
    { "input",    "输入效率", "剪贴板历史、文本片段、输入法辅助",         "input.svg"       },
    { "monitor",  "系统监控", "CPU/内存/磁盘/网络实时状态与悬浮窗",       "monitor.svg"     },
    { "display",  "显示辅助", "亮度、色温、夜间模式、护眼提醒",           "display.svg"     },
    { "media",    "媒体",     "音量控制、播放控制、截图录屏",             "media.svg"       },
    { "launcher", "启动器",   "快速启动应用、命令面板、快捷入口",         "launcher.svg"    },
    { "security", "安全",     "隐私清理、权限审计、敏感信息保护",         "security.svg"    },
    { "dev",      "开发",     "取色器、编码转换、JSON/正则辅助工具",      "dev.svg"         },
    { "personal", "个性化",   "主题、壁纸、桌面整理与桌面美化",           "personal.svg"    },
};

constexpr int kCategoryCount = static_cast<int>(std::size(kCategoryTable));

static_assert(kCategoryCount == static_cast<int>(FeatureCategory::Personalization) + 1,
              "分类表数量必须与 FeatureCategory 枚举保持一致");

/// 取分类信息，越界或 Unknown 时回退到第一个分类
const CategoryInfo &infoOf(FeatureCategory category)
{
    const int index = static_cast<int>(category);
    if (index < 0 || index >= kCategoryCount) {
        return kCategoryTable[0];
    }
    return kCategoryTable[index];
}

bool isKnown(FeatureCategory category)
{
    const int index = static_cast<int>(category);
    return index >= 0 && index < kCategoryCount;
}

} // namespace

QList<FeatureCategory> all()
{
    QList<FeatureCategory> result;
    result.reserve(kCategoryCount);
    for (int i = 0; i < kCategoryCount; ++i) {
        result.append(static_cast<FeatureCategory>(i));
    }
    return result;
}

QString displayName(FeatureCategory category)
{
    if (category == FeatureCategory::Unknown) {
        return QStringLiteral("全部功能");
    }
    return QString::fromUtf8(infoOf(category).displayName);
}

QString description(FeatureCategory category)
{
    if (category == FeatureCategory::Unknown) {
        return QStringLiteral("显示全部已加载的功能");
    }
    return QString::fromUtf8(infoOf(category).description);
}

QString iconPath(FeatureCategory category)
{
    if (!isKnown(category)) {
        return QStringLiteral(":/winease/icons/app.svg");
    }
    return QStringLiteral(":/winease/icons/") + QString::fromLatin1(infoOf(category).iconFile);
}

QIcon icon(FeatureCategory category)
{
    return QIcon(iconPath(category));
}

QString key(FeatureCategory category)
{
    if (!isKnown(category)) {
        return category == FeatureCategory::Unknown ? QStringLiteral("all") : QStringLiteral("unknown");
    }
    return QString::fromLatin1(infoOf(category).key);
}

FeatureCategory fromKey(const QString &key)
{
    if (key.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0) {
        return FeatureCategory::Unknown;
    }
    for (int i = 0; i < kCategoryCount; ++i) {
        if (key.compare(QString::fromLatin1(kCategoryTable[i].key), Qt::CaseInsensitive) == 0) {
            return static_cast<FeatureCategory>(i);
        }
    }
    return FeatureCategory::Unknown;
}

} // namespace WinEase::Category
