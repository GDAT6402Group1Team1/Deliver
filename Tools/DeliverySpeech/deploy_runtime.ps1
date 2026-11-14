param([string]$ProjectRoot = (Join-Path $PSScriptRoot '../..'))
$ErrorActionPreference = 'Stop'
$projectPath = [IO.Path]::GetFullPath($ProjectRoot)
$speechTools = Join-Path $projectPath 'Tools/DeliverySpeech'
$preparedPath = Join-Path $projectPath 'Saved/DeliverySpeech/runtime'
$destination = Join-Path $projectPath 'Binaries/ThirdParty/DeliverySpeech'
$manifestPath = Join-Path $speechTools 'runtime_manifest.json'
$buildPath = Join-Path $preparedPath 'build.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$build = Get-Content -LiteralPath $buildPath -Raw -Encoding UTF8 | ConvertFrom-Json

function Get-VerifiedHash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing prepared file: $Path" }
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $Expected.ToLowerInvariant()) { throw "Prepared file hash mismatch: $Path" }
    return $hash
}

if ($build.abi_version -ne 1 -or $manifest.abi_version -ne 1) { throw 'Unsupported speech runtime ABI' }
Get-VerifiedHash $manifestPath $build.manifest_sha256 | Out-Null
Get-VerifiedHash (Join-Path $speechTools 'native/delivery_speech_runtime.h') $build.abi_header_sha256 | Out-Null
$expectedExports = @('dsr_abi_version','dsr_begin','dsr_cancel','dsr_create','dsr_destroy','dsr_finish','dsr_get_error','dsr_get_result','dsr_push')
if (@(Compare-Object ($expectedExports | Sort-Object) ($build.exports | Sort-Object)).Count) {
    throw 'Prepared DLL export list does not match ABI v1'
}
if ($build.dependencies -match '(?i)onnxruntime\.dll') { throw 'Prepared DLL must not depend on onnxruntime.dll' }

# 只部署运行文件，SDK、下载包和测试录音留在 Saved
$files = @([pscustomobject]@{
    source = (Join-Path $preparedPath 'DeliverySpeechRuntime.dll')
    relative = 'DeliverySpeechRuntime.dll'
    sha256 = $build.dll_sha256
})
foreach ($group in @(@{prefix='models'; entries=$manifest.model.files}, @{prefix='licenses'; entries=$manifest.licenses})) {
    foreach ($entry in $group.entries) {
        if ([IO.Path]::GetFileName($entry.name) -ne $entry.name) { throw 'Manifest entries must be plain file names' }
        $relative = $group.prefix + '/' + $entry.name
        $path = Join-Path $preparedPath $relative
        if ((Get-Item -LiteralPath $path).Length -ne $entry.size) { throw "Prepared file size mismatch: $path" }
        $files += [pscustomobject]@{source=$path; relative=$relative; sha256=$entry.sha256}
    }
}
$files += [pscustomobject]@{source=$buildPath; relative='build.json'; sha256=(Get-FileHash -LiteralPath $buildPath).Hash}
$files += [pscustomobject]@{source=$manifestPath; relative='runtime_manifest.json'; sha256=$build.manifest_sha256}
foreach ($file in $files) { Get-VerifiedHash $file.source $file.sha256 | Out-Null }

# 校验完整后才覆盖已部署文件，原始清单和许可随包保留
foreach ($file in $files) {
    $targetPath = Join-Path $destination $file.relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force | Out-Null
    Copy-Item -LiteralPath $file.source -Destination $targetPath -Force
    Get-VerifiedHash $targetPath $file.sha256 | Out-Null
}
$report = [ordered]@{
    directory = $destination
    abi_version = $build.abi_version
    dll_sha256 = $build.dll_sha256
    files = @($files | Select-Object relative,sha256)
}
[IO.File]::WriteAllText((Join-Path $projectPath 'Saved/DeliverySpeech/deployment.json'),
    ($report | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
Write-Output "Deployed $($files.Count) verified speech runtime files: $destination"
