# ============================================================================
#  WinEase 主程序
# ============================================================================

set(WINEASE_RESOURCE_DIR "${CMAKE_SOURCE_DIR}/src/resources")

qt_add_executable(WinEase
    main.cpp

    core/AdminHelper.h            core/AdminHelper.cpp
    core/AppContext.h             core/AppContext.cpp
    core/ElevationClient.h        core/ElevationClient.cpp
    core/GlobalHotkeyManager.h    core/GlobalHotkeyManager.cpp
    core/HookServiceImpl.h        core/HookServiceImpl.cpp
    core/Logging.h                core/Logging.cpp
    # OverlayHost（悬浮层生命周期宿主）已移入 src/sdk/：插件 DLL 也要调用它，
    # 而主程序的符号对 DLL 不可见 → 放进 SDK，实现只保留一份（见 sdk/OverlayHost.h）
    core/PluginGuard.h            core/PluginGuard.cpp
    core/PluginManager.h          core/PluginManager.cpp
    core/SettingsManager.h        core/SettingsManager.cpp
    core/SingleInstanceGuard.h    core/SingleInstanceGuard.cpp

    ui/CategoryNavWidget.h        ui/CategoryNavWidget.cpp
    ui/FeatureCard.h              ui/FeatureCard.cpp
    ui/FeatureGridView.h          ui/FeatureGridView.cpp
    ui/HelpDialog.h               ui/HelpDialog.cpp
    ui/HotkeySettingsDialog.h     ui/HotkeySettingsDialog.cpp
    ui/MainWindow.h               ui/MainWindow.cpp
    ui/SystemTrayManager.h        ui/SystemTrayManager.cpp
    ui/ThemeManager.h             ui/ThemeManager.cpp
)

target_include_directories(WinEase PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"   # 便于写 #include "core/xxx.h" / "ui/xxx.h"
    "${CMAKE_SOURCE_DIR}/src"       # 便于写 #include "sdk/xxx.h"
)

target_link_libraries(WinEase PRIVATE
    WinEaseSdk
    WinEaseWin32
    Qt6::Widgets
    Qt6::Svg
    Qt6::Network
)

# Windows SDK：注册表、托盘、ShellExecuteEx 提权等
# （Qt6::Network 已在上面链接：提权通道 QLocalSocket 命名管道连接 WinEaseHelper）
if (WIN32)
    target_link_libraries(WinEase PRIVATE user32 advapi32 shell32 ole32)
endif ()

# 可选：QtMultimedia（视频预览/媒体面板）。未安装时不定义该宏，代码走回退分支。
if (WINEASE_HAS_MULTIMEDIA)
    target_link_libraries(WinEase PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
    target_compile_definitions(WinEase PRIVATE WINEASE_HAS_MULTIMEDIA=1)
endif ()

target_compile_definitions(WinEase PRIVATE
    WINEASE_APP_VERSION="${PROJECT_VERSION}"
)

set_target_properties(WinEase PROPERTIES
    OUTPUT_NAME "WinEase"
    WIN32_EXECUTABLE TRUE           # GUI 子系统，不弹控制台
)

foreach (_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(WinEase PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin"
    )
endforeach ()

# ---------------------------------------------------------------------------
#  Qt 资源（图标 + 样式表），资源前缀统一为 "/winease"
# ---------------------------------------------------------------------------
qt_add_resources(WinEase "winease_assets"
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

# ---------------------------------------------------------------------------
# 运行时依赖部署（可选）：cmake --install build 时会调用 windeployqt
#   如需启用，取消下面注释并配置 CMAKE_INSTALL_PREFIX
# ---------------------------------------------------------------------------
# qt_generate_deploy_app_script(TARGET WinEase OUTPUT_SCRIPT WINEASE_DEPLOY_SCRIPT NO_UNSUPPORTED_PLATFORM_ERROR)
# install(TARGETS WinEase RUNTIME DESTINATION .)
# install(SCRIPT ${WINEASE_DEPLOY_SCRIPT})
