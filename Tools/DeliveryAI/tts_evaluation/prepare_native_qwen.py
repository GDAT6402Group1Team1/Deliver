"""转换已下载的两种 B 模型并记录来源，不运行语音推理"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native_tts import file_hash
from native_tts_abi import COMMIT
from neural_tts import load_voice_config, project_path, PROJECT_ROOT

ROOT = PROJECT_ROOT / 'Saved/AIResearch/NativeQwen'


def entry(path, **extra):
    return dict(path=path.relative_to(PROJECT_ROOT).as_posix(), sha256=file_hash(path), **extra)


def verify_vocabulary(checkpoint, converted):
    import gguf
    vocabulary = json.loads((checkpoint / 'vocab.json').read_text(encoding='utf-8'))
    reader = gguf.GGUFReader(str(converted))
    field = reader.fields['tokenizer.ggml.tokens']
    for token, index in vocabulary.items():
        if bytes(field.parts[field.data[index]]).decode('utf-8') != token:
            raise ValueError(f'native_tts_vocabulary_changed:{converted.name}:{index}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--convert', action='store_true')
    parser.add_argument('--runtime', action='store_true')
    args = parser.parse_args()
    source = ROOT / 'source'
    head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if head != COMMIT:
        raise ValueError('native_source_commit_changed')
    config = load_voice_config()
    outputs = ROOT / 'models'
    outputs.mkdir(exist_ok=True)
    inputs = {'male': project_path(config['model_path']),
              'female': project_path(config['voices']['female']['model_path']),
              'codec': project_path(config['model_path']) / 'speech_tokenizer'}
    if args.convert:
        # 上游转换器使用系统默认编码读取词表，Windows 必须显式启用 UTF-8
        if not sys.flags.utf8_mode:
            raise RuntimeError('请使用 python -X utf8 运行模型转换')
        sys.path.insert(0, str(ROOT / 'converter-packages'))
        spec = importlib.util.spec_from_file_location('native_qwen_convert', source / 'convert.py')
        converter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(converter)
        for voice, model in inputs.items():
            fp32 = outputs / f'{voice}-F32.gguf'
            if not fp32.exists():
                temporary = fp32.with_suffix('.building')
                result = (converter.convert_tokenizer_12hz(model, temporary) if voice == 'codec'
                          else converter.convert_talker_base(model, temporary, '0.6B'))
                if result:
                    raise RuntimeError('conversion_failed')
                temporary.replace(fp32)
            if voice != 'codec':
                verify_vocabulary(model, fp32)
            for precision in ('BF16', 'Q8_0'):
                destination = outputs / f'{voice}-{precision}.gguf'
                if not destination.exists():
                    temporary = destination.with_suffix('.building')
                    subprocess.run([str(ROOT / 'build-cpu/quantize.exe'), str(fp32),
                                    str(temporary), precision], check=True)
                    temporary.replace(destination)
                if voice != 'codec':
                    verify_vocabulary(model, destination)
    if args.runtime:
        build = ROOT / 'build-cuda'
        # CUDA 运行库留在独立试验目录，不修改系统 PATH 或驱动
        for name in ('cudart64_12.dll', 'cublas64_12.dll', 'cublasLt64_12.dll'):
            shutil.copy2(ROOT / 'cuda/bin' / name, build / name)
        dependencies = [entry(path) for path in sorted(build.glob('*.dll')) if path.name != 'qwen.dll']
        for precision in ('BF16', 'Q8_0'):
            models = {voice: entry(outputs / f'{voice}-{precision}.gguf',
                        source_revision=config['voices'].get(voice, {}).get('model_revision', config['model_revision']))
                      for voice in inputs}
            runtime = dict(schema_version=1, source_commit=COMMIT, device='CUDA0',
                           quantization=precision, library=entry(build / 'qwen.dll'),
                           dependencies=dependencies, models=models)
            (ROOT / f'runtime-{precision.lower()}.json').write_text(
                json.dumps(runtime, indent=2), encoding='utf-8')
    provenance = dict(source_commit=COMMIT, ggml_commit=subprocess.check_output(
        ['git', '-C', str(source / 'ggml'), 'rev-parse', 'HEAD'], text=True).strip(),
        license=entry(source / 'LICENSE'), inputs={name: {
            'config': entry(path / 'config.json'), 'weights': entry(path / 'model.safetensors')}
            for name, path in inputs.items()},
        outputs=[entry(path) for path in sorted(outputs.glob('*.gguf'))])
    (ROOT / 'conversion-manifest.json').write_text(json.dumps(provenance, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
