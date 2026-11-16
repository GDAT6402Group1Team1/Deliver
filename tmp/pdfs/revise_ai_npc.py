from pathlib import Path
from PIL import Image

root=Path('D:/Udev/Delivery/Document/Interview/AI-NPC文档')
def edit(name, changes):
    p=root/name
    s=p.read_text(encoding='utf-8')
    for a,b in changes:
        if a not in s: raise ValueError((name,a[:80]))
        s=s.replace(a,b)
    p.write_text(s,encoding='utf-8')

# 截图仅裁去空白，保留原始文件，另存排版用 PNG
for name,box in [('01-companion-tuning',None),('02-npc-profiles',(0,0,1373,225)),('03-companion-blueprint',(0,0,1300,820)),('04-behavior-catalog',(0,0,1373,582))]:
    im=Image.open(root/'images'/f'{name}.png').convert('RGB')
    if box: im=im.crop(box)
    im.save(root/'images'/f'{name}-print.png')

edit('公共导言区.tex',[(r'\usepackage{xcolor}',r'\usepackage{xcolor}'+'\n'+r'\usepackage{graphicx}')])
edit('01-AI-NPC框架.tex',[
('Unreal Engine 5 开发','Unreal Engine 5.8 开发'),
('三层并行运行。本能层和规则层不发出模型请求。模型建议和服务器规则产生的动作由同一套行为树任务和 GAS 技能执行，动画、命中判定和清理流程相同。','三层可同时工作，但不同时争夺角色的动作控制。本能层和规则层不发出模型请求；获批的玩法动作复用行为树与 GAS，即时注视由表现组件处理，沿路避让和越障由移动执行代码处理。'),
('本能层只播放表现动作，不改变已安排的动作，也不发出模型请求。NPC 处于交谈、追打或搬运状态时，本能层的反应不触发。','即时注视与短反应主要负责表现；避让和越障会实际移动角色，用于完成已有安排。各入口有各自的条件，不能概括为“交谈、追打或搬运时全部禁用”。例如越障会检查倒地、受击、出拳、骑乘及家具搬运状态，并检查落点和整段跳跃路径。'),
('战斗 & 是否攻击、是否追击由模型决定。 & 主人被命中时，服务器规则触发协助战斗。主人要求停止后，二狗停止协助。', '战斗 & 通常由模型选择；被玩家或二狗真实打中后，可按服务器规则立即还手，居民互殴仍由模型决定。 & 主人真实打中对手或被打中时，可直接加入协战；主人要求收手后保留停止约束。'),
('动画不影响位移和命中判定','命中与位移由服务器校验'),
('发出请求时保存候选并建立编号索引。回复到达后，按请求、候选、动作、目标、证据逐项核对，单次查询平均为 $O(1)$。','候选编号通过哈希索引查找，平均为 $O(1)$；随后仍需核对动作、目标、证据与实时执行条件，完整校验不能概括为 $O(1)$。'),
('大模型处理全部类型的请求，响应时间为秒级。本地小模型只处理追打中的判断，响应时间约 0.1–0.2 秒。框架按请求类型分流。','大模型处理全部类型的请求，响应时间为秒级。本地小模型只处理符合分流条件的追打判断。约 0.1–0.2 秒是模型独立运行时的历史结果，不能作为与游戏、TTS 共用显卡时的响应承诺；部署对照与未达标项见第 3 篇。'),
('⑧ 服务器把动作、字幕和音频复制到所有客户端。','⑧ 服务器通过动作状态复制及表现组件的语音传输通知客户端；各客户端按收到的状态、音频和本机播放进度显示。行为树本身不负责音频编码和网络传输。'),
('主人被命中时触发协助战斗','主人命中或受击时可触发协战'),
('蓝色和橙色分类适用于所有居民','蓝色和橙色分类适用于所有居民') if False else ('本文介绍框架的组成，以及一次交谈从输入到表现的处理流程。','本文介绍框架的组成，以及一次交谈从输入到表现的处理流程。核对日期：2026 年 11 月 16 日，源码基线为 \\code{87ef036}。文中历史实验数字保留各自条件，编辑器截图用于证明资产配置，不代表重新完成了实机验收。')
])

