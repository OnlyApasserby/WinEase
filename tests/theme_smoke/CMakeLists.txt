# ============================================================================
#  theme_smoke —— 强制深色主题（黑底白字）验收测试
#
#  说明：ThemeManager 位于主程序内（src/app/ui/），不是独立库，
#        因此这里直接把它的源文件编入测试目标 —— 好处是**测的是真实代码路径**，
#        而不是在测试里抄一份主题实现（抄一份的话，改坏了实现测试也照样通过）。
# ============================================================================

set(WINEASE_RESOURCE_DIR "${CMAKE_SOURCE_DIR}/src/resources")

qt_add_executable(theme_smoke
    main.cpp

    "${CMAKE_SOURCE_DIR}/src/app/ui/ThemeManager.h"
    "${CMAKE_SOURCE_DIR}/src/app/ui/ThemeManager.cpp"
)

target_include_directories(theme_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
)

target_link_libraries(theme_smoke PRIVATE
    WinEaseWin32
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# 资源与主程序保持**同一前缀与同一份文件**：
# 这样"检查样式表/图标的配色"检查的就是主程序真正加载的那几个文件
qt_add_resources(theme_smoke "theme_assets"
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

set_target_properties(theme_smoke PROPERTIES
    OUTPUT_NAME "theme_smoke"
    WIN32_EXECUTABLE FALSE
)
