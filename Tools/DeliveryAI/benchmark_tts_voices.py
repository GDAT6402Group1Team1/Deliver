"""输出现有男女声音的试听文件，并记录实际声音名称和耗时"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import threading
import time
import wave

from tts import SpeechSynthesizer


def main():
    if os.environ.get('DELIVERY_TTS_URL') or os.name!='nt':
        raise RuntimeError('本次检查只使用已安装的 Windows 声音')
    original_shell=os.environ.get('DELIVERY_TTS_SHELL')
    output=Path(__file__).resolve().parents[2]/'Saved/AIResearch'
    text='行，听见你道歉了，先送货吧。'
    rows=[]
    try:
        for mode,shell in [('automatic',None),('windows_powershell',shutil.which('powershell.exe'))]:
            if mode=='windows_powershell' and not shell:
                continue
            if shell:os.environ['DELIVERY_TTS_SHELL']=shell
            else:os.environ.pop('DELIVERY_TTS_SHELL',None)
            synth=SpeechSynthesizer()
            try:
                start=time.perf_counter()
                synth.prepare(threading.Event())
                preparation_ms=round((time.perf_counter()-start)*1000,2)
                pid=synth.process.pid
                for voice in ('male','female'):
                    start=time.perf_counter()
                    pcm,rate=synth.synthesize(text,voice,'Calm',threading.Event())
                    elapsed_ms=round((time.perf_counter()-start)*1000,2)
                    path=output/f'npc-voice-{mode}-{voice}.wav'
                    with wave.open(str(path),'wb') as audio:
                        audio.setnchannels(1);audio.setsampwidth(2);audio.setframerate(rate);audio.writeframes(pcm)
                    rows.append({'mode':mode,'voice_setting':voice,'selected_voice':synth.voice_name,
                                 'shell':synth.shell_path,'available_voices':synth.available_voices,
                                 'preparation_ms':preparation_ms,'synthesis_ms':elapsed_ms,'bytes':len(pcm),
                                 'duration_s':len(pcm)/(rate*2),'sha256':hashlib.sha256(pcm).hexdigest(),
                                 'same_process':pid==synth.process.pid,'wav':str(path)})
            finally:synth.close()
    finally:
        if original_shell is None:os.environ.pop('DELIVERY_TTS_SHELL',None)
        else:os.environ['DELIVERY_TTS_SHELL']=original_shell
    result={'text':text,'note':'本次改变了人物声音；PCM 和时长不同属于声音效果变化，不能沿用此前同一声音的内容一致结论。',
            'results':rows}
    (output/'tts-voice-comparison.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    for row in rows:
        print(json.dumps({key:row[key] for key in ('mode','voice_setting','selected_voice','preparation_ms','synthesis_ms','duration_s','same_process')},ensure_ascii=False))


if __name__=='__main__':main()
