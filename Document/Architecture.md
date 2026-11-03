# Delivery 系统设计

Delivery 按玩法职责组织代码，一个系统可以包含组件、场景对象、接口和数据资产
目录表示功能归属，运行时的持有对象决定状态由谁管理，两者不必完全一致

## 系统与入口

| 目录 | 系统 | 阅读入口 | 职责 |
| --- | --- | --- | --- |
| Character | 角色 | DeliveryCharacter | 创建身体、镜头和功能组件，保留技能及蓝图入口 |
| Player | 玩家 | DeliveryPlayerController / DeliverPlayerState | 输入与控制权属于控制器，技能、钱包和追踪属于玩家状态 |
| Interaction | 交互 | DeliveryInteractionComponent | 管理长按、取消和服务器校验，调用目标提供的行为 |
| Carry | 携带 | DeliveryCarryComponent | 统一查询和接收快递，协调背包与车辆货架 |
| Inventory | 背包 | DeliveryInventoryComponent | 管理槽位、物品归属和手持显示 |
| Combat | 战斗 | DeliveryRagdollCombatComponent | 计算攻击方向，安排出拳与查询命中 |
| Ragdoll | 身体 | DeliveryActiveRagdollComponent | 处理站立、移动、倒地和物理同步 |
| Grab | 抓取 | DeliveryGrabComponent | 建立抓取约束，处理托举、拖拽和释放 |
| Task | 任务 | DeliveryTaskManagerComponent | 保存共享任务进度，管理接单、取件和交付 |
| Phone | 电话 | DeliveryPhoneCallQueueComponent | 安排来电顺序和通话阶段 |
| World | 地点 | DeliveryLocationRegistry | 登记当前世界中的地点并提供查询 |
| Vehicle | 车辆 | DeliveryMotorbike | 驾驶、骑手表现和车辆货架 |
| Traffic | 交通 | DeliveryTrafficCarComponent | 控制场景车辆沿路线行驶 |
| AbilitySystem | 技能 | GA_DeliverPunch / DeliverAbilitySystemComponent | 技能生命周期、属性与伤害效果 |
| UI/Controllers | 界面数据 | DeliveryTaskController / DeliveryHotbarController / DeliveryInteractionController | 提供任务文字、背包槽位和交互提示 |
| Audio | 音频 | DeliveryMusicComponent / DeliveryDialogueVoiceComponent | 背景音乐和程序语音 |
| Game | 游戏配置 | DeliveryGameMode / DeliveryGameState | 配置默认对象并持有共享系统 |

## 运行时归属

Character 创建身体、战斗、抓取、交互、携带和背包组件，保留蓝图使用的原函数作为转发入口
PlayerController 接收玩家操作并提供服务器交互入口，上车后仍然通过玩家自己的连接提交请求
PlayerState 保存技能系统、钱包和个人任务追踪，不能把这些状态搬进可被替换的角色
GameState 持有任务管理器和电话队列，服务器修改共享状态并复制给客户端
地点注册表使用 WorldSubsystem，只管理本世界的地点引用，不代替任务状态复制

## 组件之间怎样协作

有返回结果的操作使用公开方法，例如携带组件的 TryReceivePackage
界面与其他观察者使用事件或只读显示数据，不能直接修改系统内部字段
交互目标通过 CanUse 与 Use 委托提供自身行为，交互执行组件不再判断目标是取货点还是交付点
委托使用 BindUObject 绑定目标对象，对象销毁后不会继续调用失效对象

## 一次取件

角色收到输入后调用交互组件，组件向探测器取得目标，并检查距离、角色状态和目标条件
普通物品立即请求执行，配置了长按的目标先记录本机时间并显示进度
服务器单独记录开始时间，完成时重新检查目标、距离和耗时，随后清除记录再调用目标行为
取货点负责生成快递，携带组件根据当前驾驶状态选择背包或货架，任务管理器更新任务状态
骑车时控制器控制车辆，因此请求经 PlayerController 转交，服务器确认车辆驾驶员身份后才允许执行

## 一次交付

交付点查询携带组件取得当前快递，检查任务身份和交付距离
任务管理器确认完成后发出结算事件，钱包按原流程处理奖励
携带组件解除快递与背包或货架的关系，交付点销毁快递，防止残留引用阻止领取下一单
重复提交无法再次完成同一任务，交互完成记录也只消费一次

## 一次攻击

技能通过角色的战斗接口发动攻击，角色转交给战斗组件
战斗组件使用身体朝向计算拳路，安排释放与命中窗口，命中查询对同一角色去重
技能仍负责伤害规则，身体组件仍负责物理控制，没有调整冲量、计时和物理同步参数

## 界面

