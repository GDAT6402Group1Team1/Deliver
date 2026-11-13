"""把用户选定的小芳 B 原音频保存到项目美术目录，并记录生成来源"""
import hashlib
import json
import shutil
import wave

from download_models import checked_path
from evaluate import save_json
from prepare_voice_design import plan


def main():
    project, _, specification, _ = plan('voice_clone.json')
    reference = specification['reference']
    report_path = checked_path(project / reference['source_report'], project)
    report = json.loads(report_path.read_text(encoding='utf-8'))
    selected = [sample for sample in report['samples']
                if sample['voice'] == 'B' and sample['text'] == reference['text']]
    if report['status'] != 'completed' or len(selected) != 1:
        raise ValueError('无法从试听记录唯一确定选中的 B 声音')
    sample = selected[0]
    source = checked_path(sample['wav'], project)
    if sample['sha256'] != reference['sha256'] or hashlib.sha256(source.read_bytes()).hexdigest() != reference['sha256']:
        raise ValueError('原试听文件已变化，不覆盖用户选择')
    with wave.open(str(source), 'rb') as audio:
        if (audio.getframerate(), audio.getnchannels(), audio.getsampwidth(), audio.getnframes()) != (24000, 1, 2, 92160):
            raise ValueError('选中声音的格式或长度与试听记录不一致')
        pcm_hash = hashlib.sha256(audio.readframes(audio.getnframes())).hexdigest()
    destination = checked_path(project / reference['path'], project)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).hexdigest() != reference['sha256']:
            raise ValueError('项目中已有不同的 B 声音文件，请先检查')
    else:
        with source.open('rb') as original, destination.open('xb') as output:
            shutil.copyfileobj(original, output)
    manifest = {'version': 1, 'selected_by_user': True, 'synthetic_reference': True,
                'text': reference['text'], 'audio': destination.name,
                'wav_sha256': reference['sha256'], 'pcm_sha256': pcm_hash,
                'duration_s': sample['duration_s'], 'sample_rate': sample['sample_rate'],
                'model_id': report['model_id'], 'model_revision': report['revision'],
                'model_license': report['license'], 'source_report': reference['source_report'],
                'source_audio': str(source.relative_to(project)), 'description': sample['description'],
                'seed': sample['seed'], 'generation_settings': report['settings'],
                'packages': report['packages'], 'implementation': report['implementation'],
                'processing': '完整复制用户选中的 WAV，没有变调、裁剪或重新合成',
                'scope': '此声音由本项目生成，不使用真人参考；后续 Base 输出需要另行试听，不能声称与参考逐字节相同'}
    save_json(destination.with_suffix('.json'), manifest)
    print(str(destination), flush=True)


if __name__ == '__main__':
    main()
