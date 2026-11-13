"""用两种人物描述生成小芳试听，保存原始声音、固定参数和资源占用"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

from download_models import checked_path, verify
from evaluate import (GpuSampler, offline_environment, package_versions, process_memory,
                      save_json, torch_memory, write_audio)
from prepare_voice_design import plan


def assert_game_and_speech_stopped():
    # 本脚本不停止其他进程，避免试听占用玩家正在使用的显卡
    command = "Get-CimInstance Win32_Process | Where-Object {$_.Name -like 'UnrealEditor*' -or $_.Name -eq 'Delivery.exe' -or ($_.Name -like 'python*' -and $_.CommandLine -like '*neural_tts_worker.py*')} | Select-Object ProcessId,Name | ConvertTo-Json -Compress"
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command],
                            capture_output=True, text=True, timeout=15, check=True,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.stdout.strip():
        raise RuntimeError('编辑器、游戏或正式语音进程仍在运行，保留当前试玩：' + result.stdout.strip())


@contextmanager
def complete_generation(model, stopping_criterion, stopping_list, deadline):
    talker = model.model.talker
    had_value, previous = 'generate' in vars(talker), vars(talker).get('generate')
    generate = talker.generate
    class Deadline(stopping_criterion):
        def __call__(self, input_ids, scores, **kwargs):
            if time.monotonic() >= deadline:
                raise TimeoutError('本条声音超过 90 秒，保留记录，不输出截断音频')
            return False
    def checked(*args, **kwargs):
        kwargs['stopping_criteria'] = stopping_list([*(kwargs.get('stopping_criteria') or []), Deadline()])
        result = generate(*args, **kwargs)
        eos = model.model.config.talker_config.codec_eos_token_id
        if not bool((result.sequences == eos).any().item()):
            raise RuntimeError('达到生成长度上限但台词未正常结束，不保存为完成的声音')
        return result
    talker.generate = checked
    try:
        yield
    finally:
        if had_value:
            talker.generate = previous
        else:
            delattr(talker, 'generate')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true', help='确认已结束游戏和正式语音后，开始 GPU 试听')
    args = parser.parse_args()
    project, root, specification, files = plan()
    if not args.execute:
        print(json.dumps({'status': 'plan_only', 'model': specification['model_id'],
                          'voices': specification['voices'], 'sentences': specification['sentences'],
                          'runtime': str(project / 'Saved/AIResearch/TtsEvaluation/qwen/.venv/Scripts/python.exe'),
                          'production_changed': False}, ensure_ascii=False, indent=2))
        return
    environment = checked_path(project / 'Saved/AIResearch/TtsEvaluation/qwen/.venv', project)
    if Path(sys.prefix).resolve() != environment.resolve():
        raise RuntimeError('请使用已经安装的 Qwen 试验环境，不在其他 Python 环境加载模型')
    installation = json.loads((environment.parent / 'installation.json').read_text(encoding='utf-8-sig'))
    if not installation.get('completed_at') or installation.get('model_revision') != specification['reuse_revision']:
        raise ValueError('原 Qwen 环境安装记录不一致')
    downloads = json.loads((root / 'model-downloads.json').read_text(encoding='utf-8-sig'))
    if downloads.get('status') != 'completed' or downloads.get('revision') != specification['revision']:
        raise ValueError('VoiceDesign 下载尚未完成或版本不一致')
    assert_game_and_speech_stopped()
    model_dir = checked_path(root / 'model', project)
    for entry in files:
        verify(checked_path(model_dir / entry['path'], project), entry)
    for name in ('temp', 'cache'):
        checked_path(root / name, project).mkdir(exist_ok=True)
    offline_environment(root, specification['settings']['threads'])
    packages = package_versions()
    expected = {'torch': '2.7.1+cu128', 'qwen-tts': '0.1.1', 'transformers': '4.57.3'}
    if any(packages.get(name) != version for name, version in expected.items()):
        raise ValueError('Qwen 试验依赖版本已变化')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = checked_path(root / 'results' / stamp, project)
    output.mkdir(parents=True, exist_ok=False)
    report_path = output / 'evaluation.json'
    report = {'status': 'started', 'model_id': specification['model_id'], 'revision': specification['revision'],
              'license': specification['license'], 'settings': specification['settings'],
              'descriptions': specification['voices'], 'packages': packages, 'samples': [],
              'subjective_review': '等待用户试听', 'game_verified': False,
              'processing': '原采样率、原音量、原音高，不裁剪停顿或截断台词',
              'implementation': '官方 0.1.1 生成流程，SDPA/bfloat16，未启用 CUDA Graph',
              'memory_note': 'Torch 峰值包含本模型权重；GPU 定时采样还包含其他程序，可能漏掉短暂峰值',
              'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'specification_sha256': hashlib.sha256(Path(__file__).with_name('voice_design.json').read_bytes()).hexdigest()}
    def save():
        save_json(report_path, report)
        save_json(root / 'latest-audition.json', {'report': str(report_path), 'status': report['status']})
    save()
    sampler = GpuSampler()
    try:
        started = time.perf_counter()
        import torch
        from qwen_tts import Qwen3TTSModel
        from transformers import StoppingCriteria, StoppingCriteriaList
        report['import_ms'] = (time.perf_counter() - started) * 1000
        assert_game_and_speech_stopped()
        torch.set_num_threads(specification['settings']['threads'])
        torch.set_num_interop_threads(1)
        if not torch.cuda.is_available():
            raise RuntimeError('未找到 CUDA，没有自动改用 CPU')
        torch.cuda.set_device(0)
        torch.cuda.reset_peak_memory_stats(0)
        sampler.start()
        started = time.perf_counter()
        model = Qwen3TTSModel.from_pretrained(str(model_dir), device_map='cuda:0', dtype=torch.bfloat16,
                                            attn_implementation='sdpa', local_files_only=True)
        torch.cuda.synchronize()
        if model.model.tts_model_type != 'voice_design':
            raise ValueError('加载的模型类型不是 VoiceDesign')
        report['model_load_ms'] = (time.perf_counter() - started) * 1000
        report['load_torch_memory'] = torch_memory(torch)
        report['load_process_memory'] = process_memory()
        save()
        settings = {name: value for name, value in specification['settings'].items() if name != 'threads'}
        for voice in specification['voices']:
            for index, sentence in enumerate(specification['sentences']):
                assert_game_and_speech_stopped()
                seed = voice['seed'] + index
                torch.manual_seed(seed)
                torch.cuda.manual_seed_all(seed)
                torch.cuda.reset_peak_memory_stats(0)
                torch.cuda.synchronize()
                started = time.perf_counter()
                print(f"开始试听 {voice['id']}_{sentence['id']}", flush=True)
                with torch.inference_mode(), complete_generation(
                        model, StoppingCriteria, StoppingCriteriaList, time.monotonic() + 90):
                    wavs, rate = model.generate_voice_design(text=sentence['text'], language='Chinese',
                                                           instruct=voice['description'], non_streaming_mode=True,
                                                           **settings)
                torch.cuda.synchronize()
                elapsed = (time.perf_counter() - started) * 1000
                if len(wavs) != 1 or rate != 24000:
                    raise ValueError('试听输出格式与预期不一致')
                name = f"Xiaofang_{voice['id']}_{sentence['id']}"
                audio = write_audio(output / f'{name}.wav', wavs[0], rate)
                row = {'voice': voice['id'], 'text': sentence['text'], 'description': voice['description'],
                       'seed': seed, 'synthesis_ms': elapsed, 'first_synthesis': not report['samples'],
                       **audio, 'torch_memory': torch_memory(torch), 'process_memory': process_memory()}
                report['samples'].append(row)
                save()
                print(json.dumps({'voice': voice['id'], 'sentence': sentence['id'], 'synthesis_ms': elapsed,
                                  'duration_s': audio['duration_s'], 'wav': audio['wav']}, ensure_ascii=False), flush=True)
        report['status'] = 'completed'
    except BaseException as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        report['gpu_sampling'] = sampler.finish()
        save()
        print(str(report_path), flush=True)


if __name__ == '__main__':
    main()
