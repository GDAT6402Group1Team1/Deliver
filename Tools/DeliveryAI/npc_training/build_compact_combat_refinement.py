"""给 0.6B 的第二轮训练补充战况相似、结局不同的街坊争执"""
import argparse
import json
from pathlib import Path

from build_combat_round3 import make_case, running_suffix
from combat_model import COMBAT_INSTRUCTIONS, prepare_combat_context
from combat_round3_stories import hit, me, other, rush, story
from finalize_combat_round3 import ROOT, read_lines, sha, write_json


def extra_stories():
    # 两种结局共享前半段，模型要分辨最新的话和动作有没有改变处境
    return [
        story(101, '搬凳子踩脚', '翠花', 'Fight', (1, 2, 0), [me('你踩我脚还往这边走？')],
              ('Wait', [other('凳子太沉，我低头没看见。搁这儿了，我给你看看脚。')]),
              ('Flee', [other('你就这点力气？我还没用劲呢。'), hit(), rush()])),
        story(102, '院门让路', '大壮', 'Fight', (0, 1, 0), [me('你堵着门还说我不让人！')],
              ('Fight', [other('就不让你过去。有本事你来，我就在这儿等着！'), rush()]),
              ('Wait', [other('我鞋卡门坎里了。行了，拔出来了，你先过。'), me('行，我以为你故意的。')])),
        story(103, '摔掉伞柄', '翠花', 'Fight', (1, 3, 0), [other('你追这么半天，也没碰着我。')],
              ('Flee', [other('还追？那换我了！'), hit(), rush()]),
              ('Wait', [other('等会儿，我手里的伞断了。咱俩别打了，我把这尖头捡走。'), me('行，别让孩子踩着。')])),
        story(104, '晾衣绳', '大壮', 'Flee', (2, 1, 0), [me('先不闹了，我衣裳挂住了。')],
              ('Flee', [other('你想停我还没答应呢！'), rush()]),
              ('Wait', [other('我把绳子给你松开。我也不追了，衣服都快扯坏了。')])),
        story(105, '玻璃瓶回收', '翠花', 'Fight', (0, 2, 1), [me('刚才拽我胳膊那一下怎么算？')],
              ('Fight', [other('算我还想再拽一下！你别往后躲。'), rush()]),
              ('Wait', [other('我拽你是怕你倒进那堆瓶子里。你回头看，玻璃都碎了。'), me('看见了，先收起来吧。')])),
        story(106, '花盆搬到屋檐下', '大壮', 'Fight', (1, 2, 0), [me('你刚才说我不敢来，我人现在就在这儿。')],
              ('Wait', [other('我说的是不敢搬这个盆，里面有只马蜂。先把手放下来。'), me('我听岔了，你也先别碰。')]),
              ('Fight', [other('说的就是你。还要我再说一遍？'), rush()])),
        story(107, '风筝线拉断', '翠花', 'Flee', (1, 0, 0), [me('你要找那根线，别跟着我啊。')],
              ('Wait', [other('找到了，挂那边树上了。刚才算我急了。'), me('行，有话就好好说嘛。')]),
              ('Flee', [other('线我回头再找，先让你长长记性。'), hit(), rush()])),
        story(108, '茶摊挪桌子', '大壮', 'Fight', (2, 2, 0), [me('你打我两下了，别再跟过来啊。')],
              ('Flee', [other('你倒是走快点啊！'), rush()]),
              ('Wait', [other('我往回走了。这事我有错，改天再跟你说。'), me('那今天就到这儿。')])),
    ]


