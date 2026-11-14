from datetime import datetime, timezone
import hashlib, json, os
from pathlib import Path
import subprocess, time
ROOT=Path('D:/Udev/Delivery')
OUT=ROOT/'Saved/AIResearch/NpcTraining/combat-v7/export-round3-q4'
INSTALL=ROOT/'Saved/AIResearch/LocalNpcModel/qwen3-1.7b'
def stamp(): return datetime.now(timezone.utc).isoformat()
def sha(p):
 d=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''): d.update(b)
 return d.hexdigest()
report={'schema_version':1,'started_utc':stamp(),'status':'checking_files','training_performed':False,'production_configuration_changed':False,'device':'cpu','quantization':'Q4_K_M'}
def save(): (OUT/'export.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
save()
try:
 plan=json.loads((OUT/'plan.json').read_text(encoding='utf-8'))
 report['plan_sha256']=sha(OUT/'plan.json')
 frozen=ROOT/plan['invariants']['frozen_source']; prep=ROOT/plan['invariants']['input_preparation']; evaluator=ROOT/plan['invariants']['evaluation_script']
 for path,expected in ((frozen,plan['invariants']['frozen_sha256']),(prep,plan['invariants']['input_preparation_sha256']),(evaluator,plan['invariants']['evaluation_script_sha256'])):
  if sha(path)!=expected: raise RuntimeError('Input or evaluation file changed: '+str(path))
 report['frozen_sha256']=sha(frozen); report['input_preparation_sha256']=sha(prep); report['evaluation_script_sha256']=sha(evaluator)
 installation=json.loads((INSTALL/'installation.json').read_text(encoding='utf-8'))
 report['runtime_files_verified']=[]
 for entry in installation['runtime_files']:
  path=INSTALL/entry['path']; value=sha(path)
  if value!=entry['sha256']: raise RuntimeError('Runtime file changed: '+str(path))
  report['runtime_files_verified'].append({'path':str(path),'sha256':value})
 quantizer=ROOT/plan['source']['quantizer_path']
 if sha(quantizer)!=plan['source']['expected_quantizer_sha256']: raise RuntimeError('Quantizer changed')
 inp=ROOT/plan['source']['bf16_path']; output=ROOT/plan['output']['model']
 if output.exists(): raise FileExistsError(output)
 report['input_bf16']={'path':str(inp),'bytes':inp.stat().st_size,'sha256':sha(inp),'hash_recorded_now_not_during_original_export':True}
 report['command']=[str(quantizer),str(inp),str(output),'Q4_K_M']
 report['quantizer_sha256']=sha(quantizer); report['status']='quantizing'; report['quantize_started_utc']=stamp();save()
 environment=os.environ.copy(); environment['CUDA_VISIBLE_DEVICES']='-1'; environment['PATH']=str(quantizer.parent)+os.pathsep+environment.get('PATH','')
 started=time.monotonic()
 with (OUT/'quantize.log').open('wb') as log:
  process=subprocess.Popen(report['command'],cwd=quantizer.parent,env=environment,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
  report['quantizer_pid']=process.pid;save()
  report['exit_code']=process.wait()
 report['quantize_elapsed_seconds']=round(time.monotonic()-started,3);report['quantize_finished_utc']=stamp()
 if report['exit_code']!=0: raise RuntimeError('quantize failed with '+str(report['exit_code']))
 report['model_path']=str(output);report['model_bytes']=output.stat().st_size;report['model_sha256']=sha(output)
 report['quantize_log_sha256']=sha(OUT/'quantize.log');report['status']='exported_pending_acceptance'
except BaseException as error:
 report['status']='failed';report['error']=type(error).__name__+': '+str(error)
 raise
finally:
 report['finished_utc']=stamp();save()
 print(json.dumps({k:v for k,v in report.items() if k not in ('runtime_files_verified',)},ensure_ascii=False),flush=True)
