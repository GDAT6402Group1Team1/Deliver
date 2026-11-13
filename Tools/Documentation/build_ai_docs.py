"""从主文档生成离线 AI 阅读版和 SVG 图，避免 HTML 与 Markdown 分别维护"""
from pathlib import Path
from html import escape
import re
import markdown
from bs4 import BeautifulSoup
from ai_diagrams import diagrams

ROOT = Path(__file__).resolve().parents[2]
START = '## AI 居民架构参考（2026-11-12）'
END = '## LLM 行人奇遇实施方案（2026-11-12，设计阶段）'


def build():
    source = (ROOT / 'Document/Architecture.md').read_text(encoding='utf-8')
    current = source.split(START, 1)[1].split(END, 1)[0]
    graphs = diagrams()
    output = ROOT / 'Document/Diagrams/AI'
    output.mkdir(parents=True, exist_ok=True)
    for name, svg in graphs.items():
        (output / f'{name}.svg').write_text(svg, encoding='utf-8')

    sections = re.split(r'^### (.+)$', current, flags=re.M)
    navigation, articles = [], []
    for index in range(1, len(sections), 2):
        heading, content = sections[index].strip(), sections[index+1]
        ident = f'chapter-{(index+1)//2}'
        navigation.append(f'<a href="#{ident}">{escape(heading)}</a>')
        soup = BeautifulSoup(markdown.markdown(content, extensions=['tables', 'fenced_code']), 'html.parser')
        for table in soup.find_all('table'):
            wrapper = soup.new_tag('div', attrs={'class': 'table-scroll', 'tabindex': '0', 'aria-label': '可横向滚动的数据表'})
            table.wrap(wrapper)
        for img in list(soup.find_all('img')):
            name = Path(img['src']).stem
            if name not in graphs: raise ValueError(f'Unknown diagram: {name}')
            svg = graphs[name].replace('id="arrow"', f'id="{name}-arrow"').replace('id="diamond"', f'id="{name}-diamond"')
            svg = svg.replace('url(#arrow)', f'url(#{name}-arrow)').replace('url(#diamond)', f'url(#{name}-diamond)')
            figure = BeautifulSoup(f'<figure id="diagram-{name}" class="uml"><figcaption>{escape(img.get("alt", name))}</figcaption><div class="diagram-tools"><button type="button" data-size="actual">原始尺寸</button><button type="button" data-size="fit">适应宽度</button><a href="Diagrams/AI/{name}.svg" target="_blank" rel="noopener">单独查看 SVG</a></div><div class="diagram-viewport" tabindex="0" aria-label="可横向滚动的 UML 图">{svg}</div></figure>', 'html.parser')
            (img.parent if img.parent.name == 'p' else img).replace_with(figure)
        articles.append(f'<section id="{ident}"><h2>{escape(heading)}</h2>{soup}</section>')

    # 阅读摘要与正文一起维护，原始研究记录仍保存在主文档的历史章节
    css = '''
    :root{color-scheme:light;--line:#d8dfe8;--ink:#202b38;--blue:#2463a8}*{box-sizing:border-box}
    html{scroll-behavior:smooth;scroll-padding-top:22px}body{margin:0;background:#fff;color:var(--ink);font:16px/1.8 "Segoe UI","Microsoft YaHei",sans-serif}
    .layout{display:grid;grid-template-columns:235px minmax(0,1fr);gap:44px;max-width:1570px;margin:auto;padding:38px 38px 70px}
    aside{position:sticky;top:28px;align-self:start;max-height:92vh;overflow:auto}aside strong{font-size:18px}nav{display:grid;gap:4px;margin-top:18px}
    nav a{font-size:14px;padding:6px 10px;border-left:2px solid var(--line);text-decoration:none}nav a:hover{background:#edf3fb;border-color:var(--blue)}
    a{color:var(--blue);overflow-wrap:anywhere}main{min-width:0}header{border-bottom:2px solid #426f9d;padding:0 0 26px}h1{font-size:34px;line-height:1.3;margin:10px 0 20px}h2{font-size:25px;line-height:1.4;margin:0 0 22px}h3,h4{line-height:1.5}
    .meta{color:#5a6979;font-size:14px}.notice{background:#fff6df;border-left:4px solid #c39340;padding:14px 18px;margin:20px 0;font-size:14px}.actions{display:flex;gap:16px;flex-wrap:wrap}
    section{padding:34px 0;border-bottom:1px solid var(--line);overflow-wrap:anywhere}p{margin:14px 0}li{margin:7px 0}code{font:14px Consolas,"Microsoft YaHei",monospace;background:#eef2f6;padding:2px 4px;overflow-wrap:anywhere}pre{white-space:pre-wrap;overflow:auto;padding:16px;background:#eef2f6}
    .table-scroll{overflow-x:auto;margin:20px 0;max-width:100%}table{border-collapse:collapse;min-width:690px;width:100%;font-size:14px}th,td{padding:12px 14px;border:1px solid var(--line);text-align:left;vertical-align:top}th{background:#eef3fa}
    figure.uml{margin:24px 0;border:1px solid var(--line);background:#fff}figcaption{padding:12px 16px;background:#f4f7fb;font-weight:600}.diagram-tools{display:flex;align-items:center;gap:12px;padding:10px 16px;flex-wrap:wrap;font-size:13px}.diagram-viewport{overflow:auto;width:100%;padding:8px}.diagram-viewport svg{display:block;max-width:none;width:100%;height:auto}
    button{font:inherit;color:var(--blue);background:#fff;border:1px solid #aebed1;border-radius:3px;padding:5px 12px;cursor:pointer}button:hover{background:#eef3fa}:focus-visible{outline:3px solid #d38c25;outline-offset:3px}summary{cursor:pointer;color:var(--blue);padding:12px;background:#f4f7fb}footer{font-size:13px;color:#596879;margin-top:30px}
    @media(max-width:850px){.layout{display:block;padding:22px 18px}aside{position:static;max-height:none;margin-bottom:28px}nav{grid-template-columns:repeat(2,minmax(0,1fr))}h1{font-size:28px}h2{font-size:23px}}
    @media print{.layout{display:block;padding:0}aside,.diagram-tools,.actions{display:none}body{font-size:10pt}h1{font-size:23pt}h2{font-size:17pt}section{break-before:auto}figure{break-inside:avoid}.diagram-viewport{overflow:visible;padding:0}.diagram-viewport svg{width:100%!important}table{min-width:0;font-size:8pt}th,td{padding:6px}a{color:inherit}pre{font-size:8pt}}
    '''
    js = '''
    document.querySelectorAll('button[data-size]').forEach(button=>button.addEventListener('click',()=>{const svg=button.closest('figure').querySelector('svg');svg.style.width=button.dataset.size==='actual'?svg.getAttribute('width')+'px':'100%';}));
    document.getElementById('print').addEventListener('click',()=>window.print());
    '''
    result = f'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Delivery AI 居民系统 — 架构与接口参考</title><style>{css}</style></head><body><div class="layout"><aside><strong>Delivery · 技术文档</strong><div class="meta">UE 5.8 · 2026-11-13</div><nav>{''.join(navigation)}</nav></aside><main><header><div class="meta">AI / 居民系统 / 架构与接口</div><h1>AI 居民系统</h1><p>从玩家交谈开始，介绍 NPC 如何感知、请求模型、执行动作并同步给其他玩家。</p><div class="notice">大壮、翠花采用村口街坊设定。翠花已正式接入用户认可的 B 音色，大壮的声音保持原样。NPC 主动画改为可编辑的动画蓝图，当前检查与限制见正文和迭代记录。首句声音仍有明显等待；本轮修复有效首句丢失，并补齐听见玩家和受击的 B 音色短音。游戏中的动作、声音与多人效果尚待试玩。有限追打与轻拳受击已接入并通过专项检查，双方互动仍待试玩；跟班二狗继续按计划实现。</div><div class="actions"><a href="#diagram-overview">系统类图</a><a href="#diagram-conversation-sequence">交谈时序</a><a href="#chapter-10">问题记录</a><a href="Architecture.md">主文档</a><a href="AIIteration.html">研究与迭代记录</a><button id="print" type="button">打印 / 保存 PDF</button></div></header>{''.join(articles)}<footer>正文与图形内嵌，可离线阅读。生成入口：Tools/Documentation/build_ai_docs.py。具体修改与测试结果见研究与迭代记录。</footer></main></div><script>{js}</script></body></html>'''
    path = ROOT / 'Document/DeliveryAIPlan.html'
    path.write_text(result, encoding='utf-8')
    print(f'Generated {path}: {len(graphs)} UML diagrams, {(len(sections)-1)//2} reference sections')


if __name__ == '__main__':
    build()
