# ============================================================================
#  ui_smoke —— 主界面呈现验收（卡片两行 + 帮助栏目）
#
#  与 theme_smoke / plugin_smoke 同一思路：把**主程序真正使用的** UI 与核心源文件
#  编入测试目标（FeatureCard / HelpDialog / PluginManager / ThemeManager），
#  而不是在测试里另抄一份实现 —— 抄一份的话，改坏了实现测试照样通过。
#
#  运行方式：
#      .\build\bin\ui_smoke.exe        # 退出码 0 = 全部通过；2 = 前置条件不成立
# ============================================================================

set(WINEASE_RESOURCE_DIR "${CMAKE_SOURCE_DIR}/src/resources")

qt_add_executable(ui_smoke
    main.cpp

    # 卡片/帮助页要读插件元信息（崩溃隔离后走缓存读），所以整个 PluginManager 一起编进来
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

    # 被测界面
    "${CMAKE_SOURCE_DIR}/src/app/ui/FeatureCard.h"
    "${CMAKE_SOURCE_DIR}/src/app/ui/FeatureCard.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/ui/HelpDialog.h"
    "${CMAKE_SOURCE_DIR}/src/app/ui/HelpDialog.cpp"
    # 与主程序一致地套主题（卡片的字体/行距来自全局样式表）
    "${CMAKE_SOURCE_DIR}/src/app/ui/ThemeManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/ui/ThemeManager.cpp"
)

target_include_directories(ui_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(ui_smoke PRIVATE
    WinEaseSdk        # IFeaturePlugin / FeatureCategory（含分类图标）
    WinEaseWin32      # Admin::isProcessElevated / 标题栏深色
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if (WIN32)
    target_link_libraries(ui_smoke PRIVATE
        shell32
        advapi32
    )
endif ()

# 资源与主程序保持**同一前缀与同一份文件**：卡片回退用的分类图标必须是主程序真正加载的那几个
qt_add_resources(ui_smoke "ui_assets"
    PREFIX "/winease"
    BASE "${WINEASE_RESOURCE_DIR}"
    FILES
        "${WINEASE_RESOURCE_DIR}/style.qss"
        "${WINEASE_RESOURCE_DIR}/icons/app.svg"
        "${WINEASE_RESOURCE_DIR}/icons/window.svg"
        "${WINEASE_RESOURCE_DIR}/icons/file.svg"
        "${WINEASE_RESOURCE_DIR}/icons/input.svg"
        "${WINEASE_RESOURCE_DIR}/icons/monitor.svg"
        "${WINEASE_RESOURCE_DIR}/icons/display.svg"
        "${WINEASE_RESOURCE_DIR}/icons/media.svg"
        "${WINEASE_RESOURCE_DIR}/icons/launcher.svg"
        "${WINEASE_RESOURCE_DIR}/icons/security.svg"
        "${WINEASE_RESOURCE_DIR}/icons/dev.svg"
        "${WINEASE_RESOURCE_DIR}/icons/personal.svg"
)

set_target_properties(ui_smoke PROPERTIES
    OUTPUT_NAME "ui_smoke"
    WIN32_EXECUTABLE FALSE
)
