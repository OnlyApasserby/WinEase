# ============================================================================
#  stage_dist.ps1 -- build the single-file WinEase installer
#
#  !! THIS FILE MUST STAY 100% ASCII !!
#  Windows PowerShell 5.1 reads .ps1 files as ANSI (codepage 936 here).
#  A UTF-8 Chinese comment turns into mojibake *inside the parser*, which can
#  inject stray  "  /  $  characters and silently corrupt statements
#  (the same failure mode as "C2001 unterminated string literal" in cl.exe
#  when a UTF-8 source is compiled without /utf-8 -- see docs/traps.md).
#
#  Pipeline (all offline at install time):
#     1. app + helper + plugins + bridge + managed deps   (from build\bin)
#     2. Qt runtime + plugins                             (windeployqt)
#     3. app-local VC++ runtime                           (VC\Redist\...\x64\Microsoft.VC*.CRT)
#     4. .NET 8 runtime                                   (dotnet publish --self-contained)
#     5. payload.manifest.txt                             (sha256 per file)
#     6. makecab                -> dist\payload.cab
#     7. stub exe + cab + 96-byte footer -> dist\WinEase-<ver>-x64-Setup.exe
#
#  Normally invoked by the CMake target `winease_installer`.
# ============================================================================

param(
    [string]$BuildDir      = 'f:\develop\1919810\build',
    [string]$Config        = 'RelWithDebInfo',
    [string]$Version       = '0.1.0',
    [string]$QtBin         = 'D:\Qt\bin',
    [string]$VcRedistRoot  = '',
    [string]$StubPath      = '',
    # NOTE: [string], not [bool] -- PowerShell 5.1 passes command-line values as
    # strings, and a [bool] parameter rejects "true"/"false" (CMake passes those).
    #
    # DEFAULT IS 'false' (framework-dependent), and that is a *measured* decision:
    #   the self-contained layout (coreclr.dll + ijwhost + includedFrameworks)
    #   fail-fasts with 0xC0000409 when the bridge is loaded from a native host,
    #   while "use the .NET 8 runtime installed on the machine" works (verified:
    #   199 sensors enumerated). See docs/DISTRIBUTION.md "已知边界".
    #   The self-contained path is kept for experiments but is NOT a supported
    #   configuration - the installer detects .NET 8 and says what it found.
    [string]$BundleDotnet  = 'false',
    [string]$InstallerName = ''
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$text) { Write-Host ("[stage] " + $text) }
function Fail([string]$text) { Write-Host ("[stage] ERROR: " + $text); exit 2 }

$bundleRuntime = ($BundleDotnet -eq 'true' -or $BundleDotnet -eq '1' -or $BundleDotnet -eq 'yes')

# ---------------------------------------------------------------------------
#  0. Preconditions: fail loudly instead of producing a "looks fine" partial package
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $BuildDir)) { Fail ("build dir not found: " + $BuildDir) }
$binDir = Join-Path $BuildDir 'bin'
$appExe = Join-Path $binDir 'WinEase.exe'
if (-not (Test-Path -LiteralPath $appExe)) { Fail ("WinEase.exe not found - build the app first: " + $appExe) }
if ($StubPath -eq '') { $StubPath = Join-Path $binDir 'winease-setup.exe' }
if (-not (Test-Path -LiteralPath $StubPath)) { Fail ("SFX stub not found - build target winease-setup: " + $StubPath) }

$distDir  = Join-Path $BuildDir 'dist'
$stageDir = Join-Path $distDir 'stage'
if ($InstallerName -eq '') { $InstallerName = "WinEase-$Version-x64-Setup.exe" }
$setupExe = Join-Path $distDir $InstallerName
$cabPath  = Join-Path $distDir 'payload.cab'

Write-Step ("build dir : " + $BuildDir)
Write-Step ("stage dir : " + $stageDir)
Write-Step ("stub      : " + $StubPath)
Write-Step ("bundle .net runtime : " + $bundleRuntime)

if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

# ---------------------------------------------------------------------------
#  1. Application payload
#     (the bridge DLL and its managed deps MUST sit next to the exe:
#      CoreCLR probes assemblies relative to the application base directory)
# ---------------------------------------------------------------------------
foreach ($name in @('WinEase.exe', 'WinEaseHelper.exe')) {
    $source = Join-Path $binDir $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $stageDir -Force }
}
Get-ChildItem (Join-Path $binDir '*.dll') -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Force
}
Get-ChildItem (Join-Path $binDir '*.runtimeconfig.json') -File -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Force }

