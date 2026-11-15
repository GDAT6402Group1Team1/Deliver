"""显示或启动独立小模型试验服务，默认使用 CPU，不修改正式 AI 后端配置"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.error
import urllib.request

from prepare_local_model import CONFIG, INSTALL, ROOT, checked_file, file_hash, inside


OUTPUT_HEAD_ON_CPU = r'^(token_embd|output|output_norm)\.weight$=CPU'


def cpu_thread_settings(settings, *, threads=4, threads_batch=8):
    """配置未填写线程数时沿用调用方的设置，默认仍为生成 4 线程、输入 8 线程"""
    selected = (settings.get('threads', threads), settings.get('threads_batch', threads_batch))
    if any(type(value) is not int or not 1 <= value <= 64 for value in selected):
        raise ValueError('生成和输入处理的线程数必须是 1–64 的整数')
    return selected


def server_arguments(config, executable, device, *, gpu_layers=99, model_path=None, output_tokens=None,
                     temperature=0., threads=4, threads_batch=8, cpu_output_head=False, disable_host_cache=False):
    if (gpu_layers not in (99, 21, 14, 18) or cpu_output_head != (gpu_layers == 18)
            or (cpu_output_head and device != 'cuda')):
        raise ValueError('18 层需要同时使用 CUDA 和 CPU 输出层，其余可选层数为 99、21、14')
    threads, threads_batch = cpu_thread_settings({}, threads=threads, threads_batch=threads_batch)
    model = model_path or inside(INSTALL, config['files'][0]['path'])
    args = [str(executable), '--model', str(model), '--alias', 'delivery-npc-qwen3',
            '--host', config['host'], '--port', str(config['port']), '--ctx-size', str(config['context_tokens']),
            '--parallel', str(config['parallel_slots']), '--n-predict', str(output_tokens or config['output_tokens']),
            '--threads', str(threads), '--threads-batch', str(threads_batch), '--batch-size', '256', '--ubatch-size', '128',
            '--cache-type-k', 'f16', '--cache-type-v', 'f16', '--no-context-shift',
            '--jinja', '--reasoning', 'off', '--reasoning-budget', '0',
            '--temp', str(temperature), '--top-p', '0.8', '--top-k', '20', '--min-p', '0', '--metrics', '--slots', '--no-ui']
    if device == 'cpu':
        args.extend(('--device', 'none', '--n-gpu-layers', '0', '--no-kv-offload', '--flash-attn', 'off'))
    else:
        args.extend(('--n-gpu-layers', str(gpu_layers), '--flash-attn', 'on'))
    if cpu_output_head:
        args.extend(('--override-tensor', OUTPUT_HEAD_ON_CPU))
    if disable_host_cache:
        args.extend(('--cache-ram', '0', '--no-cache-idle-slots'))
    return args


def read_slots(endpoint, count, context_tokens=None):
    with urllib.request.urlopen(endpoint, timeout=2) as response:
        slots = json.load(response)
    if (not isinstance(slots, list) or len(slots) != count
            or not all(isinstance(slot, dict) and type(slot.get('id')) is int for slot in slots)
            or {slot['id'] for slot in slots} != set(range(count))
            or (context_tokens is not None and any(slot.get('n_ctx') != context_tokens for slot in slots))):
        raise ValueError('模型实际提供的缓存槽与启动配置不符')
    return slots


def existing_process(record_path, command, model_hash, *, slot_count=1, slot_context_tokens=None):
    """只复用本工具记录且命令完全一致的进程，不接管碰巧占用同一端口的程序"""
    if not record_path.is_file() or os.name != 'nt':
        return None
    record = json.loads(record_path.read_text(encoding='utf-8'))
    pid = record.get('pid')
    if type(pid) is not int or pid <= 0 or record.get('command') != command or record.get('model_sha256') != model_hash:
        return None
    host, port = (command[command.index(flag) + 1] for flag in ('--host', '--port'))
    endpoint = f'http://{host}:{port}'
    if record.get('health') != endpoint + '/health' or record.get('slots') != endpoint + '/slots':
        return None
    query = f"Get-CimInstance Win32_Process -Filter 'ProcessId={pid}' | Select-Object ProcessId,CommandLine,ExecutablePath | ConvertTo-Json -Compress"
    found = subprocess.run(['powershell', '-NoProfile', '-Command', query], capture_output=True, text=True,
                           timeout=5, creationflags=subprocess.CREATE_NO_WINDOW)
    if found.returncode or not found.stdout.strip():
        return None
    process = json.loads(found.stdout)
    if process.get('CommandLine') != subprocess.list2cmdline(command):
        return None
    if Path(process.get('ExecutablePath', '')).resolve() != Path(command[0]).resolve():
        return None
    try:
        with urllib.request.urlopen(record['health'], timeout=2) as response:
            if json.load(response).get('status') != 'ok':
                return None
        read_slots(record['slots'], slot_count, slot_context_tokens)
    except (OSError, ValueError, KeyError):
        return None
    return {**record, 'reused_existing_process': True}


def playtest_runtime_settings(combat):
    expected = {'schema_version': 1, 'device': 'cuda', 'gpu_layers': 18, 'cpu_output_head': True,
                'context_tokens': 4096, 'parallel_slots': 2, 'output_tokens': 64, 'cache_type': 'f16',
                'disable_host_cache': True, 'resident_cache': True, 'compact_output': True,
                'threads': 1, 'threads_batch': 1, 'host': '127.0.0.1', 'port': 18881}
    invalid = [key for key, value in expected.items()
               if type(combat.get(key)) is not type(value) or combat[key] != value]
    for key, value in (('timeout_seconds', 1.5), ('temperature', 0)):
        if type(combat.get(key)) not in (int, float) or combat[key] != value:
            invalid.append(key)
    if invalid:
        raise ValueError('小模型试玩配置不符：' + '、'.join(invalid))
    # llama.cpp 的总容量由两个角色平分，配置文件填写每名角色可用的容量
    return {**{key: combat[key] for key in ('host', 'port', 'parallel_slots')},
            'context_tokens': combat['context_tokens'] * combat['parallel_slots']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='store_true', help='启动独立服务；默认仅显示命令和接口地址')
    parser.add_argument('--device', choices=['cpu', 'cuda'], default='cpu')
    parser.add_argument('--gpu-layers', type=int, choices=[99, 21, 14], default=99)
    parser.add_argument('--model-path', type=Path, help='独立训练导出的 GGUF，需同时给出 SHA256')
    parser.add_argument('--model-sha256')
    parser.add_argument('--output-tokens', type=int, choices=[64, 256], default=64)
    parser.add_argument('--combat-config', type=Path, help='读取正式小模型配置或明确标记的试玩配置')
    parser.add_argument('--temperature', type=float, choices=[0., .7], default=0.)
    parser.add_argument('--threads', type=int, default=4, help='生成输出时使用的 CPU 线程数，1–64，默认 4')
    parser.add_argument('--threads-batch', type=int, default=8, help='处理输入时使用的 CPU 线程数，1–64，默认 8')
    args = parser.parse_args()
    if not 1 <= args.threads <= 64 or not 1 <= args.threads_batch <= 64:
        parser.error('生成和输入处理的线程数必须是 1–64 的整数')
    config = json.loads(CONFIG.read_text(encoding='utf-8'))
    stage = None
    slot_context_tokens = None
    record_path = INSTALL / 'running.json'
    if args.combat_config:
        combat = json.loads(args.combat_config.read_text(encoding='utf-8'))
        if combat.get('enabled') is not True:
            print(json.dumps({'enabled': False, 'reason': 'combat_model_disabled'}))
            return
        stage = combat.get('release_stage', 'validated')
        if stage not in ('validated', 'playtest'):
            raise ValueError('小模型配置的发布阶段无效')
        if stage == 'playtest':
            config.update(playtest_runtime_settings(combat))
            slot_context_tokens = combat['context_tokens']
            record_path = INSTALL / 'running-playtest.json'
        elif (combat.get('schema_version') != 1 or combat.get('context_tokens') != 4096
                or combat.get('output_tokens') != 64 or combat.get('parallel_slots') != 1
                or combat.get('host') != '127.0.0.1' or combat.get('gpu_layers') not in (99, 21, 14)):
            raise ValueError('正式小模型配置超出本轮验证范围')
        args.device, args.gpu_layers = combat['device'], combat['gpu_layers']
        args.threads, args.threads_batch = cpu_thread_settings(
            combat, threads=args.threads, threads_batch=args.threads_batch)
        args.temperature = combat.get('temperature')
        if args.temperature != 0:
            raise ValueError('正式动作模型使用已验证的确定性选择')
        if stage == 'playtest':
            args.temperature = 0.
        if sorted(combat.get('profile_ids', [])) != ['chunlai', 'xiumei']:
            raise ValueError('本轮只验证了大壮和翠花的人物配置')
        if args.device != 'cuda':
            raise ValueError('正式配置需要本轮已验证的 CUDA 模型')
        args.model_path = ROOT / combat['model_path']
        args.model_sha256 = combat['model_sha256']
        args.output_tokens = 64
        if stage == 'validated':
            evaluation_path = ROOT / combat['evaluation_path']
            if not combat['evaluation_sha256'] or file_hash(evaluation_path) != combat['evaluation_sha256']:
                raise ValueError('验收记录发生变化，停止自动启用小模型')
            evaluation = json.loads(evaluation_path.read_text(encoding='utf-8'))
            summary = evaluation.get('summary', {})
            if (summary.get('acceptance_qualified') is not True or summary.get('cases', 0) < 40
                    or summary.get('accuracy', 0) < .9
                    or summary.get('protocol_valid') != summary.get('cases')
                    or summary.get('target_valid') != summary.get('cases')
                    or evaluation.get('model_runtime', {}).get('model_sha256') != args.model_sha256):
                raise ValueError('模型未通过独立动作验收，不能自动启用')
            integration = combat.get('integration_checks', {})
            if (integration.get('passed') is not True or integration.get('critical_errors') != 0
                    or not integration.get('evidence')):
                raise ValueError('还未记录取消请求、旧动作和合法目标的接入检查')
            for evidence in integration['evidence']:
                evidence_path = (ROOT / evidence['path']).resolve()
                evidence_path.relative_to(ROOT.resolve())
                if not evidence.get('sha256') or file_hash(evidence_path) != evidence['sha256']:
                    raise ValueError('接入检查记录发生变化，请重新核对')
            config.update({key: combat[key] for key in ('host', 'port', 'context_tokens', 'parallel_slots')})
    installation_file = INSTALL / 'installation.json'
    if not installation_file.is_file():
        parser.error('请先运行 prepare_local_model.py --download')
    installation = json.loads(installation_file.read_text(encoding='utf-8'))
    if installation['config_sha256'] != file_hash(CONFIG):
        raise RuntimeError('试验配置与安装记录不符，请重新检查资源')
    executable = inside(INSTALL, installation['server_executable'])
    if bool(args.model_path) != bool(args.model_sha256):
        parser.error('--model-path 和 --model-sha256 必须同时提供')
    if args.model_path:
        args.model_path = args.model_path.resolve()
        args.model_path.relative_to((INSTALL.parents[1] / 'NpcTraining').resolve())
        if args.model_path.suffix.lower() != '.gguf' or file_hash(args.model_path) != args.model_sha256:
            raise ValueError('训练模型格式或 SHA256 不符')
    is_playtest = stage == 'playtest'
    command = server_arguments(config, executable, args.device, gpu_layers=args.gpu_layers,
                               model_path=args.model_path, output_tokens=args.output_tokens, temperature=args.temperature,
                               threads=args.threads, threads_batch=args.threads_batch, cpu_output_head=is_playtest,
                               disable_host_cache=is_playtest)
    environment_example = {'DELIVERY_AI_MODEL_URL': f"http://{config['host']}:{config['port']}/v1/chat/completions",
                           'DELIVERY_AI_MODEL': 'delivery-npc-qwen3'}
    print(json.dumps({'command': command, 'device': args.device, 'release_stage': stage,
                      'example_environment_only': environment_example,
                      'threads': args.threads, 'threads_batch': args.threads_batch,
                      'production_configuration_changed': False}, ensure_ascii=False), flush=True)
    if not args.run:
        return
    runtime_files = installation['runtime_files']
    if is_playtest and not any(inside(INSTALL, entry['path']) == executable for entry in runtime_files):
        raise ValueError('本地推理程序没有对应的安装校验记录')
    for entry in (runtime_files if is_playtest else [config['files'][0], *runtime_files]):
        if not checked_file(inside(INSTALL, entry['path']), entry):
            raise RuntimeError('模型或运行文件发生变化: ' + entry['path'])
    model_hash = args.model_sha256 or config['files'][0]['sha256']
    reusable = existing_process(record_path, command, model_hash, slot_count=config['parallel_slots'],
                                slot_context_tokens=slot_context_tokens)
    if reusable:
        reusable.update(threads=args.threads, threads_batch=args.threads_batch)
        record_path.write_text(json.dumps(reusable, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(reusable, ensure_ascii=False), flush=True)
        return
    with socket.socket() as port:
        if port.connect_ex((config['host'], config['port'])) == 0:
            raise RuntimeError('试验端口已被占用，不启动另一份模型')
    logs = INSTALL / 'logs'
    logs.mkdir(exist_ok=True)
    label = datetime.now().strftime('%Y%m%d-%H%M%S')
    log_file = logs / f'llama-{args.device}-{label}.log'
    environment = os.environ.copy()
    # 运行库只加入本次子进程搜索路径，不改系统 PATH 或已运行的语音进程
    library_folders = sorted({str(inside(INSTALL, row['path']).parent)
                              for row in installation['runtime_files'] if row['path'].lower().endswith('.dll')})
    environment['PATH'] = os.pathsep.join([str(executable.parent), *library_folders, environment.get('PATH', '')])
    with log_file.open('wb') as output:
        process = subprocess.Popen(command, cwd=executable.parent, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    started = time.monotonic()
    health = f"http://{config['host']}:{config['port']}/health"
    slots = f"http://{config['host']}:{config['port']}/slots"
    try:
        while time.monotonic() - started < 90:
            if process.poll() is not None:
                raise RuntimeError(f'试验服务启动失败，查看 {log_file}')
            try:
                with urllib.request.urlopen(health, timeout=1) as response:
                    if json.load(response).get('status') == 'ok':
                        break
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(.25)
        else:
            raise TimeoutError('模型加载超过 90 秒')
        if is_playtest:
            read_slots(slots, config['parallel_slots'], slot_context_tokens)
    except BaseException:
        process.terminate()
        process.wait(timeout=10)
        raise
    record = {'pid': process.pid, 'device': args.device, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'log': str(log_file), 'command': command, 'health': health, 'environment_example_only': environment_example,
              'gpu_layers': args.gpu_layers if args.device == 'cuda' else 0,
              'model_path': str(args.model_path or inside(INSTALL, config['files'][0]['path'])),
              'model_sha256': args.model_sha256 or config['files'][0]['sha256'],
              'context_tokens': config['context_tokens'], 'output_tokens': args.output_tokens,
              'temperature': args.temperature, 'threads': args.threads, 'threads_batch': args.threads_batch,
              'slots': slots}
    if is_playtest:
        record.update(release_stage=stage, context_tokens_per_slot=slot_context_tokens,
                      parallel_slots=config['parallel_slots'], cpu_output_head=True, host_cache_disabled=True,
                      model_runtime_release=config['runtime_release'], model_runtime_commit=config['runtime_commit'])
    record_path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(record, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
