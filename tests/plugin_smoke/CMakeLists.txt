# ============================================================================
#  plugin_smoke —— 插件异常边界与崩溃隔离验收（P0-5）
#
#  与 theme_smoke / elevation_smoke 同一思路：把**真实的 PluginManager、
#  PluginGuard、SettingsManager、Logging**（主程序源文件）编入测试目标，
#  测的是主程序真正使用的代码路径，而不是在测试里另抄一份实现。
#
#  运行方式：
#      .\build\bin\plugin_smoke.exe        # 退出码 0 表示全部通过
# ============================================================================

qt_add_executable(plugin_smoke
    main.cpp

    "${CMAKE_SOURCE_DIR}/src/app/core/PluginGuard.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/PluginGuard.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/PluginManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/PluginManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/SettingsManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/SettingsManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/AdminHelper.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/AdminHelper.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/core/Logging.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/Logging.cpp"
    # PluginManager 会调 GlobalHotkeyManager::textToSequence() 解析快捷键文本
    "${CMAKE_SOURCE_DIR}/src/app/core/GlobalHotkeyManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/GlobalHotkeyManager.cpp"
)

target_include_directories(plugin_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(plugin_smoke PRIVATE
    WinEaseSdk        # IFeaturePlugin / FeatureCategory
    WinEaseWin32      # Admin::isProcessElevated 等平台原语
    Qt6::Core
    Qt6::Gui          # QIcon / QKeySequence
    Qt6::Widgets      # QApplication（元信息探针会走到 Category::icon()）
)

if (WIN32)
    target_link_libraries(plugin_smoke PRIVATE
        shell32
        advapi32
    )
endif ()

set_target_properties(plugin_smoke PROPERTIES
    OUTPUT_NAME "plugin_smoke"
    WIN32_EXECUTABLE FALSE
)
