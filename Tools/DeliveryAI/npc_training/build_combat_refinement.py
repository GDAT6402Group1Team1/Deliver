"""第二轮补充独立故事，改变事件组合与说话人，不读取最终验收的错误详情"""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import prepare_combat_context
from build_combat_data import ROOT, stable_id, case_context, write_json

# 当前行动、受击/挥拳/命中、实际听见的发言与交手事件
STORIES = [
 ('Fight',3,3,1,[('other','Speech','刚才我太冲了，是我弄错了，你那钱没少给，对不起。')]),
 ('Fight',3,4,2,[('self','SelfSpeech','我这几下可没白挨。'),('other','Speech','是我不好，我赔你那袋菜，咱们到此为止。')]),
 ('Fight',3,2,1,[('other','Speech','咱们都别动手了。'),('self','SelfSpeech','好，我收手。'),('bystander','OverheardSpeech','别停啊，我还没看够呢！')]),
 ('Fight',3,8,6,[('other','Speech','你也挨了好几下，得意什么！'),('self','SelfSpeech','你挨的可比我多，嘴还挺硬！'),('other','FightThreat','')]),
 ('Fight',3,7,5,[('other','Speech','你反应还真不慢。'),('self','SelfSpeech','当然，我这几下可都碰到你了！'),('other','FightThreat','')]),
 ('Fight',3,5,4,[('self','SelfSpeech','说好咱们俩比划，喊停就停。'),('other','Speech','停了停了，我输了，别再来啦。')]),
 ('Fight',2,3,1,[('other','Speech','刚才那一下是我不对，我先道歉。')]),
 ('Fight',1,2,1,[('other','Speech','你倒是看看谁的鞋踩在谁脚上了！'),('other','LightMeleeHit','')]),
 ('Flee',3,1,0,[('other','Speech','好了，我不追了。'),('other','LightMeleeHit','')]),
 ('Fight',0,1,0,[('self','SelfSpeech','今天非得分个高低！'),('other','Speech','我把票拿出来了，你看，是我拿错了你的票，对不起。')]),
 ('Fight',1,1,0,[('self','SelfSpeech','算了算了，不跟你计较。'),('other','LightMeleeHit',''),('other','Speech','刚才那拳才叫计较，你看清楚没！')]),
 ('Flee',0,0,0,[('self','SelfSpeech','我就嘴上厉害，你别真来，我怕疼！'),('other','FightThreat','')]),
 ('Fight',0,0,0,[('self','SelfSpeech','我还得回去做饭，衣服也不能弄坏。'),('other','Speech','你别光找借口，有本事接招！'),('other','FightThreat','')]),
 ('Flee',1,1,0,[('other','Speech','不追了，我还有正事呢，刚才的事一笔勾销。'),('other','LostSight','')]),
 ('Fight',3,3,2,[('other','Speech','你扔的根本不是我的萝卜，是你自己的。'),('self','SelfSpeech','哎？还真是，我把袋子弄混了。')]),
 ('Fight',2,4,2,[('other','Speech','我说谁输了谁认错，现在我认错，你别再争了。')]),
 ('Fight',1,3,2,[('other','Speech','你以为我说的停手是真的？我刚才是逗你的！'),('other','FightThreat','')]),
 ('Flee',3,2,1,[('self','SelfSpeech','你别以为我怕你！'),('other','LightMeleeHit',''),('other','Speech','那你倒是别往后缩啊！')]),
 ('Fight',3,4,3,[('other','Speech','这几拳我挨得服气，你也吃亏了，咱俩就扯平吧。')]),
 ('Fight',0,0,0,[('bystander','OverheardSpeech','他想打你，他刚才跟我说的！'),('other','Speech','我什么时候说了？别听他瞎说，我刚才已经赔过不是了。')]),
 ('Fight',1,2,1,[('bystander','OverheardSpeech','我看你们已经和好了。'),('other','LightMeleeHit',''),('other','Speech','我可还没答应呢！')]),
 ('Flee',3,2,0,[('bystander','OverheardSpeech','他刚才说不追你了。'),('other','FightThreat','')]),
 ('Fight',0,1,0,[('other','Speech','你就是个小气鬼。'),('other','Speech','等下，我说的是那只守着饭盆的狗，不是你啊，别误会！')]),
 ('Fight',0,2,0,[('other','Speech','我可没拿你的水壶，你看我这只颜色都不同。'),('self','SelfSpeech','是我看岔了，刚才白生气了。')]),
 ('Flee',3,1,0,[('other','Speech','我刚才也没听清你那句话，是我动手早了，对不住。')]),
 ('Fight',3,6,4,[('other','Speech','咱们谁也没讨着便宜，还接着来吗？'),('self','SelfSpeech','你都这么问了，我总不能说怕你吧！'),('other','FightThreat','')]),
 ('Fight',1,1,0,[('other','Speech','别站着讲道理了，你刚才不是说你练过吗！'),('other','FightThreat','')]),
 ('Flee',0,1,0,[('other','Speech','你刚才躲得挺快。'),('other','Speech','哎，我先接个电话，今天不闹了。')]),
 ('Fight',3,2,0,[('other','Speech','说这么多有什么用，你不是还打不过我！'),('other','FightThreat','')]),
 ('Fight',3,4,1,[('other','Speech','我承认我挪了你的椅子，我给你放回去，别再打了。')]),
 ('Fight',0,1,0,[('self','SelfSpeech','我没说你骗人，你别扣帽子。'),('other','Speech','你就是这个意思！'),('other','FightThreat','')]),
 ('Flee',2,2,0,[('self','SelfSpeech','我认错了，我刚才说的全是气话。'),('other','Speech','你总算认错了，今天到此为止。')]),
]


