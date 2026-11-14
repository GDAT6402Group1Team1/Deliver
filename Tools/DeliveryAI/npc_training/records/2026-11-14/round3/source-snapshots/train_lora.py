"""训练或评测独立 NPC LoRA；默认只检查数据，加 --run 才加载 GPU 模型"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import random
import sys
import time


ROOT = Path(__file__).resolve().parents[3]
TRAINING_ROOT = ROOT / 'Saved/AIResearch/NpcTraining'
MODEL_ROOT = TRAINING_ROOT / 'model'
MODEL_REVISION = '70d244cc86ccca08cf5af4e1e306ecf908b1ad5e'
SEED = 20261114
EMOTIONS = ('Calm', 'Angry', 'Surprised')
REPLY_FIELDS = {'candidate_index', 'speech', 'emotion', 'speech_target_id'}
SETTINGS = {'rank': 16, 'alpha': 32, 'dropout': .05, 'target_modules': 'all-linear',
            'batch_size': 1, 'gradient_accumulation': 4, 'learning_rate': 2e-4,
            'weight_decay': 0., 'max_grad_norm': 1., 'seed': SEED, 'dtype': 'bfloat16',
            'gradient_checkpointing': True, 'checkpoint_use_reentrant': False}


def file_hash(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def training_path(value):
    path = Path(value)
    if not path.is_absolute():
        path = ROOT / path
    path = path.absolute()
    path.resolve().relative_to(TRAINING_ROOT.resolve())
    for parent in (path, *path.parents):
        if parent.exists() and (parent.is_symlink() or getattr(parent.lstat(), 'st_file_attributes', 0) & 1024):
            raise ValueError('训练输入与输出不能使用链接目录')
        if parent == ROOT:
            break
    return path.resolve()


def output_check(text, context):
    """只检查输出格式与游戏提供的编号，不给台词的自然程度打分"""
    if context.get('candidates') and all(isinstance(item, str) for item in context['candidates']):
        try:
            reply = json.loads(text)
            valid = (isinstance(reply, dict) and set(reply) == {'action', 'target_id'}
                     and reply['action'] in context['candidates']
                     and reply['target_id'] == ('' if reply['action'] == 'Wait' else 'opponent'))
            return {'json_valid': True, 'fields_valid': valid, 'candidate_valid': valid, 'valid': valid,
                    'errors': [] if valid else ['invalid_combat_choice'],
                    'selected_candidate': reply if isinstance(reply, dict) else {}}
        except (ValueError, TypeError):
            return {'json_valid': False, 'fields_valid': False, 'candidate_valid': False,
                    'valid': False, 'errors': ['invalid_json']}
    result = {'json_valid': False, 'fields_valid': False, 'candidate_valid': False,
              'speech_target_valid': False, 'speech_chars': None, 'speech_chinese_chars': None,
              'valid': False, 'errors': []}
    try:
        reply = json.loads(text)
    except (ValueError, TypeError):
        result['errors'].append('invalid_json')
        return result
    if not isinstance(reply, dict):
        result['errors'].append('reply_is_not_object')
        return result
    result['json_valid'] = True
    result['fields_valid'] = set(reply) == REPLY_FIELDS
    if not result['fields_valid']:
        result['errors'].append('reply_fields_differ')
    candidates = context.get('candidates', [])
    index = reply.get('candidate_index')
    if type(index) is int and isinstance(candidates, list) and 0 <= index < len(candidates):
        result['candidate_valid'] = True
        result['selected_candidate'] = candidates[index]
    else:
        result['errors'].append('candidate_not_offered')
    speech = reply.get('speech')
    if isinstance(speech, str):
        result['speech_chars'] = len(speech)
        result['speech_chinese_chars'] = sum('\u3400' <= char <= '\u9fff' for char in speech)
        if len(speech) > 120:
            result['errors'].append('speech_exceeds_120_chars')
    else:
        result['errors'].append('speech_is_not_string')
    if reply.get('emotion') not in EMOTIONS:
        result['errors'].append('unsupported_emotion')
    people = {person['subject_id'] for person in context.get('subjects', [])
              if isinstance(person, dict) and person.get('kind') in ('npc', 'player')
              and isinstance(person.get('subject_id'), str)
              and person['subject_id'] != context.get('self_subject_id')}
    target = reply.get('speech_target_id')
    result['speech_target_valid'] = isinstance(target, str) and (not target or target in people)
    if not result['speech_target_valid']:
        result['errors'].append('speech_target_not_offered')
    result['valid'] = not result['errors']
    return result


def encode_record(row, tokenizer, maximum, evaluate=False, max_new_tokens=160):
    if not isinstance(row, dict) or any(not isinstance(row.get(key), str) or not row[key]
                                       for key in ('id', 'group_id')):
        raise ValueError('每条记录需要非空的 id 和 group_id')
    if not row.get('source'):
        raise ValueError('每条记录需要记录 source')
    messages = row.get('messages')
    if not isinstance(messages, list) or len(messages) not in ((2, 3) if evaluate else (3,)):
        raise ValueError('messages 应包含 system、user 和训练时的 assistant')
    expected = ('system', 'user') if evaluate else ('system', 'user', 'assistant')
    for index, role in enumerate(expected):
        message = messages[index]
        if not isinstance(message, dict) or message.get('role') != role or not isinstance(message.get('content'), str):
            raise ValueError('messages 的角色顺序或内容格式不符')
    context = json.loads(messages[1]['content'])
    candidate_type = str if row.get('task') == 'combat_update' else dict
    if (not isinstance(context, dict) or not isinstance(context.get('candidates'), list)
            or not context['candidates'] or any(not isinstance(item, candidate_type) for item in context['candidates'])):
        raise ValueError('user 中必须包含本轮 candidates')
    # 评测只给模型 system 和 user，不读取标准答案构造提示或检查结果
    prompt = tokenizer.apply_chat_template(messages[:2], tokenize=False,
                                           add_generation_prompt=True, enable_thinking=False)
    prompt_ids = tokenizer.encode(prompt, add_special_tokens=False)
    encoded = {'id': row['id'], 'group_id': row['group_id'], 'source': row['source'],
               'context': context, 'prompt_ids': prompt_ids, 'prompt_tokens': len(prompt_ids),
               'prompt_sha256': hashlib.sha256(prompt.encode('utf-8')).hexdigest()}
    if evaluate:
        if len(prompt_ids) + max_new_tokens > maximum:
            raise ValueError(f'提示 {len(prompt_ids)} 加输出预算 {max_new_tokens} 超过 {maximum}，没有截断')
        return encoded
    answer = messages[2]['content']
    checked = output_check(answer, context)
    if not checked['valid']:
        raise ValueError('assistant 不符合协议: ' + ','.join(checked['errors']))
    # 提示和答案分别分词，使训练看到的提示与实际生成的提示完全一致
    answer_ids = tokenizer.encode(answer, add_special_tokens=False) + [tokenizer.eos_token_id]
    ids = prompt_ids + answer_ids
    if len(ids) > maximum:
        raise ValueError(f'完整样本需要 {len(ids)} tokens，超过 {maximum}，没有截断')
    encoded.update(input_ids=ids, labels=[-100] * len(prompt_ids) + answer_ids,
                   supervised_tokens=len(answer_ids), sequence_tokens=len(ids))
    return encoded


def read_dataset(path, tokenizer, maximum, evaluate=False, max_new_tokens=160):
    records, errors, known = [], [], set()
    with path.open(encoding='utf-8-sig') as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
                encoded = encode_record(row, tokenizer, maximum, evaluate, max_new_tokens)
                if encoded['id'] in known:
                    raise ValueError('同一个数据文件中出现重复 id')
                known.add(encoded['id'])
                records.append(encoded)
            except (ValueError, TypeError, KeyError) as error:
                errors.append({'file': str(path), 'line': line_number, 'reason': str(error)})
    if not records:
        errors.append({'file': str(path), 'reason': '没有可用样本'})
    return records, errors


def dataset_summary(path, records):
    return {'path': str(path), 'sha256': file_hash(path), 'samples': len(records),
            'groups': len({record['group_id'] for record in records}),
            'max_prompt_tokens': max((record['prompt_tokens'] for record in records), default=0),
            'max_sequence_tokens': max((record.get('sequence_tokens', 0) for record in records), default=0),
            'supervised_tokens': sum(record.get('supervised_tokens', 0) for record in records)}


def batch(record, torch, labels=True):
    ids = record['input_ids'] if labels else record['prompt_ids']
    result = {'input_ids': torch.tensor([ids], dtype=torch.long, device='cuda'),
              'attention_mask': torch.ones((1, len(ids)), dtype=torch.long, device='cuda')}
    if labels:
        result['labels'] = torch.tensor([record['labels']], dtype=torch.long, device='cuda')
    return result


def validation_loss(model, records, torch):
    model.eval()
    weighted_loss = tokens = 0
    with torch.inference_mode(), torch.autocast('cuda', dtype=torch.bfloat16):
        for record in records:
            value = float(model(**batch(record, torch), use_cache=False).loss.item())
            if not math.isfinite(value):
                raise RuntimeError('验证集 loss 不是有限数值')
            weighted_loss += value * record['supervised_tokens']
            tokens += record['supervised_tokens']
    return {'assistant_token_loss': weighted_loss / tokens, 'samples': len(records), 'supervised_tokens': tokens}


def train(model, tokenizer, train_rows, validation_rows, args, report, torch):
    from peft import LoraConfig, get_peft_model
    model.config.use_cache = False
    if args.resume_adapter:
        from peft import PeftModel
        model = PeftModel.from_pretrained(model, str(args.resume_adapter), is_trainable=True, local_files_only=True)
        report['initial_adapter'] = str(args.resume_adapter)
        report['initial_adapter_files'] = {p.name: file_hash(p) for p in args.resume_adapter.iterdir() if p.is_file()}
    else:
        model = get_peft_model(model, LoraConfig(r=16, lora_alpha=32, lora_dropout=.05,
                                               target_modules='all-linear', bias='none', task_type='CAUSAL_LM'))
    model.gradient_checkpointing_enable(gradient_checkpointing_kwargs={'use_reentrant': False})
    model.enable_input_require_grads()
    parameters = [parameter for parameter in model.parameters() if parameter.requires_grad]
    report['trainable_parameters'] = sum(parameter.numel() for parameter in parameters)
    report['total_parameters'] = sum(parameter.numel() for parameter in model.parameters())
    optimizer = torch.optim.AdamW(parameters, lr=args.learning_rate, weight_decay=0.)
    report['validation_before'] = validation_loss(model, validation_rows, torch)
    order, cursor, epoch = [], 0, 0
    randomizer = random.Random(SEED)
    optimizer.zero_grad(set_to_none=True)
    started = time.perf_counter()
    with (args.output / 'loss.jsonl').open('w', encoding='utf-8') as log:
        for step in range(1, args.max_steps + 1):
            model.train()
            step_started, losses = time.perf_counter(), []
            for _ in range(SETTINGS['gradient_accumulation']):
                if cursor >= len(order):
                    order = list(range(len(train_rows)))
                    randomizer.shuffle(order)
                    cursor, epoch = 0, epoch + 1
                record = train_rows[order[cursor]]
                cursor += 1
                with torch.autocast('cuda', dtype=torch.bfloat16):
                    loss = model(**batch(record, torch), use_cache=False).loss
                if not bool(torch.isfinite(loss).item()):
                    raise RuntimeError(f'第 {step} 步 loss 不是有限数值')
                losses.append(loss.detach())
                (loss / SETTINGS['gradient_accumulation']).backward()
            grad_norm = torch.nn.utils.clip_grad_norm_(parameters, 1., error_if_nonfinite=True)
            optimizer.step()
            optimizer.zero_grad(set_to_none=True)
            torch.cuda.synchronize()
            row = {'step': step, 'epoch_started': epoch, 'loss': float(torch.stack(losses).mean().item()),
                   'gradient_norm': float(grad_norm.item()), 'step_seconds': time.perf_counter() - step_started,
                   'elapsed_seconds': time.perf_counter() - started}
            log.write(json.dumps(row) + '\n')
            log.flush()
            print(json.dumps(row), flush=True)
            report['last_training_step'] = row
    report['training_seconds'] = time.perf_counter() - started
    report['validation_after'] = validation_loss(model, validation_rows, torch)
    model.save_pretrained(args.output / 'adapter', safe_serialization=True)
    tokenizer.save_pretrained(args.output / 'tokenizer')
    report['adapter_path'] = str(args.output / 'adapter')
    report['tokenizer_path'] = str(args.output / 'tokenizer')


def evaluate(model, tokenizer, rows, args, report, torch):
    from transformers.generation.streamers import BaseStreamer
    report['adapter_merged_for_evaluation'] = False
    if args.adapter:
        from peft import PeftModel
        model = PeftModel.from_pretrained(model, str(args.adapter), is_trainable=False, local_files_only=True)
        # 在内存中合并 LoRA，让前后评测使用相同的网络结构；原模型文件保持不变
        torch.cuda.synchronize()
        merge_started = time.perf_counter()
        try:
            model = model.merge_and_unload(safe_merge=True)
            torch.cuda.synchronize()
        finally:
            report['merge_seconds'] = time.perf_counter() - merge_started
        report['adapter_merged_for_evaluation'] = True
    model.eval()
    model.config.use_cache = True

    class FirstTokenTime(BaseStreamer):
        """generate 第一次交出的是输入，后面才是模型生成的 token"""
        def __init__(self, origin):
            self.origin, self.prompt_seen, self.first_token_ms = origin, False, None

        def put(self, _value):
            if not self.prompt_seen:
                self.prompt_seen = True
            elif self.first_token_ms is None:
                self.first_token_ms = (time.perf_counter() - self.origin) * 1000

        def end(self):
            pass

    results = []
    with (args.output / 'evaluation.jsonl').open('w', encoding='utf-8') as log:
        for record in rows:
            inputs = batch(record, torch, labels=False)
            torch.cuda.synchronize()
            started = time.perf_counter()
            timing = FirstTokenTime(started)
            with torch.inference_mode():
                generated = model.generate(**inputs, do_sample=False, max_new_tokens=args.max_new_tokens,
                                           temperature=None, top_p=None, top_k=None, use_cache=True, streamer=timing,
                                           pad_token_id=tokenizer.pad_token_id or tokenizer.eos_token_id,
                                           eos_token_id=tokenizer.eos_token_id)
            torch.cuda.synchronize()
            total_ms = (time.perf_counter() - started) * 1000
            answer_ids = generated[0, record['prompt_tokens']:].tolist()
            raw = tokenizer.decode(answer_ids, skip_special_tokens=False)
            text = tokenizer.decode(answer_ids, skip_special_tokens=True).strip()
            row = {'id': record['id'], 'group_id': record['group_id'], 'source': record['source'],
                   'prompt_sha256': record['prompt_sha256'], 'prompt_tokens': record['prompt_tokens'],
                   'output_tokens': len(answer_ids), 'raw_output': raw, 'output': text,
                   'first_token_ms': timing.first_token_ms, 'complete_ms': total_ms,
                   'ended_with_eos': bool(answer_ids) and answer_ids[-1] == tokenizer.eos_token_id,
                   'checks': output_check(text, record['context'])}
            results.append(row)
            log.write(json.dumps(row, ensure_ascii=False) + '\n')
            log.flush()
            print(json.dumps({'id': row['id'], 'valid': row['checks']['valid'],
                              'first_token_ms': row['first_token_ms'], 'complete_ms': total_ms}), flush=True)
    report['evaluation'] = {'samples': len(results), 'valid_output_count': sum(row['checks']['valid'] for row in results),
                            'json_valid_count': sum(row['checks']['json_valid'] for row in results),
                            'candidate_valid_count': sum(row['checks']['candidate_valid'] for row in results),
                            'ended_with_eos_count': sum(row['ended_with_eos'] for row in results),
                            'semantic_quality_scored': False, 'shared_prefix_cache_between_samples': False,
                            'generation': {'do_sample': False, 'max_new_tokens': args.max_new_tokens},
                            'first_sample_includes_cold_generation': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--train', type=Path, default=TRAINING_ROOT / 'data/train.jsonl')
    parser.add_argument('--validation', type=Path, default=TRAINING_ROOT / 'data/validation.jsonl')
    parser.add_argument('--evaluate-only', action='store_true')
    parser.add_argument('--eval-data', type=Path)
    parser.add_argument('--adapter', type=Path)
    parser.add_argument('--resume-adapter', type=Path, help='从指定 LoRA 继续训练，结果写入新的输出目录')
    parser.add_argument('--learning-rate', type=float, default=2e-4)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-steps', type=int, default=48)
    parser.add_argument('--max-seq-length', type=int, default=3072)
    parser.add_argument('--max-new-tokens', type=int, default=160)
    args = parser.parse_args()
    if args.max_steps < 1 or not 128 <= args.max_seq_length <= 40960 or not 1 <= args.max_new_tokens <= 512:
        parser.error('步数必须大于零，样本长度需为128–40960，评测输出预算需为1–512')
    if args.evaluate_only and args.eval_data is None:
        parser.error('--evaluate-only 需要 --eval-data')
    if args.adapter and not args.evaluate_only:
        parser.error('--adapter 仅用于独立评测')
    if args.resume_adapter and args.evaluate_only:
        parser.error('--resume-adapter 只用于继续训练')
    if not 0 < args.learning_rate <= .001:
        parser.error('learning-rate 超出本次小规模训练范围')
    if args.resume_adapter:
        args.resume_adapter = training_path(args.resume_adapter)
    args.output = training_path(args.output)
    if args.output == TRAINING_ROOT:
        parser.error('--output 必须是训练目录中的新子目录')
    args.output.mkdir(parents=True, exist_ok=False)
    os.environ.update({'HF_HUB_OFFLINE': '1', 'TRANSFORMERS_OFFLINE': '1', 'HF_HUB_DISABLE_TELEMETRY': '1',
                       'TOKENIZERS_PARALLELISM': 'false', 'CUBLAS_WORKSPACE_CONFIG': ':4096:8'})
    if not args.run:
        os.environ['CUDA_VISIBLE_DEVICES'] = ''
    report = {'started_utc': datetime.now(timezone.utc).isoformat(), 'mode': 'evaluation' if args.evaluate_only else 'training',
              'status': 'checking', 'model_path': str(MODEL_ROOT), 'model_revision': MODEL_REVISION,
              'script_sha256': file_hash(Path(__file__)), 'settings': dict(SETTINGS, max_steps=args.max_steps,
              max_seq_length=args.max_seq_length, learning_rate=args.learning_rate), 'gpu_loaded': False, 'production_configuration_changed': False}
    started = time.perf_counter()
    try:
        import torch
        from transformers import AutoTokenizer, AutoModelForCausalLM, set_seed
        report['versions'] = {name: importlib.metadata.version(name)
                              for name in ('torch', 'transformers', 'peft', 'accelerate', 'tokenizers')}
        report['adapter_used'] = bool(args.adapter)
        preparation = json.loads((TRAINING_ROOT / 'model-downloads.json').read_text(encoding='utf-8'))
        if preparation['revision'] != MODEL_REVISION:
            raise ValueError('原模型与固定版本不符')
        report['model_manifest_sha256'] = file_hash(TRAINING_ROOT / 'model-downloads.json')
        tokenizer = AutoTokenizer.from_pretrained(str(MODEL_ROOT), local_files_only=True, trust_remote_code=False)
        if tokenizer.eos_token_id is None:
            raise ValueError('分词器没有 EOS，停止训练')
        errors = []
        if args.evaluate_only:
            args.eval_data = training_path(args.eval_data)
            rows, errors = read_dataset(args.eval_data, tokenizer, args.max_seq_length, True, args.max_new_tokens)
            report['datasets'] = {'evaluation': dataset_summary(args.eval_data, rows)}
            if args.adapter:
                args.adapter = training_path(args.adapter)
                report['adapter_path'] = str(args.adapter)
                report['adapter_files'] = {path.name: file_hash(path) for path in args.adapter.iterdir() if path.is_file()}
        else:
            args.train, args.validation = training_path(args.train), training_path(args.validation)
            train_rows, train_errors = read_dataset(args.train, tokenizer, args.max_seq_length)
            validation_rows, validation_errors = read_dataset(args.validation, tokenizer, args.max_seq_length)
            errors = train_errors + validation_errors
            overlap = {row['group_id'] for row in train_rows} & {row['group_id'] for row in validation_rows}
            if overlap:
                errors.append({'reason': '训练集和验证集包含相同 group_id', 'groups': sorted(overlap)})
            report['datasets'] = {'train': dataset_summary(args.train, train_rows),
                                  'validation': dataset_summary(args.validation, validation_rows)}
        if errors:
            report['rejected_samples'] = errors
            raise ValueError(f'数据检查失败，共 {len(errors)} 项；未截断或跳过样本')
        if not args.run:
            report['status'] = 'prepared'
            report['cuda_initialized'] = torch.cuda.is_initialized()
            print(json.dumps({'status': 'prepared', 'output': str(args.output), 'datasets': report['datasets']}, ensure_ascii=False))
            return
        if not torch.cuda.is_available() or not torch.cuda.is_bf16_supported():
            raise RuntimeError('本次训练需要支持 BF16 的 CUDA 设备')
        set_seed(SEED)
        torch.cuda.set_device(0)
        torch.cuda.reset_peak_memory_stats()
        report['device'] = torch.cuda.get_device_name(0)
        report['device_total_memory_bytes'] = torch.cuda.get_device_properties(0).total_memory
        load_started = time.perf_counter()
        model = AutoModelForCausalLM.from_pretrained(str(MODEL_ROOT), dtype=torch.bfloat16, device_map={'': 0},
                                                    attn_implementation='sdpa', local_files_only=True, trust_remote_code=False)
        torch.cuda.synchronize()
        report['model_load_seconds'] = time.perf_counter() - load_started
        report['gpu_loaded'] = True
        if args.evaluate_only:
            evaluate(model, tokenizer, rows, args, report, torch)
        else:
            train(model, tokenizer, train_rows, validation_rows, args, report, torch)
        report['peak_gpu_allocated_bytes'] = torch.cuda.max_memory_allocated()
        report['peak_gpu_reserved_bytes'] = torch.cuda.max_memory_reserved()
        report['status'] = 'completed'
    except BaseException as error:
        report['status'] = 'failed'
        report['error'] = {'type': type(error).__name__, 'message': str(error)}
        if 'torch' in locals() and torch.cuda.is_initialized():
            report['peak_gpu_allocated_bytes'] = torch.cuda.max_memory_allocated()
            report['peak_gpu_reserved_bytes'] = torch.cuda.max_memory_reserved()
        raise
    finally:
        report['elapsed_seconds'] = time.perf_counter() - started
        write_json(args.output / 'run.json', report)


if __name__ == '__main__':
    main()
