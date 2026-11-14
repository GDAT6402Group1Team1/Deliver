"""独立编写持续交战验收，只使用当前游戏确实能上报的事件"""
import hashlib
import json
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from combat_model import prepare_combat_context

ROOT = Path(__file__).resolve().parents[3]

# 已发生的原话与战况进入模型，末尾可接受动作只供验收脚本读取
# 每个编号是一段独立经过，不用换名或重排候选增加数量
# 编号 / 人物 / 当前动作 / 挨打 / 挥拳 / 命中 / 按时间排列的事件 / 可接受动作
CASES = [
    ('01', '大壮', 'Fight', 0, 1, 0, [('other', 'Speech', '我才没拿你的凳子，你冲我嚷什么！'), ('self', 'SelfSpeech', '凳子上写着我名字呢！'), ('other', 'FightThreat', '')], ['Fight']),
    ('02', '翠花', 'Fight', 3, 2, 0, [('other', 'Speech', '不是说你一只手就够了吗？来呀！'), ('other', 'LightMeleeHit', '')], ['Flee']),
    ('03', '大壮', 'Fight', 0, 2, 1, [('other', 'Speech', '我真认错人了，刚才以为你抢我的伞。对不起啊。'), ('self', 'SelfSpeech', '拿好你的伞，别再弄错了。')], ['Wait']),
    ('04', '翠花', 'Flee', 3, 1, 0, [('other', 'Speech', '站住，你刚才不是挺能耐的吗？'), ('other', 'FightThreat', '')], ['Flee']),
    ('05', '大壮', 'Flee', 1, 1, 0, [('other', 'Speech', '不追你了，锅里还煮着面呢。'), ('other', 'LostSight', '')], ['Wait', 'Flee']),
    ('06', '翠花', 'Flee', 0, 1, 1, [('self', 'SelfSpeech', '旁边有人，你先让他们过去。'), ('other', 'Speech', '人都走了，你不是怕了吧？'), ('other', 'FightThreat', '')], ['Fight']),
    ('07', '大壮', 'Fight', 0, 1, 1, [('other', 'Speech', '花盆是我碰掉的，你别冲别人来了。'), ('self', 'SelfSpeech', '原来是你啊，你早说不就完了！'), ('other', 'Speech', '我给你赔一个新的，这事到这儿行吗？')], ['Wait']),
    ('08', '翠花', 'Fight', 0, 1, 0, [('self', 'SelfSpeech', '你刚才不是说我跑得慢吗？'), ('other', 'Speech', '哈哈，你再来，我让你一只手！'), ('other', 'FightThreat', '')], ['Fight']),
    ('09', '大壮', 'Fight', 3, 3, 0, [('self', 'SelfSpeech', '没事，我好着呢！'), ('other', 'LightMeleeHit', ''), ('other', 'FightThreat', '')], ['Flee']),
    ('10', '翠花', 'Flee', 2, 2, 0, [('other', 'Speech', '别跑了，我不打了。刚才那话是我不好。'), ('self', 'SelfSpeech', '你就在那儿站着，别往这儿来了。')], ['Wait', 'Flee']),
    ('11', '大壮', 'Fight', 0, 2, 0, [('other', 'Speech', '你这人就是爱占便宜！'), ('self', 'SelfSpeech', '谁占你便宜了？'), ('other', 'Speech', '等一下，收据找到了，是我记错了。对不住啊。')], ['Wait']),
    ('12', '翠花', 'Fight', 1, 1, 0, [('other', 'Speech', '行了行了，不打了。'), ('other', 'LightMeleeHit', ''), ('self', 'SelfSpeech', '你说不打还偷着来是吧！')], ['Fight']),
    ('13', '大壮', 'Flee', 3, 2, 0, [('other', 'Speech', '我又不吃人，你躲什么？'), ('other', 'FightThreat', '')], ['Flee']),
    ('14', '翠花', 'Fight', 0, 1, 1, [('self', 'SelfSpeech', '这事说开就行了。'), ('other', 'Speech', '对，咱们各退一步。'), ('bystander', 'OverheardSpeech', '怎么停了？刚才不是打得挺热闹的吗？')], ['Wait']),
    ('15', '大壮', 'Fight', 0, 2, 1, [('other', 'Speech', '你一拳都没碰着我！'), ('self', 'SelfSpeech', '你胳膊上的灰哪来的？'), ('other', 'FightThreat', '')], ['Fight']),
    ('16', '翠花', 'Fight', 0, 1, 0, [('other', 'Speech', '我喊的是后面那个小伙子，没说你啊！'), ('bystander', 'OverheardSpeech', '是叫我呢，我们约好搬东西的。'), ('self', 'SelfSpeech', '哦，那是我听岔了。')], ['Wait']),
    ('17', '大壮', 'Flee', 2, 3, 0, [('self', 'SelfSpeech', '你别跟着我了！'), ('other', 'Speech', '刚才说我没本事，现在跑什么？'), ('other', 'FightThreat', '')], ['Flee']),
    ('18', '翠花', 'Flee', 1, 1, 0, [('other', 'Speech', '有人找我，我走了。你也回去吧。'), ('other', 'LostSight', '')], ['Wait', 'Flee']),
    ('19', '大壮', 'Fight', 1, 2, 1, [('self', 'SelfSpeech', '你说我是骗子，我哪儿骗你了？'), ('other', 'Speech', '那句话是说广告里的，不是说你。刚才没讲明白，对不起。')], ['Wait']),
    ('20', '翠花', 'Fight', 3, 1, 0, [('other', 'LightMeleeHit', ''), ('other', 'Speech', '刚才不是还要跟我比吗？'), ('other', 'FightThreat', '')], ['Flee']),
    ('21', '大壮', 'Fight', 0, 1, 1, [('self', 'SelfSpeech', '这下总知道我的厉害了吧？'), ('other', 'Speech', '知道了知道了，今天就比到这儿。')], ['Wait']),
    ('22', '翠花', 'Fight', 1, 2, 1, [('other', 'LightMeleeHit', ''), ('self', 'SelfSpeech', '哎哟，你还真使劲啊！'), ('other', 'FightThreat', '')], ['Fight', 'Flee']),
    ('23', '大壮', 'Flee', 1, 1, 0, [('other', 'LostSight', '')], ['Wait', 'Flee']),
    ('24', '翠花', 'Fight', 2, 2, 0, [('self', 'SelfSpeech', '不打了，怪我先冲你喊。'), ('other', 'Speech', '你早这么说不就行了吗？')], ['Wait', 'Flee']),
    ('25', '大壮', 'Flee', 0, 1, 1, [('self', 'SelfSpeech', '这儿全是菜，我可不想赔钱。'), ('other', 'Speech', '到空地了，你还往哪儿躲？'), ('other', 'FightThreat', '')], ['Fight']),
    ('26', '翠花', 'Fight', 0, 1, 0, [('other', 'Speech', '先停一下，车要从这儿过。'), ('self', 'SelfSpeech', '行，先给人家让开。')], ['Wait']),
    ('27', '大壮', 'Fight', 0, 1, 0, [('self', 'SelfSpeech', '车过去了，这回你把话说清楚。'), ('other', 'Speech', '还要我说几遍？你就是吹牛！'), ('other', 'FightThreat', '')], ['Fight']),
    ('28', '翠花', 'Fight', 3, 3, 0, [('self', 'SelfSpeech', '我手都打麻了！'), ('other', 'LightMeleeHit', ''), ('other', 'FightThreat', '')], ['Flee']),
    ('29', '大壮', 'Flee', 2, 1, 0, [('self', 'SelfSpeech', '别过来了，疼死我了。'), ('other', 'Speech', '行，我不过来。刚才下手重了，对不起啊。')], ['Wait', 'Flee']),
    ('30', '翠花', 'Fight', 0, 2, 1, [('other', 'Speech', '你厉害，我认输了，大家都看着呢。'), ('self', 'SelfSpeech', '这还差不多。')], ['Wait']),
    ('31', '大壮', 'Fight', 0, 2, 1, [('other', 'Speech', '收手这么慢，还说你年轻时练过？'), ('other', 'FightThreat', '')], ['Fight']),
    ('32', '翠花', 'Fight', 0, 1, 0, [('other', 'Speech', '隔着棵树就找不到我了？你这眼神！'), ('other', 'FightThreat', '')], ['Fight']),
    ('33', '大壮', 'Flee', 3, 3, 0, [('self', 'SelfSpeech', '我绕了两圈你还追啊？'), ('other', 'FightThreat', '')], ['Flee']),
    ('34', '翠花', 'Fight', 0, 0, 0, [('other', 'Speech', '别动手！东西找到了，在我自己袋子里，真是冤枉你了。')], ['Wait']),
    ('35', '大壮', 'Fight', 3, 2, 0, [('self', 'SelfSpeech', '不打了！我认输还不行吗？'), ('other', 'LightMeleeHit', ''), ('other', 'FightThreat', '')], ['Flee']),
    ('36', '翠花', 'Flee', 0, 0, 0, [('other', 'Speech', '我刚才伸手接你的包呢，没要打你。'), ('self', 'SelfSpeech', '吓我一跳，我还以为你要动手。')], ['Wait']),
    ('37', '大壮', 'Fight', 1, 2, 1, [('other', 'Speech', '我就开个玩笑！'), ('other', 'LightMeleeHit', ''), ('self', 'SelfSpeech', '有你这么开玩笑的吗！')], ['Fight']),
    ('38', '翠花', 'Fight', 0, 3, 0, [('self', 'SelfSpeech', '懒得跟你计较，我还要去买菜。'), ('other', 'Speech', '我也要回去了，改天再说。')], ['Wait']),
    ('39', '大壮', 'Flee', 3, 2, 0, [('other', 'LostSight', ''), ('other', 'FightThreat', '')], ['Flee']),
    ('40', '翠花', 'Fight', 0, 1, 1, [('bystander', 'OverheardSpeech', '闹够了吧，你们一会儿又该喊疼了。'), ('other', 'Speech', '到这儿吧，别把人家门口堵着。'), ('self', 'SelfSpeech', '行，不跟你闹了。')], ['Wait']),
]

