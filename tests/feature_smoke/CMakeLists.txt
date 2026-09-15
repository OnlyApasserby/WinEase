# ============================================================================
#  feature_smoke —— P1 功能插件端到端自检
#
#  与 plugin_smoke 的关键区别：
#      plugin_smoke 把测试插件源码编进目标里，验的是插件契约与异常边界；
#      本目标**不编入任何插件源码**，而是在运行时用 QPluginLoader 加载
#      build/bin/plugins 下的真实插件 DLL —— 测的是"用户装上的那个功能"。
#
#  因此必须保证插件先于本目标构建（依赖关系在文件末尾用 GLOBAL 属性统一挂上，
#  以后新增批次（P1-B/C/D…）只要用 winease_add_plugin 注册就会自动纳入）。
#
#  运行方式：
#      .\build\bin\feature_smoke.exe        # 退出码 0 表示全部通过
# ============================================================================

qt_add_executable(feature_smoke
    main.cpp
    test_support.h
    test_support.cpp
    window_group.h
    window_group.cpp
    tool_group.h
    tool_group.cpp
    overlay_group.h
    overlay_group.cpp
    system_group.h
    system_group.cpp
    file_group.h
    file_group.cpp
    input_group.h
    input_group.cpp
    display_group.h
    display_group.cpp
    power_group.h
    power_group.cpp
    device_group.h
    device_group.cpp
    dev_group.h
    dev_group.cpp
    p3_group.h
    p3_group.cpp
    convert_group.h
    convert_group.cpp
    hud_group.h
    hud_group.cpp
    mixer_group.h
    mixer_group.cpp
    player_group.h
    player_group.cpp
    batch_move_group.h
    batch_move_group.cpp
    lan_transfer_group.h
    lan_transfer_group.cpp

    # 插件间的共享实现：自检用的是**同一份源码**（不是"照着再写一遍"），
    # 这样"文本转换是否正确"这类断言才有意义（见 plugins/common/TextTools.h 的说明）
    "${CMAKE_SOURCE_DIR}/plugins/common/TextTools.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/TextTools.cpp"

    # P2-A 文件组：三个引擎也走"同一份源码"——
    #   RenameEngine   → 预览与落盘必须是同一份计算结果（自检直接断言这条）
    #   DuplicateFinder→ 四级流水线的逐级断言（抽样字节数必须远小于全量）
    #   FileShredder   → 覆写实现本身的断言（回读校验后字节真的变成了 0x00）
    # 只编引擎、不编插件界面：面板仍由 build/bin/plugins 下的真实 DLL 提供
    # （FilePreview 不编进来：它带 WinRT/Shell 依赖，而预览的正确性由面板端到端断言覆盖）
    # P2-D 系统与媒体组：调度与电源的决策逻辑也走"同一份源码"——
    #   时间决策（跨零点 / 休眠补触发）与"倒计时可取消"的语义必须与插件完全一致，
    #   否则自检验的是另一套实现，等于没验
    "${CMAKE_SOURCE_DIR}/plugins/common/PowerControl.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/PowerControl.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/common/ScheduleEngine.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/ScheduleEngine.cpp"

    # P2-E 安全与隐私组：ConsentStore 的解析与"正在使用"判定同样走同一份源码
    #   （判定写错 = 一堆假警报，必须与插件行为完全一致）
    "${CMAKE_SOURCE_DIR}/plugins/common/DeviceUsageMonitor.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/DeviceUsageMonitor.cpp"

    # P2-F 开发运维组：hosts 的解析/校验/渲染同样走同一份源码
    #   （"往返无损"与"校验拦截"必须与插件完全一致，否则自检验的是另一套实现）
    "${CMAKE_SOURCE_DIR}/plugins/common/HostsDocument.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/HostsDocument.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/common/EnvTools.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/EnvTools.cpp"

    # P3-02 文件锁定解除：占用者保护的判定同样走同一份源码 ——
    #   "谁绝对不许杀"是安全规则，自检必须断言用户真正跑的那一套
    "${CMAKE_SOURCE_DIR}/plugins/common/UnlockPolicy.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/UnlockPolicy.cpp"

    # P3-03 批量格式转换：两套引擎都走"同一份源码" ——
    #   图片侧 plan()/apply() 是"预览与落盘同一份计算"的**判定点**，文本侧
    #   编码探测/转换是"绝不用替换字符糊过去"的判定点；自检必须断言用户真正跑的那一套。
    #   只编引擎、不编面板：面板仍由 build/bin/plugins 下的真实 DLL 提供。
    "${CMAKE_SOURCE_DIR}/plugins/common/ImageConvertEngine.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/ImageConvertEngine.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/common/TextEncodingTools.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/TextEncodingTools.cpp"

    # P3-07 硬件监控悬浮窗：显示模型（逐项开关 / 格式化 / "读不到必须写出原因"）
    # 同样走"同一份源码"——这条纪律本身就是本功能最硬的验收点，
    # 自检若照抄一份实现，验的就只是"抄得对不对"
    "${CMAKE_SOURCE_DIR}/plugins/common/HudMetrics.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/HudMetrics.cpp"

    # 新增：批量移动（正则）与局域网传输的引擎
    #   BatchMoveEngine  → 计划/预演/撤销都长在这里，"预演不动盘"必须与落盘共用同一份 Plan
    #   LanTransferEngine→ HTTP 解析与**目录穿越拦截**长在这里，
    #                      自检用构造数据反复捶（这类安全边界不能只靠真手机试）
    "${CMAKE_SOURCE_DIR}/plugins/common/BatchMoveEngine.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/BatchMoveEngine.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/common/LanTransferEngine.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/LanTransferEngine.cpp"

    # P3-09 音量混合器：聚合模型（一个进程多份会话合一行 / 代表音量 / 过期隐藏）
    # 同样走"同一份源码" —— "按应用调音量必须覆盖该进程全部会话"这条判断
    # 完全长在 buildSnapshot() 里，自检若照抄一份，验的就只是抄得对不对
    "${CMAKE_SOURCE_DIR}/plugins/common/MixerModel.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/MixerModel.cpp"

    # P3-11 媒体控制面板：显示模型（会话行 / 播放状态与进度文案 / "读不到必须写原因" /
    # 进度按采样时刻外推）同样走"同一份源码" —— "时长未知时不许显示 0%"这条纪律
    # 完全长在 PlayerModel 里，自检若照抄一份，验的就只是抄得对不对
    "${CMAKE_SOURCE_DIR}/plugins/common/PlayerModel.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/PlayerModel.cpp"

    "${CMAKE_SOURCE_DIR}/plugins/common/TaskCancel.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/RenameEngine.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/RenameEngine.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/common/DuplicateFinder.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/DuplicateFinder.cpp"
    "${CMAKE_SOURCE_DIR}/plugins/common/FileShredder.h"
    "${CMAKE_SOURCE_DIR}/plugins/common/FileShredder.cpp"

    # 需要真实的 PluginManager / SettingsManager（含 P0-5 崩溃隔离边界）：
    # 与主程序走同一条代码路径，而不是在测试里另抄一份
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
    # 输入组（P2-05 滚轮增强）需要**真的**钩子服务：
    # 断言的对象是"任务栏上那个滚轮事件有没有被吞掉 + 系统音量有没有真的变"，
    # 用桩就只能测出手写的返回值。宿主服务因此改用主程序那一份实现。
    "${CMAKE_SOURCE_DIR}/src/app/core/HookServiceImpl.h"
    "${CMAKE_SOURCE_DIR}/src/app/core/HookServiceImpl.cpp"
)

