"""用已选小芳 B 建立一次参考，检查不同台词、打断后恢复和音色一致性"""
import argparse
from contextlib import contextmanager, nullcontext
from datetime import datetime, timezone
import hashlib
import importlib
import json
from pathlib import Path
import sys
import threading
import time
import wave

from audition_voice_design import assert_game_and_speech_stopped
from download_models import checked_path, verify
from evaluate import (GpuSampler, offline_environment, package_versions, process_memory,
                      save_json, torch_memory, write_audio)
from prepare_voice_design import plan
from compare_qwen_graph import checked_vendor, fp32_graph_sampling, use_predictor_graph


class VoiceCloneCancelled(RuntimeError):
    pass


@contextmanager
def check_generation(model, stopping_criterion, stopping_list, cancel, timeout_seconds=90):
    talker = model.model.talker
    had_value, previous = 'generate' in vars(talker), vars(talker).get('generate')
    original = talker.generate
    deadline = time.monotonic() + timeout_seconds
    def check():
        if cancel.is_set():
            raise VoiceCloneCancelled('cancelled')
        if time.monotonic() >= deadline:
            raise TimeoutError('声音超过本次生成时限，不输出截断音频')
    class Stop(stopping_criterion):
        def __call__(self, input_ids, scores, **kwargs):
            check()
            return False
    def generate(*args, **kwargs):
        check()
        kwargs['stopping_criteria'] = stopping_list([*(kwargs.get('stopping_criteria') or []), Stop()])
        result = original(*args, **kwargs)
        check()
        eos = model.model.config.talker_config.codec_eos_token_id
        if not bool((result.sequences == eos).any().item()):
            raise RuntimeError('台词未正常结束，不保存截尾声音')
        return result
    talker.generate = generate
    try:
        yield
        check()
    finally:
        if had_value:
            talker.generate = previous
        else:
            delattr(talker, 'generate')


def prompt_fingerprint(prompt):
    # 比较参考编码和声纹是否保持原样，核对时才复制到 CPU
    import torch
    digest = hashlib.sha256()
    for item in prompt:
        for name in ('ref_code', 'ref_spk_embedding'):
            tensor = getattr(item, name)
            if tensor is None:
                digest.update(b'none')
                continue
            digest.update(str((name, tensor.dtype, tuple(tensor.shape))).encode())
            digest.update(tensor.detach().cpu().contiguous().view(-1).view(torch.uint8).numpy().tobytes())
        digest.update(json.dumps({'text': item.ref_text, 'x_vector_only': item.x_vector_only_mode,
                                  'icl_mode': item.icl_mode}, ensure_ascii=False, sort_keys=True).encode())
    return digest.hexdigest()


def audio_bytes(path):
    with wave.open(str(path), 'rb') as audio:
        return ((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()),
                audio.readframes(audio.getnframes()))


def first_code_difference(original, candidate):
    for frame, (left, right) in enumerate(zip(original, candidate)):
        for group, (first, second) in enumerate(zip(left, right)):
            if first != second:
                return {'frame': frame, 'group': group, 'original': first, 'graph': second}
        if len(left) != len(right):
            return {'frame': frame, 'group_count_changed': True}
    if len(original) != len(candidate):
        return {'frame': min(len(original), len(candidate)), 'frame_count_changed': True}
    return None


