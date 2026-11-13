param(
    [ValidateSet('codex-local', 'fixture', 'llm', 'jev', 'hybrid')]
    [string]$Provider = 'codex-local',
    [string]$Model = 'gpt-6.1-sol',
    [string]$PlayerModel = 'gpt-6.1-sol',
    [ValidateSet('qwen', 'windows')]
    [string]$SpeechProvider = 'qwen',
    [ValidateRange(1024, 65535)]
    [int]$Port = 8765,
    [string]$PythonExecutable = 'python.exe',
    # 把每轮候选、现场事件、选中的动作和台词写入 Saved/Logs/NpcDialogue.ndjson，排查对话问题时打开
    [switch]$DialogueLog,
    # 启用后端句内音频分段，游戏端还需单独打开对应设置
    [switch]$StreamAudio
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$serverPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'server.py'))
$logsPath = Join-Path $projectRoot 'Saved/Logs'
$stdoutPath = Join-Path $logsPath 'AIResidentBackend.log'
$stderrPath = Join-Path $logsPath 'AIResidentBackend.stderr.log'

function Get-ConfiguredBackendPort([string]$ConfigPath) {
    if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) { return 8765 }
    $inAiSection = $false
    foreach ($line in [IO.File]::ReadLines($ConfigPath)) {
        if ($line -match '^\s*\[([^\]]+)\]') {
            if ($inAiSection) { break }
            $inAiSection = $Matches[1] -eq '/Script/Delivery.DeliveryAiSettings'
            continue
        }
        if ($inAiSection -and $line -match '^\s*DecisionEndpoint\s*=\s*"?([^"\s]+)"?\s*(?:[;#].*)?$') {
            $endpoint = $null
            if ([Uri]::TryCreate($Matches[1], [UriKind]::Absolute, [ref]$endpoint) -and
                $endpoint.Scheme -in @('http', 'https') -and
                $endpoint.Host -in @('localhost', '127.0.0.1') -and
                $endpoint.Port -ge 1024 -and $endpoint.Port -le 65535) {
                return $endpoint.Port
            }
        }
    }
    return 8765
}

# 未指定端口时，使用工程配置中的本机后端端口
if (-not $PSBoundParameters.ContainsKey('Port')) {
    $Port = Get-ConfiguredBackendPort (Join-Path $projectRoot 'Config/DefaultGame.ini')
}

function Get-BackendListeners {
    @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
}

function Test-ProjectBackend([int]$BackendProcessId) {
    $processInfo = Get-CimInstance Win32_Process -Filter "ProcessId=$BackendProcessId"
    if (-not $processInfo -or $processInfo.Name -notmatch '^python(?:w|[0-9.]*)?\.exe$') {
        return $false
    }
    # 核对完整脚本路径和参数，避免把其他工程的服务当成本工程后端
    $normalizedCommand = ([string]$processInfo.CommandLine).Replace('\', '/')
    $normalizedScript = $serverPath.Replace('\', '/')
    $scriptArgument = '(?i)(?:^|[\s"])' + [regex]::Escape($normalizedScript) + '(?:[\s"]|$)'
    return $normalizedCommand -match $scriptArgument
}

$listeners = @(Get-BackendListeners)
if ($listeners.Count -gt 0) {
    $ownerIds = @($listeners | Select-Object -ExpandProperty OwningProcess -Unique)
    if ($ownerIds.Count -eq 1 -and (Test-ProjectBackend $ownerIds[0])) {
        Write-Host "AI 后端已运行，PID=$($ownerIds[0])，端口=$Port"
        if ($PSBoundParameters.ContainsKey('StreamAudio')) {
            Write-Host '本次未改变运行中的音频分段设置。请先按项目流程确认并停止本工程后端，再带此开关启动；脚本不会自动重启服务'
        }
        return
    }
    throw "端口 $Port 已被其他进程占用，未启动或停止任何服务"
}

# 同名 Python 可能出现在多个 PATH 目录，使用系统查找顺序中的第一个
$pythonCommand = Get-Command $PythonExecutable -CommandType Application -ErrorAction Stop | Select-Object -First 1
$env:DELIVERY_CODEX_MODEL = $Model
if (-not $PSBoundParameters.ContainsKey('PlayerModel') -and $env:DELIVERY_CODEX_PLAYER_MODEL) {
    $PlayerModel = $env:DELIVERY_CODEX_PLAYER_MODEL
}
$env:DELIVERY_CODEX_PLAYER_MODEL = $PlayerModel
$env:DELIVERY_AI_DIALOGUE_LOG = if ($DialogueLog) { '1' } else { '' }
$env:DELIVERY_TTS_PROVIDER = $SpeechProvider
if ($SpeechProvider -eq 'qwen') {
    $voicePython = Join-Path $projectRoot 'Saved/AIResearch/TtsEvaluation/qwen/.venv/Scripts/python.exe'
    $voiceModel = Join-Path $projectRoot 'Saved/AIResearch/TtsEvaluation/qwen/model/config.json'
    if (-not (Test-Path -LiteralPath $voicePython -PathType Leaf) -or
        -not (Test-Path -LiteralPath $voiceModel -PathType Leaf)) {
        throw '缺少已安装的 Qwen 语音环境。请先完成独立语音环境安装；需要原声音时显式使用 -SpeechProvider windows'
    }
}
New-Item -ItemType Directory -Path $logsPath -Force | Out-Null

# 保留旧日志，方便重启后检查之前的失败原因
$archiveSuffix = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
foreach ($logPath in @($stdoutPath, $stderrPath)) {
    if (Test-Path -LiteralPath $logPath) {
        Copy-Item -LiteralPath $logPath -Destination "$logPath.$archiveSuffix.bak"
    }
}

# 未传开关时沿用环境变量，显式传入 false 时关闭分段
if ($PSBoundParameters.ContainsKey('StreamAudio')) {
    $env:DELIVERY_AI_STREAM_AUDIO = if ($StreamAudio) { '1' } else { '0' }
}

# 单独后台进程，不设置开机启动，不依赖当前 PowerShell 窗口继续存在
$backendProcess = Start-Process -FilePath $pythonCommand.Source `
    -ArgumentList @('-X', 'utf8', '-u', ('"' + $serverPath + '"'), '--provider', $Provider, '--port', "$Port") `
    -WorkingDirectory $projectRoot -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru

$startupTimer = [Diagnostics.Stopwatch]::StartNew()
while ($startupTimer.Elapsed.TotalSeconds -lt 5) {
    $backendProcess.Refresh()
    if ($backendProcess.HasExited) {
        throw "AI 后端启动失败，退出码 $($backendProcess.ExitCode)，请查看 $stderrPath"
    }
    $listeners = @(Get-BackendListeners)
    if ($listeners | Where-Object { $_.OwningProcess -eq $backendProcess.Id }) {
        Write-Host "AI 后端已启动，PID=$($backendProcess.Id)，http://127.0.0.1:$Port"
        Write-Host "日志：$stdoutPath"
        return
    }
    if ($listeners.Count -gt 0) {
        throw "启动期间端口 $Port 被其他进程占用，未停止该进程，请查看 $stderrPath"
    }
    Start-Sleep -Milliseconds 200
}
throw "5 秒内未确认 AI 后端监听端口 $Port；进程 PID=$($backendProcess.Id)，请查看 $stderrPath"
