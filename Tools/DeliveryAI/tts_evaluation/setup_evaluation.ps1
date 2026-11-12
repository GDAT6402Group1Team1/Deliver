<#
仅准备独立 TTS 试听环境，不启动服务或修改游戏配置
不带 -Execute 时只显示计划；安装前先阅读 Document/AIIteration.html#tts-evaluation-plan
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('qwen', 'kokoro')]
    [string]$Candidate,
    [switch]$Execute
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$evaluationRoot = Join-Path $projectRoot 'Saved/AIResearch/TtsEvaluation'
$candidateRoot = Join-Path $evaluationRoot $Candidate
$venvPath = Join-Path $candidateRoot '.venv'
$basePython = Join-Path $env:USERPROFILE 'miniconda3/envs/mlagents/python.exe'
$requirementsPath = Join-Path $PSScriptRoot "requirements-$Candidate.txt"
$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'models.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$model = $catalog.$Candidate

function Confirm-EvaluationPath([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $allowedPrefix = $evaluationRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "路径超出独立试验目录: $fullPath"
    }
    # 拒绝指向其他目录的联接，避免安装文件落在试验目录之外
    $current = $fullPath
    while ($current -and $current.Length -ge $projectRoot.Length) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "试验路径不能包含链接或目录联接: $current"
            }
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
    return $fullPath
}

function Invoke-EvaluationPython([string]$Executable, [string[]]$PythonArguments) {
    & $Executable @PythonArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Python 执行失败，退出码 $LASTEXITCODE；保留试验文件供检查"
    }
}

$candidateRoot = Confirm-EvaluationPath $candidateRoot
$venvPath = Confirm-EvaluationPath $venvPath
$plan = [ordered]@{
    candidate = $Candidate
    base_python = $basePython
    destination = $candidateRoot
    requirements = $requirementsPath
    model_revision = $model.revision
    download_budget_gb = $model.download_budget_gb
    disk_budget_gb = $model.disk_budget_gb
    starts_service = $false
    changes_game_config = $false
    execute = $Execute.IsPresent
}
$plan | ConvertTo-Json
if (-not $Execute) { return }

if (-not (Test-Path -LiteralPath $basePython -PathType Leaf)) {
    throw "未找到已核对的 Python 3.10: $basePython"
}
$version = & $basePython -I -c 'import sys; print(chr(46).join(map(str, sys.version_info[:3])))'
if ($LASTEXITCODE -ne 0 -or $version.Trim() -ne '3.10.12') {
    throw '基础解释器与本次方案不一致，请重新核对；未开始安装'
}
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($candidateRoot))
if ($drive.AvailableFreeSpace -lt ([double]$model.disk_budget_gb * 1GB)) {
    throw '磁盘可用空间低于本次试验预留量；未开始安装'
}

$cachePath = Confirm-EvaluationPath (Join-Path $candidateRoot 'cache')
$tempPath = Confirm-EvaluationPath (Join-Path $candidateRoot 'temp')
foreach ($directory in @($candidateRoot, $cachePath, $tempPath)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

# 环境变量只影响本次脚本及子进程，结束后恢复原值
$processSettings = @{
    TEMP = $tempPath
    TMP = $tempPath
    HF_HOME = (Join-Path $cachePath 'huggingface')
    TORCH_HOME = (Join-Path $cachePath 'torch')
    XDG_CACHE_HOME = $cachePath
    PYTHONNOUSERSITE = '1'
    PYTHONDONTWRITEBYTECODE = '1'
    PIP_CONFIG_FILE = 'NUL'
}
$previousSettings = @{}
foreach ($name in $processSettings.Keys) {
    $previousSettings[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $processSettings[$name], 'Process')
}
try {
    if (Test-Path -LiteralPath $venvPath) {
        throw '已有试验环境，保留原目录并停止；请先核对上次安装结果'
    }
    Invoke-EvaluationPython $basePython @('-I', '-m', 'venv', $venvPath)
    $venvPython = Confirm-EvaluationPath (Join-Path $venvPath 'Scripts/python.exe')
    # 使用独立 venv，不激活或修改当前 Python 环境
    $pipCommon = @('-I', '-m', 'pip', '--isolated', '--disable-pip-version-check', '--cache-dir', (Join-Path $cachePath 'pip'))
    Invoke-EvaluationPython $venvPython ($pipCommon + @('install', '--no-input', '--index-url', 'https://pypi.org/simple', 'pip==25.3'))
    Invoke-EvaluationPython $venvPython ($pipCommon + @('install', '--no-input', '--index-url', 'https://pypi.org/simple', '--requirement', $requirementsPath))
    Invoke-EvaluationPython $venvPython ($pipCommon + @('check'))
    $installed = & $venvPython -I -m pip --isolated --disable-pip-version-check freeze --all
    if ($LASTEXITCODE -ne 0) { throw '无法记录已安装依赖；保留试验目录供检查' }
    [IO.File]::WriteAllLines((Join-Path $candidateRoot 'installed-packages.txt'), [string[]]$installed, [Text.UTF8Encoding]::new($false))
    Invoke-EvaluationPython $venvPython @('-I', (Join-Path $PSScriptRoot 'download_models.py'), '--candidate', $Candidate, '--execute')
    $plan['requirements_sha256'] = (Get-FileHash -LiteralPath $requirementsPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $plan['completed_at'] = (Get-Date).ToUniversalTime().ToString('o')
    $plan | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $candidateRoot 'installation.json') -Encoding UTF8
    Write-Host '独立试听环境准备完成；尚未启动服务、生成语音或修改游戏配置'
} finally {
    foreach ($name in $previousSettings.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousSettings[$name], 'Process')
    }
}
