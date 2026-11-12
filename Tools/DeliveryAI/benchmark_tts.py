"""比较本机语音服务冷启动和准备后首次合成的耗时，检查声音内容一致"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import threading
import time

from streaming import prepare_resources, close_resources, speech_synthesizer, _speech_pool
from tts import SpeechSynthesizer


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--rounds',type=int,default=3)
    args=parser.parse_args()
    if os.environ.get('DELIVERY_TTS_URL') or os.name!='nt':
        parser.error('本次比较仅使用已安装的 Windows 语音服务')
    text='行，听见你道歉了，先送货吧。'
    results=[]
    for round_index in range(args.rounds):
        cold={}
        try:
            for voice in ('male','female'):
                synth=SpeechSynthesizer()
                try:
                    start=time.perf_counter()
                    pcm,rate=synth.synthesize(text,voice,'Calm',threading.Event())
                    elapsed=time.perf_counter()-start
                    cold[voice]=(pcm,rate)
                    results.append({'round':round_index,'mode':'cold','voice':voice,'elapsed_ms':round(elapsed*1000,2),
                                    'bytes':len(pcm),'duration_s':len(pcm)/(rate*2),'sha256':hashlib.sha256(pcm).hexdigest()})
                finally:synth.close()
            start=time.perf_counter()
            futures=prepare_resources()
            submit_ms=(time.perf_counter()-start)*1000
            concurrent.futures.wait(futures,timeout=10)
            assert all(future.done() for future in futures)
            prepare_ms=(time.perf_counter()-start)*1000
            barrier=threading.Barrier(2)
            def measure(voice):
                synth=speech_synthesizer()
                pid=synth.process.pid
                barrier.wait(timeout=2)
                start=time.perf_counter()
                pcm,rate=synth.synthesize(text,voice,'Calm',threading.Event())
                return {'round':round_index,'mode':'prepared_first','voice':voice,
                        'elapsed_ms':round((time.perf_counter()-start)*1000,2),
                        'startup_submit_ms':round(submit_ms,2),'preparation_ms':round(prepare_ms,2),
                        'bytes':len(pcm),'duration_s':len(pcm)/(rate*2),'sha256':hashlib.sha256(pcm).hexdigest(),
                        'pcm_matches_cold':pcm==cold[voice][0] and rate==cold[voice][1],
                        'same_process':synth.process.pid==pid}
            pending=[_speech_pool.submit(measure,voice) for voice in ('male','female')]
            results.extend(future.result(timeout=10) for future in pending)
        finally:close_resources()
    output=Path(__file__).resolve().parents[2]/'Saved/AIResearch/tts-prepare-comparison.json'
    output.write_text(json.dumps({'text':text,'results':results},ensure_ascii=False,indent=2),encoding='utf-8')
    print(output)
    assert all(row.get('pcm_matches_cold',True) and row.get('same_process',True) for row in results)


if __name__=='__main__':main()