任务控制器选择来电、结算或当前任务内容，原任务 HUD 保留参数与显示入口
快捷栏控制器提供图标、耐久和满包反馈，控件保留现有布局和实时刷新
交互控制器提供提示文字与长按进度，目标选择和服务器规则仍属于交互系统
这次分离数据与显示，不将持续变化的倒计时、耐久和闪烁强行改成只有事件才刷新
任务界面在钱包变化时解绑旧订阅，快捷栏重新绑定时直接读取新背包

## 配置与兼容

原生类名、模块名、现有组件名称和蓝图资源路径不变，移动的是源码文件位置
Character 上已有的输入、技能、镜头和音效配置继续有效，旧函数转交给对应组件
BP_DeliveryMan 配置角色组件，BP_DeliveryManPC 配置输入与本机表现，GameState 蓝图配置任务
编辑器调参窗口仍读取原布娃娃和战斗组件，不创建第二份参数

## 输入与快捷栏

PlayerController 管理输入映射和手机的输入模式，角色的 PlayerInput 组件负责步行按键、鼠标组合键和快捷栏操作
角色的 SetupPlayerInputComponent 只调用 PlayerInput.Bind，不再保存鼠标按下状态，也不再创建快捷栏
车辆继续使用自己的驾驶输入，人物输入随人物退出输入栈，避免两个对象同时处理移动和交互

左右键首次按下后等待 0.2 秒，期间另一键按下则请求抓取，否则按首次按下的键请求出拳
失去控制权时清掉这个等待计时器，并取消移动、抓取、长按拾取和当前出拳，取消不会补发短按攻击
DoMove、DoAttackLeft、DoInteract 等角色入口保留，已有蓝图和车辆仍可调用

DeliveryHUD 由 GameMode 配置，仅在所属玩家本机创建快捷栏
HUD 每 0.1 秒检查当前控制对象，步行时取角色，骑车时取车辆驾驶员
控制对象或驾驶员尚未同步时隐藏快捷栏，准备好后恢复；同一角色上下车复用控件，更换角色时重新绑定
HotbarController 继续整理背包显示数据，HotbarWidget 负责显示，背包组件负责切换物品和网络校验
当前快捷栏仍通过控件更新读取显示数据，这次没有改成事件驱动，也没有重做界面布局

输入参数在角色蓝图的 PlayerInput 组件中编辑，快捷栏类在 DeliveryHUD 或其蓝图子类中配置
角色不再保存重复的输入和界面字段
已核对现有角色蓝图与两张地图，输入与快捷栏配置均与组件和 HUD 一致，不再需要运行时导入旧值

阅读文件：Input/DeliveryInputComponent.h 与 .cpp、UI/DeliveryHUD.h 与 .cpp、Character/DeliveryCharacter.cpp、UI/Controllers/DeliveryHotbarController.cpp

## 多人物理移动

本机收到移动输入后直接更新布娃娃组件，同时把输入发送给服务器
服务器在物理计算完成后采集全部刚体的位置、旋转和速度，腿部也包含在快照中
其他玩家的身体由相邻快照插值显示，本机则继续计算自己的步态

本机与服务器只有小幅位置差时，通过有上限的速度增量逐步追上，不逐包传送身体
偏差超过强制校正距离时才整体平移，同时移动迈步起点、落点和控制目标
校正保留脚步进度与相对速度，避免身体已经移动、脚却继续追旧目标
实现位于 Ragdoll/Network/DeliveryActiveRagdollNetwork.cpp

双人运行帧率降低时，步态使用物理模拟允许的步长，避免脚还没落地就判定超时
每帧先更新网格，再计算布娃娃目标，最后由 PhysicsControl 执行

停步后保留身体的水平支撑位置，平地与缓坡使用同一套处理
身体轻微偏移时，控制目标不会跟着移动，减少滑步和反复补脚
重新移动、起跳、受击或明显失衡时解除支撑，身体仍由物理模拟驱动
本机接受网络校正时，站立目标也逐渐对齐服务器，避免两套修正互相抵消

## 出拳命中窗口

释放拳头时开启检测，攻击结束或取消时关闭
服务器在每次物理更新后，沿拳头上一帧到本帧的位置做球形扫掠
GA_DeliverPunch 记录本拳已经命中的目标，同一角色只结算一次伤害和受击冲量
命中不再由 HitWindowDelay 的单次定时器决定

低帧率时物理子步上限为 24，保持原有每步 0.008333 秒的精度，允许 10 FPS 下推进完整的帧时间
详细身体诊断默认关闭，用 Delivery.Physics.Trace 1 按需开启

验证补充：新增 Delivery.Boxing.LowFrameRate，在 60 FPS 与 10 FPS 下检查真实物理位移、技能命中与晕倒，并检查同一拳重复接触和取消后的检测关闭。测试世界销毁前先调用 EndPlay，释放 Water 等系统的物理回调，避免跨测试回收时访问已销毁的物理场景。

