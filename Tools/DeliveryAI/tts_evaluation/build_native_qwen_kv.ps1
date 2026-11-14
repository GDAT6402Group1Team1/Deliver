param([switch]$PrepareOnly)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path "$PSScriptRoot/../../..").Path
$root = (Join-Path $project 'Saved/AIResearch/NativeQwen').Replace('\','/')
$python = Join-Path $project 'Saved/AIResearch/TtsEvaluation/qwen/.venv/Scripts/python.exe'
& $python -B -X utf8 "$PSScriptRoot/prepare_native_kv.py"
if ($LASTEXITCODE) { throw '小缓存源码校验失败' }
if ($PrepareOnly) { return }
$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw '没有找到 MSVC 工具链' }
$compilerLine = Get-Content "$root/build-cuda/CMakeCache.txt" | Where-Object { $_ -like 'CMAKE_CXX_COMPILER:FILEPATH=*' }
$compiler = ($compilerLine -split '=', 2)[1].Replace('\','/')
if ($compiler -notmatch '^(.*)/VC/Tools/MSVC/([^/]+)/') { throw '无法确认原生基线的 MSVC 版本' }
$compilerRoot = $Matches[1]
$compilerVersion = $Matches[2]
$vcvars = Join-Path $compilerRoot 'Common7/Tools/VsDevCmd.bat'
cmd /c "`"$vcvars`" -arch=x64 -host_arch=x64 -vcvars_ver=$compilerVersion >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
if ($LASTEXITCODE) { throw '原生基线的 MSVC 环境初始化失败' }
$cmake = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$vendor = Join-Path $project 'Tools/DeliveryAI/native_tts_vendor/kv1024'
& $cmake --fresh -S $vendor -B "$root/build-cuda-kv1024" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$compiler" `
    "-DQWEN_SOURCE_DIR=$root/source" "-DQWEN_PREPARED_DIR=$root/source-kv1024" "-DQWEN_BASE_BUILD_DIR=$root/build-cuda"
if ($LASTEXITCODE) { throw '小缓存 DLL 配置失败' }
& $cmake --build "$root/build-cuda-kv1024" --target qwen -j 3
if ($LASTEXITCODE) { throw '小缓存 DLL 构建失败' }
& $python -B -X utf8 "$PSScriptRoot/prepare_native_kv.py" --runtime
if ($LASTEXITCODE) { throw '小缓存对照配置生成失败' }
