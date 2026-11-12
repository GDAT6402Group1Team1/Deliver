"""离线核对文档结构、SVG 文本边界和响应式布局，不连接游戏或模型"""
from pathlib import Path
import json
import xml.etree.ElementTree as ET
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Saved/Documentation/AI'


def check():
    OUT.mkdir(parents=True, exist_ok=True)
    files = list((ROOT / 'Document/Diagrams/AI').glob('*.svg'))
    assert len(files) == 9
    for file in files: ET.parse(file)
    errors, result = [], {'diagrams':len(files), 'viewports':[]}
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path='C:/Program Files/Google/Chrome/Application/chrome.exe', headless=True)
        page = browser.new_page(viewport={'width':1440,'height':1000}, device_scale_factor=1)
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto((ROOT / 'Document/DeliveryAIPlan.html').as_uri())
        assert page.locator('figure.uml').count() == 9
        assert not page.locator('script[src]').count()
        assert not page.locator('img').count(), '图形应内嵌'
        assert page.locator('#chapter-10').inner_text().startswith('问题记录')
        ids = page.locator('[id]').evaluate_all('(els)=>els.map(e=>e.id)')
        assert len(ids)==len(set(ids)), '重复 ID'
        missing = page.locator('a[href^="#"]').evaluate_all('(els)=>els.filter(e=>!document.getElementById(e.hash.slice(1))).map(e=>e.hash)')
        assert not missing, missing
        clipping = page.evaluate('''() => [...document.querySelectorAll('g[data-node]')].flatMap(g=>{let r=g.querySelector('rect').getBBox();return [...g.querySelectorAll('text')].filter(t=>{let b=t.getBBox();return b.x<r.x||b.x+b.width>r.x+r.width-3||b.y+b.height>r.y+r.height;}).map(t=>t.textContent);})''')
        assert not clipping, f'类框文本越界: {clipping}'
        canvas_clipping = page.evaluate('''() => [...document.querySelectorAll('svg')].flatMap(s=>[...s.querySelectorAll('text')].filter(t=>{let b=t.getBBox();return b.x<0||b.x+b.width>s.viewBox.baseVal.width||b.y+b.height>s.viewBox.baseVal.height;}).map(t=>t.textContent))''')
        assert not canvas_clipping, canvas_clipping
        overlaps = page.evaluate('''() => [...document.querySelectorAll('svg')].flatMap(s=>{let ts=[...s.querySelectorAll('text')],out=[];for(let i=0;i<ts.length;i++)for(let j=i+1;j<ts.length;j++){let a=ts[i].getBBox(),b=ts[j].getBBox();if(Math.min(a.x+a.width,b.x+b.width)-Math.max(a.x,b.x)>1&&Math.min(a.y+a.height,b.y+b.height)-Math.max(a.y,b.y)>1)out.push([ts[i].textContent,ts[j].textContent]);}return out;})''')
        assert not overlaps, f'SVG 文本重叠: {overlaps}'
        for width in [1440,390,320]:
            page.set_viewport_size({'width':width,'height':1000})
            page.evaluate('window.scrollTo(0,0)')
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth'), f'{width}px 页面溢出'
            page.screenshot(path=str(OUT / f'page-{width}.png'))
            result['viewports'].append({'width':width,'overflow':False})
        page.set_viewport_size({'width':1440,'height':1000})
        for name in ['overview','conversation-sequence','configuration','combat','stream']:
            figure=page.locator(f'#diagram-{name}')
            figure.locator('button[data-size="actual"]').click()
            assert figure.locator('svg').evaluate('(e)=>e.style.width.endsWith("px")')
            figure.locator('button[data-size="fit"]').click()
            figure.screenshot(path=str(OUT/f'{name}.png'))
        page.emulate_media(media='print')
        assert page.locator('aside').is_hidden()
        page.locator('#diagram-overview').screenshot(path=str(OUT/'print-overview.png'))
        assert not errors, errors
        browser.close()
    result.update(jsErrors=errors, anchors='passed', svgTextBounds='passed', zoom='passed', printLayout='passed')
    (OUT/'check-results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False))


if __name__ == '__main__':
    check()
