"""准备独立 NPC 训练环境与原始权重；不启动训练、模型服务或 CUDA"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request


ROOT = Path(__file__).resolve().parents[3]
DESTINATION = ROOT / 'Saved/AIResearch/NpcTraining'
REQUIREMENTS = Path(__file__).with_name('requirements.txt')
REPOSITORY = 'Qwen/Qwen3-1.7B'
REVISION = '70d244cc86ccca08cf5af4e1e306ecf908b1ad5e'
TORCH_SHA256 = '5174f02de8ca14df87c8e333c4c39cf3ce93a323c9d470d690301d110a053b3c'
TORCH_WHEEL = 'torch-2.7.1+cu128-cp310-cp310-win_amd64.whl'


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def sha256(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def saved_path(relative):
    path = DESTINATION / relative
    path.resolve().relative_to(DESTINATION.resolve())
    for current in (path, *path.parents):
        if current.exists() and current.is_symlink():
            raise RuntimeError(f'训练目录不能包含链接: {current}')
        if current == ROOT:
            break
    return path


def verify_model_file(path, entry):
    if not path.is_file() or path.stat().st_size != entry['bytes']:
        return False
    if entry.get('sha256'):
        return sha256(path) == entry['sha256']
    # 小配置文件没有 LFS SHA256，使用固定提交中的 Git blob 校验原始内容
    data = path.read_bytes()
    git_hash = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
    return git_hash == entry['git_blob_sha1']


def fetch_model_file(entry):
    path = saved_path('model/' + entry['path'])
    path.parent.mkdir(parents=True, exist_ok=True)
    url = f"https://huggingface.co/{REPOSITORY}/resolve/{REVISION}/{entry['path']}"
    cached = verify_model_file(path, entry)
    if not cached:
        if path.exists():
            raise RuntimeError(f'模型文件与固定版本不符: {path}')
        temporary = path.with_suffix(path.suffix + '.part')
        for attempt in range(3):
            count = temporary.stat().st_size if temporary.exists() else 0
            if count == entry['bytes']:
                break
            if count > entry['bytes']:
                raise RuntimeError(f'未完成文件超过预期长度: {temporary}')
            headers = {'User-Agent': 'Delivery-NPC-Training-Preparation'}
            if count:
                headers['Range'] = f'bytes={count}-'
            try:
                with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as response:
                    if count and (response.status != 206 or not response.headers.get('Content-Range', '').startswith(f'bytes {count}-')):
                        raise RuntimeError('服务未接受断点续传，保留已有文件并停止')
                    with temporary.open('ab' if count else 'wb') as stream:
                        previous_report = time.monotonic()
                        while block := response.read(4 * 1024 * 1024):
                            count += len(block)
                            if count > entry['bytes']:
                                raise RuntimeError('下载内容超过预期文件长度')
                            stream.write(block)
                            if time.monotonic() - previous_report >= 5:
                                print(f"model {path.name}: {count}/{entry['bytes']}", flush=True)
                                previous_report = time.monotonic()
                break
            except (OSError, TimeoutError):
                if attempt == 2:
                    raise
        if not verify_model_file(temporary, entry):
            raise RuntimeError(f'模型大小或内容校验失败: {temporary}')
        temporary.replace(path)
    result = dict(entry, url=url, actual_sha256=sha256(path), cached=cached)
    print(f'verified model {path.name}', flush=True)
    return result


def prepare_model():
    requested = json.loads(saved_path('model-files.json').read_text(encoding='utf-8'))
    if requested['repository'] != REPOSITORY or requested['revision'] != REVISION:
        raise RuntimeError('模型清单与本次固定版本不符')
    api = f'https://huggingface.co/api/models/{REPOSITORY}/revision/{REVISION}?blobs=true'
    request = urllib.request.Request(api, headers={'User-Agent': 'Delivery-NPC-Training-Preparation'})
    metadata = json.load(urllib.request.urlopen(request, timeout=45))
    if metadata['sha'] != REVISION or metadata.get('cardData', {}).get('license') != 'apache-2.0':
        raise RuntimeError('原始模型版本或许可不符')
    listed = {entry['rfilename']: entry for entry in metadata['siblings']}
    entries = []
    for original in requested['files']:
        published = listed[original['path']]
        expected = published.get('lfs', {}).get('sha256')
        if original['bytes'] != published['size'] or original.get('sha256') != expected:
            raise RuntimeError('模型清单与 Hugging Face 元数据不符: ' + original['path'])
        entries.append(dict(original, git_blob_sha1=published['blobId']))
    total = sum(entry['bytes'] for entry in entries)
    if total > 4_100_000_000:
        raise RuntimeError('原始模型超过本次确定的 4.1 GB 范围')
    results = [fetch_model_file(entry) for entry in entries]
    report = {'repository': REPOSITORY, 'revision': REVISION, 'license': 'Apache-2.0',
              'source_metadata': api, 'total_bytes': total, 'files': results,
              'completed_utc': datetime.now(timezone.utc).isoformat(), 'loaded_model': False}
    write_json(saved_path('model-downloads.json'), report)
    return report


def training_environment():
    environment = os.environ.copy()
    for name in ('PYTHONPATH', 'PYTHONHOME'):
        environment.pop(name, None)
    temporary = saved_path('temp')
    temporary.mkdir(exist_ok=True)
    environment.update({'PYTHONNOUSERSITE': '1', 'PIP_CONFIG_FILE': 'NUL', 'CUDA_VISIBLE_DEVICES': '',
                        'HF_HOME': str(saved_path('cache/huggingface')), 'HF_HUB_DISABLE_TELEMETRY': '1',
                        'TORCH_HOME': str(saved_path('cache/torch')), 'TEMP': str(temporary), 'TMP': str(temporary)})
    return environment


def install_environment(base_python):
    environment = training_environment()
    version = subprocess.check_output([str(base_python), '-I', '-c', 'import sys; print(sys.version.split()[0])'],
                                      env=environment, text=True).strip()
    if version != '3.10.12':
        raise RuntimeError('本次使用已验证的 Python 3.10.12，请先核对基础解释器')
    virtual = saved_path('.venv')
    if not (virtual / 'Scripts/python.exe').is_file():
        subprocess.run([str(base_python), '-I', '-m', 'venv', str(virtual)], env=environment, check=True)
    python = virtual / 'Scripts/python.exe'
    requirements = REQUIREMENTS.read_text(encoding='utf-8')
    cache_report = {'torch_reused_from_cache': False}
    # 只读旧缓存，并复制经过官方 SHA256 校验的轮子；语音环境及其依赖保持原样
    old_cache = ROOT / 'Saved/AIResearch/TtsEvaluation/qwen/cache/pip'
    wheel = saved_path('wheels/' + TORCH_WHEEL)
    if not wheel.is_file() or sha256(wheel) != TORCH_SHA256:
        for candidate in old_cache.rglob('*.body'):
            if candidate.stat().st_size > 3_000_000_000 and sha256(candidate) == TORCH_SHA256:
                wheel.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(candidate, wheel)
                break
    if wheel.is_file() and sha256(wheel) == TORCH_SHA256:
        requirements = '\n'.join('torch @ ' + wheel.as_uri() + '#sha256=' + TORCH_SHA256
                                 if line.startswith('torch @ ') else line for line in requirements.splitlines()) + '\n'
        cache_report = {'torch_reused_from_cache': True, 'wheel_bytes': wheel.stat().st_size,
                        'wheel_sha256': TORCH_SHA256, 'wheel': str(wheel)}
    resolved = saved_path('resolved-requirements.txt')
    resolved.write_text(requirements, encoding='utf-8')
    command = [str(python), '-I', '-m', 'pip', '--isolated', '--disable-pip-version-check',
               '--cache-dir', str(saved_path('cache/pip'))]
    log = saved_path('environment-install.log')
    with log.open('ab') as output:
        subprocess.run(command + ['install', '--no-input', '--index-url', 'https://pypi.org/simple', 'pip==25.3'],
                       env=environment, stdout=output, stderr=subprocess.STDOUT, check=True)
        subprocess.run(command + ['install', '--no-input', '--index-url', 'https://pypi.org/simple',
                       '--requirement', str(resolved), '--report', str(saved_path('pip-install-report.json'))],
                       env=environment, stdout=output, stderr=subprocess.STDOUT, check=True)
        subprocess.run(command + ['check'], env=environment, stdout=output, stderr=subprocess.STDOUT, check=True)
    frozen = subprocess.check_output(command + ['freeze', '--all'], env=environment, text=True)
    saved_path('installed-packages.txt').write_text(frozen, encoding='utf-8')
    check = ('import json,importlib.metadata as m,torch,transformers,accelerate,peft; '
             'from transformers import Qwen3ForCausalLM,AutoConfig,Trainer; '
             'from peft import LoraConfig,get_peft_model; '
             'print(json.dumps({"versions":{n:m.version(n) for n in '
             '["torch","transformers","accelerate","peft","huggingface-hub","tokenizers","safetensors"]},'
             '"cuda_initialized":torch.cuda.is_initialized(),"imports_passed":True}))')
    checked = json.loads(subprocess.check_output([str(python), '-I', '-c', check], env=environment, text=True))
    if checked['cuda_initialized']:
        raise RuntimeError('环境检查不应初始化 CUDA')
    report = {'python': str(python), 'base_python': str(base_python), 'python_version': version,
              'requirements_sha256': sha256(REQUIREMENTS), 'completed_utc': datetime.now(timezone.utc).isoformat(),
              **checked, **cache_report}
    write_json(saved_path('environment.json'), report)
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return report


def check_model_metadata():
    """只加载配置和分词器，检查 Qwen3 与非思考模板，不读取权重到内存"""
    python = saved_path('.venv/Scripts/python.exe')
    command = ('import json,sys,torch; from transformers import AutoConfig,AutoTokenizer; '
               'path=sys.argv[1]; config=AutoConfig.from_pretrained(path,local_files_only=True); '
               'tokenizer=AutoTokenizer.from_pretrained(path,local_files_only=True); '
               'text=tokenizer.apply_chat_template([{"role":"user","content":"Hello"}],'
               'tokenize=False,add_generation_prompt=True,enable_thinking=False); '
               'print(json.dumps({"model_type":config.model_type,"tokenizer":type(tokenizer).__name__,'
               '"nonthinking_template":text.endswith("<think>\\n\\n</think>\\n\\n"),'
               '"cuda_initialized":torch.cuda.is_initialized(),"weights_loaded":False}))')
    checked = json.loads(subprocess.check_output([str(python), '-I', '-c', command, str(saved_path('model'))],
                                                 env=training_environment(), text=True))
    if checked['model_type'] != 'qwen3' or not checked['nonthinking_template'] or checked['cuda_initialized']:
        raise RuntimeError('模型配置、分词器或非思考模板检查失败')
    write_json(saved_path('model-metadata-check.json'), checked)
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true', help='安装独立环境并下载原始模型')
    parser.add_argument('--download-model', action='store_true', help='只下载、校验模型')
    parser.add_argument('--install-env', action='store_true', help='只准备独立 Python 环境')
    parser.add_argument('--base-python', type=Path, default=Path.home() / 'miniconda3/envs/mlagents/python.exe')
    args = parser.parse_args()
    DESTINATION.mkdir(parents=True, exist_ok=True)
    print(json.dumps({'destination': str(DESTINATION), 'repository': REPOSITORY, 'revision': REVISION,
                      'gpu_or_training_started': False}, ensure_ascii=False), flush=True)
    if not (args.execute or args.download_model or args.install_env):
        return
    if shutil.disk_usage(DESTINATION).free < 18_000_000_000:
        raise RuntimeError('准备阶段需预留至少 18 GB 磁盘，包含原模型、独立环境和轮子缓存')
    results = {}
    with ThreadPoolExecutor(max_workers=2) as pool:
        work = {}
        if args.execute or args.download_model:
            work['model'] = pool.submit(prepare_model)
        if args.execute or args.install_env:
            work['environment'] = pool.submit(install_environment, args.base_python.resolve())
        for name, future in work.items():
            results[name] = future.result()
    if 'model' in results and 'environment' in results:
        results['metadata_check'] = check_model_metadata()
    write_json(saved_path('preparation.json'), {'completed_utc': datetime.now(timezone.utc).isoformat(),
               'gpu_or_training_started': False, **results})
    print('独立训练环境准备完成，尚未加载模型或开始训练', flush=True)


if __name__ == '__main__':
    main()
