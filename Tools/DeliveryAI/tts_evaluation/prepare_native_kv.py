"""为短句语音准备独立的小缓存 DLL，不修改正在使用的源码或运行配置"""
import argparse
import copy
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native_tts import file_hash, load_runtime
from neural_tts import PROJECT_ROOT, project_path

ROOT = PROJECT_ROOT / 'Saved/AIResearch/NativeQwen'
VENDOR = Path(__file__).resolve().parents[1] / 'native_tts_vendor'
CAPACITY = 1024
OLD_CACHE = '''    // KV caches, one set per slot: the talker holds the LM context up
    // to 4096 positions (the longest ICL prompt observed is ~250 +
    // max_new_tokens ~ 1500, so 4096 has 60% headroom). Predictor holds
    // one frame of 16 sub-steps per slot.
    if (!kv_cache_init(&pt->talker_kv, pt->talker.num_hidden_layers, pt->talker.num_key_value_heads,
                       pt->talker.head_dim, 4096, pt->max_batch, pt->backend)) {'''
NEW_CACHE = '''    // 游戏每句最多生成 256 个音频 token，缩小常驻缓存，给渲染保留显存
    // 实际提示与生成上限在开始这句前检查，超出时拒绝，不截断台词
    if (!kv_cache_init(&pt->talker_kv, pt->talker.num_hidden_layers, pt->talker.num_key_value_heads,
                       pt->talker.head_dim, 1024, pt->max_batch, pt->backend)) {'''
OLD_PROMPT = '''    s.perf.build_ms = t_build.ms();

    if (params->dump_dir) {'''
NEW_PROMPT = '''    s.perf.build_ms = t_build.ms();

    // 先检查完整提示和本句生成上限，再使用任何解码状态
    // 用减法避免两个外部长度相加溢出，不通过截断音频满足容量
    if (s.prompt.T_ctx <= 0 || s.prompt.T_ctx > pt->talker_kv.max_seq_len ||
        params->max_new_tokens <= 0 || params->max_new_tokens > pt->talker_kv.max_seq_len - s.prompt.T_ctx) {
        qt_set_error("tts_engine_admit: context capacity exceeded (prompt=%d, generation=%d, capacity=%d)",
                     s.prompt.T_ctx, params->max_new_tokens, pt->talker_kv.max_seq_len);
        e->slots.pop_back();
        return tts_admit_fail(job, QT_STATUS_INVALID_PARAMS);
    }

    if (params->dump_dir) {'''


def patched_source(original, expected_sha256):
    if hashlib.sha256(original.encode('utf-8')).hexdigest() != expected_sha256:
        raise ValueError('native_kv_source_changed')
    for before, after in ((OLD_CACHE, NEW_CACHE), (OLD_PROMPT, NEW_PROMPT)):
        if original.count(before) != 1:
            raise ValueError('native_kv_patch_context_changed')
        original = original.replace(before, after, 1)
    return original


def prepare():
    manifest = json.loads((VENDOR / 'source-manifest.json').read_text(encoding='utf-8'))
    source = ROOT / 'source'
    for path, revision in ((source, manifest['commit']), (source / 'ggml', manifest['ggml_commit'])):
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise ValueError('native_kv_revision_changed')
    for relative, expected in manifest['files'].items():
        if file_hash(source / relative) != expected:
            raise ValueError('native_kv_source_changed:' + relative)
    original = (source / 'src/pipeline-tts.cpp').read_bytes().decode('utf-8')
    changed = patched_source(original, manifest['files']['src/pipeline-tts.cpp'])
    diff = ''.join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
               fromfile='a/src/pipeline-tts.cpp', tofile='b/src/pipeline-tts.cpp'))
    patch_path = VENDOR / 'kv1024.patch'
    if patch_path.read_bytes() != diff.encode('utf-8'):
        raise ValueError('native_kv_patch_file_changed')
    recorded = manifest['patches']['kv1024']
    if (recorded['capacity'] != CAPACITY or file_hash(patch_path) != recorded['sha256'] or
            hashlib.sha256(changed.encode('utf-8')).hexdigest() != recorded['patched_source_sha256']):
        raise ValueError('native_kv_patch_manifest_changed')
    folder = ROOT / 'source-kv1024'
    folder.mkdir(exist_ok=True)
    (folder / 'pipeline-tts.cpp').write_bytes(changed.encode('utf-8'))
    version = (ROOT / 'build-cuda/version.h').read_text(encoding='utf-8')
    if manifest['commit'][:7] not in version:
        raise ValueError('native_kv_build_version_changed')
    (folder / 'version.h').write_bytes(version.replace(manifest['commit'][:7],
                                  manifest['commit'][:7] + '-kv1024').encode('utf-8'))
    result = {'upstream_commit': manifest['commit'], 'ggml_commit': manifest['ggml_commit'],
              'name': 'talker-kv1024', 'capacity': CAPACITY,
              'patch_sha256': file_hash(patch_path),
              'original_source_sha256': manifest['files']['src/pipeline-tts.cpp'],
              'patched_source_sha256': file_hash(folder / 'pipeline-tts.cpp')}
    (ROOT / 'kv1024-patch-manifest.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def write_runtime(patch):
    baseline = ROOT / 'runtime-q8_0.json'
    original = load_runtime(baseline, verify=True)
    build = ROOT / 'build-cuda-kv1024'
    runtime = copy.deepcopy(original)
    for entry in runtime['dependencies']:
        destination = build / Path(entry['path']).name
        shutil.copy2(project_path(entry['path']), destination)
        entry['path'] = destination.relative_to(PROJECT_ROOT).as_posix()
        if file_hash(destination) != entry['sha256']:
            raise ValueError('native_kv_dependency_changed')
    library = build / 'qwen.dll'
    runtime['library'] = {'path': library.relative_to(PROJECT_ROOT).as_posix(), 'sha256': file_hash(library)}
    runtime['native_patch'] = {**patch, 'baseline_runtime_sha256': file_hash(baseline)}
    destination = ROOT / 'runtime-q8_0-kv1024.json'
    destination.write_text(json.dumps(runtime, indent=2), encoding='utf-8')
    print(destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    patch = prepare()
    if args.runtime:
        write_runtime(patch)
    else:
        print(json.dumps(patch, indent=2))
