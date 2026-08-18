param(
    [switch]$SkipTests,
    [switch]$HardwareSmoke,
    [switch]$MicrophoneSmoke,
    [switch]$MonitorSmoke,
    [switch]$ProcessIsolationTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Copy-Utf8LfTextFile {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )
    $content = [System.IO.File]::ReadAllText($SourcePath)
    $content = $content.Replace("`r`n", "`n").Replace("`r", "`n")
    $utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($DestinationPath, $content, $utf8WithoutBom)
}

function Write-AsciiLfTextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Lines
    )
    $content = ($Lines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $content, [System.Text.Encoding]::ASCII)
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools were not found. Install the Desktop development with C++ workload.'
}

$vsInstall = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $vsInstall) {
    throw 'MSVC x64 build tools were not found.'
}

$vcVars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & cmd.exe /d /c "`"$vcVars`" >nul && set"
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }
    $name = $line.Substring(0, $separator)
    # Some managed shells expose both Path and PATH. MSBuild rejects that pair.
    if ($name -ceq 'Path') { continue }
    Set-Item -LiteralPath "Env:$name" -Value $line.Substring($separator + 1)
}

$msvcRoot = Join-Path $vsInstall 'VC\Tools\MSVC'
$msvcVersion = Get-ChildItem -LiteralPath $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvcVersion) { throw 'MSVC compiler directory was not found.' }
$compiler = Join-Path $msvcVersion.FullName 'bin\Hostx64\x64\cl.exe'

$sdkBinRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
$sdkVersion = Get-ChildItem -LiteralPath $sdkBinRoot -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64\rc.exe') } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkVersion) { throw 'Windows SDK resource compiler was not found.' }
$resourceCompiler = Join-Path $sdkVersion.FullName 'x64\rc.exe'
$processLoopbackHeader = Join-Path (Join-Path 'C:\Program Files (x86)\Windows Kits\10\Include' $sdkVersion.Name) 'um\audioclientactivationparams.h'
if (-not (Test-Path -LiteralPath $processLoopbackHeader)) {
    throw 'The selected Windows SDK does not support per-application audio loopback. Install SDK 10.0.20348 or later.'
}

$buildRoot = Join-Path $projectRoot 'build-artifacts'
$appObjectRoot = Join-Path $buildRoot 'obj-app'
$testObjectRoot = Join-Path $buildRoot 'obj-tests'
$smokeObjectRoot = Join-Path $buildRoot 'obj-smoke'
$toneObjectRoot = Join-Path $buildRoot 'obj-isolation-tone'
$isolationObjectRoot = Join-Path $buildRoot 'obj-isolation-test'
$binaryRoot = Join-Path $buildRoot 'bin'
$distRoot = Join-Path $projectRoot 'dist'
$releaseStage = Join-Path $buildRoot ("release-stage-" + [guid]::NewGuid().ToString('N'))
@($appObjectRoot, $testObjectRoot, $smokeObjectRoot, $toneObjectRoot,
  $isolationObjectRoot, $binaryRoot, $releaseStage) | ForEach-Object {
    New-Item -ItemType Directory -Force -Path $_ | Out-Null
}

$common = @(
    '/nologo', '/std:c++20', '/EHsc', '/O2', '/GL', '/MT', '/W4', '/sdl', '/guard:cf', '/permissive-', '/utf-8',
    '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DNDEBUG',
    '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A00000A',
    "/I$projectRoot\src"
)
$secureLink = @('/DYNAMICBASE', '/NXCOMPAT', '/HIGHENTROPYVA', '/GUARD:CF', '/CETCOMPAT', '/LTCG', '/Brepro')

Push-Location $projectRoot
try {
    $resource = Join-Path $buildRoot 'ChromeMic.res'
    & $resourceCompiler /nologo /I resources "/fo$resource" resources\ChromeMic.rc
    if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed with exit code $LASTEXITCODE." }

    $appExe = Join-Path $releaseStage 'ChromeMic.exe'
    & $compiler @common "/Fo$appObjectRoot\" src\main.cpp src\app_processes.cpp src\audio_devices.cpp src\audio_router.cpp src\dsp.cpp src\process_loopback.cpp src\update_checker.cpp src\voice_effects.cpp $resource "/Fe:$appExe" /link /SUBSYSTEM:WINDOWS @secureLink ole32.lib uuid.lib avrt.lib propsys.lib mmdevapi.lib advapi32.lib comctl32.lib dwmapi.lib gdi32.lib shell32.lib user32.lib uxtheme.lib winhttp.lib
    if ($LASTEXITCODE -ne 0) { throw "ChromeMic compilation failed with exit code $LASTEXITCODE." }

    if (-not $SkipTests) {
        $testExe = Join-Path $binaryRoot 'chromemic_tests.exe'
        & $compiler @common "/Fo$testObjectRoot\" tests\chromemic_tests.cpp src\app_processes.cpp src\dsp.cpp src\audio_devices.cpp src\audio_router.cpp src\process_loopback.cpp src\update_checker.cpp src\voice_effects.cpp "/Fe:$testExe" /link /SUBSYSTEM:CONSOLE @secureLink ole32.lib uuid.lib avrt.lib propsys.lib mmdevapi.lib user32.lib winhttp.lib
        if ($LASTEXITCODE -ne 0) { throw "Test compilation failed with exit code $LASTEXITCODE." }
        & $testExe
        if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE." }
    }

    $runHardwareSmoke = $HardwareSmoke -or $MicrophoneSmoke -or $MonitorSmoke
    if ($runHardwareSmoke) {
        $smokeExe = Join-Path $binaryRoot 'chromemic_smoke.exe'
        & $compiler @common "/Fo$smokeObjectRoot\" tools\smoke_test.cpp src\app_processes.cpp src\audio_router.cpp src\audio_devices.cpp src\dsp.cpp src\process_loopback.cpp src\voice_effects.cpp "/Fe:$smokeExe" /link /SUBSYSTEM:CONSOLE @secureLink ole32.lib uuid.lib avrt.lib propsys.lib mmdevapi.lib user32.lib
        if ($LASTEXITCODE -ne 0) { throw "Smoke-test compilation failed with exit code $LASTEXITCODE." }
        $smokeArguments = @()
        if ($MicrophoneSmoke -or $MonitorSmoke) { $smokeArguments += '--with-mic' }
        if ($MonitorSmoke) { $smokeArguments += '--with-monitor' }
        & $smokeExe @smokeArguments
        if ($LASTEXITCODE -ne 0) { throw "Hardware smoke test could not complete (exit code $LASTEXITCODE)." }
    }

    if ($ProcessIsolationTest) {
        $toneExe = Join-Path $binaryRoot 'chromemic_tone_renderer.exe'
        & $compiler @common "/Fo$toneObjectRoot\" tools\tone_renderer.cpp "/Fe:$toneExe" /link /SUBSYSTEM:CONSOLE @secureLink ole32.lib uuid.lib avrt.lib
        if ($LASTEXITCODE -ne 0) { throw "Tone-renderer compilation failed with exit code $LASTEXITCODE." }

        $isolationExe = Join-Path $binaryRoot 'chromemic_process_isolation_test.exe'
        & $compiler @common "/Fo$isolationObjectRoot\" tools\process_isolation_test.cpp src\audio_router.cpp src\audio_devices.cpp src\dsp.cpp src\process_loopback.cpp src\voice_effects.cpp "/Fe:$isolationExe" /link /SUBSYSTEM:CONSOLE @secureLink ole32.lib uuid.lib avrt.lib propsys.lib mmdevapi.lib user32.lib
        if ($LASTEXITCODE -ne 0) { throw "Process-isolation test compilation failed with exit code $LASTEXITCODE." }
        & $isolationExe --tone-renderer $toneExe
        if ($LASTEXITCODE -ne 0) { throw "Process-isolation signal test did not pass (exit code $LASTEXITCODE)." }
    }

    foreach ($releaseDocument in @('START-HERE.txt', 'README.md', 'AUDIT.md', 'LICENSE')) {
        Copy-Utf8LfTextFile -SourcePath (Join-Path $projectRoot $releaseDocument) -DestinationPath (Join-Path $releaseStage $releaseDocument)
    }

    $exeHash = (Get-FileHash -LiteralPath $appExe -Algorithm SHA256).Hash
    $buildInfoLines = @(
        'ChromeMic 1.2.0 - app + optional microphone mixer, unsigned x64 release'
        "MSVC toolset: $($msvcVersion.Name)"
        "Windows SDK: $($sdkVersion.Name)"
        'Runtime: statically linked (/MT)'
        'Hardening: ASLR, high-entropy VA, DEP/NX, CFG, CET compatibility'
        'Package timestamps: normalized for deterministic archives'
        "Automated tests run: $(-not $SkipTests)"
        "Endpoint-open smoke run: $([bool]$runHardwareSmoke)"
        "Physical-microphone smoke run: $([bool]($MicrophoneSmoke -or $MonitorSmoke))"
        "Headphone-loopback smoke run: $([bool]$MonitorSmoke)"
        "Process-isolation signal test run: $([bool]$ProcessIsolationTest)"
        "ChromeMic.exe SHA-256: $exeHash"
        'Authenticode: unsigned; verify SHA-256 and expect a Windows reputation warning'
    )
    Write-AsciiLfTextFile -Path (Join-Path $releaseStage 'BUILD-INFO.txt') -Lines $buildInfoLines

    $hashLines = Get-ChildItem -LiteralPath $releaseStage -File | Sort-Object Name | ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$hash  $($_.Name)"
    }
    Write-AsciiLfTextFile -Path (Join-Path $releaseStage 'SHA256SUMS.txt') -Lines $hashLines

    $normalizedTimestamp = [DateTime]::Parse('2026-01-01T00:00:00Z').ToUniversalTime()
    Get-ChildItem -LiteralPath $releaseStage -File | ForEach-Object {
        $_.LastWriteTimeUtc = $normalizedTimestamp
    }

    $resolvedProject = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd('\')
    $resolvedDist = [System.IO.Path]::GetFullPath($distRoot)
    if (-not $resolvedDist.StartsWith($resolvedProject + '\', [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolvedDist) -ne 'dist') {
        throw 'Refusing to replace an unexpected release directory.'
    }
    $distBackup = Join-Path $buildRoot ("dist-backup-" + [guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $resolvedDist) {
        Move-Item -LiteralPath $resolvedDist -Destination $distBackup
    }
    try {
        Move-Item -LiteralPath $releaseStage -Destination $resolvedDist
    } catch {
        if ((Test-Path -LiteralPath $distBackup) -and -not (Test-Path -LiteralPath $resolvedDist)) {
            Move-Item -LiteralPath $distBackup -Destination $resolvedDist
        }
        throw
    }
    if (Test-Path -LiteralPath $distBackup) {
        Remove-Item -LiteralPath $distBackup -Recurse -Force
    }

    $zipPath = Join-Path $projectRoot 'ChromeMic-1.2.0-win-x64.zip'
    Compress-Archive -Path (Join-Path $resolvedDist '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force
    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
    Write-AsciiLfTextFile -Path ($zipPath + '.sha256.txt') -Lines @("$zipHash  $(Split-Path -Leaf $zipPath)")

    Write-Host "Build complete: $(Join-Path $resolvedDist 'ChromeMic.exe')"
    Write-Host "Release archive: $zipPath"
} finally {
    Pop-Location
    if (Test-Path -LiteralPath $releaseStage) {
        Remove-Item -LiteralPath $releaseStage -Recurse -Force
    }
}
