# ============================================================================
#  WinEase —— Windows 易用性增强工具集
#  顶层构建脚本
#
#  参考环境：CMake 4.4.2 / MSVC 2026 (v14.5x) / Qt 6.8.4 / Windows 11 x64
#  构建示例：
#     cmake -S . -B build -G "Visual Studio 18 2026" -DCMAKE_PREFIX_PATH="C:/Qt/6.8.4/msvc2022_64"
#     cmake --build build --config Release
# ============================================================================
cmake_minimum_required(VERSION 3.28)

project(WinEase
    VERSION 0.1.0
    DESCRIPTION "WinEase - Windows 易用性增强工具集"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# 语言标准：C++20
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT MSVC)
    message(FATAL_ERROR "WinEase 目前仅支持 MSVC（Windows 11 x64）工具链构建。")
endif ()

if (MSVC)
    add_compile_options(
        /utf-8              # 源码与执行字符集统一为 UTF-8，保证中文字符串/注释正确
        /W4                 # 高警告等级
        /permissive-        # 严格标准一致性
        /Zc:__cplusplus     # 让 __cplusplus 返回真实标准版本
        /Zc:preprocessor    # 启用符合标准的新预处理器
        /MP                 # 多进程编译
        /EHsc
        /bigobj             # 模板/信号槽较多时避免 .obj 段数溢出
    )
    add_compile_definitions(
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif ()

# ---------------------------------------------------------------------------
# QT_NO_KEYWORDS（P0-6）
#
# 禁用 Qt 的关键字宏 signals / slots / emit，改用 Q_SIGNALS / Q_SLOTS / Q_EMIT。
# 原因：C++/WinRT 的头文件（winrt/*.h）会把 signals / slots 当作普通标识符使用，
# 与 Qt 的宏定义冲突，导致的报错极其晦涩且与报错位置无关。
#
# 这是 P0 阶段必须一次做到位的工程约定：
#   * 现在改：11 个头文件 + 10 个实现文件，纯机械替换
#   * 等 P1 的 14 个插件写完再改：每个插件都要重来一遍
#
# 全员统一使用 Q_SIGNALS / Q_SLOTS / Q_EMIT，不再出现隐藏的包含顺序依赖。
# ---------------------------------------------------------------------------
add_compile_definitions(QT_NO_KEYWORDS)

# ---------------------------------------------------------------------------
# 统一输出目录：所有产物落在 <build>/bin，插件额外落在 <build>/bin/plugins
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach (_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach ()

# 主程序启动时按此目录扫描功能插件
set(WINEASE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/plugins")
# 预先创建空目录：插件尚未实现时主程序扫描不到目录会打警告日志
file(MAKE_DIRECTORY "${WINEASE_PLUGIN_OUTPUT_DIR}")

# ---------------------------------------------------------------------------
# 依赖：仅 Qt 官方模块（Core/Gui/Widgets/Svg/Network）+ Windows SDK
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg Network)
qt_standard_project_setup(REQUIRES 6.8)

# ---------------------------------------------------------------------------
# 可选依赖：QtMultimedia（+ MultimediaWidgets）
#   仅"视频内嵌播放"需要；未安装时相应功能回退为缩略图方案，编译不受影响。
#   注意：必须同时请求 MultimediaWidgets，否则 Qt6::MultimediaWidgets 目标不会生成。
#   检测结果通过 WINEASE_HAS_MULTIMEDIA 传给代码（见 ENV-SETUP.md）。
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 QUIET COMPONENTS Multimedia MultimediaWidgets)
if (TARGET Qt6::Multimedia)
    set(WINEASE_HAS_MULTIMEDIA ON)
else ()
    set(WINEASE_HAS_MULTIMEDIA OFF)
endif ()

# ---------------------------------------------------------------------------
# 构建选项
# ---------------------------------------------------------------------------
option(WINEASE_BUILD_SMOKE_TESTS "构建平台能力层自检程序（win32_smoke）" ON)

# ---------------------------------------------------------------------------
# ⚠ Qt 调试版可用性检查（很重要，别删）
#
# MSVC 下 Debug 与 Release 的 **STL 类型布局与 CRT 堆不兼容**。
# 若 Qt 是 release-only 安装（只有 Qt6Core.dll，没有 Qt6Cored.dll），
# 用它去支撑 Debug 构建的程序时，凡是"Qt 返回/接收 STL 类型"的接口
# （std::wstring / std::string / std::vector / std::function 等）都会内存损坏。
#
# 症状极其隐蔽：返回值看似正常、数值巧合正确、直到退出或下一次分配才崩溃
# （本项目实测：QString::toStdWString() 把 12 个 UTF-16 单元变成 15 个，
#   转换回 QString 得到空串，进程退出时访问违例）。
#
# 结论：与 release-only 的 Qt 搭配时，必须使用 Release 或 RelWithDebInfo。
# ---------------------------------------------------------------------------
get_filename_component(WINEASE_QT_ROOT "${Qt6Core_DIR}/../../.." ABSOLUTE)
if (EXISTS "${WINEASE_QT_ROOT}/bin/Qt6Cored.dll")
    set(WINEASE_QT_HAS_DEBUG_LIBS ON)
else ()
    set(WINEASE_QT_HAS_DEBUG_LIBS OFF)
endif ()

# 插件构建辅助函数 winease_add_plugin()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WinEasePlugin)

add_subdirectory(src)
add_subdirectory(plugins)
add_subdirectory(tests)

message(STATUS "-----------------------------------------------------------")
message(STATUS " WinEase ${PROJECT_VERSION}")
message(STATUS " Qt          : ${Qt6_VERSION}  (${WINEASE_QT_ROOT})")
message(STATUS " Qt 调试库    : ${WINEASE_QT_HAS_DEBUG_LIBS}")
message(STATUS " QtMultimedia : ${WINEASE_HAS_MULTIMEDIA}")
message(STATUS " 输出目录     : ${CMAKE_BINARY_DIR}/bin")
message(STATUS " 插件输出目录 : ${WINEASE_PLUGIN_OUTPUT_DIR}")
message(STATUS " 自检程序     : ${WINEASE_BUILD_SMOKE_TESTS}")
message(STATUS "-----------------------------------------------------------")

if (NOT WINEASE_QT_HAS_DEBUG_LIBS)
    message(WARNING
        "Qt 未提供调试库（未找到 Qt6Cored.dll），属 release-only 安装。\n"
        "  请使用 Release 或 RelWithDebInfo 构建：\n"
        "      cmake --build build --config RelWithDebInfo --parallel\n"
        "  用 Debug 构建与此 Qt 搭配时，跨模块传递 STL 类型（std::wstring 等）\n"
        "  会因布局与堆不兼容而内存损坏，症状隐蔽（数值异常 / 退出时崩溃）。")
endif ()
