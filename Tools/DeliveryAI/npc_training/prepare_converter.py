"""下载与现有 llama.cpp 同版本的转换脚本，不更改训练和语音依赖"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[3]
DESTINATION = ROOT / 'Saved/AIResearch/NpcTraining/converter'
REVISION = '47a39665e7081dc482feec169961acc09750a5c4'
URL = f'https://codeload.github.com/ggml-org/llama.cpp/zip/{REVISION}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download', action='store_true')
    args = parser.parse_args()
    print(json.dumps({'url': URL, 'directory': str(DESTINATION), 'gpu_used': False}))
    if not args.download:
        return
    DESTINATION.mkdir(parents=True, exist_ok=True)
    archive = DESTINATION / 'source.zip'
    if not archive.is_file():
        urllib.request.urlretrieve(URL, archive)
    base = DESTINATION / 'llama.cpp'
    base.mkdir(exist_ok=True)
    included = []
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.infolist():
            relative = Path(*Path(member.filename).parts[1:])
            if member.is_dir() or not relative.parts:
                continue
            if relative.parts[0] not in ('gguf-py', 'conversion', 'convert_hf_to_gguf.py', 'LICENSE'):
                continue
            target = (base / relative).resolve()
            target.relative_to(base.resolve())
            content = zipped.read(member)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
            included.append({'path': str(relative), 'sha256': hashlib.sha256(content).hexdigest()})
    (DESTINATION / 'source.json').write_text(json.dumps({'source': URL, 'revision': REVISION,
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(), 'files': included}, indent=2), encoding='utf-8')
    print(json.dumps({'saved_files': len(included), 'archive_bytes': archive.stat().st_size}))


if __name__ == '__main__':
    main()
