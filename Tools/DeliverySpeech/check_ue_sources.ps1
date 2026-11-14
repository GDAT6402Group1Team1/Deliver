param(
    [string]$ProjectRoot = (Join-Path $PSScriptRoot '../..'),
    [string]$EngineRoot = 'D:/Epic Games/UE_5.8',
    [string[]]$Sources = @(
        'Speech/DeliverySpeechRecognizer.cpp',
        'Speech/DeliverySpeechAudioProcessor.cpp',
        'Speech/DeliverySpeechAudioCapture.cpp',
        'Speech/Tests/DeliverySpeechTurnTests.cpp',
        'Speech/Tests/DeliverySpeechAudioTests.cpp'
    ),
    [string]$OutputName = 'UeSourceCheck',
    [string[]]$GeneratedOverrides = @(),
    [string]$StageRoot = ''
)

# 复用本工程已生成的 UE 编译参数，只生成独立目标文件，不触发 UBT、链接或编辑器
# 默认只检查 Source/Delivery，避免验证已经过期的暂存副本
# 显式传入 -StageRoot 时才优先检查指定目录
# 示例：./check_ue_sources.ps1 -Sources 'Speech/DeliverySpeechRecognizer.cpp'
# 多文件在当前 PowerShell 中用 -Sources @('Speech/A.cpp','Speech/B.cpp') 传数组
$ErrorActionPreference = 'Stop'
$projectPath = [IO.Path]::GetFullPath($ProjectRoot)
$enginePath = [IO.Path]::GetFullPath($EngineRoot)
if ($OutputName -notmatch '^[A-Za-z0-9_-]+$') { throw 'OutputName 只能包含字母、数字、下划线和连字符' }
$checkPath = Join-Path $projectPath "Saved/DeliverySpeech/$OutputName"
$stagePath = Join-Path $projectPath 'Tools/DeliverySpeech/ue'
$sourcePath = Join-Path $projectPath 'Source/Delivery'
$useStageFirst = -not [string]::IsNullOrWhiteSpace($StageRoot)
if ($useStageFirst) {
    $stagePath = if ([IO.Path]::IsPathRooted($StageRoot)) { [IO.Path]::GetFullPath($StageRoot) }
        else { [IO.Path]::GetFullPath((Join-Path $projectPath $StageRoot)) }
    if (-not (Test-Path -LiteralPath $stagePath -PathType Container)) { throw "暂存目录不存在：$stagePath" }
}
$sourceRoots = @(if ($useStageFirst) { $stagePath; $sourcePath } else { $sourcePath })
$nativePath = Join-Path $projectPath 'Tools/DeliverySpeech/native'
$cachedPath = Join-Path $projectPath 'Intermediate/Build/Win64/x64/UnrealEditor/Development/Delivery'
$templatePath = Join-Path $cachedPath 'DeliveryNpcSpeechTests.cpp.obj.rsp'
if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw '没有找到 DeliveryEditor 的现有编译参数，请先按项目流程完成一次完整构建'
}
if (-not (Test-Path -LiteralPath (Join-Path $enginePath 'Engine/Build/Build.version') -PathType Leaf)) {
    throw 'EngineRoot 不是已安装的 Unreal Engine 目录'
}

$compiler = Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($compiler) {
    $compilerPath = $compiler.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw '未找到 MSVC 或 vswhere' }
    $installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $installation) { throw '未找到带 C++ 工具链的 Visual Studio 安装' }
    $toolset = Get-ChildItem -LiteralPath (Join-Path $installation 'VC/Tools/MSVC') -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $compilerPath = Join-Path $toolset.FullName 'bin/Hostx64/x64/cl.exe'
    if (-not (Test-Path -LiteralPath $compilerPath -PathType Leaf)) { throw '未找到 x64 MSVC 编译器' }
}