target_include_directories(feature_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/app"
    "${CMAKE_SOURCE_DIR}/plugins/common"   # TextTools.h / ColorFormats.h 等插件间共享头
    # P3-07：面板的**只读属性名**与面板尺寸常量需要与插件保持同一份定义
    # （插件是独立 DLL、不导出 C++ 符号，自检只能走运行期元对象读它，
    #   所以这里只放 include 路径以便对照常量与对象名，不链接任何插件符号）
    "${CMAKE_SOURCE_DIR}/plugins/monitor_hardware_hud"
)

target_link_libraries(feature_smoke PRIVATE
    WinEaseSdk        # IFeaturePlugin / PluginServices
    WinEaseWin32      # WindowTarget / WindowUtils 等平台原语
    Qt6::Core
    Qt6::Gui          # QIcon / QKeySequence / QScreen
    Qt6::Widgets      # QApplication（插件元信息探针与设置面板都需要）
    Qt6::Network      # 局域网传输组要用 QTcpSocket 打回环请求（127.0.0.1，不碰真实局域网）
)

if (WIN32)
    target_link_libraries(feature_smoke PRIVATE
        user32        # SetCursorPos / SetWindowPos / GetWindowLongPtrW
        advapi32
        shell32
        ws2_32        # 工具组自己开回环监听套接字（端口占用用例的真实占用源）
        ole32         # P3-09 音频会话探针自己 CoInitializeEx + CoCreateInstance 造真实会话
                      # （WinEaseWin32 把 ole32 挂在 PRIVATE，这里必须自己再挂一次）
    )
endif ()

# 依赖全部已注册的插件目标：否则单独构建 feature_smoke 时 plugins 目录可能是空的
get_property(_winease_plugin_targets GLOBAL PROPERTY WINEASE_PLUGIN_TARGETS)
if (_winease_plugin_targets)
    add_dependencies(feature_smoke ${_winease_plugin_targets})
endif ()

set_target_properties(feature_smoke PROPERTIES
    OUTPUT_NAME "feature_smoke"
    WIN32_EXECUTABLE FALSE   # 控制台子系统：自检输出要能直接看到
)
