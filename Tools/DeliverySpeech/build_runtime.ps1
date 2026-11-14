param([switch]$SkipPrepare)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$speechRoot = Join-Path $projectRoot 'Saved/DeliverySpeech'
$pythonCommand = Get-Command python.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
if ($SkipPrepare) {
    & $pythonCommand.Source -B -X utf8 (Join-Path $PSScriptRoot 'prepare_runtime.py') --verify-only
} else {
    & $pythonCommand.Source -B -X utf8 (Join-Path $PSScriptRoot 'prepare_runtime.py')
}
if ($LASTEXITCODE) { throw '固定语音运行库校验失败' }
$prepared = Get-Content -LiteralPath (Join-Path $speechRoot 'runtime/prepared.json') -Raw -Encoding UTF8 | ConvertFrom-Json

$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw '未找到 Visual Studio Installer 的 vswhere.exe' }
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw '未找到 MSVC x64 工具链' }
$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
# 仅导入本进程的编译环境，不修改系统配置或启动可见窗口
cmd /d /c "`"$devCommand`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
if ($LASTEXITCODE) { throw 'MSVC 编译环境初始化失败' }
$cmake = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $cmake) -or !(Test-Path -LiteralPath $ninja)) { throw '未找到 VS 自带 CMake 和 Ninja' }
$buildPath = Join-Path $speechRoot 'build-runtime'
& $cmake -S (Join-Path $PSScriptRoot 'native') -B $buildPath -G Ninja `
    '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DSHERPA_ROOT=$($prepared.sdk)" "-DDELIVERY_RUNTIME_DIR=$($prepared.runtime)"
if ($LASTEXITCODE) { throw '语音运行库 CMake 配置失败' }
& $cmake --build $buildPath --target DeliverySpeechRuntime --parallel 2
if ($LASTEXITCODE) { throw '语音运行库构建失败' }

$runtimeDll = Join-Path $prepared.runtime 'DeliverySpeechRuntime.dll'
$dependencies = & dumpbin /nologo /dependents $runtimeDll
if ($LASTEXITCODE -or ($dependencies -match '(?i)onnxruntime\.dll')) {
    throw '语音 DLL 依赖检查失败，不得与 UE 的 onnxruntime.dll 争用'
}
$exports = & dumpbin /nologo /exports $runtimeDll
if ($LASTEXITCODE) { throw '语音 DLL 导出检查失败' }
$exportNames = @($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $Matches[1] } })
$expected = @('dsr_abi_version','dsr_begin','dsr_cancel','dsr_create','dsr_destroy','dsr_finish','dsr_get_error','dsr_get_result','dsr_push')
if (@(Compare-Object ($expected | Sort-Object) ($exportNames | Sort-Object)).Count) {
    throw "DLL 导出与 ABI v1 不一致: $($exportNames -join ', ')"
}
$result = [ordered]@{
    abi_version = 1
    dll = $runtimeDll
    dll_sha256 = (Get-FileHash -LiteralPath $runtimeDll -Algorithm SHA256).Hash.ToLowerInvariant()
    abi_header_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'native/delivery_speech_runtime.h') -Algorithm SHA256).Hash.ToLowerInvariant()
    manifest_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'runtime_manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    exports = $exportNames
    dependencies = $dependencies
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $prepared.runtime 'build.json') -Encoding utf8
Write-Output "本地识别运行库已构建并检查 ABI: $runtimeDll"
