"""让迭代记录沿用方案文档的阅读版式，正文仍直接保存在 HTML 中"""
from pathlib import Path
from bs4 import BeautifulSoup

ROOT=Path(__file__).resolve().parents[2]

def build():
    path=ROOT/'Document/AIIteration.html'
    source=BeautifulSoup(path.read_text(encoding='utf-8'),'html.parser')
    plan=BeautifulSoup((ROOT/'Document/DeliveryAIPlan.html').read_text(encoding='utf-8'),'html.parser')
    main=source.main
    sections=main.find_all('section',recursive=False)
    if not sections:
        sections=[]
        section=None
        for node in list(main.children):
            if getattr(node,'name',None)=='h2':
                section=source.new_tag('section'); sections.append(section)
            if section is not None: section.append(node.extract())
    navigation=[]
    for index,section in enumerate(sections,1):
        section['id']=f'iteration-{index}'
        navigation.append(f'<a href="#iteration-{index}">{section.h2.get_text()}</a>')
        for table in section.find_all('table'):
            if table.parent.get('class')!=['table-scroll']:
                table.wrap(source.new_tag('div',attrs={'class':'table-scroll','tabindex':'0','aria-label':'可横向滚动的数据表'}))
    content=''.join(str(s) for s in sections)
    css=plan.style.get_text()+'.finding{border-left:3px solid #426f9d;background:#f3f7fc;padding:12px 18px;margin:18px 0}.evidence{color:#536579;font-size:14px}nav a{line-height:1.55}'
    html=f'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Delivery AI · 研究与迭代记录</title><style>{css}</style></head>
<body><div class="layout"><aside><strong>Delivery · 技术文档</strong><div class="meta">UE 5.8 · 研究与迭代</div><nav><a href="DeliveryAIPlan.html">返回方案文档</a>{''.join(navigation)}</nav></aside><main>
<header><div class="meta">AI / 交谈、语音与表情 / 2026-11-13</div><h1>研究与迭代记录</h1><p>记录每次修改的原因、参考依据和测试结果。先说明问题与结论，再展开实现和验证过程。</p>
<div class="notice">本页保留试验中的失败与后续修复。大军的 B 音色已接入常驻 Qwen，小芳保留 SAPI；后台检查通过，新的游戏听感、等待和多人效果仍待试玩确认。</div>
<div class="actions"><a href="DeliveryAIPlan.html">当前方案</a><a href="#iteration-1">交谈延迟</a><a href="#iteration-2">人物表情</a><a href="#iteration-3">验证记录</a><button id="print" type="button">打印 / 保存 PDF</button></div></header>
{content}<footer>与方案文档使用相同版式，支持离线阅读。持续记录本项目的实际结果，不把外部演示指标当作本项目性能。</footer></main></div><script>document.getElementById('print').addEventListener('click',()=>window.print());</script></body></html>'''
    path.write_text(html,encoding='utf-8')
    print(path)

if __name__=='__main__': build()