FACTS = {'FightThreat': '这个人朝我冲过来，准备动手',
         'LightMeleeHit': '这个人打了我一拳，我后仰了一下，仍然站着',
         'LostSight': '暂时看不见对方，可能只是转头或被遮挡，不能据此判断对方已经走远；记录的位置是最后一次看见的位置'}


def identifier(text):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, 'delivery-independent-combat-acceptance/' + text))


def build():
    personas = {}
    for line in (ROOT / 'Saved/Logs/NpcDialogue.ndjson').read_text(encoding='utf-8-sig').splitlines():
        row = json.loads(line)
        name = row.get('model_context', {}).get('self_name')
        if name in ('大壮', '翠花') and row.get('full_persona'):
            personas[name] = row['full_persona']
    if len(personas) != 2:
        raise ValueError('缺少两位人物的正式性格，未生成验收数据')
    rows = []
    for key, name, action, received, swings, hits, facts, acceptable in CASES:
        case = 'independent-' + key
        own, other, bystander = [identifier(case + '/' + role) for role in ('self', 'other', 'bystander')]
        other_name = '翠花' if name == '大壮' else '大壮'
        ids = {'self': own, 'other': other, 'bystander': bystander}
        events = []
        for i, (who, kind, text) in enumerate(facts):
            events.append(dict(event_id=identifier(case + '/event-' + str(i)), subject_id=ids[who],
                               addressee_id=other if who == 'self' else own, kind=kind,
                               source='ActionResult' if kind == 'SelfSpeech' else 'Heard' if 'Speech' in kind else 'Seen',
                               description=text or FACTS[kind], reply_to_event_id=''))
        target_evidence = next(e['event_id'] for e in events if e['subject_id'] == other)
        fight_round = '本轮' if action == 'Fight' else '上一轮'
        body = (f'这阵子实际挨了 {received} 记轻拳；{fight_round}交战挥拳 {swings} 次，实际命中 {hits} 次。现在站着，可以行动。'
                f'；我正在对{other_name}执行{action}，剩余约3.0秒。追近、出拳和受击仍在继续；可决定继续、逃跑或停手')
        events.append(dict(event_id=identifier(case + '/body'), subject_id=own, addressee_id='',
                           kind='SelfCombat', source='ActionResult', description=body, reply_to_event_id=''))
        context = dict(protocol_version=1, capability_version=1, world_epoch=identifier('world'),
                       npc_id=own, request_id=identifier(case + '/request'), context_revision=1, plan_revision=1,
                       request_source='ambient', pending_player_message=False, persona=personas[name],
                       self_name=name, self_subject_id=own, profile_id='', evidence=events,
                       subjects=[dict(subject_id=own, name=name, kind='npc'),
                                 dict(subject_id=other, name=other_name, kind='npc'),
                                 dict(subject_id=bystander, name='快递员', kind='player')],
                       allowed_actions=['Wait', 'Fight', 'Flee'],
                       candidates=[dict(action=a, target_id='' if a == 'Wait' else other,
                                        duration={'Wait': 2., 'Fight': 8., 'Flee': 5.}[a],
                                        evidence_ids=[] if a == 'Wait' else [target_evidence])
                                   for a in ('Wait', 'Flee', 'Fight')],
                       current_action=dict(action=action, target_id=other, source='Model',
                                           action_id=identifier(case + '/action'), remaining_seconds=3.))
        prompt, _ = prepare_combat_context(context)
        rows.append(dict(id=case, group_id=case, usage='acceptance_only', context=context, prompt=prompt,
                         source=dict(kind='independent_assistant_authored', gameplay_recording=False,
                                     observable_event_format='current UE DescribeCombatState/ObserveFact/HearSpeech',
                                     authored_by='root', labels_not_in_prompt=True),
                         label=dict(acceptable_actions=acceptable, critical_actions=[])))
    return rows


if __name__ == '__main__':
    output = ROOT / 'Saved/AIResearch/NpcTraining/acceptance-reviewed.json'
    payload = json.dumps(build(), ensure_ascii=False, indent=2).encode('utf-8')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(payload)
    print(json.dumps(dict(path=str(output), cases=len(CASES), sha256=hashlib.sha256(payload).hexdigest())))
