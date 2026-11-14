"""训练故事只使用游戏已有事件，教师独立选择动作，不提供预设答案"""

# 当前行动、自己受击/挥拳/命中、观察距离、末尾现场事件、己方原话、对方原话
# 两段原话均是已经发生的发言，SelfCombat 文本由同一组数字构造
STORIES = '''
Fight|0,1,0|150|FightThreat|你凭什么说我只会吹牛？|就说你了，来，让我看看你有多大本事！
Fight|1,1,1|130|LightMeleeHit|这条路又不是你家的。|你刚才推我那一下，我还没跟你算呢！
Fight|0,2,0|240|FightThreat|谁先来的谁先过，有这么难吗？|少跟我讲这些，你先碰我的！
Fight|0,1,0|180|FightThreat|我没偷你的凳子，那是我自己的。|我的名字还在上面，你还嘴硬！
Fight|1,2,1|140|LightMeleeHit|你把我的菜踩坏了还不道歉？|我赔你钱了，你还揪着不放！
Fight|0,0,0|280|FightThreat|比就比，我这腿脚好着呢。|光站那儿说算什么，有本事过来！
Fight|1,3,1|165|FightThreat|别拿我的年龄开玩笑。|哟，还生气了？你刚才不是挺厉害吗？
Fight|0,2,0|200|FightThreat|你卖的称少了，你心里清楚。|当着这么多人胡说，你今天得说明白！
Fight|1,1,0|120|LightMeleeHit|说好比划一下，你还真上劲了。|是你先使劲的，还怨上我了？
Fight|0,1,1|150|FightThreat|我没说你怂啊，别血口喷人！|没说就没说嘛，你这是什么态度！
Fight|4,1,0|125|LightMeleeHit|哎哟，你怎么下手这么重！|刚才说不怕的是谁？别往后缩！
Fight|3,2,0|140|LightMeleeHit|我这把老骨头可经不起你折腾。|是你自己先冲过来的，你别赖我！
Fight|5,1,0|115|FightThreat|你先放下手，有话慢慢说。|现在知道慢慢说了？刚才可不是这口气！
Fight|4,3,0|130|LightMeleeHit|疼死了，谁跟你玩真的啊！|现在说玩笑，你刚才还追着我呢！
Fight|3,1,0|180|FightThreat|我的眼镜都快掉了！|你别装，刚才嘴还硬得很！
Fight|4,2,1|145|LightMeleeHit|我不跟你争谁厉害了，行了吧。|你说不争就不争？先把话说清楚！
Fight|5,3,0|160|FightThreat|哎哟，我刚才就是开个玩笑。|你拿我开玩笑还没完没了了？
Fight|3,2,0|170|LightMeleeHit|我认错，我说错了还不行吗！|早干什么去了，你站住！
Fight|4,1,0|130|FightThreat|刚才那一下真疼，别来了。|你追着我的时候怎么不说别来？
Fight|5,2,1|140|LightMeleeHit|菜钱我不要了，你先冷静一点。|刚才拦着我不让走，现在想算了？
Fight|0,1,0|310|Perceived|那帽子本来就是我的。|是我看错了，你看我的在这儿，刚才对不起。
Fight|1,2,1|360|Perceived|你刚才当众说我骗人。|我说错了，我当着大家赔不是，这下行了吧。
Fight|0,0,0|420|Perceived|我就是想让你把称拿出来看看。|称在这儿，你随便看，是我脾气不好。
Fight|1,1,1|330|Perceived|你别揪着我的衣服不放。|我松手了，刚才急眼了，咱们都别动手。
Fight|0,2,0|400|Perceived|原来你说的是隔壁老王啊。|对啊，我又没说你，这不是误会了吗。
Fight|2,2,1|370|Perceived|说好喊停就停，你听见没有？|听见了，停，我不跟你比了。
Fight|0,1,1|380|Perceived|别再说我插队了，票号在这儿。|我看清楚了，是我弄错了，您先请。
Fight|1,3,2|450|Perceived|这本来就是场玩笑。|行行行，你厉害，我认输，不闹了。
Fight|0,1,0|350|Perceived|你踩了菜苗，跟人家说一声吧。|我已经赔了，刚才也道歉了，你听见了吗。
Fight|1,2,1|500|Perceived|你把那句骗子收回去。|我收回，刚才说重了，咱们别打了。
Flee|4,1,0|150|FightThreat|别追了，我都说对不起了！|你先站住，把刚才那事说完！
Flee|3,2,0|210|LightMeleeHit|哎哟，怎么还追到这儿来了。|你倒是继续笑啊，刚才不是挺得意吗！
Flee|5,1,0|170|FightThreat|帽子我还你了，你还想怎样！|你刚才扔我的帽子怎么算？
Flee|3,2,1|190|FightThreat|我喘口气，你别跟着我。|你刚才追我的时候可没让我喘！
Flee|4,3,0|130|LightMeleeHit|我那句真不是骂你。|现在还解释，你当我听不出来吗！
Flee|2,1,0|160|FightThreat|大伙儿看着呢，你差不多得了。|刚才你起哄的时候怎么不说差不多？
Flee|4,2,0|220|FightThreat|我手里还拎着菜呢！|你拎着菜照样推了我！
Flee|3,1,0|200|LightMeleeHit|说好到路口就算了。|那是你说的，我可没答应！
Flee|5,3,1|150|FightThreat|有话好好说，别动手了。|我刚才好好说你听了吗！
Flee|3,2,0|180|FightThreat|你真把我当沙包了啊？|你刚才不是还让我两下吗！
Flee|3,1,0|620|Perceived|别跟过来了啊，我可看着呢。|行了行了，不追了，我在这儿歇会儿。
Flee|4,2,0|740|Perceived|这事到此为止，谁也别提了。|行，我答应，刚才也有我的不对。
Flee|2,1,0|680|Perceived|你的袋子我放这儿了。|看见了，谢谢，刚才误会你了，对不住。
Flee|3,2,1|800|Perceived|我都跑这么远了，还没完啊？|完了完了，我不追你了，回家吃饭。
Flee|4,1,0|700|LostSight|你别从后面吓唬我。|我进屋了，门也关了，不跟你闹了。
Flee|3,1,0|900|Perceived|你承认刚才说错话就行。|我承认，向你道歉，不打了。
Flee|2,2,0|650|Perceived|我的鞋都跑掉了，你看！|行了，慢慢捡，我不追你。
Flee|4,2,1|820|Perceived|我可不会再回去跟你比了。|没人让你回来，我也累了，就这样吧。
Flee|3,3,1|760|Perceived|刚才那笔钱真不是我拿的。|人家找着了，是我冤枉你了，对不起。
Flee|2,1,0|850|Perceived|我在这儿站一会，你别过来。|好，我不过来，你也缓缓。
Flee|0,1,0|150|FightThreat|刚才有小孩挡着，我才让开的。|这回没人挡了，你倒是接着来啊！
Flee|0,2,1|180|FightThreat|车过去了，路空了。|那正好，刚才那一拳还没算清楚！
Flee|1,2,1|160|LightMeleeHit|我躲的是水坑，不是你。|你还嘴硬，转过来比划比划啊！
Flee|0,1,0|190|FightThreat|我没跑，就是把菜放旁边。|放好了就过来，我等着呢！
Flee|1,1,1|140|FightThreat|凳子拿开，别碰到人家的东西。|拿开了，你还找什么理由？
Flee|0,2,0|200|FightThreat|在窄道里推什么，出来说。|出来了，你不是挺能打的吗！
Flee|0,1,1|180|FightThreat|刚才鞋带松了，差点绊倒。|系好了？那来，别又怪鞋带！
Flee|1,2,1|150|LightMeleeHit|你别说我家里人，这跟他们没关系。|你能说我，我就不能说你了？
Flee|0,0,0|210|FightThreat|我先让老人过去。|老人过去了，我看你还有什么话说！
Flee|0,1,0|170|FightThreat|帽子放好了，别给我踩坏了。|你还挺讲究，来啊，光说有什么用！
Fight|1,1,0|220|Perceived|刚才没站稳，你别得意。|我也没占便宜，你看我袖子都歪了。
Fight|2,3,1|160|FightThreat|你这人是真犟。|彼此彼此，你不是也不肯松口吗！
Fight|1,2,1|340|Perceived|你那拳也没多重。|你也一样，要不咱俩歇一下？
Flee|2,1,0|510|Perceived|我到这儿了，你别再跟着。|我就站这儿，谁跟着你了。
Flee|1,2,1|290|Perceived|你有完没完，追着干什么。|我就是想问清楚，不碰你。
Fight|0,1,0|380|Perceived|刚才那拳没碰到你吧？|没有，你打偏了，我也没打着你。
Fight|2,2,0|270|Perceived|我不跟你比力气。|那你想怎么说？我听着。
Flee|2,1,0|900|LostSight|人呢？刚才还在后面。|你别喊了，我听得见。
Fight|1,3,1|180|LightMeleeHit|你这一拳还挺快。|那当然，我早跟你说了。
Flee|0,1,0|450|Perceived|你是不是不比了？|你说呢，刚才只是热个身。
Fight|0,2,0|400|Perceived|刚才我还以为你骂我呢。|谁骂你了，我说的是那只偷菜的狗。
Fight|4,1,0|160|LightMeleeHit|我先前说不疼，那是嘴硬。|我看你现在还挺能说啊！
Fight|1,1,0|140|FightThreat|你刚才明明说算了。|我越想越气，这事不能这么算！
Flee|3,1,0|780|Perceived|你刚才不是说非追上我不可吗。|气话，我现在消气了，不追了。
Flee|4,2,0|150|LightMeleeHit|你说了不打，怎么又来了！|谁让你刚才又回头笑我的！
Fight|0,1,0|350|Perceived|旁边人还叫咱们再比呢。|别听他们的，他们看热闹不嫌事大，不比了。
Fight|5,2,0|140|LightMeleeHit|哎哟，我可真挨着了。|你别装，我都没怎么使劲！
Fight|1,1,0|380|Perceived|你说把我打趴下了，可我还站着呢。|行行行，你站得稳，刚才吹牛呢，不打了。
Flee|3,1,0|130|FightThreat|你不是都说自己赢了吗。|赢了也得把话说清楚，你站住！
Fight|0,2,1|160|FightThreat|你刚才可实实在在挨了一下。|那是我没留神，你再来试试！
Fight|0,0,0|320|Perceived|你先把花盆放稳，别摔了。|是，里面是我老伴种的花，咱俩别闹了。
Fight|4,1,0|120|FightThreat|这地方太挤，别推我。|刚才你堵着我的时候怎么不说挤！
Fight|0,2,0|950|LostSight|你躲门后面干什么？|我进屋吃饭了，今天不跟你说了。
Fight|0,1,0|300|FightThreat|跨个路沿也叫跑了？|那你跟上来啊，我就在这儿！
Flee|3,2,0|260|FightThreat|我从这边走，你别堵着。|你把我刚才那句话答了再走！
Fight|0,1,0|400|Perceived|那边有人摔倒了，先别争。|看见了，咱们过去搭把手，刚才的事算了。
Fight|4,2,0|150|LightMeleeHit|台阶就在后面，你别再挤我。|那你出来，别躲在那儿！
Flee|2,1,0|700|Perceived|车这么多，你别过来了。|我就在这边等车，不追你了。
Fight|0,1,0|500|Perceived|送货车来了，让人家搬东西。|行，今天就到这儿，别给人添乱。
Fight|0,1,0|160|FightThreat|车走了，你还挡着我干什么？|刚才咱俩的事还没完呢！
Fight|0,0,0|400|Perceived|你连我名字都叫错了。|啊，你不是隔壁老张？认错人了，对不住。
Fight|0,2,0|200|FightThreat|别老当着大伙儿说我没本事。|有没有本事，你倒是拿出来看看啊！
Fight|2,1,0|130|LightMeleeHit|疼疼疼，我最怕别人打这儿。|刚才不是你自己说随便来的吗！
Fight|1,2,1|420|Perceived|你服气没有？|服了，你反应真快，咱们别比了。
Fight|0,1,0|180|FightThreat|哈哈，你也没比我准多少。|笑什么，你再接我一下试试！
Flee|4,1,0|150|FightThreat|你别过来啊，我可要生气了！|你不是已经生气了吗，怎么还跑！
Fight|0,1,0|380|Perceived|我听清楚了，原来你不是那意思。|对，就是个误会，别让人看笑话了。
Flee|0,0,0|700|Perceived|你刚才抬手，我以为你要打我。|我捡帽子呢，哪有功夫打你，你看。
Fight|1,1,0|140|LightMeleeHit|我平时不跟人争，不代表你能推我。|少跟我说这套，你刚才也推了！
Fight|5,2,0|160|FightThreat|刚才是我说话太满了。|现在承认有什么用，你倒是接着来啊！
Fight|0,1,0|450|Perceived|你怎么不吭声了？|肚子不舒服，我先回去了，不比了。
Flee|3,1,0|800|Perceived|我朋友也看见你追我了。|是我刚才过分了，给你道个歉，我不过去。
Fight|0,1,0|170|FightThreat|说好了，只比一下反应。|来啊，刚才那一下我还没看清呢！
Fight|1,2,1|400|Perceived|喊停就停，这是事先说好的。|停停停，我不比了，听见没有。
Fight|3,1,0|150|LightMeleeHit|我说不玩了，你怎么还认真了。|刚才你往我身上撞的时候可不像玩！
Flee|0,2,1|160|FightThreat|你别堵路，退一步。|就堵着了，你刚才不是挺横吗！
Fight|0,1,0|500|Perceived|桥都过去了，还争什么先后。|是，咱俩刚才真是闲的，走吧。
Flee|2,1,0|240|FightThreat|你都没打到我，还追什么。|那我更得追上你，你别跑！
Fight|0,2,0|180|FightThreat|我刚才那拳只是没找准。|那你找准了再来，我还站这儿呢！
Fight|0,1,0|900|Perceived|你离那么远嚷什么？|我就说两句，不过去了，你也别来。
Fight|4,3,0|130|LightMeleeHit|怎么我一拳都碰不到你啊。|你那慢吞吞的样子，当然碰不到！
Flee|2,1,0|850|Perceived|你还追吗，我可问最后一遍。|不追了，我跟老刘聊会儿，你走吧。
Fight|0,1,0|350|Perceived|我的帽子掉了。|我帮你捡起来了，刚才别往心里去，咱们和气点。
Fight|1,2,1|140|LightMeleeHit|你假装转身干什么？|这叫反应快，你刚才又没看见吧！
Flee|3,1,0|170|FightThreat|你堵着我回家的路干什么。|先别回家，你把刚才说我的话收回去！
Fight|0,1,0|430|Perceived|哈哈，咱俩像不像两个小孩。|可不是嘛，别闹了，让人笑话。
Fight|2,2,1|180|FightThreat|你刚才那拳还真有点疼。|你也打到我了，谁也没占便宜！
Flee|2,1,0|800|LostSight|刚才还追着呢，现在看不见了。|我听着呢，你先别走远。
Fight|0,2,1|420|Perceived|我确实碰到你了，不是吹牛。|我知道，你厉害，这回到此为止。
Flee|3,2,0|190|FightThreat|我都退这么远了，你还跟着。|你刚才笑我的事还没说清楚！
Fight|0,1,0|360|Perceived|你的助听器掉到哪儿了？|就在脚边，先别动，刚才是我听岔了，对不起。
Fight|4,1,0|120|LightMeleeHit|你往灌木那边推我干什么。|你别转移话题，刚才是不是你先碰我的！
Flee|3,1,0|210|FightThreat|上坡都追，你不累吗？|我不累，你先停下来把话说了！
Fight|0,1,0|150|FightThreat|刚才那一下我躲开了。|躲开一次有什么用，再接着来！
Fight|1,1,0|400|Perceived|帽子都让你打歪了。|我给你扶正，刚才是我不好，咱们算了吧。
Flee|3,1,0|850|LostSight|我在树底下，看不见你了。|我买水去了，你别跑了，已经不追了。
Fight|1,2,1|190|LightMeleeHit|咱俩这回谁也没占便宜。|是啊，我挨了一下，你也挨了一下。
Fight|0,1,0|650|Perceived|你电话响了。|老伴叫我回家，我得走了，不闹了。
Flee|3,2,0|780|Perceived|你答应他们不追了，可别反悔。|不反悔，就到这儿，我站着不动了。
Fight|5,2,0|140|LightMeleeHit|我这手都打抖了。|现在知道我厉害了吧，你刚才可不是这么说的！
Fight|0,0,0|400|Perceived|原来你学的是电视剧里的那句话。|是啊，我没骂你，刚才让你误会了，对不住。
Flee|4,1,0|180|FightThreat|报刊亭都绕过去了，你怎么还跟着。|你笑话我的时候怎么不嫌烦呢！
Fight|0,2,0|160|FightThreat|你也打空了，我可看见了。|那就再比一次，看谁先碰到谁！
Fight|1,2,1|480|Perceived|你别退了，先把话说清。|我认栽了，我不跟你争了，这样可以了吗。
Flee|2,1,0|600|Perceived|你揉手腕干什么？|刚才使劲使大了，缓一下。
'''


def training_stories():
    result = []
    for number, line in enumerate(STORIES.strip().splitlines(), 1):
        action, counters, distance, event, self_speech, speech = line.split('|')
        received, punches, hits = map(int, counters.split(','))
        # 正式角色四次连续轻拳会倒地，这批只训练仍能执行 Fight/Flee 的状态
        received = min(received, 3)
        result.append({'id': 'combat-' + str(number).zfill(3), 'current_action': action,
                       'received': received, 'punches': punches, 'hits': hits, 'distance': int(distance),
                       'last_event': event, 'self_speech': self_speech, 'speech': speech})
    assert len(result) == 135
    return result
