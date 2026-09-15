# ============================================================================
#  src/bridge —— C++/CLI 桥接层（P3-07 硬件监控）
#
#  背景：硬件传感器读数（CPU 封装温度 / 主板 SuperIO 温度 / 风扇转速 …）
#  在原生 C++ 侧没有等价物，而 refrences/LiteMonitor 已经用
#  LibreHardwareMonitorLib 把这条路走通。本项目不重复造轮子，改为
#  **C++/CLI 混合模式程序集**做桥：
#
#      原生 C++（WinEaseWin32 / 插件）──LoadLibrary + 纯 C ABI──►
#      WinEaseLiteMonitorBridge.dll（/clr:netcore）──托管调用──►
#      LibreHardwareMonitorLib.dll（与参考工程同一个 NuGet 包）
#
#  ⚠ 本目录是整个工程里唯一允许 /clr 的地方，且只因为"要用 .NET 生态里
#     已有的硬件库"这一个理由。任何新功能都不要顺手往这里塞。
#
#  构建前提（缺一项就自动跳过，插件侧会如实报"桥接不可用"）：
#      1. .NET 8 SDK（dotnet 在 PATH 里）—— 托管依赖靠它还原
#      2. MSVC 的 C++/CLI 支持（/clr:netcore）与 ijwhost.lib
#      3. 运行时：机器上有 .NET 8 运行时（Windows 11 常见；没有则插件如实报不可用）
# ============================================================================

