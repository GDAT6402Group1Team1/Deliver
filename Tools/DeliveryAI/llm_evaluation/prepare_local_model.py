"""下载固定版本的小模型和便携推理程序，仅写入 Saved，不安装或启动服务"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import stat
import time
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[3]
CONFIG = Path(__file__).with_name('qwen_local.json')
INSTALL = ROOT / 'Saved/AIResearch/LocalNpcModel/qwen3-1.7b'


def file_hash(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def inside(root, relative):
    target = (root / relative).resolve()
    target.relative_to(root.resolve())
    return target


def checked_file(path, entry):
    return path.is_file() and path.stat().st_size == entry['bytes'] and file_hash(path) == entry['sha256']


def download(entry):
    path = inside(INSTALL, entry['path'])
    path.parent.mkdir(parents=True, exist_ok=True)
    if checked_file(path, entry):
        return {'path': entry['path'], 'bytes': entry['bytes'], 'sha256': entry['sha256'], 'cached': True}
    if path.exists():
        raise RuntimeError(f'已有文件与固定版本不符，请先检查: {path}')
    partial = path.with_suffix(path.suffix + '.part')
    for attempt in range(3):
        offset = partial.stat().st_size if partial.exists() else 0
        if offset == entry['bytes']:
            break
        if offset > entry['bytes']:
            raise RuntimeError(f'未完成文件超过预期大小: {partial}')
        headers = {'User-Agent': 'Delivery-Local-NPC-Evaluation'}
        if offset:
            headers['Range'] = f'bytes={offset}-'
        request = urllib.request.Request(entry['url'], headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                if offset and (response.status != 206 or not response.headers.get('Content-Range', '').startswith(f'bytes {offset}-')):
                    raise RuntimeError('下载服务没有接受断点续传，保留已有文件并停止')
                with partial.open('ab' if offset else 'wb') as output:
                    last_report = time.monotonic()
                    while block := response.read(4 * 1024 * 1024):
                        offset += len(block)
                        if offset > entry['bytes']:
                            raise RuntimeError('服务器返回内容超过固定文件大小')
                        output.write(block)
                        if time.monotonic() - last_report >= 5:
                            print(f"download {path.name}: {offset}/{entry['bytes']}", flush=True)
                            last_report = time.monotonic()
            break
        except (OSError, TimeoutError):
            if attempt == 2:
                raise
    if not checked_file(partial, entry):
        raise RuntimeError(f'下载大小或 SHA256 不符，保留文件供检查: {partial}')
    partial.replace(path)
    print(f'verified {path.name}', flush=True)
    return {'path': entry['path'], 'bytes': entry['bytes'], 'sha256': entry['sha256'], 'cached': False}


def extract(entry):
    archive = inside(INSTALL, entry['path'])
    destination = inside(INSTALL, entry['extract'])
    with zipfile.ZipFile(archive) as source:
        if sum(item.file_size for item in source.infolist()) > 3_000_000_000:
            raise RuntimeError('压缩包展开体积超过试验上限')
        for member in source.infolist():
            target = inside(destination, member.filename)
            if stat.S_ISLNK(member.external_attr >> 16):
                raise RuntimeError('压缩包含链接，停止解压')
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(member) as stream, target.open('wb') as output:
                shutil.copyfileobj(stream, output, 1024 * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download', action='store_true', help='下载并校验；默认只显示预算')
    args = parser.parse_args()
    config = json.loads(CONFIG.read_text(encoding='utf-8'))
    total = sum(entry['bytes'] for entry in config['files'])
    if total > config['download_limit_bytes']:
        raise RuntimeError('下载总量超过已确定的 2 GB 上限')
    print(json.dumps({'download_bytes': total, 'install': str(INSTALL), 'model_license': config['model_license'],
                      'runtime_license': config['runtime_license'], 'gpu_started': False}, ensure_ascii=False), flush=True)
    if not args.download:
        return
    INSTALL.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(INSTALL).free < 5_000_000_000:
        raise RuntimeError('需预留至少 5 GB 磁盘，用于下载文件和便携运行库')
    records = []
    for entry in config['files']:
        records.append(download(entry))
        if 'extract' in entry:
            extract(entry)
    runtime_files = [{'path': str(path.relative_to(INSTALL)), 'bytes': path.stat().st_size,
                      'sha256': file_hash(path)} for path in sorted((INSTALL / 'runtime').rglob('*')) if path.is_file()]
    executables = [row['path'] for row in runtime_files if Path(row['path']).name == 'llama-server.exe']
    if len(executables) != 1:
        raise RuntimeError('便携包应包含且只包含一个 llama-server.exe')
    result = {'completed_utc': datetime.now(timezone.utc).isoformat(), 'config_sha256': file_hash(CONFIG),
              'download_bytes': total, 'files': records, 'runtime_files': runtime_files,
              'server_executable': executables[0], 'gpu_started': False, 'config': config}
    (INSTALL / 'installation.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'installed': str(INSTALL), 'files_verified': len(records), 'runtime_files': len(runtime_files),
                      'gpu_started': False}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