$pluginSource = Join-Path $binDir 'plugins'
if (-not (Test-Path -LiteralPath $pluginSource)) { Fail "plugins dir not found (build all plugins first)" }
$pluginTarget = Join-Path $stageDir 'plugins'
New-Item -ItemType Directory -Force -Path $pluginTarget | Out-Null
Copy-Item (Join-Path $pluginSource '*.dll') -Destination $pluginTarget -Force
$pluginCount = (Get-ChildItem (Join-Path $pluginTarget '*.dll')).Count
Write-Step ("app + plugins staged (plugins = " + $pluginCount + ")")

# ---------------------------------------------------------------------------
#  2. Qt runtime + plugins (windeployqt is Qt's own tool -> no hand-written list)
#     --no-compiler-runtime : the VC++ runtime is copied explicitly in step 3
#     --no-opengl-sw / --no-system-d3d-compiler : widget app does not need them
# ---------------------------------------------------------------------------
$windeployqt = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path -LiteralPath $windeployqt)) { Fail ("windeployqt not found at " + $windeployqt) }
$deployArgs = @('--release', '--no-compiler-runtime', '--no-opengl-sw', '--no-system-d3d-compiler',
                '--dir', $stageDir, (Join-Path $stageDir 'WinEase.exe'))
& $windeployqt @deployArgs | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed for WinEase.exe" }
Write-Step "windeployqt done (app)"

# Qt Network is only used by the LAN transfer plugin; run windeployqt on that
# plugin as well, otherwise its TLS backends are not deployed.
$lanPlugin = Join-Path $pluginTarget 'we_net_lan_transfer.dll'
if (Test-Path -LiteralPath $lanPlugin) {
    & $windeployqt @('--release', '--no-compiler-runtime', '--no-opengl-sw',
                     '--no-system-d3d-compiler', '--dir', $stageDir, $lanPlugin) | Out-Null
    Write-Step "windeployqt done (lan transfer plugin)"
}

# Safety net for the Qt plugins that make the difference between "starts" and
# "does not start". windeployqt's decisions vary between Qt versions.
$qtPluginDir = Join-Path $QtBin '..\plugins'
$mustHaveQtPlugins = @(
    @{ Source = 'platforms\qwindows.dll'; Required = $true },
    @{ Source = 'styles\qmodernwindowsstyle.dll'; Required = $false },
    @{ Source = 'imageformats\qsvg.dll'; Required = $false },
    @{ Source = 'tls\qschannelbackend.dll'; Required = $false }
)
foreach ($entry in $mustHaveQtPlugins) {
    $targetPath = Join-Path $stageDir $entry.Source
    if (Test-Path -LiteralPath $targetPath) { continue }
    $sourcePath = Join-Path $qtPluginDir $entry.Source
    if (Test-Path -LiteralPath $sourcePath) {
        New-Item -ItemType Directory -Force -Path (Split-Path $targetPath -Parent) | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $targetPath -Force
        Write-Step ("qt plugin copied manually: " + $entry.Source)
    } elseif ($entry.Required) {
        Fail ("required Qt plugin missing: " + $entry.Source)
    }
}
$qtNetworkPath = Join-Path $stageDir 'Qt6Network.dll'
if ((Test-Path -LiteralPath $lanPlugin) -and (-not (Test-Path -LiteralPath $qtNetworkPath))) {
    $sourcePath = Join-Path $QtBin 'Qt6Network.dll'
    if (Test-Path -LiteralPath $sourcePath) {
        Copy-Item -LiteralPath $sourcePath -Destination $stageDir -Force
        Write-Step "Qt6Network.dll copied"
    } else {
        Fail "Qt6Network.dll missing (the lan transfer plugin needs it)"
    }
}

# ---------------------------------------------------------------------------
#  3. VC++ runtime, app-local (copy DLLs only: no system redist, no registry)
# ---------------------------------------------------------------------------
if ($VcRedistRoot -eq '') {
    $vcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC'
    if (Test-Path -LiteralPath $vcRoot) {
        # NOTE: pick the newest version dir that actually HAS an x64 subdir.
        # Sorting by name alone picks "v145" (the alias dir, no x64 inside) over
        # "14.51.36231", which then fails on the very next Get-ChildItem.
        $candidates = Get-ChildItem $vcRoot -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64') } |
            Sort-Object Name -Descending
        if ($candidates -and $candidates.Count -gt 0) { $VcRedistRoot = $candidates[0].FullName }
    }
}
if ($VcRedistRoot -eq '' -or -not (Test-Path -LiteralPath $VcRedistRoot)) {
    Fail "VC redist dir not found (pass -VcRedistRoot <...\VC\Redist\MSVC\<ver>>)"
}
if (-not (Test-Path -LiteralPath (Join-Path $VcRedistRoot 'x64'))) {
    Fail ("no x64 subdir under " + $VcRedistRoot + " (pass -VcRedistRoot explicitly)")
}
$crtDir = Get-ChildItem (Join-Path $VcRedistRoot 'x64') -Directory -Filter 'Microsoft.VC*.CRT' |
          Select-Object -First 1