本轮验证：DeliveryEditor 完整编译通过，22 项自动化全部通过（17 项无警告、5 项带警告）。同机双人 PIE 原地图的前进、右移、反向移动平均速度约为 57、58、60 cm/s，修复前约为 47、36、45 cm/s；停步采样中脚部最大单轴变化约 0.02 cm。该结果证明本次改动有改善，不表示低帧率步态与单人高帧率完全一致。

兼容字段清理：删除角色的 10 个旧输入字段和旧快捷栏字段、输入反射迁移、HUD 旧值回退；删除无效的 HitWindowDelay、RiderSteerRatio、RiderNeckSteerRatio、RiderNeckBone、HighlightActors 及无调用的高亮查询。保留仍在使用的骑手姿态更新与小地图旧摄像机停用逻辑。

本次清理验证：完整编译通过，主角与摩托车蓝图编译通过，两张地图加载通过；清理前后输入动作、0.75 鼠标灵敏度、R 召车键与快捷栏类保持一致。22 项自动化通过，其中 5 项带警告，没有失败。


## 交互登记与操作命名

DeliveryInteractionRegistry 是世界子系统，每个世界分别保存交互组件的弱引用
组件在 BeginPlay 登记，在 EndPlay 注销；查询时清理失效引用，世界结束时清空列表
玩家探测器向本世界注册表取候选目标，注册表拒绝其他世界的组件和查询者
范围检查与最近目标排序统一使用 GetInteractionDistance，因此门口锚点与碰撞表面都会参与距离判断

公共操作条件使用 CanUse，执行委托使用 Use；InteractionComponent 用 BeginUse、CancelUse 和 ExecuteUse 管理过程
物品真正进入背包的函数仍叫 Pickup，取货点领取包裹也保留拾取命名
收件点的交互回调改为 CanDeliver 和 Deliver，内部仍使用原有任务检查与 TryDeliver 完成交付
蓝图已有的 E 键输入、角色入口和任务配置保持原有资产引用，运行时执行转交给新的公共操作方法

这次检查同时核对地点注册表、任务与电话状态、携带和背包接口
地点已经归 WorldSubsystem，共享任务和电话状态已经归 GameState，不新增重复管理器
本次修正集中在交互组件的静态运行时列表、公共操作命名、收件点命名与交互距离排序

验证：完整编译通过，23 项自动化通过（5 项带警告、无失败），其中 WorldRegistry 检查双世界隔离和登记生命周期；主角与摩托车蓝图编译、两张地图加载通过。UE MCP 已完成原地图双人 PIE 启动与退出检查，此检查不替代所有玩法的手动验收。

布娃娃状态重构：保留单个组件与原物理前后 Tick 顺序，内部使用 Movement、Punch、Recovery、Network 管理步态、出拳、恢复进度和快照。网络位移经 Movement.Translate/CorrectStandAnchor 调整步态缓存，不在网络代码中逐个写脚步位置。身体模式、复制字段和物理应用仍由主组件负责；诊断移到 Debug。详细流程见 Document/RagdollUpdate.md。

本轮验证：127 项 UE 属性声明保持一致；完整编译与 23 项自动化通过（5 项带警告、无失败）。低帧率出拳、收拳目标、步态和网络校正的诊断结果与重构前一致；网络校正测试新增起身交接和快照序号回绕检查。


## 玩家镜头组件

`Camera/DeliveryCameraComponent` 管理本机撞击反馈，`Camera/DeliveryCameraConfig` 提供基础、跟随、撞击三组参数，玩家配置为 `/Game/Blueprint/Camera/DA_PlayerCamera`。Character 继续创建原 CameraBoom / FollowCamera 并保留网络通知和视角访问入口，镜头逐帧更新及状态已移入组件。配置共享只读，反馈状态每个组件独立保存。连续撞击不累计距离，恢复结束或失去本机控制后关闭更新。摩托车镜头不在本次拆分范围内。配置方法见 [Camera.md](Camera.md)


## 对象事件与游戏音效

`Events/DeliveryEventComponent` 提供对象范围的本地事件，`Audio/DeliveryAudioComponent` 在开始时订阅、结束时解绑，通过 `DeliveryAudioConfig.Sounds` 查找一次性音效。角色取件和交付 RPC 只发布事件；车辆用命名枚举的 `MulticastVehicleEvent` 代替数字音效编号。`DeliveryVehicleAudioComponent` 继承普通音效组件，独立控制原 EngineAudio 的循环播放，车辆只传入驾驶状态和速度比例。配置资源为 `/Game/Sound/Config/DA_PlayerAudio` 与 `DA_VehicleAudio`，原蓝图参数已迁移，地图车辆无单独音效覆盖。背景音乐、电子语音、电话音效保持原流程，未全部纳入新事件。详见 Document/GameplayAudio.md
