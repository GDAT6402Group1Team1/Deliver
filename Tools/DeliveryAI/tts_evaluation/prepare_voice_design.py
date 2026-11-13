"""准备小芳的独立音色试验，只下载 VoiceDesign 并复制已有相同文件"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import time
import urllib.request

from download_models import checked_path, verify


def plan(specification_name='voice_design.json'):
    project = Path(__file__).resolve().parents[3]
    specification = json.loads(Path(__file__).with_name(specification_name).read_text(encoding='utf-8'))
    previous = json.loads(Path(__file__).with_name('models.json').read_text(encoding='utf-8'))['qwen']
    if previous['revision'] != specification['reuse_revision']:
        raise ValueError('原有模型版本已变化，需要重新核对可复用文件')
    entries = {item['path']: item for item in previous['files']}
    files = []
    for item in specification['new_files']:
        files.append({**item, 'method': 'download'})
    for name in specification['reuse_files']:
        files.append({**entries[name], 'method': 'copy',
                      'source': f'Saved/AIResearch/TtsEvaluation/qwen/model/{name}'})
    for item in files:
        item['url'] = f"https://huggingface.co/{specification['model_id']}/resolve/{specification['revision']}/{item['path']}"
    files.append({**specification['license_copy'], 'method': 'copy'})
    if sum(item['bytes'] for item in files if item['method'] == 'download') != specification['download_bytes']:
        raise ValueError('新增下载量与批准的清单不一致')
    root = checked_path(project / 'Saved/AIResearch/TtsEvaluation' / specification['candidate'], project)
    return project, root, specification, files


def download_file(entry, partial):
    # 中断后只继续同一固定版本，全部下载完再核对发布方哈希
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > entry['bytes']:
        raise ValueError('未完成文件已超过登记大小')
    if offset == entry['bytes']:
        return 0
    headers = {'User-Agent': 'Delivery-VoiceDesign-Evaluation/1'}
    if offset:
        headers['Range'] = f'bytes={offset}-'
    request = urllib.request.Request(entry['url'], headers=headers)
    with urllib.request.urlopen(request, timeout=60) as response:
        if offset:
            expected = f"bytes {offset}-{entry['bytes']-1}/{entry['bytes']}"
            if response.status != 206 or response.headers.get('Content-Range') != expected:
                raise ValueError('服务器未返回请求的后续字节，保留文件并停止')
        elif response.status != 200:
            raise ValueError(f'下载返回异常状态：{response.status}')
        total, received, last_report = offset, 0, time.monotonic()
        with partial.open('ab' if offset else 'xb') as output:
            while block := response.read(1024 * 1024):
                total += len(block)
                received += len(block)
                if total > entry['bytes']:
                    raise ValueError('下载超过登记大小')
                output.write(block)
                if time.monotonic() - last_report >= 10:
                    print(f"下载 {entry['path']} {total}/{entry['bytes']} bytes", flush=True)
                    last_report = time.monotonic()
    return received


def main(specification_name='voice_design.json'):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    project, root, specification, files = plan(specification_name)
    if not args.execute:
        print(json.dumps({'destination': str(root), 'new_download_bytes': specification['download_bytes'],
                          'final_bytes': sum(item['bytes'] for item in files),
                          'gpu_started': False, 'production_changed': False}, ensure_ascii=False))
        return
    root.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(root).free < 5_000_000_000:
        raise ValueError('试验目录所在磁盘可用空间不足 5 GB')
    model_dir = checked_path(root / 'model', project)
    model_dir.mkdir(exist_ok=True)
    report_path = checked_path(root / 'model-downloads.json', project)
    report = {'candidate': specification['candidate'], 'revision': specification['revision'],
              'model_id': specification['model_id'], 'license': specification['license'],
              'status': 'preparing', 'network_bytes_this_run': 0, 'files': []}
    def save():
        temporary = report_path.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        temporary.replace(report_path)
    save()
    try:
        # 先复制已验证的小文件和编解码器，再开始下载新权重
        for entry in sorted(files, key=lambda item: (item['method'] == 'download', item['bytes'])):
            target = checked_path(model_dir / entry['path'], project)
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                result = verify(target, entry)
                result['method'] = 'verified_existing'
            else:
                partial = checked_path(target.with_name(target.name + '.partial'), project)
                if entry['method'] == 'copy':
                    source = checked_path(project / entry['source'], project)
                    verify(source, entry)
                    if not partial.exists():
                        with source.open('rb') as source_file, partial.open('xb') as output:
                            shutil.copyfileobj(source_file, output, 1024 * 1024)
                    result = verify(partial, entry)
                    result['copied_from'] = str(source)
                else:
                    report['network_bytes_this_run'] += download_file(entry, partial)
                    result = verify(partial, entry)
                partial.rename(target)
                result['method'] = entry['method']
            report['files'].append(result)
            save()
            print(f"已核对 {entry['path']} ({result['method']}, {result['bytes']} bytes)", flush=True)
        report.update(status='completed', completed_at=datetime.now(timezone.utc).isoformat())
    except BaseException as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
