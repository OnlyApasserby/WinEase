// ============================================================================
//  feature_smoke —— P1 功能插件端到端自检
//
//  与其它自检程序的分工：
//      win32_smoke   验平台原语（无状态函数）
//      hook_smoke    验全局钩子服务
//      overlay_smoke 验悬浮层
//      theme_smoke   验主题
//      elevation_smoke 验提权助手
//      plugin_smoke  验插件契约与崩溃隔离（把插件源码编进测试）
//      feature_smoke 验**用户真正装上的那些插件 DLL 到底有没有把活干对**：
//                    用 QPluginLoader 加载 build/bin/plugins 下的真实插件，
//                    经 PluginManager::dispatchHotkey 触发功能，
//                    再回到 Win32 侧读真实窗口状态做断言。
//
//  目标窗口为什么放在独立进程：
//      平台层 isManageableWindow() 会排除"本进程的窗口"（WinEase 自己的界面、
//      悬浮层都不该被当作操作目标）。若测试窗口与插件同进程，正确的产品行为
//      反而会让测试做不下去，而且测到的也不是真实场景（真实场景里被操作的
//      窗口一定是别的进程）。因此目标窗口跑在本 exe 的 --probe-window 子模式里。
//
//  ⚠ 本自检会短暂干扰真实桌面（有意为之，否则测不出东西）：
//      * 会移动真实光标（"跟随鼠标"模式必须真的移光标），结束时还原
//      * 会让探针窗口短暂置顶 / 半透明 / 置底，并可能抢一次前台
//      * 会在临时目录里跑配置，不污染 %APPDATA%/WinEase
//
//  运行方式：
//      .\build\bin\feature_smoke.exe        # 退出码 0 表示全部通过
// ============================================================================

#include "convert_group.h"
#include "dev_group.h"
#include "win32/MediaSessions.h"
#include "device_group.h"
#include "display_group.h"
#include "file_group.h"
#include "batch_move_group.h"
#include "hud_group.h"
#include "input_group.h"
#include "lan_transfer_group.h"
#include "mixer_group.h"
#include "overlay_group.h"
#include "p3_group.h"
#include "player_group.h"
#include "power_group.h"
#include "system_group.h"
#include "test_support.h"
#include "tool_group.h"
#include "window_group.h"

#include "app/core/HookServiceImpl.h"
#include "app/core/PluginManager.h"
#include "app/core/SettingsManager.h"
#include "win32/ComApartment.h"

// 电源/调度的任务模型与插件共用同一份源码（自检的种子任务要用同一个结构体）
#include "ScheduleEngine.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QStringList>
#include <QTime>

#include <cstdio>

