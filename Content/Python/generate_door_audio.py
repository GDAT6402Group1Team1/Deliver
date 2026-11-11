"""生成木门敲击的短促低频共振与碎裂噪声，不引入第三方音频依赖"""
import math,random,wave,struct
from pathlib import Path
out=Path(__file__).resolve().parents[2] / 'Design/Audio/Door';out.mkdir(parents=True,exist_ok=True)
rate=48000
for name,duration,broken in [('SFX_Door_AxeHit',.38,False),('SFX_Door_Break',.8,True)]:
 rng=random.Random(443 if broken else 442); values=[];low=0
 for n in range(int(rate*duration)):
  t=n/rate;noise=rng.uniform(-1,1);low=.7*low+.3*noise
  hit=sum(math.sin(2*math.pi*f*t)*math.exp(-t*k)*a for f,k,a in [(115,20,.45),(235,29,.22),(463,35,.14),(790,45,.07)])
  hit+=low*math.exp(-t*38)*.7
  if broken:
   for delay in [0,.038,.09,.17,.26,.39]:
    u=t-delay
    if u>=0: hit+=(noise-low)*math.exp(-u*22)*.22
  values.append(hit)
 peak=max(abs(v) for v in values)
 with wave.open(str(out/(name+'.wav')),'wb') as w:
  w.setparams((1,2,rate,0,'NONE','not compressed'));w.writeframes(b''.join(struct.pack('<h',int(v/peak*28000)) for v in values))
 print(name,duration)
