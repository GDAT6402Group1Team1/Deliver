# 持续运行 Windows 语音服务，将合成的声音交给游戏播放
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$voices = @($synth.GetInstalledVoices() | Where-Object Enabled | ForEach-Object { $_.VoiceInfo.Name })
try {
    if ($voices -contains 'Microsoft Kangkang') { $synth.SelectVoice('Microsoft Kangkang') }
    else { $synth.SelectVoice('Microsoft Huihui Desktop') }
    $synth.SetOutputToNull()
    @{ ready = $true; voices = $voices } | ConvertTo-Json -Compress | ForEach-Object { [Console]::WriteLine($_) }
    while ($null -ne ($line = [Console]::ReadLine())) {
        $buffer = New-Object System.IO.MemoryStream
        try {
            $request = $line | ConvertFrom-Json
            if ($request.text.Length -gt 120) { throw 'text_too_long' }
            $preferred = if ($request.voice -eq 'female') { 'Microsoft Huihui' } else { 'Microsoft Kangkang' }
            if ($voices -contains $preferred) { $synth.SelectVoice($preferred) }
            else { $synth.SelectVoice('Microsoft Huihui Desktop') }
            $synth.Rate = if ($request.emotion -eq 'Angry') { 1 } else { 0 }
            $synth.SetOutputToAudioStream($buffer, $format)
            $synth.Speak([string]$request.text)
            $synth.SetOutputToNull()
            @{ pcm = [Convert]::ToBase64String($buffer.ToArray()); sample_rate = 16000; voice_name = $synth.Voice.Name } | ConvertTo-Json -Compress | ForEach-Object { [Console]::WriteLine($_) }
        } catch {
            [Console]::WriteLine('{"error":"speech_failed"}')
        } finally { $buffer.Dispose() }
    }
} finally { $synth.Dispose() }