@contextmanager
def collect_codes(model, result):
    owner = model.model
    had_value, previous = 'generate' in vars(owner), vars(owner).get('generate')
    original = owner.generate
    def generate(*args, **kwargs):
        output = original(*args, **kwargs)
        result['codes'] = output[0]
        return output
    owner.generate = generate
    try:
        yield
    finally:
        if had_value:
            owner.generate = previous
        else:
            delattr(owner, 'generate')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true', help='结束游戏和正式语音后，运行独立 GPU 试听')
    parser.add_argument('--predictor-graph', action='store_true', help='只比较已批准的内部预测 Graph，不改变 Talker 生成循环')
    parser.add_argument('--baseline-report', help='与已有原版试验逐句比较 PCM，需要相同参考、台词和种子')
    parser.add_argument('--diagnose-first-codec', action='store_true', help='只对首句依次运行原版和 Graph，保存编码并定位首处差异')
    args = parser.parse_args()
    if args.diagnose_first_codec and not args.predictor_graph:
        parser.error('--diagnose-first-codec 需要 --predictor-graph')
    project, root, specification, files = plan('voice_clone.json')
    if not args.execute:
        print(json.dumps({'status': 'plan_only', 'model': specification['model_id'],
                          'reference': specification['reference'], 'sentences': specification['sentences'],
                          'prompt_created_once': True, 'predictor_graph': args.predictor_graph,
                          'production_changed': False}, ensure_ascii=False, indent=2))
        return
    environment = checked_path(project / 'Saved/AIResearch/TtsEvaluation/qwen/.venv', project)
    if Path(sys.prefix).resolve() != environment.resolve():
        raise RuntimeError('请使用已安装的 Qwen 试验环境')
    installation = json.loads((environment.parent / 'installation.json').read_text(encoding='utf-8-sig'))
    if not installation.get('completed_at') or installation.get('model_revision') != specification['reuse_revision']:
        raise ValueError('已有 Qwen 安装记录与本次试验不一致')
    downloads = json.loads((root / 'model-downloads.json').read_text(encoding='utf-8-sig'))
    if downloads.get('status') != 'completed' or downloads.get('revision') != specification['revision']:
        raise ValueError('Base 模型下载尚未完成或版本不一致')
    baseline = None
    if args.baseline_report:
        baseline_path = checked_path(args.baseline_report, project)
        baseline = json.loads(baseline_path.read_text(encoding='utf-8'))
        if (baseline.get('status') != 'completed' or baseline['revision'] != specification['revision']
                or baseline['reference'] != specification['reference']
                or baseline['settings'] != specification['settings']
                or baseline.get('predictor_graph', False)):
            raise ValueError('原版对照的参考、模型或参数不一致')
    assert_game_and_speech_stopped()
    model_dir = checked_path(root / 'model', project)
    for entry in files:
        verify(checked_path(model_dir / entry['path'], project), entry)
    reference = checked_path(project / specification['reference']['path'], project)
    if hashlib.sha256(reference.read_bytes()).hexdigest() != specification['reference']['sha256']:
        raise ValueError('用户选择的参考声音已变化')
    for name in ('temp', 'cache'):
        checked_path(root / name, project).mkdir(exist_ok=True)
    offline_environment(root, specification['settings']['threads'])
    packages = package_versions()
    expected = {'torch': '2.7.1+cu128', 'qwen-tts': '0.1.1', 'transformers': '4.57.3'}
    if any(packages.get(name) != version for name, version in expected.items()):
        raise ValueError('试验依赖版本发生变化')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    output = checked_path(root / 'results' / stamp, project)
    output.mkdir(parents=True, exist_ok=False)
    report_path = output / 'evaluation.json'
    report = {'status': 'started', 'model_id': specification['model_id'], 'revision': specification['revision'],
              'model_license': specification['license'], 'reference': specification['reference'],
              'settings': specification['settings'], 'packages': packages, 'samples': [],
              'subjective_review': '等待用户比较与原 B 的听感', 'game_verified': False,
              'implementation': '官方生成循环，SDPA/bfloat16；可选已批准的 float32 采样 PredictorGraph',
              'predictor_graph': args.predictor_graph, 'baseline_report': args.baseline_report,
              'codec_diagnostic_only': args.diagnose_first_codec,
              'non_streaming_mode': True, 'x_vector_only_mode': False,
              'processing': '保留原音高、音量与停顿，不把新台词输出称为参考声音的字节副本',
              'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'specification_sha256': hashlib.sha256(Path(__file__).with_name('voice_clone.json').read_bytes()).hexdigest()}
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
        if model.model.tts_model_type != 'base':
            raise ValueError('加载的模型不是 Base')
        report['model_load_ms'] = (time.perf_counter() - started) * 1000
        report['load_torch_memory'] = torch_memory(torch)
        report['load_process_memory'] = process_memory()
        torch.manual_seed(specification['sentences'][0]['seed'])
        torch.cuda.manual_seed_all(specification['sentences'][0]['seed'])
        started = time.perf_counter()
        with torch.inference_mode():
            prompt = model.create_voice_clone_prompt(ref_audio=str(reference),
                                                    ref_text=specification['reference']['text'],
                                                    x_vector_only_mode=False)
        torch.cuda.synchronize()
        report['reference_prepare_ms'] = (time.perf_counter() - started) * 1000
        report['reference_prepare_count'] = 1
        original_prompt_hash = prompt_fingerprint(prompt)
        report['prompt_sha256'] = original_prompt_hash
        if baseline is not None and baseline.get('prompt_sha256') != original_prompt_hash:
            raise ValueError('本次参考编码与原版对照不一致，停止性能比较')
        # 后续只传已经建立的参考，意外重新提取会直接报告错误
        def refuse_rebuild(*args, **kwargs):
            raise RuntimeError('本次实验要求复用同一参考，不重复提取声纹')
        model.create_voice_clone_prompt = refuse_rebuild
        settings = {name: value for name, value in specification['settings'].items() if name != 'threads'}
        graph = None
        predictor = model.model.talker.code_predictor
        report['predictor_generation_config'] = predictor.generation_config.to_dict()
        hidden_size = model.model.config.talker_config.hidden_size
        if args.predictor_graph:
            if model.model.config.talker_config.num_code_groups != 16:
                raise ValueError('Base 编码组数量与已验证 Graph 不一致')
            vendor, manifest = checked_vendor(environment.parent)
            sys.path.insert(0, str(vendor.parent))
            try:
                module = importlib.import_module('faster_qwen_v032.predictor_graph')
            finally:
                sys.path.remove(str(vendor.parent))
            if Path(module.__file__).resolve() != (vendor / 'predictor_graph.py').resolve():
                raise ValueError('导入了不同来源的 Graph 模块')
            before = torch_memory(torch)
            started = time.perf_counter()
            with fp32_graph_sampling(module):
                graph = module.PredictorGraph(predictor, predictor.model.config, hidden_size,
                                              device='cuda:0', dtype=torch.bfloat16, do_sample=True,
                                              top_k=50, top_p=1.0, temperature=.9)
                graph.capture(num_warmup=3)
            torch.cuda.synchronize()
            report['graph_capture'] = {'ms': (time.perf_counter()-started)*1000,
                                       'before': before, 'after': torch_memory(torch), 'vendor': manifest}
        save()

        def generate(text, seed, cancel, codes=None, use_graph=True):
            torch.manual_seed(seed)
            torch.cuda.manual_seed_all(seed)
            adapter = (use_predictor_graph(predictor, graph, hidden_size=hidden_size,
                       device=torch.device('cuda:0'), dtype=torch.bfloat16, output_dtype=torch.long)
                       if graph is not None and use_graph else nullcontext())
            capture = collect_codes(model, codes) if codes is not None else nullcontext()
            with torch.inference_mode(), adapter, capture, check_generation(model, StoppingCriteria, StoppingCriteriaList, cancel):
                return model.generate_voice_clone(text=text, language='Chinese', voice_clone_prompt=prompt,
                                                  non_streaming_mode=True, **settings)

        def record(sentence, variant=None, use_graph=True):
            assert_game_and_speech_stopped()
            torch.cuda.reset_peak_memory_stats(0)
            torch.cuda.synchronize()
            started = time.perf_counter()
            print('开始固定音色试听 ' + sentence['id'], flush=True)
            codes = {}
            wavs, rate = generate(sentence['text'], sentence['seed'], threading.Event(), codes, use_graph=use_graph)
            torch.cuda.synchronize()
            elapsed = (time.perf_counter() - started) * 1000
            if len(wavs) != 1 or rate != 24000:
                raise ValueError('输出音频格式不一致')
            if prompt_fingerprint(prompt) != original_prompt_hash:
                raise ValueError('生成后参考编码或声纹发生变化')
            suffix = f'_{variant}' if variant else ''
            audio = write_audio(output / f"Xiaofang_B_{sentence['id']}{suffix}.wav", wavs[0], rate)
            row = {**sentence, **audio, 'synthesis_ms': elapsed, 'prompt_unchanged': True,
                   'torch_memory': torch_memory(torch), 'process_memory': process_memory()}
            # 合成计时结束后再读取编码，避免每步复制到 CPU 改变生成耗时
            code_bytes = b''.join(value.detach().cpu().contiguous().numpy().tobytes() for value in codes['codes'])
            row['codec_sha256'] = hashlib.sha256(code_bytes).hexdigest()
            row['codec_shapes'] = [list(value.shape) for value in codes['codes']]
            if args.diagnose_first_codec:
                row['variant'] = variant
                row['codec_values'] = [value.detach().cpu().tolist() for value in codes['codes']]
            row['pcm_sha256'] = hashlib.sha256(audio_bytes(audio['wav'])[1]).hexdigest()
            if baseline is not None:
                previous = [sample for sample in baseline['samples'] if sample['id'] == sentence['id']]
                if len(previous) != 1 or previous[0]['text'] != sentence['text'] or previous[0]['seed'] != sentence['seed']:
                    raise ValueError('原版对照没有相同台词和种子')
                original_path = checked_path(previous[0]['wav'], project)
                if hashlib.sha256(original_path.read_bytes()).hexdigest() != previous[0]['sha256']:
                    raise ValueError('原版对照音频已变化')
                row['matches_baseline_pcm'] = audio_bytes(audio['wav']) == audio_bytes(original_path)
                row['matches_baseline_codec'] = (row['codec_sha256'] == previous[0]['codec_sha256']
                                                if 'codec_sha256' in previous[0] else None)
            report['samples'].append(row)
            save()
            print(json.dumps({'sample': sentence['id'], 'synthesis_ms': elapsed,
                              'duration_s': audio['duration_s'], 'wav': audio['wav']}, ensure_ascii=False), flush=True)

        if args.diagnose_first_codec:
            # 取消已在独立对照中检查，这次只增加两条相同台词来定位编码差异
            record(specification['sentences'][0], variant='original', use_graph=False)
            record(specification['sentences'][0], variant='graph', use_graph=True)
            original, candidate = report['samples']
            report['first_codec_difference'] = first_code_difference(
                original['codec_values'][0], candidate['codec_values'][0])
            report['pcm_equal'] = original['pcm_sha256'] == candidate['pcm_sha256']
            report['status'] = 'completed'
            return
        for sentence in specification['sentences']:
            record(sentence)
        assert_game_and_speech_stopped()
        cancel = threading.Event()
        interruption = {'voice': 'Xiaofang_B', 'cancelled': False, 'audio_saved': False}
        def stop():
            interruption['cancel_requested_at'] = time.monotonic()
            cancel.set()
        timer = threading.Timer(.35, stop)
        try:
            timer.start()
            generate('你先别走，这件事情还没说清楚。', 20261115, cancel)
            raise RuntimeError('本次取消没有中断生成')
        except VoiceCloneCancelled:
            torch.cuda.synchronize()
            interruption['cancelled'] = True
            interruption['cancel_to_exit_ms'] = (time.monotonic()-interruption['cancel_requested_at'])*1000
        finally:
            timer.cancel()
            timer.join(timeout=1)
            report['interruption'] = interruption
            save()
        if prompt_fingerprint(prompt) != original_prompt_hash:
            raise ValueError('取消后参考编码或声纹发生变化')
        record(specification['after_cancel'])
        first, last = report['samples'][0], report['samples'][-1]
        report['repeat_after_cancel_matches_first'] = first['sha256'] == last['sha256']
        if baseline is not None:
            report['all_match_baseline_pcm'] = all(sample['matches_baseline_pcm'] for sample in report['samples'])
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
