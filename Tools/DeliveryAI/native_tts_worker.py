"""原生语音独立进程，输入线程接收取消，合成线程按顺序处理台词"""
import argparse
from contextlib import redirect_stdout
import json
import queue
import sys
import threading
import time
from types import SimpleNamespace

from neural_tts import NeuralTtsError, load_voice_config, neural_voice_speakers
from neural_tts_worker import WorkerRequests
from native_tts import load_runtime, native_identities
from native_tts_engine import NativeVoiceEngines


def warm_up_voices(engine, speakers, closed):
    """用一条短句准备首次解码，生成的声音留在进程内"""
    voices = ('male', 'female') if engine.voice_residency == 'both' else ('male',)
    for voice in voices:
        task = SimpleNamespace(task_id=f'warmup_{voice}', text='听得见吗？', voice=voice, speaker=speakers[voice],
                               cancel=threading.Event(), chunk_frames=8)
        result = engine.synthesize(task, closed, lambda _message: None)
        metrics = result['stream_metrics']
        print(f'native tts voice_ready voice={voice} first_audio_ms={metrics["first_audio_ms"]:.0f} '
              f'synthesis_ms={metrics["synthesis_ms"]:.0f}', file=sys.stderr, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--runtime', required=True)
    args = parser.parse_args()
    output, output_lock = sys.stdout, threading.Lock()

    def send(message):
        with output_lock:
            output.write(json.dumps(message, ensure_ascii=False, separators=(',', ':')) + '\n')
            output.flush()

    config = load_voice_config(args.config)
    speakers = neural_voice_speakers(config)
    requests = WorkerRequests(send, speakers)
    engine = None

    def read_input():
        try:
            while not requests.closed.is_set():
                line = sys.stdin.readline(8193)
                if not line:
                    break
                if len(line) > 8192 or not line.endswith('\n'):
                    raise ValueError('command_too_large')
                requests.receive(json.loads(line))
        except (OSError, ValueError, queue.Full):
            pass
        finally:
            requests.close()

    try:
        # NumPy 先完成导入，再让输入线程阻塞读取 Windows 标准输入
        import numpy
        threading.Thread(target=read_input, name='native-qwen-input', daemon=True).start()
        runtime = load_runtime(args.runtime, verify=True)
        with redirect_stdout(sys.stderr):
            engine = NativeVoiceEngines(config, runtime)
            warm_up_voices(engine, speakers, requests.closed)
        send({'type': 'ready', 'protocol_version': 1, 'audio_stream_version': 1,
              'voice_name': 'Uncle_Fu', 'voices': speakers, 'sample_rate': 24000,
              'voice_identities': native_identities(config, runtime)})
        while not requests.closed.is_set() or not requests.pending.empty():
            try:
                task = requests.pending.get(timeout=.1)
            except queue.Empty:
                continue
            started = time.monotonic()
            try:
                with redirect_stdout(sys.stderr):
                    message = engine.synthesize(task, requests.closed, send)
            except Exception as error:
                code = error.code if isinstance(error, NeuralTtsError) else 'neural_tts_generation_failed'
                print(f'native qwen failed={code} detail={error!r}', file=sys.stderr, flush=True)
                message = {'type': 'cancelled' if code == 'cancelled' else 'error', 'task_id': task.task_id, 'code': code}
            finally:
                requests.finish(task)
            if task.cancel.is_set() or requests.closed.is_set():
                message = {'type': 'cancelled', 'task_id': task.task_id, 'code': 'cancelled'}
            message['elapsed_ms'] = int((time.monotonic() - started) * 1000)
            send(message)
        return 0
    except Exception as error:
        code = error.code if isinstance(error, NeuralTtsError) else 'native_tts_startup_failed'
        print(f'native qwen startup failed={error!r}', file=sys.stderr, flush=True)
        send({'type': 'error', 'code': code})
        return 1
    finally:
        requests.close()
        if engine:
            engine.close()


if __name__ == '__main__':
    raise SystemExit(main())