append1=r'''
\clearpage
\section{工程实拍与最新职责补充}
\subsection{角色蓝图如何组装现有能力}
\begin{figure}[H]
\centering
\includegraphics[width=\textwidth]{images/03-companion-blueprint-print.png}
\caption{二狗角色蓝图：组件与包袋挂接节点（编辑器实拍，裁去右侧空白）}
\end{figure}
\explain{资产：\path{/Game/NPC/Companion/BP_Companion_Ergou}。左侧能看到 Ability System、Agent、Combat、Companion、Conversation、HelpUp、Look 和 Presentation；事件图的三个 Attach Component To Component 节点用于包袋挂接。图中存在蓝图节点，不意味着服务器玩法规则也写在这里。}

沿一次“把抱枕放到桌上”的操作，模型从真实物件与目的地候选中选择；服务器核对主人、房屋解锁、物件与槽位；行为树推进当前安排，同伴组件处理接近、拿取、搬运与摆放；动画蓝图按执行阶段播放已导入动画。模型说“放好了”不能作为完成依据，实物交接和摆放结果仍由游戏确认。

\begin{note}[明确保留的规则降级]
11 月 15 日试玩反馈后，居民的 \code{RequestRetaliation} 允许对真实打中自己的玩家或二狗立即还手，开打后再请模型接话；它不会让两个普通居民无条件相互还击。二狗的 \code{bJoinAssistWithoutModel} 默认开启，主人真实命中或受击后可直接协战。停止约束、目标合法性、请求版本与实际命中证据仍须核对。这样处理源于战斗等待过长和选错目标的试玩问题，不能把这部分反应计作 LLM 的判断质量。
\end{note}

\noindent\textbf{实现定位：}角色组装见 \path{Source/Delivery/AI/Companion/DeliveryCompanionCharacter.cpp}；家居执行见同目录的 \path{DeliveryCompanionHomeExecution.cpp}；协战见 \path{DeliveryCompanionAssist.cpp}；居民还手见 \path{Source/Delivery/AI/Decision/DeliveryNpcAgentComponent.cpp} 的 \code{RequestRetaliation}。设计与验收范围见 \path{Document/Architecture.md} 二狗章节及 \path{Document/AIIteration.html}。
'''
edit('01-AI-NPC框架.tex',[(r'\end{document}',append1+'\n'+r'\end{document}')])

