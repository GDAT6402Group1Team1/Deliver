"""检查正式 B 音色进程的合成、打断和恢复，结果保存为 JSON 和 WAV"""
import hashlib
import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import wave

from neural_tts import QwenSpeechSynthesizer, project_path
from tts_evaluation.evaluate import GpuSampler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--include-female', action='store_true', help='试听小芳的配置音色，并检查切回大军与取消')
    parser.add_argument('--config', help='试验配置的项目内路径，留空使用正式配置')
    parser.add_argument('--output', help='结果的项目内目录，留空沿用原位置')
    args = parser.parse_args()
    if os.name == 'nt':
        # 生成试验与游戏错开，避免语音对照占用玩家正在使用的 GPU
        processes = subprocess.run(['powershell', '-NoProfile', '-Command',
            "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'UnrealEditor*' -or ($_.Name -eq 'python.exe' -and $_.CommandLine -match 'neural_tts_worker.py') } | Select-Object -ExpandProperty ProcessId"],
            capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW, check=True)
        if processes.stdout.strip():
            raise RuntimeError('请先结束编辑器和正式语音进程，再运行独立对照')
    folder = 'FemaleVoiceIntegration' if args.include_female else 'NeuralVoiceIntegration'
    output = project_path(args.output) if args.output else Path(__file__).resolve().parents[2] / 'Saved/AIResearch' / folder
    output.mkdir(parents=True, exist_ok=True)
    report = {'status': 'started', 'voice': 'Uncle_Fu', 'samples': [], 'game_verified': False}
    report_path = output / 'latest.json'
    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    synth = QwenSpeechSynthesizer(config_path=args.config) if args.config else QwenSpeechSynthesizer()
    report['voices'] = synth.speakers
    report['config'] = args.config or 'Tools/DeliveryAI/neural_voice_config.json'
    sampler = GpuSampler()
    cancel = threading.Event()
    def speak(name, text, voice='male'):
        started = time.monotonic()
        pcm, rate = synth.synthesize(text, voice, 'Calm', threading.Event())
        row = {'name': name, 'text': text, 'synthesis_ms': (time.monotonic()-started)*1000,
               'sample_rate': rate, 'duration_s': len(pcm)/(rate*2), 'bytes': len(pcm),
               'pcm_sha256': hashlib.sha256(pcm).hexdigest(), 'voice_name': synth.voice_name,
               'voice': voice}
        path = output / (name+'.wav')
        with wave.open(str(path), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(rate)
            audio.writeframes(pcm)
        row['wav'] = str(path)
        if name == 'accepted_b':
            accepted = project_path('Saved/AIResearch/TtsEvaluation/qwen/graph-comparisons/20261112T202452462720Z/Uncle_Fu_graph_1.wav')
            if accepted.is_file():
                with wave.open(str(accepted), 'rb') as reference:
                    row['matches_accepted_sample'] = (reference.getframerate() == rate and
                        reference.readframes(reference.getnframes()) == pcm)
        report['samples'].append(row)
        save()
        print(json.dumps(row, ensure_ascii=False), flush=True)
    try:
        sampler.start()
        started = time.monotonic()
        synth.prepare(cancel)
        report['prepare_ms'] = (time.monotonic()-started)*1000
        save()
        speak('accepted_b', '小芳，你别急，先听我把话说完。')
        if args.include_female:
            speak('xiaofang_greeting', '哎，你找我什么事？', 'female')
            speak('xiaofang_clarification', '你别听他瞎说，我刚才说的是车。', 'female')
        # 正在合成时打断，下一条必须等它真正退出，不能收到旧声音
        result = {}
        entered = threading.Event()
        def interrupted():
            entered.set()
            try:
                voice = 'female' if args.include_female else 'male'
                pcm, _ = synth.synthesize('你先别走，这件事情咱们还没有说清楚呢。', voice, 'Calm', cancel)
                result['unexpected_pcm_bytes'] = len(pcm)
            except Exception as error:
                result['error'] = type(error).__name__
                result['error_code'] = getattr(error, 'code', None)
        task = threading.Thread(target=interrupted)
        task.start()
        if not entered.wait(2):
            raise TimeoutError('cancel_test_not_started')
        time.sleep(.35)
        stopped_at = time.monotonic()
        cancel.set()
        task.join(5)
        result['cancel_to_exit_ms'] = (time.monotonic()-stopped_at)*1000
        result['thread_exited'] = not task.is_alive()
        report['interruption'] = result
        save()
        if task.is_alive() or 'unexpected_pcm_bytes' in result or result.get('error_code') != 'cancelled':
            raise RuntimeError('cancel_test_failed')
        speak('after_interrupt', '行，那你先忙。')
        if args.include_female:
            speak('xiaofang_after_interrupt', '哎，你找我什么事？', 'female')
            first = next(row for row in report['samples'] if row['name'] == 'xiaofang_greeting')
            report['female_repeat_matches'] = first['pcm_sha256'] == report['samples'][-1]['pcm_sha256']
        report['status'] = 'completed'
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        cancel.set()
        started = time.monotonic()
        synth.close()
        synth.close()
        report['close_ms'] = (time.monotonic()-started)*1000
        report['gpu_sampling'] = sampler.finish()
        save()
    print(report_path, flush=True)


if __name__ == '__main__':
    main()
