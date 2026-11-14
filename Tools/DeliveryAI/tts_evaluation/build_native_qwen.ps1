param([switch]$SkipConversion, [switch]$LowMemory, [switch]$PrepareOnly)
$ErrorActionPreference = 'Stop'
if ($LowMemory) {
    & "$PSScriptRoot/build_native_qwen_kv.ps1" -PrepareOnly:$PrepareOnly
    return
}
if ($PrepareOnly) { throw 'PrepareOnly 只用于 LowMemory 独立对照' }
$project = (Resolve-Path "$PSScriptRoot/../../..").Path
$root = Join-Path $project 'Saved/AIResearch/NativeQwen'
$commit = 'f1a6924789883bd92eeb59176793a94af55730b3'
$source = Join-Path $root 'source'
New-Item -ItemType Directory -Force $root | Out-Null
if (!(Test-Path "$source/.git")) {
    git clone --no-checkout https://github.com/ServeurpersoCom/qwentts.cpp.git $source
    if ($LASTEXITCODE) { throw '源码下载失败' }
    git -C $source checkout --detach $commit
}
if ((git -C $source rev-parse HEAD) -ne $commit) { throw '源码版本与固定版本不一致' }
git -C $source submodule update --init --recursive --depth 1
if ($LASTEXITCODE) { throw 'ggml 下载失败' }

$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw '没有找到 MSVC 工具链' }
$vcvars = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
cmd /c "`"$vcvars`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$cmake = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'

# 只下载编译与运行库，不安装驱动，也不修改系统环境变量
$redistribution = Invoke-RestMethod https://developer.download.nvidia.com/compute/cuda/redist/redistrib_12.8.1.json
New-Item -ItemType Directory -Force "$root/downloads", "$root/cuda" | Out-Null
foreach ($name in 'cuda_nvcc', 'cuda_cudart', 'cuda_cccl', 'libcublas') {
    $entry = $redistribution.$name.'windows-x86_64'
    $zip = Join-Path "$root/downloads" ($entry.relative_path -split '/')[-1]
    if (!(Test-Path $zip)) {
        Invoke-WebRequest ('https://developer.download.nvidia.com/compute/cuda/redist/' + $entry.relative_path) -OutFile $zip
    }
    if ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower() -ne $entry.sha256) { throw "CUDA 包校验失败: $name" }
    Expand-Archive -LiteralPath $zip -DestinationPath "$root/downloads" -Force
    $directory = Join-Path "$root/downloads" ([IO.Path]::GetFileNameWithoutExtension($zip))
    Copy-Item -Path "$directory/*" -Destination "$root/cuda" -Recurse -Force
}
$redistribution | ConvertTo-Json -Depth 20 | Set-Content "$root/cuda-redistrib.json" -Encoding utf8
$env:CUDA_PATH = "$root/cuda"
$env:PATH = "$env:CUDA_PATH/bin;$env:PATH"

& $cmake -S $source -B "$root/build-cpu" -G Ninja -DCMAKE_BUILD_TYPE=Release -DQWEN_SHARED=ON -DGGML_CUDA=OFF -DGGML_NATIVE=OFF
if ($LASTEXITCODE) { throw 'CPU 配置失败' }
& $cmake --build "$root/build-cpu" --target qwen quantize -j 4
if ($LASTEXITCODE) { throw 'CPU 构建失败' }
$python = Join-Path $project 'Saved/AIResearch/TtsEvaluation/qwen/.venv/Scripts/python.exe'
& $python -m pip install --target "$root/converter-packages" --no-deps gguf==0.17.1
if ($LASTEXITCODE) { throw 'GGUF 转换库安装失败' }
if (!$SkipConversion) {
    & $python -B -X utf8 "$PSScriptRoot/prepare_native_qwen.py" --convert
    if ($LASTEXITCODE) { throw '模型转换失败' }
}
& $cmake -S $source -B "$root/build-cuda" -G Ninja -DCMAKE_BUILD_TYPE=Release -DQWEN_SHARED=ON -DGGML_CUDA=ON -DGGML_NATIVE=OFF -DCMAKE_CUDA_ARCHITECTURES=120a -DCUDAToolkit_ROOT="$env:CUDA_PATH" -DCMAKE_CUDA_COMPILER="$env:CUDA_PATH/bin/nvcc.exe"
if ($LASTEXITCODE) { throw 'CUDA 配置失败' }
& $cmake --build "$root/build-cuda" --target qwen -j 4
if ($LASTEXITCODE) { throw 'CUDA 构建失败' }
& $python -B -X utf8 "$PSScriptRoot/prepare_native_qwen.py" --runtime
if ($LASTEXITCODE) { throw '运行配置生成失败' }
