// ============================================================================
//  WinEase.LiteMonitorDeps 的占位类型
//
//  本工程的目的与代码无关，只是借 `dotnet publish` 把 LibreHardwareMonitorLib
//  及它的一串依赖（HidSharp / DiskInfoToolkit / RAMSPDToolkit-NDD …）平铺到
//  build/bin。C++/CLI 桥接层（WinEaseLiteMonitorBridge）在运行时才引用它们。
//
//  ⚠ 这里刻意不写任何逻辑：本 DLL 不会被原生侧 LoadLibrary，也不会被 #using。
// ============================================================================

namespace WinEase.LiteMonitorDeps;

internal static class Placeholder
{
    // 只是为了让工程能产出一个合法程序集，没有任何调用方。
    internal const string Purpose = "LibreHardwareMonitorLib dependency carrier for WinEase C++/CLI bridge";
}
