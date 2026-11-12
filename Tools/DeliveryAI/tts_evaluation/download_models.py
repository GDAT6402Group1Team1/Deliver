"""按固定版本下载试听模型，文件只写入本次试验目录"""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import urllib.request


def checked_path(path, root):
    path = Path(path).absolute()
    root = Path(root).absolute()
    path.relative_to(root)
    for current in (path, *path.parents):
        if current.exists() or current.is_symlink():
            attributes = getattr(current.lstat(), 'st_file_attributes', 0)
            if current.is_symlink() or attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise ValueError(f'试验路径不能包含链接或目录联接: {current}')
        if current == root:
            break
    path.resolve().relative_to(root.resolve())
    return path


def verify(path, entry, previous=None):
    size = path.stat().st_size
    if size > entry.get('max_bytes', entry.get('bytes', 0)):
        raise ValueError(f'文件超过登记大小: {path.name}')
    if 'bytes' in entry and size != entry['bytes']:
        raise ValueError(f'文件大小不一致: {path.name}')
    sha256 = hashlib.sha256()
    git_hash = hashlib.sha1(f'blob {size}\0'.encode())
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            sha256.update(chunk)
            git_hash.update(chunk)
    digest = sha256.hexdigest()
    if entry.get('sha256') and digest != entry['sha256']:
        raise ValueError(f'发布方 SHA-256 不一致: {path.name}')
    if entry.get('git_blob_sha1') and git_hash.hexdigest() != entry['git_blob_sha1']:
        raise ValueError(f'固定提交中的文件内容不一致: {path.name}')
    if previous and digest != previous['sha256']:
        raise ValueError(f'文件与上次下载记录不一致: {path.name}')
    return {'path': entry['path'], 'bytes': size, 'sha256': digest,
            'url': entry['url'],
            'publisher_hash_checked': bool(entry.get('sha256') or entry.get('git_blob_sha1'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True, choices=('qwen', 'kokoro'))
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[3]
    catalog = json.loads(Path(__file__).with_name('models.json').read_text(encoding='utf-8'))
    model = catalog[args.candidate]
    destination = checked_path(project / 'Saved/AIResearch/TtsEvaluation' / args.candidate, project)
    if not args.execute:
        print(json.dumps({'candidate': args.candidate, 'destination': str(destination),
                          'revision': model['revision'], 'status': 'plan_only'}, ensure_ascii=False))
        return
    model_dir = checked_path(destination / 'model', project)
    record_path = checked_path(destination / 'model-downloads.json', project)
    model_dir.mkdir(parents=True, exist_ok=True)
    previous = {}
    if record_path.exists():
        previous = {item['path']: item for item in json.loads(record_path.read_text(encoding='utf-8'))['files']}
    completed = []
    for entry in model['files']:
        target = checked_path(model_dir / entry['path'], model_dir)
        if target.exists():
            if not entry.get('sha256') and not entry.get('git_blob_sha1') and entry['path'] not in previous:
                raise ValueError(f'已有文件缺少校验记录，保留文件并停止: {target}')
            result = verify(target, entry, previous.get(entry['path']))
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = checked_path(target.with_name(target.name + '.partial'), model_dir)
            # 下载失败时保留未完成文件，下一次运行不会覆盖它
            total = 0
            with partial.open('xb') as output:
                request = urllib.request.Request(entry['url'], headers={'User-Agent': 'Delivery-TTS-Evaluation/1'})
                with urllib.request.urlopen(request, timeout=30) as response:
                    if response.status != 200:
                        raise ValueError(f'下载返回异常状态: {response.status}')
                    while chunk := response.read(1024 * 1024):
                        total += len(chunk)
                        if total > entry.get('max_bytes', entry.get('bytes', 0)):
                            raise ValueError(f'下载超过登记大小: {target.name}')
                        output.write(chunk)
            result = verify(partial, entry)
            partial.rename(target)
        completed.append(result)
        # 每完成一个文件保存校验结果，便于中断后核对已有下载
        record_path.write_text(json.dumps({'candidate': args.candidate, 'revision': model['revision'],
                                          'files': completed}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(f"已核对 {entry['path']} ({result['bytes']} bytes)", flush=True)


if __name__ == '__main__':
    main()
