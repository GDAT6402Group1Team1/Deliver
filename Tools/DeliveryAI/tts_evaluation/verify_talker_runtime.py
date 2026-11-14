"""检查可选 Talker Graph 的长句、取消和下一句，正式配置保持原样"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import uuid

from compare_stream_runtime import AI_ROOT, CASES, ROOT, existing_workers, load_voice_config, run_case, save_json


def memory(torch):
    free, total = torch.cuda.mem_get_info(0)
    return dict(allocated_bytes=torch.cuda.memory_allocated(0), reserved_bytes=torch.cuda.memory_reserved(0),
                peak_allocated_bytes=torch.cuda.max_memory_allocated(0),
                peak_reserved_bytes=torch.cuda.max_memory_reserved(0), device_free_bytes=free, device_total_bytes=total)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu', action='store_true')
    parser.add_argument('--release-unused-cache', action='store_true', help='预热后仅清理一次未使用的 CUDA 缓存并比较下一句')
    parser.add_argument('--output', type=Path, default=ROOT / 'Saved/AIResearch/TalkerRuntime' /
                        datetime.now().strftime('%Y%m%d-%H%M%S'))
    args = parser.parse_args()
    config = load_voice_config()
    config['talker_mode'] = 'cuda_graph'
    output = args.output.resolve()
    output.relative_to(ROOT / 'Saved/AIResearch')
    output.mkdir(parents=True, exist_ok=False)
    report = dict(gpu_loaded=False, config_changed=False, ue_playback_measured=False, rows=[], cancellation=[])
    save_json(output / 'summary.json', report)
    if not args.gpu:
        print(json.dumps({'preflight': 'passed', 'summary': str(output / 'summary.json')}))
        return
    if workers := existing_workers():
        raise RuntimeError(f'语音进程仍在运行: {workers}')
    sys.path.insert(0, str(AI_ROOT))
    import neural_tts_worker as worker
    from neural_tts import NeuralTtsError
    closed = threading.Event()
    runtime = worker.QwenVoiceEngines(config, closed)
    report.update(gpu_loaded=True, memory_loaded=memory(runtime.custom.torch), voices=runtime.identities)
    try:
        warmup = output / 'warmup'
        warmup.mkdir()
        for voice in ('male', 'female'):
            case = next(case for case in CASES if case[0] == voice + '_short_1')
            row = run_case(worker, runtime.engines[voice], case, '8', warmup, profile=False)
            row['stage'] = 'warmup'
            report['rows'].append(row)
        report['memory_warm'] = memory(runtime.custom.torch)
        for stage in (('before_cache_release', 'after_cache_release') if args.release_unused_cache else ('short',)):
            if stage == 'after_cache_release':
                runtime.synchronize()
                runtime.custom.torch.cuda.empty_cache()
                report['memory_after_release'] = memory(runtime.custom.torch)
            folder = output / stage
            folder.mkdir()
            for voice in ('male', 'female'):
                case = next(case for case in CASES if case[0] == voice + '_short_1')
                row = run_case(worker, runtime.engines[voice], case, '8', folder, profile=False)
                row['stage'] = stage
                report['rows'].append(row)
                save_json(output / 'summary.json', report)
        long_folder = output / 'long'
        long_folder.mkdir()
        for voice in ('male', 'female'):
            case = next(case for case in CASES if case[0] == voice + '_long')
            row = run_case(worker, runtime.engines[voice], case, '8', long_folder, profile=False)
            row['stage'] = 'long'
            report['rows'].append(row)
            save_json(output / 'summary.json', report)
        for voice in ('male', 'female'):
            engine = runtime.engines[voice]
            original_forward = engine.model.model.talker.model.forward
            original_decode = engine.model.model.speech_tokenizer.decode
            task = SimpleNamespace(task_id=uuid.uuid4().hex, voice=voice,
                text=next(case[2] for case in CASES if case[0] == voice + '_long'),
                speaker=config['voices'][voice]['speaker'], chunk_frames=8, cancel=threading.Event())
            sent = []
            def stop_after_first(message):
                sent.append(time.perf_counter())
                task.cancel.set()
            try:
                runtime.synthesize(task, closed, stop_after_first)
            except NeuralTtsError as error:
                if error.code != 'cancelled' or len(sent) != 1:
                    raise
                stopped_ms = (time.perf_counter() - sent[0]) * 1000
            else:
                raise AssertionError('取消后不应返回成功')
            if (engine.model.model.talker.model.forward != original_forward
                    or engine.model.model.speech_tokenizer.decode != original_decode):
                raise AssertionError('取消后模型接口没有恢复')
            recovered = output / 'after_cancel'
            recovered.mkdir(exist_ok=True)
            case = next(case for case in CASES if case[0] == voice + '_short_1')
            row = run_case(worker, engine, case, '8', recovered, profile=False)
            row['stage'] = 'after_cancel'
            report['rows'].append(row)
            baseline = next(entry for entry in report['rows'] if entry['stage'] == 'warmup' and entry['voice'] == voice)
            report['cancellation'].append(dict(voice=voice, stopped_ms=stopped_ms, sent_chunks=len(sent),
                same_recovered_codec=row['codec_sha256'] == baseline['codec_sha256'],
                same_recovered_pcm=row['pcm_sha256'] == baseline['pcm_sha256']))
            save_json(output / 'summary.json', report)
        report.update(status='completed', memory_final=memory(runtime.custom.torch))
    except BaseException as error:
        report.update(status='failed', error=dict(type=type(error).__name__, message=str(error)))
        raise
    finally:
        closed.set()
        save_json(output / 'summary.json', report)
    print(json.dumps({'summary': str(output / 'summary.json'), 'status': report['status']}), flush=True)


if __name__ == '__main__':
    main()