edit('02-NPC的AI信息.tex',[
('蓝色和橙色分类适用于所有居民，绿色分类只用于跟班。','蓝色入口主要为共享行为；橙色战斗分类中的 JoinAssist、StopAssist 仍是同伴专用能力，不能因为同属战斗分类就提供给普通居民。绿色入口用于跟班。'),
('模型从候选中选择一项，不能修改其中的字段。','模型从候选中选择一项；动作与目标须匹配该候选，时长和情绪等输出仍按协议范围校验，不能自行扩展动作权限。'),
('目标存在、未倒地、可到达','目标有效、符合该动作条件'),
('修改人物和关系的 JSON 文件后，重启 AI 后端即可生效，不需要重新编译游戏。两个人物之间没有配置关系时，请求中不包含关系条目，模型只依据本局的事件记录。','后端的背景、动作示例、战斗偏好与关系读取有缓存，修改后重启后端使其重新读取。UE 中的 Persona、声音与动画引用来自人物表，并由角色 ApplyProfile 应用；只重启后端不会同步修改 UE 数据资产。人物性格变化需要核对两端配置与实际请求。没有配置人物关系时，请求不附加该关系，仍可使用已配置的背景和本局事件。')
])
append2=r'''
\clearpage
\section{可编辑资产实拍与配置流向}
以下画面于 2026 年 11 月 16 日从本机 UE 5.8 编辑器采集。原始截图保存在同目录 \path{images/}；排版图只裁去空白。截图证明可编辑配置存在，不证明每个动作已经通过游戏和多人验收。

\subsection{人物表：身份、声音与动作表现}
\begin{figure}[H]
\centering
\includegraphics[width=\textwidth]{images/02-npc-profiles-print.png}
\caption{居民人物表：大壮与翠花的两条配置行}
\end{figure}
\explain{资产：\path{/Game/NPC/Residents/Config/DT_NpcProfiles}。当前行名仍为 chunlai、xiumei，显示名为大壮、翠花；内部 ID 与显示名分开使用。可见列包括人物性格、TTS 音色、声音配置和 Behavior Presentation。}

角色在 BeginPlay 中调用 \code{ApplyProfile}，读取 \code{FDeliveryNpcProfileRow}，将 Persona 交给 Agent，将声音配置交给 Presentation，并保存按 Gameplay Tag 配置的动画表现。后端则按请求的 \code{profile_id} 从 JSON 取背景和有向关系。姓名变化不应让缓存、人物关系或资产引用串到另一个人。

\begin{table}[H]\small
\begin{tabularx}{\textwidth}{@{}l L@{}}
\toprule \textbf{入口} & \textbf{主要用途及更新边界} \\
\midrule
UE 人物表 & Persona、显示名、音色路由、短反应音和动画引用；修改后保存资产，并重新初始化角色检查实际应用值。 \\
人物与关系 JSON & 作者背景、说话与动作示例、战斗倾向、有向关系；后端启动后读取并缓存，配置不等于当局事实。 \\
调参 Data Asset & 初遇、跟随、关系增减、跑腿等参数；当前关系、主人和委托由运行时组件维护。 \\
行为目录表 & 行为名称、Tag、说明和可提供范围；不包含一次请求的全部实时执行权限。 \\
\bottomrule
\end{tabularx}
\caption{配置与运行状态的区别}
\end{table}
\noindent\textbf{实现定位：}\path{Source/Delivery/AI/Characters/DeliveryNpcProfile.h}、\path{DeliveryNpcCharacter.cpp} 的 \code{ApplyProfile}，以及 \path{Tools/DeliveryAI/dialogue_background.py}、\path{npc_relationships.py}。人物资产同步工具在 \path{Content/Python/update_npc_personas.py}；二狗人物表另见 \path{/Game/NPC/Companion/DT_CompanionProfiles}。

\clearpage
\subsection{二狗调参：配置不保存当局状态}
\begin{figure}[H]
\centering
\includegraphics[width=.86\textwidth,height=15cm,keepaspectratio]{images/01-companion-tuning-print.png}
\caption{DA\_CompanionTuning：初遇、跟随与关系参数}
\end{figure}
\explain{资产：\path{/Game/NPC/Companion/DA_CompanionTuning}。截图可见初遇范围 900 cm、扶起长按 1.0 s、恢复时间 1.6 s、跟随后距 190 cm、侧距 95 cm、导航更新间隔 0.35 s，以及初始关系值 70。它们是此次打开资产时的配置值。}

该 DA 由 \code{UDeliveryCompanionTuning} 定义。例如 \code{HelpUpAffinity=5} 表示成功辅助站立后的关系增量；按下 E 或开始长按并不等于关系已经增加。主人 PlayerState、当前关系、手持实物和委托阶段保存在同伴组件中，不能回写为全体角色共用的资产状态。服务器负责检查完成条件和修改状态。

\clearpage
\subsection{共享行为表：目录如何变成本次候选}
\begin{figure}[H]
\centering
\includegraphics[width=\textwidth]{images/04-behavior-catalog-print.png}
\caption{DT\_NpcBehaviorCatalog：部分行为定义与筛选字段}
\end{figure}
\explain{资产：\path{/Game/NPC/Shared/AI/DT_NpcBehaviorCatalog}。图中展示前 21 行，可见 Gameplay Tag、Action、Display Name、Category、Description、Offer to Model 与 Player Request Only；完整目录还包含下方未显示的入口。}

表中的 \code{Offer to Model} 表示该定义是否允许进入模型候选生成流程，不能解释为“这一动作在当前现场一定可用”。服务器还须按人物种类、请求来源、倒地状态、真实物件、主人授权和目标条件筛选。一次请求封存候选编号；模型返回后重新检查，执行到交接点时还要复核物件与目的地。

\begin{note}[如何增加一个动作]
先确认能复用的服务器接口或 GAS 技能，再登记 Tag 与行为定义，补充对应的候选生成、输出校验、可取消执行和结果事件，最后配置动画与声音。只增加一行表格或一段提示词，不能让模型获得尚未实现的游戏能力。JSON 目录与 UE 数据表也需要同步核对，不能假定任意文件变更都会自动热更新资产。
\end{note}
'''
edit('02-NPC的AI信息.tex',[(r'\end{document}',append2+'\n'+r'\end{document}')])

