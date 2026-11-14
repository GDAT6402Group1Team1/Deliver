param([string]$EngineRoot = 'D:/Epic Games/UE_5.8')
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputRoot = Join-Path $projectRoot 'Saved/DeliverySpeech/ConversationAssetCheck'
$template = Join-Path $projectRoot 'Plugins/DeliveryTuning/Intermediate/Build/Win64/x64/UnrealEditor/Development/DeliveryTuning/DeliveryConversationAssetTools.cpp.obj.rsp'
if (!(Test-Path -LiteralPath $template)) { throw 'Build DeliveryEditor once to obtain the editor module compiler arguments' }

# 只编译本翻译单元，结果写入 Saved，不启动 UBT、编辑器或链接器
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$toolset = Get-ChildItem -LiteralPath (Join-Path $installation 'VC/Tools/MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$compiler = Join-Path $toolset.FullName 'bin/Hostx64/x64/cl.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'MSVC x64 compiler not found' }
$arguments = @(Get-Content -LiteralPath $template | Where-Object {
    $_ -notmatch '^/Fo|^/Yu|^/Fp|^/FI.*SharedPCH|^/experimental:log|^/sourceDependencies'
})
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$response = Join-Path $outputRoot 'asset_builder.rsp'
$object = Join-Path $outputRoot 'asset_builder.obj'
$arguments += '/Fo"' + $object.Replace('\','/') + '"'
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllLines($response,$arguments,$utf8)
$watch = [Diagnostics.Stopwatch]::StartNew()
Push-Location (Join-Path $EngineRoot 'Engine/Source')
try {
    $output = @(& $compiler "@$response" 2>&1)
    $compileExit = $LASTEXITCODE
} finally { Pop-Location }
$watch.Stop()
[IO.File]::WriteAllLines((Join-Path $outputRoot 'compile.log'),[string[]]$output,$utf8)
$output | ForEach-Object { Write-Output $_ }
$report = [ordered]@{
    exit_code = $compileExit
    elapsed_seconds = [Math]::Round($watch.Elapsed.TotalSeconds,3)
    source = 'Plugins/DeliveryTuning/Source/DeliveryTuning/Private/DeliveryConversationAssetTools.cpp'
    compiler = $compiler
    object = $object
    scope = 'Single translation unit; no linking, editor, asset saving or gameplay acceptance'
}
[IO.File]::WriteAllText((Join-Path $outputRoot 'checks.json'),($report | ConvertTo-Json),$utf8)
exit $compileExit