if (-not $crtDir) { Fail ("Microsoft.VC*.CRT not found under " + $VcRedistRoot) }
# vccorlib140.dll is for C++/CX only - we do not use it
$crtSkip = @('vccorlib140.dll')
$crtCopied = 0
Get-ChildItem (Join-Path $crtDir.FullName '*.dll') -File | ForEach-Object {
    if ($crtSkip -contains $_.Name) { return }
    Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Force
    $crtCopied++
}
Write-Step ("vc++ runtime staged (" + $crtCopied + " files from " + $crtDir.FullName + ")")

# ---------------------------------------------------------------------------
#  4. .NET 8 runtime (self-contained publish)
#     framework-dependent would require .NET 8 on the target machine;
#     self-contained satisfies "no extra configuration on the target machine"
#     at the cost of roughly +70 MB.
# ---------------------------------------------------------------------------
if ($bundleRuntime) {
    # the build dir lives directly under the repository root: one Split-Path is enough
    $repoRoot = Split-Path $BuildDir -Parent
    $managedProject = Join-Path $repoRoot 'src\bridge\managed\WinEase.LiteMonitorDeps.csproj'
    if (-not (Test-Path -LiteralPath $managedProject)) {
        Fail ("managed deps project not found: " + $managedProject)
    }
    $publishDir = Join-Path $BuildDir '_dist_dotnet_selfcontained'
    if (Test-Path -LiteralPath $publishDir) { Remove-Item -LiteralPath $publishDir -Recurse -Force }
    Write-Step "dotnet publish (self-contained win-x64) ... this can take a few minutes"
    & dotnet publish $managedProject -c Release -r win-x64 --self-contained true `
        -o $publishDir --nologo | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "dotnet publish failed" }

    $runtimeCopied = 0
    Get-ChildItem $publishDir -File | ForEach-Object {
        if ($_.Extension -in @('.pdb', '.xml')) { return }
        if ($_.Name -eq 'WinEase.LiteMonitorDeps.runtimeconfig.json') { return }
        Copy-Item -LiteralPath $_.FullName -Destination $stageDir -Force
        $runtimeCopied++
    }
    # ijwhost reads the runtimeconfig named after the IJW assembly, and it must
    # say "includedFrameworks" (self-contained layout) so that coreclr.dll is
    # loaded from the application directory.
    #
    # NOTE: a self-contained *library* publish does NOT emit a runtimeconfig.json
    # (only executables/apphosts get one), so we generate it here. The version must
    # be the real bundled runtime version - take it from coreclr.dll itself
    # instead of hard-coding (the SDK roll-forward picks whatever patch is installed).
    $coreclrFile = Join-Path $publishDir 'coreclr.dll'
    if (-not (Test-Path -LiteralPath $coreclrFile)) {
        Fail "self-contained publish did not produce coreclr.dll"
    }
    # Version source of truth: the deps.json runtimepack entry, which is exactly
    # what "includedFrameworks" must match. Fallback: hostpolicy.dll ProductVersion
    # (coreclr.dll's is a 4-part file version with a build number, no good).
    $runtimeVersion = ''
    $depsFile = Get-ChildItem $publishDir -Filter '*.deps.json' -File | Select-Object -First 1
    if ($depsFile) {
        $pattern = 'Microsoft\.NETCore\.App\.Runtime\.win-x64":\s*"([0-9][^"]*)"'
        $match = [regex]::Match((Get-Content -LiteralPath $depsFile.FullName -Raw), $pattern)
        if ($match.Success) { $runtimeVersion = $match.Groups[1].Value }
    }
    if ($runtimeVersion -eq '') {
        $policyFile = Join-Path $publishDir 'hostpolicy.dll'
        if (Test-Path -LiteralPath $policyFile) {
            $productVersion = (Get-Item -LiteralPath $policyFile).VersionInfo.ProductVersion
            if ($productVersion) { $runtimeVersion = ($productVersion -split ' ')[0] }
        }
    }
    if ($runtimeVersion -eq '') {
        Fail "cannot determine the bundled .NET runtime version (looked at deps.json and hostpolicy.dll)"
    }
    # SANITIZE: a stray newline here produces a *syntactically broken* runtimeconfig
    # (the JSON ends up as "version": "\n8.0.28\n") and ijwhost then fail-fasts with
    # 0xC0000409 when the bridge is loaded -- a crash with no useful message at all.
    $runtimeVersion = $runtimeVersion.Trim()
    if ($runtimeVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+') {
        Fail ("nonsense runtime version parsed from deps.json: [" + $runtimeVersion + "]")
    }
    # !! Do NOT use the "+" operator for concatenation inside an @() array literal:
    #    PowerShell splits "'a' + $v + 'b'" into THREE elements, which turns this
    #    one-line JSON field into three lines and produces a runtimeconfig that
    #    ijwhost cannot parse (it then fail-fasts with 0xC0000409 at load time).
    #    Use -f formatting instead - it stays a single element.
    $runtimeConfigLines = @(
        '{',
        '  "runtimeOptions": {',
        '    "tfm": "net8.0",',
        '    "includedFrameworks": [',
        '      {',
        '        "name": "Microsoft.NETCore.App",',
        ('        "version": "{0}"' -f $runtimeVersion),
        '      }',
        '    ]',
        '  }',
        '}'
    )
    # ★ ijwhost (via hostpolicy) resolves the framework's file list from a deps.json
    #   named after the IJW assembly. Without it the trusted-platform-assembly list
    #   stays empty, CoreCLR cannot even load System.Private.CoreLib, and the whole
    #   thing dies as 0xC0000409 (fail-fast) with no message -- which is exactly what
    #   happened before this line existed. So rename the published deps.json too.
    $depsSource = Join-Path $publishDir 'WinEase.LiteMonitorDeps.deps.json'
    if (-not (Test-Path -LiteralPath $depsSource)) {
        Fail "self-contained publish did not produce a deps.json"
    }
    $depsTarget = Join-Path $stageDir 'WinEaseLiteMonitorBridge.deps.json'
    Copy-Item -LiteralPath $depsSource -Destination $depsTarget -Force
    $depsText = Get-Content -LiteralPath $depsTarget -Raw
    if ($depsText -notmatch 'System\.Private\.CoreLib\.dll') {
        Fail "renamed deps.json does not list the framework assemblies (self-contained layout broken)"
    }
    Write-Step "bridge deps.json staged (rename of the self-contained publish output)"

    $runtimeConfigPath = Join-Path $stageDir 'WinEaseLiteMonitorBridge.runtimeconfig.json'
    [System.IO.File]::WriteAllLines($runtimeConfigPath, $runtimeConfigLines, [System.Text.UTF8Encoding]::new($false))
    # self-check: a broken runtimeconfig is a crash without a message at load time,
    # so refuse to ship one (ConvertFrom-Json throws -> staging fails loudly here)
    try {
        $parsed = Get-Content -LiteralPath $runtimeConfigPath -Raw | ConvertFrom-Json
        $parsedVersion = $parsed.runtimeOptions.includedFrameworks[0].version
        if ($parsedVersion -ne $runtimeVersion) {
            Fail ("runtimeconfig self-check failed: parsed version [" + $parsedVersion + "]")
        }
    } catch {
        Fail ("generated runtimeconfig is not valid JSON: " + $_.Exception.Message)
    }
    Write-Step ("bridge runtimeconfig written (includedFrameworks " + $runtimeVersion + ")")
    Write-Step ("dotnet runtime staged (" + $runtimeCopied + " files, self-contained)")
    Write-Step "WARNING: the self-contained layout is NOT verified to work (0xC0000409 at bridge load) - see docs/DISTRIBUTION.md"
} else {
    Write-Step "dotnet bundle disabled (framework-dependent): the installer checks for .NET 8 on the target machine and reports what it found"
}

# ---------------------------------------------------------------------------
#  5. Install notes (shipped so the user sees the boundaries before double-clicking)
# ---------------------------------------------------------------------------
$repoRoot = Split-Path $BuildDir -Parent
$notes = Join-Path $repoRoot 'packaging\README-install.txt'
if (Test-Path -LiteralPath $notes) {
    Copy-Item -LiteralPath $notes -Destination (Join-Path $stageDir 'README-install.txt') -Force
} else {
    Write-Step ("WARN: install notes not found: " + $notes)
}

# ---------------------------------------------------------------------------
#  6. Manifest (sha256 per file): integrity proof + dependency audit trail
# ---------------------------------------------------------------------------
$manifestPath = Join-Path $stageDir 'payload.manifest.txt'
$stageFull = (Resolve-Path -LiteralPath $stageDir).Path
$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('# WinEase payload manifest v1')
$lines.Add('# app-version=' + $Version)
$lines.Add('# config=' + $Config)
$lines.Add('# generated=' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss'))
$totalBytes = [int64]0
$fileCount = 0
Get-ChildItem $stageDir -Recurse -File | Sort-Object FullName | ForEach-Object {
    if ($_.FullName -eq $manifestPath) { return }
    $relative = $_.FullName.Substring($stageFull.Length).TrimStart('\')
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
    $lines.Add(($hash + '  ' + $_.Length + '  ' + $relative))
    $totalBytes += $_.Length
    $fileCount++
}
[System.IO.File]::WriteAllLines($manifestPath, $lines, [System.Text.UTF8Encoding]::new($false))
Write-Step ("manifest written: " + $fileCount + " files, " + [math]::Round($totalBytes / 1MB, 1) + " MB raw")

# ---------------------------------------------------------------------------
#  7. makecab -> payload.cab (LZX compression, Windows built-in tool)
# ---------------------------------------------------------------------------
$ddfPath = Join-Path $distDir 'payload.ddf'
$ddf = New-Object 'System.Collections.Generic.List[string]'
$ddf.Add('; WinEase payload cabinet (generated by scripts/stage_dist.ps1)')
$ddf.Add('.OPTION EXPLICIT')
$ddf.Add('.Set CabinetNameTemplate=payload.cab')
$ddf.Add('.Set DiskDirectoryTemplate=' + $distDir)
$ddf.Add('.Set Cabinet=on')
$ddf.Add('.Set Compress=on')
$ddf.Add('.Set CompressionType=LZX')
$ddf.Add('.Set CompressionMemory=21')
$ddf.Add('.Set MaxDiskSize=0')
$ddf.Add('.Set ReservePerCabinetSize=0')
$ddf.Add('.Set UniqueFiles=off')
Get-ChildItem $stageDir -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($stageFull.Length).TrimStart('\')
    $ddf.Add('"' + $_.FullName + '" "' + $relative + '"')
}
[System.IO.File]::WriteAllLines($ddfPath, $ddf, [System.Text.UTF8Encoding]::new($false))

if (Test-Path -LiteralPath $cabPath) { Remove-Item -LiteralPath $cabPath -Force }
# NOTE: makecab always drops setup.inf / setup.rpt in the CURRENT directory.
# If we let it run from the repository root, those two get committed by mistake,
# so run it inside the dist dir and clean them up afterwards.
Push-Location $distDir
try {
    & makecab /f $ddfPath | Out-Null
} finally {
    Pop-Location
}
Remove-Item (Join-Path $distDir 'setup.inf'), (Join-Path $distDir 'setup.rpt') -Force -ErrorAction SilentlyContinue
if (-not (Test-Path -LiteralPath $cabPath)) { Fail "makecab did not produce payload.cab" }
$cabSize = (Get-Item -LiteralPath $cabPath).Length
Write-Step ("cab built: " + [math]::Round($cabSize / 1MB, 1) + " MB compressed")

# ---------------------------------------------------------------------------
#  8. Append payload + 96-byte footer -> the single-file installer
#     The footer layout must match `struct SfxFooter` in tools/sfx/main.cpp.
# ---------------------------------------------------------------------------
$stubBytes = [System.IO.File]::ReadAllBytes($StubPath)
$cabBytes  = [System.IO.File]::ReadAllBytes($cabPath)
$cabHash   = (Get-FileHash -LiteralPath $cabPath -Algorithm SHA256).Hash.ToLower()

$footer = New-Object byte[] 96
[System.Text.Encoding]::ASCII.GetBytes('WESFX/01').CopyTo($footer, 0)
[System.BitConverter]::GetBytes([uint64]$stubBytes.Length).CopyTo($footer, 8)
[System.BitConverter]::GetBytes([uint64]$cabBytes.Length).CopyTo($footer, 16)
[System.Text.Encoding]::ASCII.GetBytes($cabHash).CopyTo($footer, 24)

$stream = [System.IO.File]::Create($setupExe)
try {
    $stream.Write($stubBytes, 0, $stubBytes.Length)
    $stream.Write($cabBytes, 0, $cabBytes.Length)
    $stream.Write($footer, 0, $footer.Length)
} finally {
    $stream.Close()
}

Copy-Item -LiteralPath $manifestPath `
          -Destination (Join-Path $distDir ($InstallerName -replace '\.exe$', '.manifest.txt')) -Force

$setupSize = (Get-Item -LiteralPath $setupExe).Length
Write-Step ("installer : " + $setupExe)
Write-Step ("size      : " + [math]::Round($setupSize / 1MB, 1) + " MB (single file, offline)")
Write-Step ("files     : " + $fileCount + " (plugins " + $pluginCount + ")")
Write-Step "done"
exit 0