def main():
    previous = ROOT / 'Saved/AIResearch/NpcTraining/combat-v4'
    output = ROOT / 'Saved/AIResearch/NpcTraining/combat-v5'
    output.mkdir(parents=True, exist_ok=False)
    old = json.loads((previous / 'situations.json').read_text(encoding='utf-8'))
    profiles = {r['context']['self_name']: r['context']['persona'] for r in old if r['source']['kind']=='authored_ue_event_scenario'}
    rows = deepcopy(old)
    for number, (action, received, punches, hits, story) in enumerate(STORIES, 1):
        identifier = 'refinement-' + str(number).zfill(3)
        context = case_context(identifier, action, number)
        context['persona'] = profiles[context['self_name']]
        own, target = context['self_subject_id'], context['current_action']['target_id']
        bystander = stable_id(identifier + '/bystander')
        actors = {'self': own, 'other': target, 'bystander': bystander}
        if any(actor=='bystander' for actor, _, _ in story):
            context['subjects'].append({'subject_id': bystander, 'name': '邻居老周', 'kind': 'npc'})
        events = []
        def add(kind, actor, text, source):
            events.append({'event_id': stable_id(identifier+'/event/'+str(len(events))),
                           'kind': kind, 'subject_id': actors[actor], 'description': text, 'source': source,
                           'addressee_id': target if actor=='self' and kind=='SelfSpeech' else own if kind=='Speech' else '',
                           'reply_to_event_id': ''})
        add('Perceived','other',f"看见附近的{context['subjects'][1]['name']}，当时距离约 200 厘米",'Seen')
        for actor, kind, text in story:
            if kind=='FightThreat': text='这个人朝我冲过来，准备动手'
            elif kind=='LightMeleeHit': text='这个人打了我一拳，我后仰了一下，仍然站着'
            elif kind=='LostSight': text='暂时看不见'+context['subjects'][1]['name']
            add(kind, actor, text, 'ActionResult' if kind=='SelfSpeech' else 'Heard' if 'Speech' in kind else 'Seen')
        current = '本轮' if action=='Fight' else '上一轮'
        body = '刚挨打，正在后仰' if story[-1][1]=='LightMeleeHit' else '站着，可以行动'
        add('SelfCombat','self',f'这阵子实际挨了 {received} 记轻拳；{current}交战挥拳 {punches} 次，实际命中 {hits} 次。现在{body}。','ActionResult')
        context['evidence'] = events
        for candidate in context['candidates']:
            candidate['evidence_ids'] = [] if candidate['action']=='Wait' else [events[0]['event_id']]
        prompt, _ = prepare_combat_context(context)
        rows.append({'id':identifier,'group_id':identifier,'usage':'train','label':None,
                     'source':{'kind':'authored_second_round_ue_events','gameplay_recording':False,
                               'basis':'first-round training shortcut audit and internal validation only'},
                     'context':context,'prompt':prompt})
    write_json(output/'situations.json',rows)
    teacher = json.loads((previous/'teacher-corrected.json').read_text(encoding='utf-8'))
    teacher['reused_teacher_source'] = str(previous/'teacher-corrected.json')
    teacher['reused_teacher_sha256'] = hashlib.sha256((previous/'teacher-corrected.json').read_bytes()).hexdigest()
    teacher['situations_sha256'] = hashlib.sha256((output/'situations.json').read_bytes()).hexdigest()
    teacher['previous_calls'] = teacher.pop('calls')
    teacher['calls'] = []
    write_json(output/'teacher.json',teacher)
    write_json(output/'manifest.json', {'round':2,'new_independent_training_stories':32,
        'total_training_rows':sum(r['usage']=='train' for r in rows),
        'total_training_groups':len({r['group_id'] for r in rows if r['usage']=='train'}),
        'validation_groups':len({r['group_id'] for r in rows if r['usage']=='validation'}),
        'acceptance_used_for_refinement':False,'acceptance_second_evaluation_planned':True,
        'previous_data_sha256':hashlib.sha256((previous/'situations.json').read_bytes()).hexdigest()})
    print(json.dumps({'output':str(output),'new_cases':32,'training_rows':160,'validation_rows':15}))


if __name__=='__main__':
    main()
