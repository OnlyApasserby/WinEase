# ============================================================================
#  WinEasePlugin.cmake —— 功能插件构建辅助模块
#
#  用法（在 plugins/<name>/CMakeLists.txt 中）：
#      winease_add_plugin(we_window_toolbox
#          CLASS_NAME WindowToolboxPlugin
#          SOURCES
#              window_toolbox_plugin.h
#              window_toolbox_plugin.cpp
#              window_toolbox_plugin.json
#      )
#
#  说明：
#    * CLASS_NAME 必须与插件类名一致（Qt 的 Q_PLUGIN_METADATA 依赖它）
#    * 产物输出到 <build>/bin/plugins，主程序启动时自动扫描该目录
# ============================================================================

function(winease_add_plugin plugin_target)
    cmake_parse_arguments(ARG "" "CLASS_NAME" "SOURCES" ${ARGN})

    if (NOT ARG_CLASS_NAME)
        message(FATAL_ERROR "winease_add_plugin(${plugin_target}) 缺少 CLASS_NAME 参数。")
    endif ()
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "winease_add_plugin(${plugin_target}) 缺少 SOURCES 参数。")
    endif ()

    # 生成动态库插件（.dll）
    # 注意：qt_add_plugin 不接受 "SOURCES" 关键字，源文件列表需直接展开
    qt_add_plugin(${plugin_target}
        SHARED
        CLASS_NAME ${ARG_CLASS_NAME}
        ${ARG_SOURCES}
    )

    target_include_directories(${plugin_target} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/plugins/common"   # 插件间共享的头文件（如 WindowFeatureState.h）
    )

    # 插件依赖：插件契约 SDK + Windows 平台能力层 + Qt 官方模块
    target_link_libraries(${plugin_target} PRIVATE
        WinEaseSdk
        WinEaseWin32
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )

    # 插件需要的可选模块（按需在 winease_add_plugin 之后自行 target_link_libraries 追加）
    if (WINEASE_HAS_MULTIMEDIA)
        target_compile_definitions(${plugin_target} PRIVATE WINEASE_HAS_MULTIMEDIA=1)
    endif ()

    # Windows 下插件可能用到 Win32 API
    if (WIN32)
        target_link_libraries(${plugin_target} PRIVATE user32 advapi32 shell32 ole32)
    endif ()

    target_compile_definitions(${plugin_target} PRIVATE
        WINEASE_PLUGIN_NAME="${plugin_target}"
    )

    set_target_properties(${plugin_target} PROPERTIES
        OUTPUT_NAME "${plugin_target}"
        PREFIX ""                       # Windows 下不生成前缀
        LIBRARY_OUTPUT_DIRECTORY "${WINEASE_PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${WINEASE_PLUGIN_OUTPUT_DIR}"
    )

    # 多配置生成器（VS）：为每个配置显式指定目录，避免生成 Debug/Release 子目录
    foreach (_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${plugin_target} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${WINEASE_PLUGIN_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${WINEASE_PLUGIN_OUTPUT_DIR}"
        )
    endforeach ()

    # 登记到全局属性：需要"先构建全部插件"的目标（例如端到端自检 feature_smoke
    # 要加载 build/bin/plugins 下的真实 DLL）可以用它一次性挂上依赖，
    # 以后新增批次不会因为忘记补 add_dependencies 而测到陈旧的插件。
    set_property(GLOBAL APPEND PROPERTY WINEASE_PLUGIN_TARGETS "${plugin_target}")

    message(STATUS "[WinEase] 注册插件目标: ${plugin_target} (类名 ${ARG_CLASS_NAME})")
endfunction()
