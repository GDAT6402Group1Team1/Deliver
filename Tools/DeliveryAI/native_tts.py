"""原生 Qwen 的独立对照入口，沿用现有排队、音频协议和子进程退出规则"""
import hashlib
import json
from pathlib import Path

from neural_tts import (QwenSpeechSynthesizer, NeuralTtsError, project_path,
                        neural_voice_identities)
from native_tts_abi import COMMIT

DEFAULT_RUNTIME = 'Saved/AIResearch/NativeQwen/runtime-q8_0.json'


def file_hash(path):
    """按 1 MB 分块计算文件的 SHA-256"""
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def load_runtime(path, verify=False):
    """读取本地语音运行时配置，检查版本、源码提交和模型文件；verify 为 True 时同时核对文件哈希"""
    runtime = json.loads(project_path(path).read_text(encoding='utf-8'))
    voice_residency = runtime.get('voice_residency', 'both')
    shared_codec = runtime.get('shared_codec', False)
    if (runtime.get('schema_version') != 1 or runtime.get('source_commit') != COMMIT
            or runtime.get('device') != 'CUDA0' or runtime.get('quantization') not in ('BF16', 'Q8_0')
            or voice_residency not in ('both', 'active') or not isinstance(shared_codec, bool)
            or (shared_codec and voice_residency != 'both')
            or set(runtime.get('models', {})) != {'male', 'female', 'codec'}):
        raise NeuralTtsError('native_tts_invalid_runtime')
    entries = [runtime['library'], *runtime['dependencies'], *runtime['models'].values()]
    for entry in entries:
        asset = project_path(entry['path'])
        if verify and (not asset.is_file() or file_hash(asset) != entry['sha256']):
            raise NeuralTtsError('native_tts_asset_changed')
    return runtime


def native_identities(config, runtime):
    """按音色配置生成本地语音用的音色信息，去掉只给旧引擎用的字段"""
    identities = neural_voice_identities(config)
    for voice, value in identities.items():
        value.pop('talker_mode', None)
        value.pop('predictor_mode', None)
        value.update(runtime='qwentts_cpp', source_commit=COMMIT,
                     gguf_sha256=runtime['models']['male' if voice == 'ergou' else voice]['sha256'],
                     codec_sha256=runtime['models']['codec']['sha256'],
                     quantization=runtime['quantization'])
    return identities


class NativeQwenSpeechSynthesizer(QwenSpeechSynthesizer):
    """正式声音保持原配置；只有显式创建本类时才使用原生对照进程"""
    def __init__(self, config_path=None, runtime_path=DEFAULT_RUNTIME):
        """读取音色配置和本地运行时配置"""
        super().__init__(config_path)
        self.runtime_path = project_path(runtime_path)
        self.runtime = load_runtime(self.runtime_path)
        self.identities = native_identities(self.config, self.runtime)

    def _worker_command(self, executable):
        """启动本地语音子进程的命令行"""
        return [str(executable), '-B', '-X', 'utf8', '-u',
                str(Path(__file__).with_name('native_tts_worker.py')),
                '--config', str(self.config_path), '--runtime', str(self.runtime_path)]

    def _worker_log_path(self):
        """本地语音子进程的日志位置"""
        return project_path('Saved/Logs/NativeQwenSpeechWorker.log')