def more_flee_stories():
    # 开发集里，小模型经常沿用逃跑。让两种结局共用早期处境，只改最后发生的事
    opponent_leaves = ('other', 'FleeStarted', '这个人正在远离我跑开')
    return [
        story(111, '晒谷场的滑泥', '大壮', 'Flee', (0, 0, 0), [me('这儿滑，先走到砖地上。')],
              ('Fight', [other('站稳了正好，有种别退！'), me('腿脚好着呢，过来试试。'), rush()]),
              ('Flee', [other('谁还让你挑地方？'), hit(), me('这腿一下子发软，先拉开。'), rush()])),
        story(112, '村口分两条路', '翠花', 'Flee', (0, 1, 0), [other('你去哪儿我都跟着！'), hit(), rush()],
              ('Flee', [me('这一拳打得我眼前发白。'), other('站都站不稳了还回头？')]),
              ('Fight', [other('这回算你闪得快。'), me('刚才是给车让路，这会儿我已经站稳了。')])),
        story(113, '晒席腾空', '翠花', 'Flee', (0, 2, 0), [me('别踩大伙的粮食，有话外头说。'), rush()],
              ('Wait', [other('我拿笤帚是收粮，刚才冲你吼错了。'), me('行，我沿边走，你忙你的。'), opponent_leaves]),
              ('Fight', [other('现在地上清了，我就冲你喊的。'), me('好，话说明白了，别往我这儿扑。'), rush()])),
        story(114, '折叠椅晃了一下', '大壮', 'Flee', (0, 0, 0), [me('我去旁边站，别把折叠椅碰翻。')],
              ('Fight', [other('我追的是你！'), me('都站稳了，我倒要看看你追来干什么。'), rush()]),
              ('Wait', [other('我过来扶椅子的，往后退点，螺丝掉地上了。'), me('行，旁边有个盒子可以装。')])),
        story(115, '箩筐边的小孩', '大壮', 'Flee', (1, 1, 1), [me('小孩在这儿，咱们先别挤这条道。')],
              ('Flee', [other('你还想挡我的路？'), rush(), me('先别碰我，这边肩膀到现在都抬不起来。')]),
              ('Fight', [other('人都走开了，你怎么还缩着？'), me('刚才是忙着护孩子，我现在站稳了。'), rush()])),
        story(116, '找丢了的库房钥匙', '翠花', 'Flee', (0, 0, 0), [me('你举着手冲过来干吗？')],
              ('Wait', [other('给你送钥匙，刚从库房门上拔下来。'), me('我看见了。刚才我还当你冲我抡手呢。')]),
              ('Fight', [other('钥匙在墙上挂着呢，我这趟就是找你算账。'), me('我刚才手里腾不开，现在你再来。'), rush()])),
        story(117, '高凳上的红灯笼', '翠花', 'Flee', (1, 2, 0), [me('先停一下，别撞倒人家的高凳。'), hit()],
              ('Flee', [other('你今天跑不了！'), rush(), me('手都麻得直抖了，你还追！')]),
              ('Wait', [other('我停了，灯笼挂稳了。刚才打你那下算我急了。'), me('好，先把凳子收起来。')])),
        story(118, '被踢走的水桶', '大壮', 'Flee', (0, 0, 0), [me('我先把水桶搁路边，别害人绊倒。')],
              ('Fight', [other('桶归桶，我骂的就是你。'), me('我手空了，咱俩当面说。'), rush()]),
              ('Wait', [other('桶是我脚勾的，刚才没看见路上有人。'), me('行，挪边上就完了。')])),
        story(119, '戏箱搬过窄桥', '翠花', 'Flee', (0, 1, 1), [me('桥窄，等木箱过去再说。'), hit(), rush()],
              ('Fight', [other('我到桥这头了，你现在还躲吗？'), me('木箱过去了，刚才那拳也没把我怎么样。')]),
              ('Flee', [me('腰还疼得直不起来。'), other('旁边那条宽路我也能跟过去。')])),
        story(120, '新刷的白线', '大壮', 'Flee', (1, 1, 0), [me('先别踩这条线，油漆还没干。')],
              ('Wait', [other('我刚才是怕你踩上去，伸手挡得急了。你绕过去吧。'), me('行，我顺着边走。')]),
              ('Flee', [other('线不线的，刚才打轻了。'), rush(), me('我这手腕到现在还疼，你别再碰。')])),
        story(121, '桌边滚下的青砖', '大壮', 'Flee', (0, 2, 1), [me('先让我把脚拔出来，这砖卡鞋了。'), hit()],
              ('Fight', [other('出来了吧？还躲个什么！'), me('鞋好着呢，我不退了。'), rush()]),
              ('Wait', [other('砖挪开了。我刚才那下太急了，鞋没坏吧？'), me('鞋没事，这事就搁这儿吧。'), opponent_leaves])),
        story(122, '菜板掉在门口', '翠花', 'Flee', (0, 0, 0), [me('菜板掉这儿了，脚抬高点。')],
              ('Wait', [other('我捡菜板，没工夫跟你斗。刚才嘴快，算我说错了。'), me('行，你拿走吧。')]),
              ('Flee', [other('你自己小心，我可还没消气！'), rush(), hit()])),
    ]