int main(int argc, char *argv[])
{
    // ⚠ 输出不缓冲：自检里一旦有人 abort/崩溃，管道里那 4KB 缓冲会连同"崩在哪一行"
    //   一起吞掉，留下的现场正好断在最需要看的地方（本次排查 P3-03 就吃了这个亏）
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 用 QApplication：插件元信息探针会走到 Category::icon()，
    // 设置面板（QWidget）也需要 Widgets 应用实例
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("feature_smoke"));

    const QStringList args = QCoreApplication::arguments();

    // ---- 子进程模式：只建窗口跑事件循环，不加载插件、不碰配置 ----
    const int probeIndex = args.indexOf(QStringLiteral("--probe-window"));
    if (probeIndex >= 0) {
        if (probeIndex + 1 >= args.size()) {
            std::printf("[失败] --probe-window 缺少哨兵文件参数\n");
            return 2;
        }
        return FeatureSmoke::ProbeWindow::runChild(args.at(probeIndex + 1));
    }

    // ---- 子进程模式：独占锁定一个文件并一直持有（P3-02 的"真实占用源"）----
    //      为什么不用自检进程自己当占用者：插件**拒绝结束 WinEase 自身**（这条规则本身
    //      也要验），而"结束之后能不能删掉"必须由一个真的可以被结束的进程来演。
    const int lockIndex = args.indexOf(QStringLiteral("--lock-file"));
    if (lockIndex >= 0) {
        if (lockIndex + 2 >= args.size()) {
            std::printf("[失败] --lock-file 需要 <文件路径> <就绪文件> 两个参数\n");
            return 2;
        }
        return FeatureSmoke::FileLockProbe::runChild(args.at(lockIndex + 1),
                                                     args.at(lockIndex + 2));
    }

    // ---- 子进程模式：建一份**真实**音频会话并持有（P3-09 的"正在播放的应用"）----
    //      为什么不用自检进程自己当播放器：一个进程只有一份会话，而且自检进程
    //      "看起来就不像应用"。这里让本 exe 的**子进程**去当播放器 —— 起两个就是
    //      两个进程、两份会话、同一个进程名，正好覆盖"同一应用多份会话"这条路径。
    const int audioIndex = args.indexOf(QStringLiteral("--audio-session"));
    if (audioIndex >= 0) {
        if (audioIndex + 2 >= args.size()) {
            std::printf("[失败] --audio-session 需要 <就绪文件> <音量(0~1)> 两个参数\n");
            return 2;
        }
        bool ok = false;
        const double volume = args.at(audioIndex + 2).toDouble(&ok);
        if (!ok) {
            std::printf("[失败] --audio-session 的音量参数不是数字：%s\n",
                        qPrintable(args.at(audioIndex + 2)));
            return 2;
        }
        return FeatureSmoke::AudioSessionProbe::runChild(args.at(audioIndex + 1),
                                                         static_cast<float>(volume));
    }

    // ---- 子进程模式：自己注册一份**真实**媒体会话并持有（P3-11 的"正在播放的播放器"）----
    //      为什么不用自检进程自己当播放器：那样只能证明"自己写的自己读得回来"，
    //      而面板要处理的是**系统里的**会话列表。这里让本 exe 的子进程扮演播放器，
    //      父进程再走 `GlobalSystemMediaTransportControlsSessionManager` 把它读出来、
    //      真的暂停/跳转、回读真实播放状态。
    const int mediaIndex = args.indexOf(QStringLiteral("--media-session"));
    if (mediaIndex >= 0) {
        if (mediaIndex + 1 >= args.size()) {
            std::printf("[失败] --media-session 缺少就绪文件参数\n");
            return 2;
        }
        return FeatureSmoke::MediaSessionProbe::runChild(args.at(mediaIndex + 1));
    }

    // COM 套间：桌面壁纸（IDesktopWallpaper）等平台能力要求调用线程已初始化 COM。
    // 与主程序一样，这个守卫必须**活到进程结束**（别放进任何函数作用域里）
    const WinEase::Win32::ComApartment comApartment = WinEase::Win32::ComApartment::initialize();

    // ---- 把配置目录重定向到临时目录，避免污染真实 %APPDATA%/WinEase ----
    const QString tempAppData = QDir::temp().filePath(
        QStringLiteral("winease_feature_smoke_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(tempAppData);
    qputenv("APPDATA", tempAppData.toLocal8Bit());
    WinEase::SettingsManager::instance().initialize();

    FeatureSmoke::Reporter reporter(QStringLiteral("功能插件端到端自检"));
    std::printf("=== 功能插件端到端自检 / 窗口组(A) + 工具组(B) + 悬浮层组(C) + 系统组(D) "
                "+ 文件组(E) + 输入组(F) + 显示与媒体组(G) + 系统与媒体组(H) "
                "+ 安全与隐私组(I) + 开发运维组(J) + P3 组(K) + 批量格式转换(P3-03) "
                "+ 硬件监控悬浮窗(P3-07) ===\n");

    // -----------------------------------------------------------------------
    //  宿主服务桩 + 插件加载
    // -----------------------------------------------------------------------
    FeatureSmoke::StubServices services;

    // 钩子服务用**真的**（不是桩）：P2-05 滚轮增强要断言"任务栏上的滚轮事件被吞掉了"
    // 与"系统音量真的变了"，桩里没有钩子就只能测手写的返回值。
    // 起在 loadPlugins 之前：插件在 onEnable 里订阅时服务已在运行。
    WinEase::HookServiceImpl hookService;
    hookService.start();
    services.setHookService(&hookService);
    reporter.check(hookService.isRunning(),
                   QStringLiteral("全局输入钩子服务已启动（输入组的 P2-05 依赖它）"),
                   hookService.lastError());

    // 预设配置：用来验证"配置真的被插件读走了"，而不是把默认值写死在代码里
    services.preset(QStringLiteral("window.opacity"), QStringLiteral("stepPercent"), 25);
    // 工具组：取色格式与坐标（坐标能证明插件用的是**物理**像素）
    services.preset(QStringLiteral("monitor.color_picker"), QStringLiteral("format"),
                    QStringLiteral("hex"));
    services.preset(QStringLiteral("monitor.color_picker"), QStringLiteral("includePosition"), true);
    // 文本类插件：自检里**不碰键盘注入、也不碰用户的前台窗口**
    //   useSelection=false → 只读剪贴板（否则会向当前前台窗口发 Ctrl+C）
    //   pasteBack=false    → 不把结果粘贴回前台窗口（否则会改到用户正在编辑的内容）
    // 这两条正是"自检不得污染真实桌面"（踩坑 #17）在插件配置上的落点。
    for (const QString &pluginId : { QStringLiteral("dev.text_format"), QStringLiteral("dev.encoder") }) {
        services.preset(pluginId, QStringLiteral("useSelection"), false);
        services.preset(pluginId, QStringLiteral("pasteBack"), false);
    }
    services.preset(QStringLiteral("dev.text_format"), QStringLiteral("indent"), 2);
    // 悬浮层组：标尺从像素起、默认点击穿透；聚光灯把亮区半径定死，
    // 这样"同一像素有没有被压暗"的比对才有确定预期
    services.preset(QStringLiteral("display.ruler"), QStringLiteral("unit"), QStringLiteral("px"));
    services.preset(QStringLiteral("display.ruler"), QStringLiteral("interactive"), false);
    services.preset(QStringLiteral("display.focus_highlight"), QStringLiteral("radius"), 110);
    services.preset(QStringLiteral("display.focus_highlight"), QStringLiteral("veilAlpha"), 110);
    services.preset(QStringLiteral("display.focus_highlight"), QStringLiteral("ringOnly"), false);

    // 显示与媒体组：
    //  · 亮度色温（P2-08）：预设色温刻意用 5000（不是代码里的默认 6500）——
    //    "配置真的被读走"才有确定预期：启用后屏幕上就该是 5000K 的暖色
    services.preset(QStringLiteral("display.brightness"), QStringLiteral("kelvin"), 5000);
    services.preset(QStringLiteral("display.brightness"), QStringLiteral("eyeCare"), false);

    // 系统与媒体组：
    //  · 电源面板（P2-06）：倒计时压到 3 秒（默认 60 秒，自检等不起）；
    //    confirm=false 绕开二次确认的模态框（没人点的模态框会把自检卡死）。
    //    ⚠ 电源请求打到**记录型提权桩**上，自检绝不真的关机
    services.preset(QStringLiteral("monitor.power_panel"), QStringLiteral("countdownSeconds"), 3);
    services.preset(QStringLiteral("monitor.power_panel"), QStringLiteral("confirm"), false);
    //  · 定时任务（P2-07）：同样 3 秒倒计时；任务种子的 nextFireAt 落在**过去** ——
    //    这就是"系统休眠三小时后唤醒"的同一种状态（调度器只认绝对触发时刻），
    //    于是"错过的任务会不会被补触发"能在启用瞬间验完，不用等到明天
    services.preset(QStringLiteral("monitor.scheduler"), QStringLiteral("countdownSeconds"), 3);
    {
        const QDateTime seedNow = QDateTime::currentDateTime();
        QList<WinEase::Common::ScheduleTask> seeds;

        WinEase::Common::ScheduleTask remind;
        remind.id = QStringLiteral("seed_remind");
        remind.action = QStringLiteral("remind");
        remind.title = QStringLiteral("自检：该起来活动一下");
        remind.dailyTime = QTime(9, 0);
        remind.repeatDaily = true;
        remind.nextFireAt = seedNow.addSecs(-30);
        seeds.append(remind);

        WinEase::Common::ScheduleTask mute;
        mute.id = QStringLiteral("seed_mute");
        mute.action = QStringLiteral("mute");
        mute.title = QStringLiteral("自检：定时静音");
        mute.repeatDaily = false;
        mute.nextFireAt = seedNow.addSecs(-10);
        seeds.append(mute);

        WinEase::Common::ScheduleTask powerNow;
        powerNow.id = QStringLiteral("seed_power_now");
        powerNow.action = QStringLiteral("shutdown");
        powerNow.title = QStringLiteral("自检：定时关机（倒计时会被取消）");
        powerNow.repeatDaily = false;
        powerNow.nextFireAt = seedNow.addSecs(-2);
        seeds.append(powerNow);

        WinEase::Common::ScheduleTask powerLater;
        powerLater.id = QStringLiteral("seed_power_later");
        powerLater.action = QStringLiteral("shutdown");
        powerLater.title = QStringLiteral("自检：定时关机（走到点）");
        powerLater.repeatDaily = false;
        powerLater.nextFireAt = seedNow.addSecs(4);
        seeds.append(powerLater);

        services.preset(QStringLiteral("monitor.scheduler"), QStringLiteral("tasks"),
                        WinEase::Common::tasksToList(seeds));
    }

    // 安全与隐私组：
    //  · 设备使用提醒（P2-11）：轮询压到 1 秒（默认 3 秒，验收要求"打开相机后 5 秒内提示"）；
    //    **只开摄像头监视** —— 自检夹具造的正是摄像头记录，关掉麦克风可以避免
    //    本机真实的麦克风使用把断言搅浑
    services.preset(QStringLiteral("security.device_alert"), QStringLiteral("pollSeconds"), 1);
    services.preset(QStringLiteral("security.device_alert"), QStringLiteral("webcamEnabled"), true);
    services.preset(QStringLiteral("security.device_alert"), QStringLiteral("microphoneEnabled"),
                    false);
    services.preset(QStringLiteral("security.device_alert"), QStringLiteral("notify"), true);
    services.preset(QStringLiteral("security.device_alert"), QStringLiteral("approved"),
                    QStringList());

    // 系统组：
    //  · 网页搜索：dryRun 只复制链接不弹浏览器（自检才能逐字符校验 URL）
    //  · 壁纸：用自检自己造的 3 张 BMP，intervalMinutes=0（不靠等定时器）
    services.preset(QStringLiteral("launcher.web_search"), QStringLiteral("dryRun"), true);
    services.preset(QStringLiteral("launcher.web_search"), QStringLiteral("useSelection"), false);
    services.preset(QStringLiteral("launcher.web_search"), QStringLiteral("defaultEngine"),
                    QStringLiteral("bing"));
    // 引擎表格式："别名[,别名…]|名称|URL模板"；短别名（gh/bd/gg）是用户实际会打的
    services.preset(QStringLiteral("launcher.web_search"), QStringLiteral("engines"),
                    QStringList{ QStringLiteral("bing,bi|必应|https://www.bing.com/search?q=%1"),
                                 QStringLiteral("bd,baidu|百度|https://www.baidu.com/s?wd=%1"),
                                 QStringLiteral("gg,google|Google|https://www.google.com/search?q=%1"),
                                 QStringLiteral("gh,github|GitHub|https://github.com/search?q=%1"),
                                 QStringLiteral("dx|DuckDuckGo|https://duckduckgo.com/?q=%1") });

    QString fixtureError;
    const QString wallpaperFolder =
        FeatureSmoke::createWallpaperFixture(tempAppData, &fixtureError);
    reporter.check(!wallpaperFolder.isEmpty(),
                   QStringLiteral("自检素材：已生成 3 张测试壁纸（BMP，放在临时目录里）"),
                   fixtureError);
    services.preset(QStringLiteral("personal.wallpaper"), QStringLiteral("folder"), wallpaperFolder);
    services.preset(QStringLiteral("personal.wallpaper"), QStringLiteral("mode"),
                    QStringLiteral("sequential"));
    services.preset(QStringLiteral("personal.wallpaper"), QStringLiteral("intervalMinutes"), 0);
    services.preset(QStringLiteral("personal.wallpaper"), QStringLiteral("dryRun"), false);
    services.preset(QStringLiteral("personal.wallpaper"), QStringLiteral("perMonitor"), false);

    // 文件组：
    //  · 改名 / 查重 / 粉碎都带"执行前二次确认"的模态框 → 自检必须绕开（否则会卡死在没人点的对话框上）。
    //    ⚠ 绕开的只是**弹窗**：粉碎面板里那个"我知道这些文件无法恢复"的勾选照旧必须打勾（那是产品安全阀，
    //    自检特意去验它拦得住）。
    //  · 粉碎：自检用一遍 0x00（够证明"覆写→校验→改名→删除"整条链，不浪费磁盘时间）
    //  · 快速预览：文本上限压到 4 KB（"只读头部、不整文件载入"才有确定预期）；spaceKey=false（本机没有
    //    真实键盘输入源，别去装钩子 —— 自检走快捷键入口）
    for (const QString &pluginId : { QStringLiteral("file.batch_rename"),
                                     QStringLiteral("file.duplicate_finder"),
                                     QStringLiteral("security.file_shredder") }) {
        services.preset(pluginId, QStringLiteral("confirm"), false);
    }
    services.preset(QStringLiteral("security.file_shredder"), QStringLiteral("mode"),
                    QStringLiteral("zero1"));
    services.preset(QStringLiteral("file.quick_look"), QStringLiteral("maxPreviewKb"), 4);
    services.preset(QStringLiteral("file.quick_look"), QStringLiteral("spaceKey"), false);

    // 输入组：
    //  · 剪贴板历史落在自检沙盒里（不让它写真实 %APPDATA%）；清空前不弹模态确认框
    //    （模态框没人点 → 自检会卡死在那里）；单条上限压到 4 KB（"超上限不记录"才有确定预期）
    //  · pasteBack=false → 只回填剪贴板，不往用户的前台窗口注入 Ctrl+V
    services.preset(QStringLiteral("input.clipboard_history"), QStringLiteral("confirm"), false);
    services.preset(QStringLiteral("input.clipboard_history"), QStringLiteral("pasteBack"), false);
    services.preset(QStringLiteral("input.clipboard_history"), QStringLiteral("maxItemKb"), 4);
    services.preset(QStringLiteral("input.clipboard_history"), QStringLiteral("imageMode"),
                    QStringLiteral("thumbnail"));
    services.preset(QStringLiteral("input.clipboard_history"), QStringLiteral("storageDir"),
                    FeatureSmoke::clipboardHistoryDir(tempAppData));
    //  · 滚轮增强：步长刻意用 7（不是代码里的默认 5）——"设置面板上显示的"与"滚一格实际
    //    生效的"都必须来自配置，这样"配置真的被读走"才是被断言出来的，而不是靠默认值巧合
    services.preset(QStringLiteral("input.wheel_enhance"), QStringLiteral("stepPercent"), 7);
    services.preset(QStringLiteral("input.wheel_enhance"), QStringLiteral("requireModifiers"),
                    QStringLiteral("none"));
    services.preset(QStringLiteral("input.wheel_enhance"), QStringLiteral("invert"), false);

    // P3 组：
    //  · 文件锁定解除（P3-02）：confirm=false 绕开二次确认的模态框（没人点的模态框会卡死自检）。
    //    ⚠ 绕开的只是**弹窗**：面板上那个「我确认…会丢数据」的勾选照旧必须打勾（那是产品安全阀，
    //    自检特意去验它拦得住）。占用源由自检自己起的子进程扮演（见 FileLockProbe）。
    services.preset(QStringLiteral("file.unlock"), QStringLiteral("confirm"), false);
    //  · 虚拟桌面（P3-01）：目标窗口用「跟随光标」——自检自己把光标停到探针窗口上，
    //    前置条件会**断言出来**（而不是假设光标恰好在哪儿）
    services.preset(QStringLiteral("window.virtual_desktop"), QStringLiteral("targetMode"),
                    QStringLiteral("follow_cursor"));
    //  · 音量混合器（P3-09）：轮询压到 1 秒（默认 2 秒）——自检要连续等"会话来去
    //    自动刷新"与"记忆在下一轮被套用"，2 秒 × 好几轮太慢；
    //    rememberVolume 先**关着**起步：自检要按顺序验证"开关关着时改音量不落盘"
    //    → "打开开关才落盘" → "新会话自动套用" → "清除后不再套用"，这四步都要是
    //    断言出来的，不能靠预置值巧合；rememberedVolumes 同理清空。
    //    ⚠ 自检跑的这份配置在临时目录里：本机真实应用被记住的音量不会落到用户配置上。
    services.preset(QStringLiteral("media.mixer"), QStringLiteral("pollSeconds"), 1);
    services.preset(QStringLiteral("media.mixer"), QStringLiteral("rememberVolume"), false);
    services.preset(QStringLiteral("media.mixer"), QStringLiteral("rememberedVolumes"),
                    QStringList());
    services.preset(QStringLiteral("media.mixer"), QStringLiteral("showExpired"), false);
    services.preset(QStringLiteral("media.mixer"), QStringLiteral("groupByProcess"), true);

    WinEase::PluginManager manager;
    manager.setServices(&services);
    const QString pluginDirectory =
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    reporter.info(QStringLiteral("插件目录：%1").arg(pluginDirectory));

    const int loaded = manager.loadPlugins(pluginDirectory);
    reporter.check(loaded > 0,
                   QStringLiteral("从插件目录加载到了真实插件 DLL"),
                   QStringLiteral("成功 %1 个").arg(loaded));
    reporter.check(manager.loadErrors().isEmpty(),
                   QStringLiteral("没有插件加载失败（QPluginLoader::errorString 全空）"),
                   manager.loadErrors().join(QStringLiteral("；")));

    // -----------------------------------------------------------------------
    //  目标窗口探针（独立进程）
    // -----------------------------------------------------------------------
    FeatureSmoke::ProbeWindow probe;
    QString probeError;
    const bool probeReady = probe.start(&probeError);
    reporter.check(probeReady,
                   QStringLiteral("目标窗口探针子进程已就绪（跨进程：平台层会排除本进程窗口）"),
                   probeError);

    if (!probeReady) {
        std::printf("=== 共 %d 项，失败 %d 项 ===\n", reporter.total(), reporter.failures());
        manager.unloadAll();
        WinEase::SettingsManager::instance().sync();
        QDir(tempAppData).removeRecursively();
        return 1;
    }
    reporter.info(QStringLiteral("探针窗口 %1（%2）").arg(FeatureSmoke::handleText(probe.handle()),
                                                       FeatureSmoke::rectText(probe.rect())));

    // -----------------------------------------------------------------------
    //  窗口组用例（P1-01 ~ P1-04）
    // -----------------------------------------------------------------------
    const int windowGroupFailures =
        FeatureSmoke::runWindowGroupTests(reporter, manager, services, probe);
    reporter.info(QStringLiteral("窗口组失败项：%1").arg(windowGroupFailures));

    // -----------------------------------------------------------------------
    //  工具组用例（P1-06 / P1-10 / P1-11 / P1-12）
    //  断言读的是"真实外部状态"：剪贴板内容、屏幕像素、系统网络表
    // -----------------------------------------------------------------------
    const int toolGroupFailures =
        FeatureSmoke::runToolGroupTests(reporter, manager, services, probe);
    reporter.info(QStringLiteral("工具组失败项：%1").arg(toolGroupFailures));

    // -----------------------------------------------------------------------
    //  悬浮层组用例（P1-07 / P1-08）
    //  断言读的是宿主登记表、窗口扩展样式与**合成后的屏幕像素**
    // -----------------------------------------------------------------------
    const int overlayGroupFailures =
        FeatureSmoke::runOverlayGroupTests(reporter, manager, services, probe);
    reporter.info(QStringLiteral("悬浮层组失败项：%1").arg(overlayGroupFailures));

    // -----------------------------------------------------------------------
    //  系统组用例（P1-05 / P1-09 / P1-13 / P1-14）
    //  这一组会**短暂改动真实系统**（注册表菜单项 / 主题 / 壁纸），用例内均会还原
    // -----------------------------------------------------------------------
    const int systemGroupFailures =
        FeatureSmoke::runSystemGroupTests(reporter, manager, services, probe);
    reporter.info(QStringLiteral("系统组失败项：%1").arg(systemGroupFailures));

    // -----------------------------------------------------------------------
    //  文件组用例（P2-01 / P2-02 / P2-03 / P2-12）
    //  这一组的外在状态是**磁盘本身**：文件名、文件内容、文件还在不在（以及回收站条目数）。
    //  会动到用户环境的两处：临时目录里的文件名（用例内撤销）、查重把 2 个自检临时文件
    //  移进回收站（无法程序化还原，用例内已明说）。
    // -----------------------------------------------------------------------
    const int fileGroupFailures =
        FeatureSmoke::runFileGroupTests(reporter, manager, services, probe);
    reporter.info(QStringLiteral("文件组失败项：%1").arg(fileGroupFailures));

    // -----------------------------------------------------------------------
    //  输入组用例（P2-04 剪贴板历史 / P2-05 滚轮增强）
    //  这一组动的是"用户正在输入的那一刻"：系统剪贴板（结束还原）与系统音量
    //  （`AudioVolumeGuard` 结束还原），并且会移动真实光标到任务栏。
    // -----------------------------------------------------------------------
    const int inputGroupFailures =
        FeatureSmoke::runInputGroupTests(reporter, manager, services, probe, tempAppData);
    reporter.info(QStringLiteral("输入组失败项：%1").arg(inputGroupFailures));

    // -----------------------------------------------------------------------
    //  显示与媒体组用例（P2-08 亮度色温护眼 · P2-09 放大镜增强 · P2-10 麦克风静音）
    //  这一组改的是**用户看得见 / 听得见**的系统状态：真实 gamma ramp 的逐元素读回、
    //  真实的原生放大镜窗口（HWND + `MagGetWindowSource` 源矩形 + `GetWindowRgn` 遮罩）、
    //  真实麦克风静音状态与真实屏幕亮度（WMI / DDC/CI 读回），
    //  用例结束均还原（光标、静音状态、屏幕亮度；色温由插件在停用时自行还原）。
    // -----------------------------------------------------------------------
    const int displayGroupFailures =
        FeatureSmoke::runDisplayGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("显示与媒体组失败项：%1").arg(displayGroupFailures));

    // -----------------------------------------------------------------------
    //  系统与媒体组用例（P2-06 快速关机 · P2-07 定时任务）
    //  这一组动的是**后果不可撤销的系统状态**（关机/重启/静音），所以：
    //    * 电源请求全部打到**记录型提权桩**上 —— 链路（确认→倒计时→参数→请求）真实跑，
    //      但绝不会真的关机；「锁定」按钮一次都不点（点了会真锁屏）；
    //    * 唯一的真实改动是"定时静音"任务会真改麦克风，用例内用 MicMuteGuard 还原。
    // -----------------------------------------------------------------------
    const int powerGroupFailures =
        FeatureSmoke::runPowerGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("系统与媒体组失败项：%1").arg(powerGroupFailures));

    // -----------------------------------------------------------------------
    //  安全与隐私组用例（P2-11 摄像头/麦克风使用提醒）
    //  这一组是本工程第一个**只读**系统的功能：它的"外在状态"就是系统隐私页面
    //  里的那份使用记录，所以自检会在注册表里临时造一条自检专用记录
    //  （ConsentStore/NonPackaged），用完立刻删除并断言不留残留。
    // -----------------------------------------------------------------------
    const int deviceGroupFailures =
        FeatureSmoke::runDeviceGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("安全与隐私组失败项：%1").arg(deviceGroupFailures));

    // -----------------------------------------------------------------------
    //  开发运维组用例（P2-13 Hosts 快速编辑）
    //  这一组**不会真的写 hosts**：写请求打在记录型提权桩上；但"读真实文件 → 校验 →
    //  组装字节"这几步全部真实跑 —— 断言的重心是"校验不过必须真的不写"与"往返无损"。
    // -----------------------------------------------------------------------
    const int devGroupFailures =
        FeatureSmoke::runDevGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("开发运维组失败项：%1").arg(devGroupFailures));

    // -----------------------------------------------------------------------
    //  P3 组用例（P3-01 虚拟桌面增强 / P3-02 文件锁定解除 / P3-14 USB 设备管控）
    //  ⚠ 这一组的"有代价的动作"各有各的克制：
    //     · P3-01：搬的是自检自己的窗口（每次搬完读数回验 + 搬回原位），
    //              **不点**切桌面按钮（那会把用户的屏幕切走，留手测）；
    //     · P3-02：锁的是自检自己造的临时文件，占用源是自检自己起的子进程，
    //              删除走回收站；**绝不**碰用户的文件；
    //     · P3-14：**绝不执行设备弹出**（用户接的移动硬盘上可能有正在写入的数据）。
    // -----------------------------------------------------------------------
    const int p3GroupFailures =
        FeatureSmoke::runP3GroupTests(reporter, manager, services, probe);
    reporter.info(QStringLiteral("P3 组失败项：%1").arg(p3GroupFailures));

    // -----------------------------------------------------------------------
    //  批量格式转换（P3-03）
    //  这一组全程只在**自检自己造的临时素材**上干活：
    //     · 引擎侧：计划/落盘一致、绝不原地覆盖、冲突三态、取消只在开工前生效；
    //     · 面板侧：拖入与剪贴板两条入口、逐条结果、进度、跳过不发静默、
    //       「没指定输出目录就先拦住」（文本侧同名同目录 = 会把源文件写坏）。
    //  ⚠ 唯一动到用户环境的是**系统剪贴板**（剪贴板入口必须真的读它），用 ClipboardGuard 还原。
    // -----------------------------------------------------------------------
    const int convertGroupFailures =
        FeatureSmoke::runConvertGroupTests(reporter, manager, tempAppData);
    reporter.info(QStringLiteral("批量格式转换失败项：%1").arg(convertGroupFailures));

    // -----------------------------------------------------------------------
    //  硬件监控悬浮窗（P3-07 第一批）
    //  这一组验的是"**面板上写的是不是真话**"：
    //     · 纯函数侧：逐项开关、格式化边界、以及"读不到必须写出原因、
    //       绝不允许显示 0 或空白"这条最硬的纪律（与插件编同一份 HudMetrics 源码）；
    //     · 真实面板侧：启用即出一个置顶/分层/不抢焦点/点击穿透的小面板，
    //       内存与磁盘温度必须是与平台层同源的真实读数，
    //       温度类指标必须来自 **C++/CLI 桥接（LibreHardwareMonitor）**：要么真实读数、
    //       要么非空的具体原因（权限不够 / 组件缺失），不许 0°C 或空白。
    //  ⚠ 本组不移动光标、不改系统状态，只在桌面上短暂显示一块面板；
    //     收尾断言宿主登记表清空（不留置顶窗口）。
    // -----------------------------------------------------------------------
    const int hudGroupFailures =
        FeatureSmoke::runHudGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("硬件监控悬浮窗失败项：%1").arg(hudGroupFailures));

    // -----------------------------------------------------------------------
    //  音量混合器（P3-09）
    //  这一组的关键在于"被测对象是不是真的"：自检**自己起子进程**当播放器
    //  （--audio-session，最小 WASAPI 渲染客户端，灌静音数据），于是机器上就有了
    //  两份**真实的**音频会话 —— 两个不同进程、同一个进程名，正好覆盖
    //  "一个应用多份会话"这条最容易漏的路径。
    //  断言一律回 Win32 侧逐份读真实音量，外加一条分界线：
    //  **调会话音量绝不能动系统主音量**。
    //  ⚠ 代价：会在默认输出设备上短暂产生两份静音的音频流（听不见声音），
    //     退出时一并释放；不会改系统主音量、不会碰用户正在播放的其它应用。
    // -----------------------------------------------------------------------
    const int mixerGroupFailures =
        FeatureSmoke::runMixerGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("音量混合器失败项：%1").arg(mixerGroupFailures));

    // -----------------------------------------------------------------------
    //  媒体控制面板（P3-11）
    //  验收原文是"控制 Chrome / Spotify / 系统播放器均可识别"，而机器上通常
    //  一个播放器都没开着 —— 所以自检**自己造一份真实的媒体会话**：
    //  子进程用 `ISystemMediaTransportControlsInterop::GetForWindow` 当 SMTC 发布方
    //  （--media-session，带曲目/艺术家/专辑/3 分钟进度/一张纯色封面）。
    //  然后父进程把它**读出来**、真的暂停/播放/跳转，并回读播放器改过的真实状态；
    //  最后点面板上的按钮，验"用户点按钮"这条完整链路。
    //  ⚠ 代价：用例期间系统媒体列表里会短暂多一条"WinEase 自检曲目"（**不发声**，
    //     子进程退出即消失），并且它会短暂成为"当前会话"，所以这一组发的媒体键
    //     打到的是探针而不是用户正在放的东西；不会改任何系统设置。
    // -----------------------------------------------------------------------
    const int playerGroupFailures =
        FeatureSmoke::runPlayerGroupTests(reporter, manager, services);
    reporter.info(QStringLiteral("媒体控制面板失败项：%1").arg(playerGroupFailures));

    // -----------------------------------------------------------------------
    //  批量移动文件（新增：正则匹配 + 计划/预演/撤销）
    //      这一组会真的在临时目录里搬文件，并点"撤销"搬回来 ——
    //      断言落在**磁盘状态**上（文件真的在/真的不在），不是函数返回值。
    // -----------------------------------------------------------------------
    const int batchMoveStart = reporter.failures();
    {
        FeatureSmoke::runBatchMoveGroupTests(reporter, manager, services);
    }
    const int batchMoveFailures = reporter.failures() - batchMoveStart;
    reporter.info(QStringLiteral("批量移动文件失败项：%1").arg(batchMoveFailures));

    // -----------------------------------------------------------------------
    //  局域网文件传输（新增：手机浏览器 / PC↔PC）
    //      ⚠ 只连 127.0.0.1 回环：不起广播、不碰真实局域网，任何机器上都能重跑。
    //        验的是"路径穿越必须被挡"这条安全红线 + 真上传真下载真落盘。
    // -----------------------------------------------------------------------
    const int lanTransferStart = reporter.failures();
    {
        FeatureSmoke::runLanTransferGroupTests(reporter, manager, services);
    }
    const int lanTransferFailures = reporter.failures() - lanTransferStart;
    reporter.info(QStringLiteral("局域网文件传输失败项：%1").arg(lanTransferFailures));

    // 先卸载插件（让各插件在探针窗口仍然存在时完成还原），再关掉子进程与钩子
    manager.unloadAll();
    probe.stop();
    hookService.stop();

    std::printf("=== 共 %d 项，失败 %d 项 ===\n", reporter.total(), reporter.failures());

    WinEase::SettingsManager::instance().sync();
    QDir(tempAppData).removeRecursively();

    return reporter.failures() == 0 ? 0 : 1;
}