$template = @(Get-Content -LiteralPath $templatePath | Where-Object {
    $_ -notmatch '^".*\.cpp"$' -and
    $_ -notmatch '^/Fo|^/Yu|^/Fp|^/FI.*SharedPCH|^/experimental:log|^/sourceDependencies'
})
$includes = @($GeneratedOverrides | ForEach-Object {
    $directory = [IO.Path]::GetFullPath($_)
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) { throw "生成头或替代头目录不存在：$directory" }
    $directory
}) + $sourceRoots + @(
    $nativePath,
    (Join-Path $enginePath 'Engine/Source/Runtime/AudioCaptureCore/Public'),
    (Join-Path $enginePath 'Engine/Source/ThirdParty/WebRTC/5414/Include'),
    (Join-Path $enginePath 'Engine/Source/ThirdParty/WebRTC/CompatInclude'),
    (Join-Path $enginePath 'Engine/Source/ThirdParty/WebRTC/5414/Include/third_party/abseil-cpp'),
    (Join-Path $enginePath 'Engine/Source/ThirdParty/WebRTC/5414/Include/third_party/libyuv/include')
)
$definitions = @('/DWEBRTC_WIN=1', '/DWEBRTC_5414=1', '/DAUDIOCAPTURECORE_API=DLLIMPORT')
$utf8 = [Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $checkPath -Force | Out-Null
$results = @()
Push-Location (Join-Path $enginePath 'Engine/Source')
try {
    foreach ($relativeSource in $Sources) {
        if ([IO.Path]::IsPathRooted($relativeSource) -or $relativeSource -match '(^|[\\/])\.\.([\\/]|$)' -or
            [IO.Path]::GetExtension($relativeSource) -ne '.cpp') {
            throw "无效的相对 C++ 路径：$relativeSource"
        }
        $inputPath = Join-Path $sourceRoots[0] $relativeSource
        if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf) -and $sourceRoots.Count -gt 1) {
            $inputPath = Join-Path $sourceRoots[1] $relativeSource
        }
        if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw "源码不存在：$relativeSource" }
        $stem = $relativeSource.Replace('/', '_').Replace('\', '_')
        $responsePath = Join-Path $checkPath "$stem.rsp"
        $objectPath = Join-Path $checkPath "$stem.obj"
        $logPath = Join-Path $checkPath "$stem.log"
        # 选定目录先于 Shared.rsp，源码与同名头遵循同一优先级
        $response = @('"' + $inputPath.Replace('\', '/') + '"')
        $response += $includes | ForEach-Object { '/I"' + $_.Replace('\', '/') + '"' }
        $response += $template + $definitions
        $response += '/Fo"' + $objectPath.Replace('\', '/') + '"'
        [IO.File]::WriteAllLines($responsePath, $response, $utf8)
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $compilerOutput = @(& $compilerPath "@$responsePath" 2>&1)
        $compilerExit = $LASTEXITCODE
        $watch.Stop()
        [IO.File]::WriteAllLines($logPath, [string[]]$compilerOutput, $utf8)
        $compilerOutput | ForEach-Object { Write-Output $_ }
        $results += [pscustomobject]@{
            source = $relativeSource
            source_path = $inputPath
            exit_code = $compilerExit
            elapsed_seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
            object = $objectPath
            log = $logPath
        }
    }
} finally {
    Pop-Location
}
$report = [pscustomobject]@{
    compiler = $compilerPath
    engine = $enginePath
    source_preference = $(if ($useStageFirst) { 'stage' } else { 'project' })
    stage_root = $stagePath
    note = '仅独立编译；未链接、未运行 UE 自动化，也不替代完整项目构建'
    results = $results
}
[IO.File]::WriteAllText((Join-Path $checkPath 'checks.json'), ($report | ConvertTo-Json -Depth 4), $utf8)
if (@($results | Where-Object exit_code -ne 0).Count) { exit 1 }
Write-Output "独立编译通过：$($results.Count) 个源文件；结果在 $checkPath"