def set_time_left(case, remaining_seconds):
    context = case['context']
    combat_event = context['evidence'][-1]
    previous_suffix = running_suffix(context)
    if combat_event['kind'] != 'SelfCombat' or not combat_event['description'].endswith(previous_suffix):
        raise ValueError('无法更新训练情境中的交战时间')
    context['current_action']['remaining_seconds'] = remaining_seconds
    combat_event['description'] = combat_event['description'].removesuffix(previous_suffix) + running_suffix(context)
    case['prompt'], _ = prepare_combat_context(context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--second-refinement', action='store_true', help='在原补训数据上增加新的逃跑情境')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = ROOT / 'Saved/AIResearch/NpcTraining'
    previous = root / 'combat-v7'
    compact = root / 'combat-compact-20261115'
    output = (args.output or compact / ('second-refinement-data' if args.second_refinement else 'refinement-data')).resolve()
    output.relative_to(root.resolve())
    parent_train = (compact / 'refinement-data/train.jsonl' if args.second_refinement else previous / 'train.jsonl')
    train = read_lines(parent_train)
    validation = read_lines(previous / 'validation.jsonl')
    rows = json.loads((previous / 'situations.json').read_text(encoding='utf-8'))
    personas = {row['context']['self_name']: row['context']['persona'] for row in rows if row['usage'] == 'train'}
    stories = more_flee_stories() if args.second_refinement else extra_stories()
    cases = [make_case(item, branch, personas) for item in stories for branch in range(2)]
    remaining_times = (1.2, 2.0, 2.7, 3.4, 4.2, 4.8)
    for index, case in enumerate(cases):
        if args.second_refinement:
            set_time_left(case, remaining_times[(index // 2) % len(remaining_times)])
        action, = case['label']['acceptable_actions']
        case['source']['kind'] = 'authored_compact_model_training'
        case['label']['status'] = 'author_reviewed_paired_outcomes'
        train.append({'id': case['id'], 'group_id': case['group_id'], 'source': case['source'],
                      'task': 'combat_update', 'target_label_source': 'author_reviewed_paired_outcomes',
                      'messages': [{'role': 'system', 'content': COMBAT_INSTRUCTIONS},
                                   {'role': 'user', 'content': json.dumps(case['prompt'], ensure_ascii=False, separators=(',', ':'))},
                                   {'role': 'assistant', 'content': json.dumps({'action': action, 'target_id': '' if action == 'Wait' else 'opponent'}, separators=(',', ':'))}]})
    if len({row['id'] for row in train}) != len(train) or {row['group_id'] for row in train} & {row['group_id'] for row in validation}:
        raise ValueError('训练编号重复，或训练与开发集包含同一故事')
    output.mkdir(parents=True, exist_ok=False)
    (output / 'train.jsonl').write_text(''.join(json.dumps(row, ensure_ascii=False) + '\n' for row in train), encoding='utf-8')
    write_json(output / 'new-situations.json', cases)
    write_json(output / 'data.json', {'train_rows': len(train), 'new_stories': len(cases) // 2,
                                     'train_sha256': sha(output / 'train.jsonl'),
                                     'parent_train_sha256': sha(parent_train),
                                     'validation_sha256': sha(previous / 'validation.jsonl'),
                                     'acceptance_cases_used': False})


if __name__ == '__main__':
    main()
