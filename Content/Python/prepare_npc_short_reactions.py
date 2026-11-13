"""保留 B 音色原始 WAV，只缩短确定的开头静音并调整短音响度，不加载语音模型"""
import argparse
from array import array
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import wave


ROOT = Path(__file__).resolve().parents[2]
SILENCE_MAX_PCM = 8
MIN_SILENCE_SECONDS = .08
KEEP_LEAD_SECONDS = .06
TARGET_PEAK_DBFS = -6.0


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(manifest_path, output):
    original = json.loads(manifest_path.read_text(encoding='utf-8'))
    if original.get('status') != 'generated_pending_review' or not original.get('worker_closed'):
        raise RuntimeError('原始候选没有完整生成')
    result = copy.deepcopy(original)
    output.mkdir(parents=True, exist_ok=False)
    result['processing'] = '保留原始 WAV；仅缩短开头连续近零静音和调整振幅，不改变音高、速度或结尾'
    result['prepared_from_manifest'] = str(manifest_path.resolve().relative_to(ROOT)).replace('\\', '/')
    for saved, clip in zip(original['clips'], result['clips']):
        # 正式清单保留原版来源，复建时仍从原版处理，避免对处理版再次增加音量
        raw = {**saved, **saved.get('original', {})}
        source = ROOT / raw['source_relative_path']
        if digest(source) != raw['sha256']:
            raise RuntimeError('原始 WAV 与生成记录不一致')
        with wave.open(str(source), 'rb') as sound:
            if (sound.getnchannels(), sound.getsampwidth(), sound.getframerate()) != (1, 2, 24000):
                raise RuntimeError('保留原始单声道 PCM16 / 24000 Hz')
            rate = sound.getframerate()
            values = array('h', sound.readframes(sound.getnframes()))
        if sys.byteorder != 'little':
            values.byteswap()
        leading = next((i for i, value in enumerate(values) if abs(value) > SILENCE_MAX_PCM), len(values))
        if leading == len(values):
            raise RuntimeError('短音没有超过近零阈值的声音')
        trim = max(0, leading - round(KEEP_LEAD_SECONDS * rate)) if leading >= round(MIN_SILENCE_SECONDS * rate) else 0
        retained = values[trim:]
        gain = (10 ** (TARGET_PEAK_DBFS / 20) * 32768) / max(abs(value) for value in retained)
        processed = array('h', (round(value * gain) for value in retained))
        peak = max(abs(value) for value in processed) / 32768
        rms = math.sqrt(sum(value * value for value in processed) / len(processed)) / 32768
        if any(value in (-32768, 32767) for value in processed):
            raise RuntimeError('响度调整产生削波')
        target = output / (clip['asset_name'] + '.wav')
        if sys.byteorder != 'little':
            processed.byteswap()
        pcm = processed.tobytes()
        with wave.open(str(target), 'wb') as sound:
            sound.setnchannels(1)
            sound.setsampwidth(2)
            sound.setframerate(rate)
            sound.writeframes(pcm)
        clip['original'] = {key: raw[key] for key in ('source_relative_path', 'sha256', 'pcm_sha256', 'frames',
                                                     'duration_s', 'peak', 'rms', 'peak_dbfs', 'rms_dbfs')}
        clip['preparation'] = {'silence_max_abs_pcm16': SILENCE_MAX_PCM,
                               'silence_max_dbfs': 20 * math.log10(SILENCE_MAX_PCM / 32768),
                               'minimum_continuous_silence_s': MIN_SILENCE_SECONDS,
                               'retained_lead_s': KEEP_LEAD_SECONDS,
                               'original_leading_near_zero_samples': leading,
                               'trimmed_samples': trim, 'trimmed_seconds': trim / rate,
                               'trimmed_region_max_abs_pcm16': max((abs(value) for value in values[:trim]), default=0),
                               'gain': gain, 'gain_db': 20 * math.log10(gain),
                               'target_peak_dbfs': TARGET_PEAK_DBFS}
        clip.update(source_relative_path=str(target.resolve().relative_to(ROOT)).replace('\\', '/'),
                    sha256=digest(target), pcm_sha256=hashlib.sha256(pcm).hexdigest(), frames=len(pcm) // 2,
                    duration_s=len(pcm) / (2 * rate), peak=peak, rms=rms, peak_dbfs=20 * math.log10(peak),
                    rms_dbfs=20 * math.log10(rms), clipped_samples=0)
        if digest(source) != raw['sha256']:
            raise RuntimeError('原始 WAV 意外变化')
    path = output / 'Manifest.json'
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(str(path))
    return path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    prepare(arguments.manifest, arguments.output)
