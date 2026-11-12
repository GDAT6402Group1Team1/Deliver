param(
    [ValidateSet('codex-local', 'fixture', 'llm', 'jev', 'hybrid')]
    [string]$Provider = 'codex-local',
    [ValidateRange(1024, 65535)]
    [int]$Port = 8765,
    [string]$PythonExecutable = 'python.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$serverPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'server.py'))
$logsPath = Join-Path $projectRoot 'Saved/Logs'
$stdoutPath = Join-Path $logsPath 'AIResidentBackend.log'
$stderrPath = Join-Path $logsPath 'AIResidentBackend.stderr.log'

function Get-BackendListeners {
    @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
}

function Test-ProjectBackend([int]$BackendProcessId) {
    $processInfo = Get-CimInstance Win32_Process -Filter "ProcessId=$BackendProcessId"
    if (-not $processInfo -or $processInfo.Name -notmatch '^python(?:w|[0-9.]*)?\.exe$') {
        return $false
    }
    # 比较完整脚本参数，不能只凭端口或 server.py 文件名认领其他服务
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
        return
    }
    throw "端口 $Port 已被其他进程占用，未启动或停止任何服务"
}

$pythonCommand = Get-Command $PythonExecutable -CommandType Application -ErrorAction Stop
New-Item -ItemType Directory -Path $logsPath -Force | Out-Null

# 保留旧日志，避免重启后丢失此前失败请求的证据
$archiveSuffix = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
foreach ($logPath in @($stdoutPath, $stderrPath)) {
    if (Test-Path -LiteralPath $logPath) {
        Copy-Item -LiteralPath $logPath -Destination "$logPath.$archiveSuffix.bak"
    }
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
