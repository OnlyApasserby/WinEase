# ============================================================================
#  WinEaseSdk —— 插件开发 SDK（静态库）
#
#  主程序与所有功能插件共同链接本库，其中只包含"契约"级内容：
#    - 功能分类定义 FeatureCategory
#    - 插件抽象基类 IFeaturePlugin
#    - 宿主服务接口 PluginServices
#
#  SDK 中不允许出现任何单例/全局状态，避免插件 DLL 与主程序状态分裂。
# ============================================================================
qt_add_library(WinEaseSdk STATIC
    FeatureCategory.h
    FeatureCategory.cpp
    HookService.h
    HookService.cpp
    IFeaturePlugin.h
    IFeaturePlugin.cpp
    OverlayWindow.h
    OverlayWindow.cpp
    OverlayHost.h
    OverlayHost.cpp
    PluginServices.h
    WinEaseVersion.h
)

# 让 SDK 使用者可以用 "sdk/xxx.h" 形式包含头文件
target_include_directories(WinEaseSdk
    PUBLIC "${CMAKE_SOURCE_DIR}/src"
)

# OverlayWindow（OverlayKit）需要 WinEaseWin32 的点击穿透 / 不抢焦点 /
# 多屏与 DPI 换算原语，因此 SDK 依赖平台能力层。
# 注意：这是 SDK → WinEaseWin32 的单向依赖，不存在循环。
target_link_libraries(WinEaseSdk
    PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets WinEaseWin32
)

set_target_properties(WinEaseSdk PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(WinEaseSdk PUBLIC WINEASE_SDK_VERSION="${PROJECT_VERSION}")