set(WINEASE_BRIDGE_TARGET "WinEaseLiteMonitorBridge")
set(WINEASE_BRIDGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")

option(WINEASE_BUILD_LITEMONITOR_BRIDGE
       "构建 P3-07 的 C++/CLI 硬件监控桥接层（需要 .NET 8 SDK）" ON)

find_program(WINEASE_DOTNET_EXE NAMES dotnet)

if (NOT WINEASE_BUILD_LITEMONITOR_BRIDGE)
    message(STATUS "[WinEase] C++/CLI 桥接层：已按开关关闭（WINEASE_BUILD_LITEMONITOR_BRIDGE=OFF）")
    set(WINEASE_HAS_LITEMONITOR_BRIDGE OFF)
    return()
endif ()

if (NOT WINEASE_DOTNET_EXE)
    message(STATUS "[WinEase] C++/CLI 桥接层：未找到 dotnet，跳过（P3-07 将走原生降级路径）")
    set(WINEASE_HAS_LITEMONITOR_BRIDGE OFF)
    return()
endif ()

# ---------------------------------------------------------------------------
#  第 1 步：托管依赖（LibreHardwareMonitorLib + 它的一串依赖）
#
#  产物平铺到 build/bin：CoreCLR 解析托管程序集时按"应用基目录"探测，
#  与桥接 DLL、主程序同目录是最不容易出错的部署形态。
# ---------------------------------------------------------------------------
set(WINEASE_BRIDGE_MANAGED_DIR "${CMAKE_BINARY_DIR}/_bridge_managed")
set(WINEASE_BRIDGE_MANAGED_STAMP "${WINEASE_BRIDGE_MANAGED_DIR}/publish.stamp")

add_custom_command(
    OUTPUT "${WINEASE_BRIDGE_MANAGED_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${WINEASE_BRIDGE_MANAGED_DIR}"
    COMMAND "${WINEASE_DOTNET_EXE}" publish
            "${CMAKE_CURRENT_SOURCE_DIR}/managed/WinEase.LiteMonitorDeps.csproj"
            -c Release -o "${WINEASE_BRIDGE_MANAGED_DIR}" --nologo
    COMMAND ${CMAKE_COMMAND} -E touch "${WINEASE_BRIDGE_MANAGED_STAMP}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/managed/WinEase.LiteMonitorDeps.csproj"
            "${CMAKE_CURRENT_SOURCE_DIR}/managed/Placeholder.cs"
    COMMENT "[WinEase] 还原并发布 LibreHardwareMonitorLib 及其依赖"
    VERBATIM
)

add_custom_target(we_litemonitor_managed DEPENDS "${WINEASE_BRIDGE_MANAGED_STAMP}")

add_custom_command(TARGET we_litemonitor_managed POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${WINEASE_BRIDGE_MANAGED_DIR}" "${WINEASE_BRIDGE_OUTPUT_DIR}"
    COMMENT "[WinEase] 部署托管依赖到 ${WINEASE_BRIDGE_OUTPUT_DIR}")

# ---------------------------------------------------------------------------
#  第 2 步：ijwhost.dll
#
#  原生进程 LoadLibrary 一个 /clr:netcore 的 IJW 程序集时，由 ijwhost.dll
#  负责把 CoreCLR 拉起来；它必须和 IJW 程序集同目录。
#  它不在共享框架里，只在 Microsoft.NETCore.App.Host 包里 —— 按版本号取最新的一个。
# ---------------------------------------------------------------------------
file(GLOB WINEASE_IJWHOST_CANDIDATES
     "C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/*/runtimes/win-x64/native/ijwhost.dll")
if (WINEASE_IJWHOST_CANDIDATES)
    list(SORT WINEASE_IJWHOST_CANDIDATES ORDER DESCENDING)
    list(GET WINEASE_IJWHOST_CANDIDATES 0 WINEASE_IJWHOST_DLL)
else ()
    set(WINEASE_IJWHOST_DLL "")
endif ()

if (NOT WINEASE_IJWHOST_DLL)
    message(STATUS "[WinEase] C++/CLI 桥接层：未找到 ijwhost.dll，跳过（P3-07 走原生降级路径）")
    set(WINEASE_HAS_LITEMONITOR_BRIDGE OFF)
    return()
endif ()

# ijwhost.lib 与 .dll 同目录，链接时需要它
get_filename_component(WINEASE_IJWHOST_DIR "${WINEASE_IJWHOST_DLL}" DIRECTORY)

# ---------------------------------------------------------------------------
#  .NET 8 引用程序集目录
#
#  ⚠ C++ 工程默认的 TargetFrameworkVersion 是 v4.0，MSBuild 的
#    ResolveAssemblyReferences 会去要"v4.0 的引用程序集"；本机没装 .NET Framework
#    4.x 目标包（Windows 11 + VS 2026 常见）就会报 MSB3644 并中断。
#    桥接层实际引用的是 net8，把 FrameworkPathOverride 指到 net8 的引用包即可，
#    解析结果由我们显式给的 /FU 决定，RAR 只是"别拦我"而已。
# ---------------------------------------------------------------------------
file(GLOB WINEASE_NET_REF_CANDIDATES
     "C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net8.0")
if (WINEASE_NET_REF_CANDIDATES)
    list(SORT WINEASE_NET_REF_CANDIDATES ORDER DESCENDING)
    list(GET WINEASE_NET_REF_CANDIDATES 0 WINEASE_NET_REF_DIR)
else ()
    set(WINEASE_NET_REF_DIR "")
endif ()

# ---------------------------------------------------------------------------
#  第 3 步：C++/CLI 桥接程序集
#
#  ⚠ 与全局编译选项的冲突都在这里显式改写（/clr 系列的兼容性很脆）：
#      * /clr:netcore 只接受 /EHa（全局是 /EHsc）→ 显式给 /EHa（后出现者生效）
#      * /clr 不接受 /permissive- → 用 /permissive 复位
#      * /clr 不接受 C++20 → 本目标降到 C++17
#      * /clr 不接受 /RTC1（Debug 默认）→ 显式关掉
#      * 需要 /utf-8：源文件是 UTF-8，中文字符串字面量必须按 UTF-8 解释
# ---------------------------------------------------------------------------
add_library(${WINEASE_BRIDGE_TARGET} SHARED
    WinEaseLiteMonitorBridge.cpp
)

# ⚠ 关掉 VC 目标对 Microsoft.NET.Sdk 的导入（ImportNETCoreSdkFromVCTargets=false）：
#   CMake 只要看到 /clr:netcore 就写 `<CLRSupport>NetCore</CLRSupport>`，
#   Microsoft.CppCommon.targets 随即 `Import Sdk="Microsoft.NET.Sdk"`。
#   而 VS 自带的 MSBuild 在纯原生工程里解析不了这个 SDK（MSB4236，SDK 解析器
#   不在 VS 的 MSBuild 里，它在 dotnet CLI 那边）。我们的桥接层本来就不需要那套
#   导入（runtimeconfig.json 自己生成，托管依赖也是自己发布），关掉即可。
set_target_properties(${WINEASE_BRIDGE_TARGET} PROPERTIES
    VS_GLOBAL_ImportNETCoreSdkFromVCTargets "false"
    VS_GLOBAL_FrameworkPathOverride "${WINEASE_NET_REF_DIR}")

# ⚠ 这里**刻意不设** COMMON_LANGUAGE_RUNTIME 属性：
#   CMake 一旦看到它就往 vcxproj 里注入 `Sdk="Microsoft.NET.Sdk"`，
#   而 VS 自带的 MSBuild 在纯原生工程里解析不了这个 SDK（MSB4236）。
#   我们本来就是"原生工程 + 手写 /clr:netcore"，runtimeconfig.json 也是
#   自己生成，用不上那套 SDK 导入 —— 手动给 /clr:netcore 即可（CMake 会警告，无害）。
set_target_properties(${WINEASE_BRIDGE_TARGET} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "${WINEASE_BRIDGE_TARGET}"
    RUNTIME_OUTPUT_DIRECTORY "${WINEASE_BRIDGE_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${WINEASE_BRIDGE_OUTPUT_DIR}"
)
foreach (_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(${WINEASE_BRIDGE_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${WINEASE_BRIDGE_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${WINEASE_BRIDGE_OUTPUT_DIR}")
endforeach ()

target_compile_options(${WINEASE_BRIDGE_TARGET} PRIVATE
    /clr:netcore
    /EHa
    /permissive
    /utf-8
    /bigobj
    /GR            # /clr 必须有 RTTI
    /wd4679        # 托管接口里的静态抽象成员（IFloatingPointIeee754 等）导入不了，纯噪音
)
# 关掉与 /clr 互斥的调试运行期检查（Debug 配置下 CMake 默认加 /RTC1）
set(WINEASE_BRIDGE_CMAKE_CXX_FLAGS_DEBUG_SAVED "${CMAKE_CXX_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" WINEASE_BRIDGE_DEBUG_FLAGS "${CMAKE_CXX_FLAGS_DEBUG}")
set_source_files_properties(WinEaseLiteMonitorBridge.cpp PROPERTIES
    COMPILE_FLAGS "${WINEASE_BRIDGE_DEBUG_FLAGS}")

target_compile_definitions(${WINEASE_BRIDGE_TARGET} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# 托管引用：/AI 指定搜索目录，/FU 强制 #using（等价于 C# 的项目引用）
#
# ⚠ 路径必须**紧贴**在开关后面（/AI<目录> 而不是 /AI <目录>）：
#   CMake 的 VS 选项表会把 "/AI" 识别成一个开关、把下一个列表项当它的值，
#   结果 AdditionalUsingDirectories 被写成空、路径反倒漏进 AdditionalOptions，
#   cl 于是把目录当源文件去编译（C1083）。紧贴写法 CMake 不认，原样透传给 cl。
target_compile_options(${WINEASE_BRIDGE_TARGET} PRIVATE
    "/AI${WINEASE_NET_REF_DIR}"
    "/AI${WINEASE_BRIDGE_MANAGED_DIR}"
    "/FU${WINEASE_BRIDGE_MANAGED_DIR}/LibreHardwareMonitorLib.dll"
    # net8 把 BCL 拆成了多个程序集，只用得到哪几个就显式 #using 哪几个
    # （List<> 在 System.Collections 里，缺了它报的是莫名其妙的 "语法错误: 标识符 List"）
    "/FU${WINEASE_NET_REF_DIR}/System.Runtime.dll"
    "/FU${WINEASE_NET_REF_DIR}/System.Collections.dll"
    "/FU${WINEASE_NET_REF_DIR}/System.Runtime.InteropServices.dll"
)

target_link_options(${WINEASE_BRIDGE_TARGET} PRIVATE
    "/LIBPATH:${WINEASE_IJWHOST_DIR}"
)

add_dependencies(${WINEASE_BRIDGE_TARGET} we_litemonitor_managed)

# ijwhost.dll 与 runtimeconfig.json 必须与桥接 DLL 同目录
add_custom_command(TARGET ${WINEASE_BRIDGE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINEASE_IJWHOST_DLL}" "${WINEASE_BRIDGE_OUTPUT_DIR}/ijwhost.dll"
    COMMENT "[WinEase] 部署 ijwhost.dll")

# runtimeconfig.json：ijwhost 靠它决定加载哪个共享框架（net8.0）
file(WRITE "${WINEASE_BRIDGE_OUTPUT_DIR}/${WINEASE_BRIDGE_TARGET}.runtimeconfig.json"
"{
  \"runtimeOptions\": {
    \"tfm\": \"net8.0\",
    \"framework\": {
      \"name\": \"Microsoft.NETCore.App\",
      \"version\": \"8.0.0\"
    }
  }
}
")

set(WINEASE_HAS_LITEMONITOR_BRIDGE ON)
message(STATUS "[WinEase] C++/CLI 桥接层：启用（dotnet=${WINEASE_DOTNET_EXE}）")
