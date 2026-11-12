"""用同一组虚构交谈比较模型首句时间，不修改正在运行的后端配置"""
import argparse
import json
import os
import time
import threading
import uuid
from pathlib import Path
from app_server import AppServer
from streaming import SentenceParser, STREAM_INSTRUCTIONS, compact_context
from server import validate_action
from tts import SpeechSynthesizer

SCENARIOS=[('provoke','他刚才是不是在说你？','同伴刚才说车挡在路上，没有提到你'),
           ('apologize','对不起，刚才是我太急了。','快递员刚才撞倒了你的同伴'),
           ('blame','不是我撞的，是他自己冲出来。','你亲眼看见快递员骑车撞到了同伴')]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--models',nargs='+',default=['gpt-6-astra','gpt-6.1-sol','gpt-6-luna'])
    parser.add_argument('--rounds',type=int,default=1)
    parser.add_argument('--output',default='dialogue-benchmark.json');args=parser.parse_args()
    out=Path(__file__).resolve().parents[2]/'Saved/AIResearch'/args.output
    results=[];synth=SpeechSynthesizer()
    try:
        for model in args.models:
            os.environ['DELIVERY_CODEX_MODEL']=model;service=AppServer()
            before=time.monotonic();service._start();startup=time.monotonic()-before
            try:
                for round_index in range(args.rounds):
                    for name,text,fact in SCENARIOS:
                        event,subject=str(uuid.uuid4()),str(uuid.uuid4())
                        context={**{k:str(uuid.uuid4()) for k in ['world_epoch','npc_id','request_id']},
                            'protocol_version':1,'capability_version':1,'context_revision':1,'plan_revision':1,
                            'persona':'你是急脾气但不愚蠢的街坊大军，爱面子，自己看见的事实优先于挑拨，不信就直接反驳。',
                            'allowed_actions':['Wait','LookAt'],
                            'evidence':[{'event_id':str(uuid.uuid4()),'subject_id':subject,'kind':'SeenEvent','description':fact},
                                        {'event_id':event,'subject_id':subject,'kind':'PlayerSpeech','description':text}],
                            'candidates':[{'action':'Wait','target_id':'','duration':1,'evidence_ids':[]},
                                          {'action':'LookAt','target_id':subject,'duration':1,'evidence_ids':[event]}]}
                        row={'model':model,'scenario':name,'round':round_index,'startup_ms':round(startup*1000),'speech':[]}
                        start=time.monotonic();first=[None];sentences=[]
                        def emit(kind,data):
                            if kind=='sentence':
                                if first[0] is None:first[0]=time.monotonic()-start
                                sentences.append(data)
                        reader=SentenceParser(context,validate_action,emit)
                        def delta(value):
                            if value and 'first_delta_ms' not in row:row['first_delta_ms']=round((time.monotonic()-start)*1000)
                            reader.feed(value)
                        try:
                            prompt=json.dumps(compact_context(context),ensure_ascii=False,separators=(',',':'))
                            row['prompt_chars']=len(prompt)+len(STREAM_INSTRUCTIONS)
                            service.stream(prompt,STREAM_INSTRUCTIONS,threading.Event(),delta);reader.finish()
                            row['complete_ms']=round((time.monotonic()-start)*1000)
                            row['first_sentence_ms']=round(first[0]*1000) if first[0] is not None else None
                            row['speech']=[s['text'] for s in sentences]
                            row['action']=reader.proposal['action']
                            if sentences:
                                before=time.monotonic();pcm,rate=synth.synthesize(sentences[0]['text'],'male',sentences[0]['emotion'],threading.Event())
                                row['tts_ms']=round((time.monotonic()-before)*1000);row['audio_seconds']=len(pcm)/(rate*2)
                            row['valid']=True
                        except Exception as error:row.update(valid=False,error=type(error).__name__)
                        results.append(row);out.write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
                        print(json.dumps(row,ensure_ascii=False),flush=True)
            finally:service.close()
    finally:synth.close()

if __name__=='__main__':main()