edit('03-测试与迭代.tex',[
('首块声音','后端首块音频'),
('测量对象：一次玩家对 NPC 的发言。结果：首段文字 1.98 秒，首句完整 2.80 秒，动作批准 3.15 秒，后端首块音频 5.22 秒；排队时间为零。这是单次记录。','测量对象：11 月 14 日 16:07 的一次翠花交谈。首段文字约 1.98 秒、首句完整约 2.80 秒为模型侧阶段记录；动作批准 3.15 秒从 UE 发出请求起算；后端首块音频 5.22 秒从后端收到请求起算。请求调度未排队，但语音队列等待约 1.28 秒。此图只作阶段示意，不能据相邻刻度相减计算跨进程耗时，也不是玩家听到声音的端到端延迟。'),
('一次玩家交谈的阶段计时（单位：秒，单次试玩记录）','一次交谈的阶段示意（秒；不同计时起点，非统一时间轴）'),
('自动化通过/{原有规则没有被破坏}','自动化通过/{所覆盖的规则断言通过}')
])
append3=r'''
\clearpage
\section{本次文档核对：证据与尚未验证范围}
\textbf{核对日期：2026 年 11 月 16 日；源码基线：\code{87ef036}。} 本次工作核对了聊天中的设计说明、主文档、相关源码与编辑器资产，没有重新运行游戏构建、UE 自动化、真实模型性能对照或双人 PIE。下表是历史记录的定位入口，不是本次新增测试成绩。

\begin{table}[H]\small
\begin{tabularx}{\textwidth}{@{}l L L@{}}
\toprule \textbf{证据} & \textbf{能说明什么} & \textbf{不能说明什么} \\
\midrule
编辑器资产实拍 & 调参 DA、人物表、共享行为表和角色蓝图确实存在，字段可在编辑器中编辑。 & 游戏每个入口都成功、完整流程稳定或多人结果一致。 \\
11 月 15 日构建日志 & DeliveryEditor 历史构建 Result: Succeeded，耗时 55.61 秒。 & 本次重新构建通过；该批所有玩法已经验收。 \\
同批后端检查 & home context 与 companion travel 定向检查共 67 项通过。 & 替代文中 412 项历史全量检查，或覆盖真实 LLM 与 TTS。 \\
用户试玩反馈 & 已记录原包裹交付、战斗等待和家具操作问题，并据此迭代。 & 自动推导其他地图、客户端或所有家具都正常。 \\
\bottomrule
\end{tabularx}
\caption{本次核对的证据强度}
\end{table}

\noindent\textbf{历史日志：}\path{Saved/Logs/ClaudeHandoffBuild-20261115.log}；\path{Saved/Logs/ClaudeHandoffBackendTests-20261115.log}。412 项记录另见 \path{Saved/Logs/NpcLocalPlaytestFinalTests-20261115.log}，两者范围不同，不相加计算覆盖率。

\subsection{从试玩反馈解释规则降级}
用户在试玩中提出战斗等待过长、二狗没有及时帮忙。当前代码允许真实命中后由服务器直接启动部分还手和协战，然后继续让模型决定台词和后续变化。这应记录为响应策略的取舍：保留命中证据、权限和取消约束，但降低了开打这一刻由模型独立决定的比例。评估时须分别统计规则触发与模型选择，不能将即时还手写成小模型延迟改善的结果。

\subsection{下一次有效验收应记录什么}
对相同版本记录玩家实际操作、次数、画面与两端结果：E 扶起并确认站立后加关系；T 请求搬物并确认真实物件到位；主人上车、下车后验证跟随归属；攻击命中后观察协战，再要求收手并确认停止；取消、目标销毁和结束 PIE 后检查旧语音及旧动作没有恢复。家具功能已接入与“坐下、起身、放稳、声音和多人表现已验收”应分开记录。

测量等待时，至少区分发言结束、UE 提交、后端收到、模型首字、首句、动作获批、音频到达和客户端实际播放。记录样本数、中位数、P95、GPU 共用负载与帧时间；独立模型的 0.12 秒、语音首块合成时间、完整游戏的等待感不能互相替代。

\noindent\textbf{回查入口：}\path{Document/AIIteration.html} 的 NPC 决策等待分析、二狗试玩修复和小模型部署对照；\path{Document/Architecture.md} 的二狗家居职责。另核对了“实施牛二狗长期陪伴角色”“梳理 AI NPC 决策技术”聊天中关于编译结果、家居执行分工及未完成验收的记录。
'''
edit('03-测试与迭代.tex',[(r'\end{document}',append3+'\n'+r'\end{document}')])
print('Updated three documents and four print screenshots')
