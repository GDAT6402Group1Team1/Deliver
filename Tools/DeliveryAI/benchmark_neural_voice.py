"""检查正式 B 音色进程的合成、打断和恢复，结果保存为 JSON 和 WAV"""
import hashlib
import json
from pathlib import Path
import threading
import time
import wave

from neural_tts import QwenSpeechSynthesizer


def main():
    output = Path(__file__).resolve().parents[2] / 'Saved/AIResearch/NeuralVoiceIntegration'
    output.mkdir(parents=True, exist_ok=True)
    report = {'status': 'started', 'voice': 'Uncle_Fu', 'samples': [], 'game_verified': False}
    report_path = output / 'latest.json'
    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    synth = QwenSpeechSynthesizer()
    cancel = threading.Event()
    def speak(name, text):
        started = time.monotonic()
        pcm, rate = synth.synthesize(text, 'male', 'Calm', threading.Event())
        row = {'name': name, 'text': text, 'synthesis_ms': (time.monotonic()-started)*1000,
               'sample_rate': rate, 'duration_s': len(pcm)/(rate*2), 'bytes': len(pcm),
               'pcm_sha256': hashlib.sha256(pcm).hexdigest(), 'voice_name': synth.voice_name}
        path = output / (name+'.wav')
        with wave.open(str(path), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(rate)
            audio.writeframes(pcm)
        row['wav'] = str(path)
        if name == 'accepted_b':
            accepted = output.parent / 'TtsEvaluation/qwen/graph-comparisons/20261112T202452462720Z/Uncle_Fu_graph_1.wav'
            if accepted.is_file():
                with wave.open(str(accepted), 'rb') as reference:
                    row['matches_accepted_sample'] = (reference.getframerate() == rate and
                        reference.readframes(reference.getnframes()) == pcm)
        report['samples'].append(row)
        save()
        print(json.dumps(row, ensure_ascii=False), flush=True)
    try:
        started = time.monotonic()
        synth.prepare(cancel)
        report['prepare_ms'] = (time.monotonic()-started)*1000
        save()
        speak('accepted_b', '小芳，你别急，先听我把话说完。')
        # 正在合成时打断，下一条必须等它真正退出，不能收到旧声音
        result = {}
        entered = threading.Event()
        def interrupted():
            entered.set()
            try:
                pcm, _ = synth.synthesize('你先别走，这件事情咱们还没有说清楚呢。', 'male', 'Calm', cancel)
                result['unexpected_pcm_bytes'] = len(pcm)
            except Exception as error:
                result['error'] = type(error).__name__
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
        if task.is_alive() or 'unexpected_pcm_bytes' in result or 'error' not in result:
            raise RuntimeError('cancel_test_failed')
        speak('after_interrupt', '行，那你先忙。')
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
        save()
    print(report_path, flush=True)


if __name__ == '__main__':
    main()
