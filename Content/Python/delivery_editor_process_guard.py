"""静态资源操作前核对独立编辑器窗口，只查询进程，不停止或启动进程"""
import os
import json
import subprocess


def assert_exclusive_process():
    """只读核对独立 UE 窗口，拒绝其他编辑器和编译器，不退出任何进程"""
    query = r'''
    $ErrorActionPreference = 'Stop'
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $taskSelfPid = CURRENT_PID
    $names = @(Get-Process | Where-Object {
        $_.Id -ne $taskSelfPid -and
        $_.ProcessName -match '^(UnrealEditor(?:-Cmd)?|UnrealBuildTool|UnrealHeaderTool|LiveCodingConsole|LiveCodingServer)$'
    } | Select-Object @{Name='Name';Expression={$_.ProcessName + '.exe'}},@{Name='ProcessId';Expression={$_.Id}})
    $hosted = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ProcessId -ne $taskSelfPid -and $_.ProcessId -ne $PID -and (
        $_.Name -match '^(UnrealEditor(?:-Cmd)?|UnrealBuildTool|UnrealHeaderTool|LiveCodingConsole|LiveCodingServer)\.exe$' -or
        ($_.Name -match '^(dotnet|MSBuild|cmd|powershell|pwsh)\.exe$' -and
         $_.CommandLine -match '(UnrealEditor|UnrealBuildTool|UnrealHeaderTool|LiveCodingConsole|LiveCodingServer|DeliveryEditor)') -or
        ($_.Name -match '^(dotnet|MSBuild)\.exe$' -and [string]::IsNullOrWhiteSpace($_.CommandLine)))
    } | Select-Object Name,ProcessId)
    [ordered]@{names=$names;hosted=$hosted} | ConvertTo-Json -Depth 4 -Compress
    '''.replace('CURRENT_PID', str(os.getpid()))
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', query],
                            check=True, capture_output=True, text=True, encoding='utf-8', timeout=30)
    snapshot = json.loads(result.stdout.strip())
    if not isinstance(snapshot, dict) or any(not isinstance(snapshot.get(k), list) for k in ('names', 'hosted')):
        raise RuntimeError('无法核对 UE/编译器进程')
    if snapshot['names'] or snapshot['hosted']:
        raise RuntimeError('其他 UE/编译器仍在运行：' + json.dumps(snapshot, ensure_ascii=False))
    return snapshot
