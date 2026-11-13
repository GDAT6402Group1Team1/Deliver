"""生成 AI 文档的 UML 图，不依赖浏览器或外部渲染服务"""
from html import escape
import textwrap
import re


def label(x, y, value, size=15, anchor="start", bold=False):
    return f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" font-weight="{600 if bold else 400}">{escape(value)}</text>'


def canvas(title, width, height, body):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">
<title>{escape(title)}</title><defs>
<marker id="arrow" markerWidth="10" markerHeight="10" refX="9" refY="5" orient="auto"><path d="M1 1 L9 5 L1 9" fill="none" stroke="#5b6b7a"/></marker>
<marker id="diamond" markerWidth="14" markerHeight="12" refX="1" refY="6" orient="auto"><path d="M1 6 L7 1 L13 6 L7 11 Z" fill="#5b6b7a"/></marker>
</defs><style>text{{font-family:'Segoe UI','Microsoft YaHei',sans-serif;fill:#253447}} .edge{{fill:none;stroke:#5b6b7a;stroke-width:1.4}} .lifeline{{stroke:#98a5b2;stroke-dasharray:5 5}} </style>
<rect width="100%" height="100%" fill="#fff"/>{label(24,34,title,22,bold=True)}{body}</svg>'''


def node(name, role, fields, methods, status="已实现", kind="class"):
    # 类图先展示职责，再给出代码名；每框只选两个主要接口，其余在正文解释
    descriptions = {
        'UDeliveryNpcAgentComponent': ('NPC 决策', ['保存所见所闻和当前请求', '记录正在执行的动作'], [('RequestDecision()', '安排一次模型请求'), ('StartAction()', '检查条件并开始动作')]),
        'UDeliveryAiGatewaySubsystem': ('模型网络请求', ['保存排队和发送中的请求', '记录超时与所属游戏世界'], [('Enqueue()', '把请求加入队列'), ('Cancel()', '取消这个 NPC 的请求')]),
        'ADeliveryNpcCharacter': ('NPC 角色', ['创建决策、战斗和表现组件', '保存当前执行的动作编号'], [('HearSpeech()', '接收玩家或其他 NPC 的话'), ('ExecuteAction()', '按动作类型调用相应功能')]),
        'UDeliveryNpcPresentationComponent': ('对白与人物表现', ['安排逐句对白和客户端播放', '管理声音、字幕与面部表情'], [('QueueSentence()', '按序保存这次回复的句子'), ('GetDisplayedSpeech()', '读取本机实际播放的句子')]),
        'ADeliveryNpcAIController': ('NPC 控制器', ['使用 UE 的视听感知', '更新行为树需要的条件'], [('OnTargetSeenOrHeard()', '记录发现或失去目标的通知'), ('RefreshBlackboard()', '更新倒地、动作和目标信息')]),
        'UBTTask_DeliveryExecuteNpcAction': ('动作执行任务', ['保存本次动作编号与目标', 'Fight 等待靠近、收拳与冷却'], [('ExecuteTask()', '开始执行行为树任务'), ('TickFight()', '检查次数、距离与停止条件')]),
        'UDeliveryConversationComponent': ('玩家交谈', ['位于玩家控制器上', '记录发言编号和发送间隔'], [('Submit()', '从玩家本机发送消息'), ('ServerSubmit()', '服务器检查后交给 NPC')]),
        'DecisionServer / DecisionHandler': ('AI 后端', ['模型结束后可调用下一条请求', '本轮语音完成后再结束 HTTP 回复'], [('do_POST()', '接收请求、取消和逐句连接'), ('run_job() [模块函数]', '调用模型并安排语音合成')]),
        'BT_Resident / BTT_ResidentExecute': ('NPC 行为树', ['保存各个行为分支', '倒地时优先停止普通动作'], [('选择动作分支', '读取黑板中的条件'), ('运行动作任务', '交给 C++ 任务类执行')]),
        'UAIPerceptionComponent': ('UE 视听感知', ['记录感知到的对象', '提供视觉和听觉通知'], [('OnTargetPerceptionUpdated', '通知目标的感知情况变化'), ('GetCurrentlyPerceivedActors()', '查询目前感知到的对象')]),
        'FDeliveryNpcObservation': ('一次观察', ['保存事件来源和对象', '保存位置、内容和有效期'], [('由感知或玩法代码填写', '例如听见发言、受到撞击'), ('交给 ObserveEvent()', '检查后加入 NPC 的记录')]),
        'FDeliveryNpcEvidence': ('NPC 已知事件', ['保存事件编号和描述', '区分亲眼所见与他人转述'], [('由 Agent 保存', '模型只能使用已知信息'), ('超过有效期后移除', '不把旧位置当作当前位置')]),
        'FDeliveryNpcDecisionRequest': ('模型请求数据', ['记录 NPC、请求和世界编号', '包含相关事件及可用动作'], [('BuildDecisionRequest()', '从当前信息中选取请求内容'), ('EncodeRequest()', '转换为后端接收的 JSON')]),
        'UDeliveryConversationWidget': ('轻量交谈输入', ['下方显示玩家原话和输入', '例句按需展开，可选中复制'], [('Send() / CommitInput()', '点击发送或输入框回车才提交'), ('按发言编号确认回应', 'NPC 台词显示在人物附近')]),
        'Scheduler': ('后端请求安排', ['队满时玩家替换未开始的闲聊', '玩家交谈期间暂停新闲聊'], [('submit()', '优先接收并安排玩家消息'), ('cancel()', '排队立即移除，运行等实际结束')]),
        'UBlackboardComponent': ('行为树黑板', ['保存倒地、动作与倾听条件', '由 NPC 控制器持续更新'], [('供行为树读取', '决定当前运行哪个分支'), ('由控制器更新', '不重复保存完整动作记录')]),
        'BT_Resident': ('NPC 行为树', ['倒地、执行动作、倾听和空闲', '条件变化时切换分支'], [('读取黑板', '选择当前应执行的行为'), ('条件变化时中止', '例如 NPC 受击或目标离开')]),
        'BTT_NpcListen': ('倾听任务蓝图', ['父类 BTTask_DeliveryListen', '暂时停下移动并看向说话者'], [('ExecuteTask()', '设置本次倾听的朝向'), ('OnTaskFinished()', '解除本任务设置的朝向')]),
        'AppServer': ('本机模型连接', ['取消获确认后保留已有进程', '每轮建立独立会话并选择模型'], [('stream(model=...)', '指定本次模型并逐步接收文字'), ('close()', '超时或连接异常时关闭进程')]),
        'SentenceParser': ('完整句子检查', ['首条结果确定候选动作', '格式错误交给后端处理'], [('feed()', '收集文字并解析完整 JSON'), ('line()', '检查动作与句子后通知后端')]),
        'SpeechSynthesizer': ('可选 Windows 语音', ['显式选择 windows 时使用 SAPI', '两个合成线程复用各自语音进程'], [('prepare()', '提前准备 Windows 中文声音'), ('synthesize()', '返回一句完整 PCM 音频')]),
        'SpeechQueue': ('NPC 共用的语音队列', ['玩家交谈优先于未开始的闲聊', '一个 GPU 实例依次合成句子'], [('submit()', '按来源安排完整句子'), ('close()', '取消排队项并通知当前合成停止')]),
        'QwenSpeechSynthesizer': ('NPC 台词语音', ['大壮 B 不变，翠花 B 用 Graph', '按请求选择整句或分段声音'], [('synthesize()', '按完整句返回声音'), ('synthesize_stream()', '生成过程中返回新增声音')]),
        'UDeliveryNpcBodyAnimInstance': ('主动作参数', ['读取角色速度、动作与开始时间', '倒地和起身优先于轻拳后仰'], [('NativeUpdateAnimation()', '在游戏线程准备动画所需数值'), ('StrikeTime / HitTime', '按服务器时间显示挥拳与后仰')]),
        'ABP_NpcBody': ('主动画蓝图', ['十种姿势在图中平滑切换', '走路、踱步和快步使用同步组'], [('Blend Poses by Int', '按动作编号混合姿势'), ('Sequence Evaluator', '按时间显示攻击、受击与恢复')]),
        'FDeliveryNpcSentence': ('一句台词及语音', ['保存句子序号、文字和情绪', '保存 PCM 音频及采样率'], [('Sequence / Text / Emotion', '按序播放同一次回复'), ('Pcm / SampleRate', '交给客户端声音播放')]),
        'FDeliveryNpcLocalVoice': ('本机待播语音', ['保存声音、解码器和字幕进度', '同一句积累声音后接续播放'], [('State.bAudioComplete', '句末到达后才能结束本句'), ('QueuedBytes / TotalBytes', '记录已排入声音与最终长度')]),
        'FDeliveryNpcVoiceStreamEncoder': ('句内声音编码', ['每句话保留同一个 Opus 编码器', 'V2 到句末才确定声音总长'], [('Append()', '收到新 PCM 后接着编码'), ('Finish()', '补齐尾音并通知最终长度')]),
        'UDeliveryNpcExpressionSet': ('表情与交谈姿态配置', ['保存眼睛、眉毛和眨眼曲线', '保存倾听时长、前倾和头部过渡参数'], [('Poses / BlinkCurve', '按情绪选择形态键权重'), ('ListeningReactionDuration', '短暂倾听后进入等待姿态')]),
        'UDeliveryNpcConversationAnimInstance': ('交谈姿态参数', ['读取倾听、思考与实际播放状态', '平滑视线、胸部和头部旋转'], [('NativeUpdateAnimation()', '在游戏线程准备姿态参数'), ('NativeUninitializeAnimation()', '清除目标并恢复叠加权重')]),
        'UDeliveryNpcLookComponent': ('经过与擦身时的注视', ['服务器选择目标和注视时间', '配置距离、冷却与转头幅度'], [('GetLookTarget()', '读取当前要看的人'), ('IsAfterBrush()', '区分经过与轻微擦碰')]),
        'ADeliveryPedestrian': ('普通路人', ['沿原路线和 Boids 规则行走', '保留程序化步态与受击行为'], [('NpcLook', '短暂关注附近玩家'), ('头部叠加旋转', '走路时看一眼，再恢复')]),
        'ABP_NpcConversation': ('交谈动画蓝图', ['保留原动作，调整 Spine2 与 Head', '仅用于两份独立 Face 网格'], [('LookAt / ModifyBone 节点', '跟随视线并叠加前倾和侧头'), ('姿态权重降到零', '挥拳、倒地、拽车时退出')]),
        'FDeliveryNpcSpeechState': ('对白状态', ['保存台词、情绪和发言编号', '保存句子顺序、时间与音频信息'], [('Speech / DisplayedSpeech', '分别记录安排和本机实际播放'), ('ReplyToEventId / Sequence', '对应玩家发言和句子顺序')]),
        'UDeliveryNpcCombatComponent': ('NPC 战斗', ['Fight：默认 5 秒、3 拳、6 米', '轻拳累计，重击保留倒地规则'], [('StartFightStrike()', '检查本轮目标和次数再出一拳'), ('ReceiveFightHit()', '记录轻拳，后仰或倒地')]),
        'UDeliveryVehicleHoldComponent': ('车辆拽车处理', ['记录拽住车辆的 NPC', '记录拽车和持续油门时间'], [('TryHold()', '检查条件后建立拽车关系'), ('Release()', '松手并恢复正常速度倍率')]),
        'ADeliveryMotorbike': ('电瓶车', ['处理驾驶与车辆移动', '持有拽车处理组件'], [('读取 GetSpeedScale()', '根据拽车状态调整速度'), ('调用 UpdateThrottle()', '累计油门时间以便挣脱')]),
        'FDeliveryNpcProfileRow': ('NPC 人物配置', ['DisplayName / Persona：名字与性格', 'TtsVoice / VoiceSet：两类声音配置'], [('继承 FTableRowBase', 'DT_NpcProfiles 每行对应一名角色'), ('chunlai / xiumei', '分别配置大壮与翠花')]),
        'UDeliveryNpcVoiceSet': ('台词音量与短音配置', ['DialogueVolume：完整台词音量', 'DialogueAttenuation：台词距离衰减', 'ReactionAttenuation：短音距离衰减', 'HeardPlayer / Hit：听见与受击短音', 'StreamingStartBufferSeconds：开播缓冲', 'StreamingResumeBufferSeconds：续播缓冲'], [('GetSound(Reaction)', '查找对应 Sound Wave 或 Sound Cue'), ('Volume / Pitch / MaxDuration', '分别调整短反应音的播放效果')]),
        'UDeliveryAiSettings': ('AI 项目设置', ['保存后端地址和启用开关', '保存请求数量上限和超时'], [('在项目设置中修改', '配置写入 DefaultGame.ini'), ('由网络子系统读取', '不再建立重复的数据表')]),
        'FDeliveryConversationOptionRow': ('旧例句配置设计，未创建', ['目前使用只读建议文字', '不再按表行直接发送'], [('选中复制后编辑', '显示文字使用 FText'), ('提交玩家确认的文字', '仍需检查是否允许交谈')]),
        'UDeliveryNpcPresentationData': ('动画与声音配置', ['保存移动、战斗和说话动画', '保存声音和面部形态键名称'], [('使用 UDataAsset 保存资源', '由 NPC 选择对应资产'), ('资源加载完成后使用', '交给表现组件播放')]),
    }
    title, fields, methods = descriptions[name]
    if name == 'UDeliveryNpcAgentComponent' and '倾听' in role:
        fields = ['记录说话者、发言与倾听开始时间', '保存当前请求及动作状态']
        methods = [('BeginListening()', '玩家插话时结束旧交谈'), ('EndListening()', '清除倾听目标和发言编号')]
    if name == 'UDeliveryNpcAgentComponent' and '逐句' in role:
        fields = ['检查请求、动作和回复编号', '只接收本次交谈的有效句子']
        methods = [('ReceiveSentence()', '接收旧格式的完整句子'), ('ReceiveAudioChunk()', '接收当前句的新增声音')]
    if name == 'UDeliveryAiGatewaySubsystem' and '逐句' in role:
        fields = ['检查句子顺序、音频段与最终长度', '取消或结束关卡后忽略旧回复']
        methods = [('ReadStream()', '在游戏线程读取完整消息'), ('Cancel()', '停止接收并通知后端取消')]
    if name == 'UDeliveryNpcPresentationComponent' and '分段播放' in role:
        fields = ['按句安排声音、字幕与情绪', '同一句持续编码并同步音频']
        methods = [('BeginSentenceAudio()', '开始本句，暂不要求最终长度'), ('EndSentenceAudio()', '确认句末或退回文字显示')]
    if name == 'ADeliveryNpcCharacter' and '人物配置' in role:
        fields = ['Profile：选择人物数据表及表行', '保留原模型、动画和关卡位置']
        methods = [('ApplyProfile()', '检查后应用名字、性格和声音'), ('未选表或配置无效', '保留角色原有设置')]
    if name == 'UDeliveryNpcPresentationComponent' and '短反应音' in role:
        fields = ['TtsVoice：后端朗读完整中文台词', 'VoiceSet：台词音量、衰减与短音']
        methods = [('PlayReaction()', '服务器按事件编号通知播放'), ('StopReaction()', '正式台词开始或被打断时停声')]
    kind = {'class':'类','struct':'数据结构','asset':'蓝图资产','module':'代码模块','DTO':'传输数据', 'Python classes':'Python 类','design':'后端类', 'BT task':'行为树任务','DataTable row':'数据表行','DataAsset':'数据资产','DeveloperSettings':'项目设置'}.get(kind,kind)
    status = {'已实现':'已有代码','拟扩展':'计划修改','拟新增':'计划增加'}.get(status,status)
    return dict(name=name, title=title, role=role, fields=fields, methods=methods, status=status, kind=kind)


def class_diagram(title, nodes, edges):
    # 总图采用运行环境分区，局部图采用紧凑类框；编号将长说明移出连线通道
    width, boxw = 1240, 340
    overview = title.startswith('Delivery NPC')
    positions = [(35+(i%3)*410,160+(i//3)*340) for i in range(len(nodes))]
    if overview:
        positions=[(70,1090),(445,420),(445,150),(445,765),(35,765),(855,420),(820,1090),(855,765),(855,150)]
    boxes, lines, notes, rects = [], [], [], []
    for i, n in enumerate(nodes):
        x, y = positions[i]
        planned = "计划" in n['status']
        color, fill = ('#b99253','#fff8eb') if planned else ('#8aa5c6','#eff4fb')
        short=re.sub(r'^[UAF]Delivery','',n['name']).replace('UBTTask_Delivery','BTTask_')
        head=80
        split=head+16+len(n['fields'])*20
        boxh=split+14+len(n['methods'])*38
        rects.append((x,y,boxw,boxh))
        boxes.append(f'<g data-node="{escape(n["name"])}"><title>{escape(n["name"])}</title><rect x="{x}" y="{y}" width="{boxw}" height="{boxh}" fill="white" stroke="{color}"/>')
        boxes.append(f'<path d="M{x+1} {y+1} H{x+boxw-1} V{y+head} H{x+1} Z" fill="{fill}"/>')
        boxes.append(label(x+12,y+18,f'«{n["kind"]}»',11))
        boxes.append(label(x+boxw-12,y+18,n['status'],11,'end'))
        boxes.append(label(x+12,y+43,n['title'],18,bold=True))
        boxes.append(label(x+12,y+65,short,12))
        boxes.append(f'<path d="M{x} {y+head} H{x+boxw} M{x} {y+split} H{x+boxw}" stroke="{color}" stroke-width=".8"/>')
        for j, value in enumerate(n['fields']): boxes.append(label(x+12,y+head+22+j*20,value,13))
        for j, (method, meaning) in enumerate(n['methods']):
            boxes.append(label(x+12,y+split+23+j*38,method,13))
            boxes.append(label(x+12,y+split+40+j*38,meaning,12))
        boxes.append('</g>')
    bottom=max(y+h for x,y,w,h in rects)
    for number,(a,b,text,kind) in enumerate(edges,1):
        x1,y1,w1,h1=rects[a]; x2,y2,w2,h2=rects[b]
        if overview and (a,b)==(0,1):
            points=[(x1,y1+h1/2),(15,y1+h1/2),(15,y2+h2/2),(x2,y2+h2/2)]
        elif overview and (a,b)==(3,6):
            points=[(x1+w1/2,y1+h1),(x1+w1/2,1045),(x2+w2/2,1045),(x2+w2/2,y2)]
        elif overview and (a,b)==(4,7):
            points=[(x1+w1/2,y1),(x1+w1/2,710),(x2+w2/2,710),(x2+w2/2,y2)]
        elif overview and (a,b)==(5,4):
            points=[(x1,y1+h1/2),(817,y1+h1/2),(817,735),(x2+w2/2,735),(x2+w2/2,y2)]
        elif nodes[a]['name']=='QwenSpeechSynthesizer' and nodes[b]['name']=='FDeliveryNpcSentence':
            # 语音结果从下方绕到右侧，不穿过新增的持续编码类
            points=[(x1+w1/2,y1+h1),(x1+w1/2,y1+h1+28),(1218,y1+h1+28),(1218,y2+h2/2),(x2+w2,y2+h2/2)]
        elif nodes[a]['name']=='QwenSpeechSynthesizer' and nodes[b]['name']=='UDeliveryAiGatewaySubsystem':
            # 可选分段沿外侧发送到 Gateway，不经过旧整句数据结构
            points=[(x1+w1/2,y1+h1),(x1+w1/2,y1+h1+28),(15,y1+h1+28),(15,y2-34),(x2+w2/2,y2-34),(x2+w2/2,y2)]
        elif nodes[a]['name']=='SentenceParser' and nodes[b]['name']=='SpeechQueue':
            # 沿两列之间下行，避开 Gateway；起点稍错开输入线和连线编号
            points=[(x1,y1+h1/2+22),(430,y1+h1/2+22),(430,y2-28),(x2+w2/2,y2-28),(x2+w2/2,y2)]
        elif len(nodes)==11 and (a,b)==(5,9):
            # 跨过中间一行的组件关系沿外侧走，避免穿过动画类的文字
            points=[(x1+w1,y1+h1/2),(1218,y1+h1/2),(1218,y2-28),(x2+w2/2,y2-28),(x2+w2/2,y2)]
        elif abs(y1-y2)<10 and abs(x1-x2)>w1*1.5:
            # 跨过同一行其他类时从上方连接，避免箭头穿过文字
            points=[(x1+w1/2,y1),(x1+w1/2,y1-34),(x2+w2/2,y2-34),(x2+w2/2,y2)]
        elif abs(y1-y2)<10:
            right=x2>x1; cy=min(y1+h1/2,y2+h2/2)
            points=[(x1+w1 if right else x1,cy),(x2 if right else x2+w2,cy)]
        else:
            down=y2>y1
            sx=x1+w1/2; sy=y1+h1 if down else y1
            ex=x2+w2/2; ey=y2 if down else y2+h2
            lane=(sy+ey)/2
            points=[(sx,sy),(sx,lane),(ex,lane),(ex,ey)]
        points=[point for j,point in enumerate(points) if j==0 or point!=points[j-1]]
        segments=list(zip(points,points[1:]))
        p,q=max(segments,key=lambda pair:abs(pair[0][0]-pair[1][0])+abs(pair[0][1]-pair[1][1]))
        lx=(p[0]+q[0])/2; ly=(p[1]+q[1])/2
        dash=' stroke-dasharray="6 4"' if kind=='dependency' else ''
        marker=' marker-start="url(#diamond)"' if kind=='composition' else ''
        coords=' '.join(f'{x},{y}' for x,y in points)
        lines.append(f'<polyline class="edge" points="{coords}" marker-end="url(#arrow)"{marker}{dash}/><circle cx="{lx}" cy="{ly}" r="11" fill="white" stroke="#bdc9d7"/>{label(lx,ly+4,str(number),11,"middle")}')
        notes.append(label(35+((number-1)%2)*610,bottom+64+((number-1)//2)*26,f'{number:02}  {text}',13))
    legend=label(24,65,'◆ 拥有组件   → 调用或使用   ⇢ 传递数据   代码名省略 Delivery 前缀，完整名称见正文',13)
    panels=''
    if overview:
        for x,y,w,h,name in [(20,90,1200,935,'游戏服务器：检查条件并控制 NPC'),(20,1040,450,300,'玩家客户端：通过控制器发送消息'),(510,1040,710,300,'AI 后端：调用模型，返回台词和动作建议')]:
            panels+=f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="3" fill="#f8fafc" stroke="#dce3eb"/>{label(x+14,y+24,name,12,bold=True)}'
    return canvas(title,width,bottom+90+((len(edges)+1)//2)*26,legend+panels+''.join(lines+boxes+notes))


def sequence(title, actors, steps):
    width=max(1180,len(actors)*190+40); height=160+len(steps)*74
    xs=[70+i*(width-140)/(len(actors)-1) for i in range(len(actors))]
    parts=[label(24,62,'实线：调用 / 请求    虚线：返回 / 通知    绿色注释：执行条件；计划步骤单独标注',14)]
    for i,name in enumerate(actors):
        x=xs[i]
        parts.append(f'<rect x="{x-65}" y="83" width="130" height="45" fill="#eaf1fc" stroke="#83a9e7"/>')
        for j,line in enumerate(name.split('|')): parts.append(label(x,102+j*17,line,12,'middle',True))
        parts.append(f'<path class="lifeline" d="M{x} 128 V{height-25}"/>')
    for i,(a,b,text,kind) in enumerate(steps):
        y=174+i*74
        if kind=='note':
            parts.append(f'<rect x="{xs[a]-45}" y="{y-25}" width="{xs[b]-xs[a]+90}" height="38" fill="#f0f7ef" stroke="#9ec79d"/>{label((xs[a]+xs[b])/2,y-2,text,14,"middle")}')
            continue
        dashed=' stroke-dasharray="5 4"' if kind=='return' else ''
        if a==b:
            points=f'{xs[a]},{y-8} {xs[a]+28},{y-8} {xs[a]+28},{y+13} {xs[a]},{y+13}'
            parts.append(f'<polyline class="edge" points="{points}" marker-end="url(#arrow)"/>{label(xs[a]+36,y,text,13)}')
        else:
            parts.append(f'<path class="edge" d="M{xs[a]} {y} H{xs[b]}" marker-end="url(#arrow)"{dashed}/>{label((xs[a]+xs[b])/2,y-10,text,13,"middle")}')
    return canvas(title,width,height,''.join(parts))


def diagrams():
    n=node
    agent=n('UDeliveryNpcAgentComponent','服务器：个人证据与动作状态',['- Evidence / KnownSubjects','- ActiveRequest / PublicState'],['+ ObserveEvent()','+ RequestDecision()','+ TryFinishAction()'])
    gateway=n('UDeliveryAiGatewaySubsystem','每个 World：请求生命周期',['- Pending / Queue','- WorldEpoch'],['+ Enqueue()','+ Cancel()','- Complete()'])
    resident=n('ADeliveryNpcCharacter','服务器：居民组件组装与协调',['+ Agent / Combat / Presentation','+ BehaviorTree'],['+ HearSpeech()','+ ExecuteAction()','+ StopAction()'])
    presentation=n('UDeliveryNpcPresentationComponent','服务器发布；各端播放',['- Speech / PlayedTurn','- Voice / Caption'],['+ PublishSpeech()','+ CancelSpeech()','- OnRep_Speech()'])
    controller=n('ADeliveryNpcAIController','服务器：感知与黑板更新',['- Sight / Hearing','继承 AAIController'],['+ RefreshBlackboard()','- OnTargetSeenOrHeard()','OnPossess / OnUnPossess'])
    task=n('UBTTask_DeliveryExecuteNpcAction','服务器：批准动作的执行节点',['- ActionId / FinishAt','- FocusTarget'],['+ ExecuteTask()','+ TickTask()','+ AbortTask()'])
    conversation=n('UDeliveryConversationComponent','PC 组件：提交与服务器校验',['- LastSubmissionId','- NextLocalSendAt'],['+ Submit()','+ ServerSubmit() [RPC]','+ ClientResult() [RPC]'])
    backend=n('DecisionServer / DecisionHandler','独立进程：HTTP 与模型调用',['provider / slots / admission','支持逐句语音和完整结果'],['do_POST() / reply()','validate_context() [模块函数]','decide() [模块函数]'],kind='Python classes')
    d={}
    d['overview']=class_diagram('Delivery NPC AI 系统类图 · 当前实现',[
        conversation,resident,controller,gateway,agent,task,backend,presentation,
        n('BT_Resident / BTT_ResidentExecute','服务器：蓝图行为树资产',['黑板：动作目标与执行条件','原生执行器见上方任务类'],['运行批准动作','倒地条件优先','通过任务回报执行结果'],kind='asset')
    ],[(0,1,'服务器检查消息后交给 NPC','dependency'),(2,1,'控制 NPC 移动与朝向','association'),(1,4,'角色创建决策组件','composition'),(4,3,'发送请求，接收模型回复','dependency'),(3,6,'通过 HTTP 传递 JSON 数据','dependency'),(4,7,'通过 NPC 角色发布对白','dependency'),(5,4,'开始动作并报告执行结果','dependency'),(8,5,'调用动作任务','dependency')])
    d['perception']=class_diagram('环境感知类图 · 当前实现',[
        n('UAIPerceptionComponent','UE 内置视听感知',['当前刺激 / 感知对象'],['OnTargetPerceptionUpdated','GetCurrentlyPerceivedActors()']),controller,
        n('FDeliveryNpcObservation','一次感知输入',['Kind / Subject / Source','EventId / ReplyToEventId'],['记录来源、位置与有效期'],kind='struct'),
        n('FDeliveryNpcEvidence','Agent 保存的个人证据',['事件编号 / 来源 / 描述','主体编号 / 有效期'],['仅保存已接收的观察事实'],kind='struct'),agent,
        n('FDeliveryNpcDecisionRequest','发送时的上下文快照',['RequestId / WorldEpoch','Evidence / 候选动作'],['EncodeRequest() [编码函数]'],kind='DTO')
    ],[(1,0,'持有感知组件','composition'),(1,2,'记录这次所见所闻','dependency'),(2,5,'准备发送给模型的事件','dependency'),(1,4,'ObserveEvent','dependency'),(4,3,'保存 NPC 已知的事件','composition'),(4,5,'BuildDecisionRequest','dependency')])
    d['priority']=class_diagram('玩家交谈类图 · 当前代码，效果待本轮检查',[
        n('UDeliveryConversationWidget','本机：输入与回复显示',[],[]),conversation,
        n('FDeliveryNpcDecisionRequest','请求来源与可用动作',[],[],kind='DTO'),
        n('UDeliveryNpcAgentComponent','服务器：玩家倾听与请求',[],[]),
        n('UDeliveryAiGatewaySubsystem','玩家请求优先发送',[],[]),
        n('Scheduler','后端：玩家消息优先',[],[],kind='Python classes')
    ],[(0,1,'确认输入后发送，复制建议不发送','dependency'),(1,3,'检查后通过 NPC 开始倾听','dependency'),(3,2,'bPlayerRequest 标记玩家发言','dependency'),(3,4,'加入游戏端请求队列','dependency'),(4,5,'逐句 HTTP 请求与取消通知','dependency')])
    d['behavior']=class_diagram('行为树与交谈动画类图 · 当前代码，效果待本轮检查',[
        controller,n('UBlackboardComponent','服务器：分支条件',[],[]),
        n('BT_Resident','服务器：行为分支',[],[],kind='asset'),
        n('BTT_NpcListen','服务器：倾听任务',[],[],kind='BT task'),
        n('UDeliveryNpcAgentComponent','服务器：倾听和动作状态',[],[]),resident,
        n('UDeliveryNpcExpressionSet','表情与姿态配置',[],[],kind='DataAsset'),
        n('UDeliveryNpcConversationAnimInstance','本机：姿态参数',[],[]),
        n('ABP_NpcConversation','正式动画图创建工具已加入',[],[],kind='asset'),
        n('UDeliveryNpcLookComponent','短暂注视玩家',[],[]),
        n('ADeliveryPedestrian','普通行人',[],[])
    ],[(0,1,'更新倒地、动作和倾听条件','dependency'),(1,2,'行为树读取条件选择分支','dependency'),(2,3,'运行倾听任务','dependency'),(3,0,'停止移动并设置朝向，结束时解除','dependency'),(5,4,'角色持有决策组件','composition'),(4,7,'提供倾听目标和模型等待状态','dependency'),(6,7,'读取姿态幅度与过渡速度','dependency'),(7,8,'提供平滑后的视线和旋转','dependency'),(8,5,'在 NPC 原动作上调整胸部和头部','dependency'),(5,9,'持有注视组件','composition'),(10,9,'复用注视组件','composition'),(9,7,'没有重要动作时短暂转头','dependency')])
    d['body-animation']=class_diagram('NPC 身体动画类图 · 游戏效果待试玩',[
        resident,
        n('UDeliveryNpcCombatComponent','服务器战斗时间',[],[]),
        n('UDeliveryNpcBodyAnimInstance','主动作参数',[],[]),
        presentation,
        n('ABP_NpcBody','身体动画混合',[],[],kind='asset'),
        n('ABP_NpcConversation','交谈后处理',[],[],kind='asset')
    ],[(0,1,'角色持有战斗组件','composition'),(1,2,'提供挥拳、受击与恢复的开始时间','dependency'),(0,2,'提供速度和当前动作','dependency'),(3,2,'提供动画配置和本机正在说的话','dependency'),(2,4,'更新动作编号、步频和播放时间','dependency'),(4,5,'先得到身体姿势，再叠加转头和倾听','dependency')])
    d['stream']=class_diagram('对白播放类图 · 已接入试玩，游戏声音与多人待验收',[
        n('AppServer','本机模型连接',[],[],kind='Python classes'),
        n('SentenceParser','完整句子检查',[],[],kind='Python classes'),
        n('SpeechSynthesizer','独立线程合成语音',[],[],kind='Python classes'),
        n('UDeliveryNpcAgentComponent','服务器：逐句检查',[],[]),
        n('UDeliveryAiGatewaySubsystem','服务器：逐句消息',[],[]),
        n('FDeliveryNpcSentence','完整句子及语音',[],[],kind='DTO'),
        n('UDeliveryNpcPresentationComponent','服务器与本机：分段播放',[],[]),
        n('FDeliveryNpcLocalVoice','各客户端：待播句子',[],[],kind='struct'),
        n('FDeliveryNpcSpeechState','实际播放的对白状态',[],[],kind='struct'),
        n('SpeechQueue','Qwen 语音队列',[],[],kind='Python classes'),
        n('QwenSpeechSynthesizer','常驻 NPC 语音',[],[],kind='Python classes'),
        n('FDeliveryNpcVoiceStreamEncoder','句内声音编码',[],[])
    ],[(0,1,'模型文字逐步到达','dependency'),(1,2,'选择 windows 时合成完整句子','dependency'),(2,5,'返回文本、情绪、PCM 与句子序号','dependency'),(5,4,'兼容方式：整句音频经 HTTP 发送','dependency'),(4,3,'在游戏线程检查本次请求','dependency'),(3,6,'通过检查后按序安排句子','dependency'),(6,7,'后台压缩 Opus，客户端收到后解码','dependency'),(7,8,'开始播放时更新本机字幕与情绪','dependency'),(1,9,'Qwen 句子按玩家优先顺序排队','dependency'),(9,10,'前一句结束或确认取消后调用','dependency'),(10,5,'整句模式返回完整 PCM，过长报错','dependency'),(6,11,'启用分段时，同一句持续编码','dependency'),(10,4,'启用分段时，经后端 HTTP 发送台词、音频与句末','dependency')])
    d['combat']=class_diagram('战斗与拽车类图 · 当前实现',[
        task,resident,n('UDeliveryNpcCombatComponent','服务器：攻击与倒地状态',['AttackId / HitAt','bDown / bRecovering'],['StartStrike() / CancelStrike()','ReceiveImpact()','SetDeliberateProne()']),
        agent,n('UDeliveryVehicleHoldComponent','车辆组件：独占拽持事务',['Holder / HoldActionId','ExpiresAt / ThrottleSeconds'],['CanHold() / TryHold()','Release() / UpdateThrottle()','GetSpeedScale()']),
        n('ADeliveryMotorbike','服务器与驾驶客户端：车辆移动',['+ ResidentHold','驾驶与移动状态'],['读取 GetSpeedScale()','更新油门与车辆运动'])
    ],[(0,1,'ExecuteAction','dependency'),(1,2,'持有战斗组件','composition'),(0,3,'TryFinishAction','dependency'),(1,4,'请求拽车或松手','dependency'),(5,4,'车辆创建拽车处理组件','composition')])
    d['configuration']=class_diagram('人物与声音配置类图 · 当前实现',[
        n('FDeliveryNpcProfileRow','正式人物表行',[],[],kind='DataTable row'),
        n('ADeliveryNpcCharacter','人物配置应用',[],[]),
        n('UDeliveryNpcVoiceSet','台词音量与短音配置',[],[],kind='DataAsset'),
        agent,
        n('UDeliveryNpcPresentationComponent','短反应音与完整台词',[],[]),
        n('UDeliveryNpcExpressionSet','现有表情与交谈姿态配置',[],[],kind='DataAsset')
    ],[(1,0,'通过 Profile 选择数据表和行','dependency'),(0,2,'VoiceSet 字段引用声音配置','association'),(1,3,'ApplyProfile 更新人物性格','dependency'),(1,4,'ApplyProfile 更新两类声音配置','dependency'),(4,2,'读取台词音量与衰减，按事件选择短音','dependency'),(4,5,'读取表情与交谈姿态，保持原配置','dependency')])
    d['conversation-sequence']=sequence('玩家交谈时序图 · 当前代码，效果待本轮检查',[
        '玩家端|Widget','PC 组件|Server RPC','NPC Agent|服务器','Gateway|每个 World','AI 后端|模型与语音','BT / 表现|各端'
    ],[
        (0,1,'Send / Enter：提交确认后的文字','call'),(1,2,'检查距离与遮挡 → HearSpeech → ObserveEvent','call'),
        (2,5,'设置倾听状态，BT 更新朝向','call'),(2,4,'玩家请求优先；未执行的闲聊往后排','note'),
        (2,3,'RequestDecision / Enqueue','call'),(3,4,'相关事件、最新玩家发言和可用动作','call'),
        (4,3,'第一条完整结果中的动作建议','return'),(3,2,'检查请求与动作条件','return'),
        (5,2,'StartAction：开始前再次检查','call'),(2,5,'执行通过检查的动作','return'),
        (4,4,'首句进入语音队列，后续文字继续生成','call'),(4,3,'陆续返回短句、语音与句子序号','return'),
        (3,2,'检查句子是否属于这次回复','return'),(2,5,'按序同步台词与音频','call'),
        (5,0,'客户端按实际播放更新字幕和情绪','return'),
        (2,5,'受击、目标失效或世界结束：停止相关请求、动作和语音','note')])
    d['hold-sequence']=sequence('碰撞与拽车时序图 · 当前实现',[
        '玩家 / 车辆','居民 / Combat','NPC Agent','Gateway / 模型','BT 执行节点','VehicleHold'
    ],[
        (0,1,'碰撞 → ReceiveImpactFrom','call'),(1,2,'记录受击事件并取消当前计划','call'),
        (1,5,'立即停止对白 / 处理倒地；模型不参与碰撞结算','note'),(1,2,'恢复后允许重新决策','call'),
        (2,3,'RequestDecision：已知对象与合法候选','call'),(3,2,'HoldVehicle 建议（条件允许时）','return'),
        (4,2,'StartAction：检查当前目标与动作条件','call'),(4,1,'ExecuteAction','call'),
        (1,5,'TryHold：校验占用、位置、车速和驾驶者','call'),(5,0,'复制 Holder；车辆读取 GetSpeedScale','return'),
        (0,5,'UpdateThrottle：持续油门挣脱','call'),(1,5,'Release：受击 / 结束 / 超时等路径','call'),
        (5,0,'清除占用并恢复速度倍率','return'),(4,2,'TryFinishAction：记录动作结束；对白另行记录','return'),
        (1,5,'当前未实现单独预约车辆或双手精确贴合车身','note')])
    # 跟班图单独描述计划中的扩展，避免把现有类画成已经支持新玩法
    def companion_node(name, title, fields, methods, kind='类', status='计划复用'):
        return dict(name=name, title=title, role=title, fields=fields, methods=methods,
                    status=status, kind=kind)
    d['companion']=class_diagram('二狗类图 · 后续设计，尚未实现',[
        companion_node('UDeliveryConversationComponent', '玩家交代任务',
                       ['沿用玩家控制器上的交谈入口', '跑腿指令需检查是否来自主人'],
                       [('Submit() / ServerSubmit()', '发送原话，服务器检查后交给 NPC')]),
        companion_node('ADeliveryCompanionCharacter', '二狗角色',
                       ['继承 ADeliveryNpcCharacter', '沿用 NPC 的战斗、声音和表情'],
                       [('拟重写 CanExecute() / ExecuteAction()', '检查并执行跟随、等候和跑腿动作'),
                        ('拟重写 StopAction()', '停下已经取消的动作')], status='计划增加'),
        companion_node('UDeliveryCompanionComponent', '主人关系与跑腿记录',
                       ['主人 PlayerState、亲密度、跟随状态', '当前跑腿与手上的快递由服务器保存'],
                       [('计划提供归属与跑腿接口', '具体函数在实现时确定'),
                        ('复制主人和当前状态', '上下车仍属于同一位主人')], status='计划增加'),
        companion_node('UDeliveryNpcAgentComponent', '模型上下文与动作记录',
                       ['记录 NPC 听见的话和现场事件', '读取跟班组件提供的任务与关系'],
                       [('RequestDecision()', '请模型从当前可用动作中选择'),
                        ('TryFinishAction()', '记录游戏确认的动作结果')]),
        companion_node('UDeliveryAiGatewaySubsystem', '请求模型判断',
                       ['发送相关事件、关系和可用动作', '复用已有后端和取消处理'],
                       [('Enqueue() / Cancel()', '安排请求或取消旧请求')]),
        companion_node('BT_Companion', '二狗行为树蓝图',
                       ['从 BT_Resident 扩展跟随与跑腿', '倒地、取消和目标变化时停止动作'],
                       [('复用 BTTask_DeliveryExecuteNpcAction', '调用角色接口执行模型选定的动作'),
                        ('新增跟随与跑腿分支', '蓝图节点与参数在实现时配置')], '蓝图资产', '计划增加')
    ],[(0,1,'通过检查的玩家原话交给 NPC','dependency'),
       (1,2,'角色持有跟班组件，由组件保存主人与跑腿状态','composition'),
       (1,3,'角色沿用 NPC Agent 处理现场信息与模型建议','composition'),
       (2,3,'将当前任务和主人关系提供给模型上下文','dependency'),
       (3,4,'请求模型判断，接收台词和动作建议','dependency'),
       (3,5,'将游戏检查通过的动作交给行为树','dependency'),
       (5,1,'行为树调用角色的动作接口，结果交回 Agent','dependency')])
    return d
