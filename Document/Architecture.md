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

输入与操作提示的职责、通知来源和按键配置见 [InputHints.md](InputHints.md)

手机处理器、HUD 与 PC 的职责见 [PhoneMVC.md](PhoneMVC.md)

## 开始界面与后台加载入口

`/Game/Level/L_MainMenu` 是轻量持久关卡，原 `TestForCharacter` 以 Blueprint Streaming 子关卡加入，位置与原地图相同。两张地图都沿用正式 `BP_DeliverGameMode`，子关卡自己的 GameMode 不参与切换。原地图保持完整，没有拆 Actor 或转换 World Partition。`Content/Python/setup_startup_map.py` 在入口不存在时创建，再次执行只检查和补齐入口持久层的 StartupFlow，不覆盖已经配置的菜单，也不保存原地图。添加子关卡后必须切回入口持久层再生成控制器，避免控制器被放进尚未加载的游戏子关卡

### UI 接线

1. 创建 Widget Blueprint，父类选 `DeliveryStartupWidget`，布局由美术和蓝图完成
2. 打开 `L_MainMenu`，选中 `StartupFlow`，把该 Widget 填到 `Menu Widget Class`
3. Start 按钮调用继承的 `Request Start Game`；返回 true 表示请求已接收，不代表加载已经结束。不要再调用 Open Level 或自行生成角色
4. 实现 `On Startup State Changed`，根据 Phase 切换按钮、Loading 提示或错误文本。初次创建自动同步当前状态，无须 Tick 轮询；StartRequested 为 true 时可以禁用重复点击
5. Retry 按钮调用 `Retry Loading`。Playing 时 HUD 自动移除菜单并恢复游戏输入，不在 Widget 里直接写鼠标开关
6. 从 `L_MainMenu` 开始 PIE 测试。UI 完成后将 Project Settings → Maps & Modes → Game Default Map 设为 `L_MainMenu`，打包地图列表需要包含入口和游戏地图。当前默认启动地图暂保留原地图，避免菜单尚未制作时进入无按钮的界面

| 查询或事件 | 含义 |
| --- | --- |
| `Get Delivery Startup Flow(WorldContext)` | 取得当前世界的唯一启动服务；直接运行原地图时为空 |
| `GetPhase()` / `OnPhaseChanged` | Loading、Ready、WaitingForPlayers、Starting、Playing、Failed |
| `IsSceneReady()` / `OnSceneReady` | 本端子关卡已加载并可见，找到指定 PlayerStart，向下探测到阻挡 Pawn 的静态地面 |
| `IsStartRequested()` | 主机已点击开始，可能仍在加载或等待其他连接玩家 |
| `HasGameStarted()` / `OnGameStarted` | 服务器已释放开局；本端 Playing 还会等待所属 Pawn 到位 |
| `GetLoadingError()` / `OnLoadingFailed` | 缺少子关卡、加载超时、出生失败等错误说明 |
| `RequestStartGame(Player)` | 仅单人本机或监听主机可请求，重复调用不重置任务、不重复生成角色 |
| `RetryLoading()` | 失败后重新检查和请求加载；客户端不能清除服务器的开局错误 |
| `GetLoadingProgress()` / `OnLoadingProgressChanged(Progress)` | 本机显示用的 0～1 估算进度，未就绪不超过 0.9，真实就绪为 1；Flow 和 Widget 都提供 |

进度条接线：`DeliveryStartupWidget` 子类添加 `Event On Loading Progress Changed`，执行线接 Progress Bar 的 `Set Percent`，把 `Progress` 直接接入 `In Percent`，关闭 `Is Marquee`。事件在 Widget 构建时同步当前值，之后沿 Flow 原有的 0.1 秒检查更新，不需要 Widget Tick 或额外计时器；也可以将 Percent 绑定到 Widget 的 `Get Loading Progress`，两种方式选一种。原 `OnStartupStateChanged` 的签名保持不变，继续处理状态文本与按钮

加载条 PNG 源图在 `Content/UI/Image/T_LoadingTrack.png`（空槽）与 `T_LoadingFill.png`（满格），均为 2172×724 的透明 RGBA 图，匹配开始界面的奶油黄/橙金边框、紫色描边和亮绿色填充。导入为 UI 贴图后分别放入 Progress Bar 的 Background Image / Fill Image，两张 Brush 使用 Image 绘制；Bar Fill Style 选 Mask、方向 Left to Right、Border Padding 设零、Fill Color and Opacity 设纯白，按相同尺寸叠合。图片上下含透明留白，可用外层容器调整布局，不能独立裁剪两张图造成错位。来源与内置图像生成提示词保存在同目录 `LoadingBar.generation.json`；仅生成 PNG，尚未导入或修改用户的 Widget，需在 UMG 中验收实际显示

`StartupFlow.EstimatedLoadSeconds` 默认 20 秒，是估算条从 0 缓动到 90% 的时间，不是加载超时或强制等待。曲线先快后慢，在真实就绪前最多停在 90%；真实就绪立即补到 100%，提前点击开始仍由真实条件释放。失败时停止增长，加载失败后重试从零重新计；场景已就绪但出生失败的重试保留 100%。各端独立估算，不复制主机的进度；同一次加载不倒退，Widget 重建读取已有值，退出时解绑。主线程初始化卡顿时 UMG 也可能暂时无法刷新，这个显示接口不改变场景初始化性能

实现依据：本机 UE 5.8 的 `FAsyncLoadingThread2::GetAsyncLoadPercentage` 直接返回 -1，不能作为当前地图的有效百分比。`FStreamableHandle::GetProgress(Load)` 按该请求已完成资源数/请求总数统计，不代表整张流送地图和碰撞已经可用，且完成计数可能包含加载失败项。当前保留原 Level Streaming 管线，以关卡可见和出生地面检查作为真实完成条件；估算值只用于 UI

估算进度验证（2026-11-10）：关闭编辑器后完整编译 `DeliveryEditor Win64 Development -NoHotReloadFromIDE -MaxParallelActions=2` 通过，`Automation RunTests Delivery.Startup` 四项全部通过，包括新增 `Progress` 的未就绪上限、失败冻结、重试归零、提前开始不能绕过真实门控，以及双人 PIE 中各端就绪时进度为 1。报告在 `Saved/Automation/StartupProgress`，日志在 `Saved/Logs/StartupProgressTests.log`；真实地图 PIE 仍有场景/加载警告，没有测试错误。没有修改开始界面资产，进度条布局与视觉平滑效果需在用户 Widget 接线后验收，打包表现尚未验证

Ready 只表示达到当前可进入游戏的条件，不表示所有贴图最高 mip、着色器或所有异步资源已完成。加载或出生检查默认 180 秒超时，不能用 Delay 几秒替代成功条件。关卡中的环境 Actor 在子关卡可见后可以正常 BeginPlay，交通与行人不做全局暂停；玩家物理角色和任务计时由正式开始门控

### 对象与网络职责

- `Game/DeliveryStartupFlow`：放在入口持久关卡，每个世界独立。各端异步请求同一个已配置子关卡，监听显示事件并以低频检查出生地面；不持有其他 PIE 世界对象。开始请求、开局标记和服务器错误由服务器复制
- `DeliveryGameMode`：入口存在启动服务时延后正常出生；收到服务器就绪决定后沿原 Pawn / PC / PlayerState 配置出生。直接打开原地图走原逻辑
- `DeliveryPlayerController`：客户端通过拥有的 `ServerReportStartupReady` 报告本机加载就绪。服务器核对当前世界的启动服务，不接受客户端提供出生位置或任务数据。主机等待已连接玩家就绪，后加入玩家加载完才出生；此流程不包含房间创建、查找和连接 UI
- `DeliveryTaskManagerComponent`：BeginPlay 只建立 Locked 状态；启动服务存在时不解锁、不来电，`GetTimeSinceLevelStart` 保持零。服务器 `StartGameplay` 幂等地设置时间基准并启动原解锁轮询，基准和开始标记同步给客户端。EndPlay 清除解锁与催单计时器
- `DeliveryHUD` / `DeliveryStartupWidget`：HUD 管理菜单和输入模式，Widget 仅转发按钮、监听状态并呈现。启动面板优先使用 UIOnly，其他手机或背包面板不能提前释放游戏输入；关闭、重建、切图时解除委托与引用

没有配置 `MenuWidgetClass` 时仍自动加载，便于先接后端，但没有用户按钮。自定义 Widget 也可直接使用启动服务接口，需自行处理事件解绑。入口持久层只应放一个 StartupFlow，`PlayerStartTag` 留空时使用游戏子关卡的出生点，避免误用编辑器视窗位置创建的 PlayerStartPIE。多人共用出生点时检查附近有地面、坡度适中且无阻挡的位置，默认 `SpawnSearchRadius=400 cm`，仍通过正式 GameMode 生成角色，不修改地图出生点或强制重叠出生

回归入口：`Delivery.Startup.Requests`（跨世界拒绝、提前/重复请求、失败状态）、`Delivery.Startup.TaskGate`（菜单时钟和来电、开始幂等、原地图兼容）、`Delivery.Startup.ListenServerFlow`（真实入口双人 PIE、任务清单、等待、双方出生和 HUD 输入恢复）。PIE 检查依赖仅在编辑器目标加入 UnrealEd，不引入运行时编辑器依赖

本次验证（2026-11-10）：`Build.bat DeliveryEditor Win64 Development -Project=Delivery.uproject -WaitMutex -NoHotReloadFromIDE` 完整编译通过。最终 `Automation RunTests Delivery.Startup` 的 3 项全部通过，报告在 `Saved/Automation/StartupFlowSpawnRetest`；此前同轮运行的 4 项 `Delivery.Task` 奖励测试、`Delivery.UI.PhoneController` 和 `Delivery.UI.SettlementReveal` 也全部通过，共 9 项。双人 PIE 确认双方加载完成后仍无角色且任务时钟为零，主机请求开始后双方拥有 Pawn、HUD 退出 Startup 输入模式，重复请求不重新生成或重置时钟。最终 PIE 有 36 条场景/加载警告，包括斧头 ComplexAsSimple、导航空边界、行人未设网格和加载长帧；没有测试失败。原 `TestForCharacter.umap` 和默认启动配置无改动，Python 设置脚本语法检查通过

未覆盖用户尚未制作的菜单视觉、键鼠/手柄焦点体验、打包后的加载表现和中途加入场景。异步流送仍需在游戏线程注册组件与执行场景 BeginPlay，大地图激活可能短暂停顿；Ready 不作为加载过程全程流畅的保证

交通与行人采用单向礼让：交通车探测行人并减速停车，所有 DeliveryPedestrian（包含过街路线）不因交通车、车辆通道碰撞体或电瓶车停步，避免人车互等；行人之间、玩家和场景障碍的原有避让保留。

电瓶车音效与鸣笛：骑车复用 IA_AttackLeft（鼠标左键）开始鸣笛，Completed/Canceled 停止；服务器校验驾驶员并复制 bHornActive，换控制对象、下车时清除，音频组件 EndPlay 停止并销毁独立喇叭音源。循环喇叭、撞车和撞人分别配置在 DA_VehicleAudio 的 VehicleHorn、VehicleImpacted、VehicleHitPerson，碰撞沿已有服务器事件多播；撞人音效有 0.25 秒去重。鸣笛每 0.2 秒查询前方默认 800 cm、半角 40°、无遮挡的行人，通过 DeliveryHornReceiver 接口通知所属路线；仅设置有效 CrosswalkActor 且非自由漫游的过街路线响应，普通行人不受影响。路线刷新 0.35 秒加速租约，整条线的在场 NPC 一起加速，默认 2 倍，和过街倍率取最大值，不叠乘。停止、离开范围或车辆销毁后租约自动过期，整条路线恢复各自原速并保留原路线和避障规则，不因喇叭改变红绿灯校验。卡通音色脚本 Content/Python/setup_bike_cartoon_audio.py 生成原创 WAV 并导入、绑定，不修改地图。NPC 拳击命中使用 PunchNPCHitSound，由现有服务器命中窗口及每拳去重控制，未配置时回退 PunchHitSound，挥空不播放命中音。

电瓶车鸣笛也可催促同一前方范围内的交通车：仅当前车速已达其蓝图报告 MaxSpeed、路线正常且无停车/减速限制时，服务器接收 0.35 秒短租约；畅通时临时速度倍率默认 1.3，可在 Motorbike|Horn 调整。所有红灯、过街限速、行人、跟车和路线状态检查优先，出现限制立即撤销额外倍率，随后按原刹车规则运行。只改变临时 SpeedMultiplier，不写 MaxSpeed，重复请求不叠乘，重置路线清租约。

上车短提示依次显示原下车提示、鸣笛操作提示（1 秒）；鸣笛提示从当前输入映射读取按键，键鼠默认左键，下车或失去本机控制时取消待显示提示，不保留旧驾驶员的队列。

本次验证（2026-11-11）：`Build.bat DeliveryEditor Win64 Development -Project=Delivery.uproject -WaitMutex -NoHotReloadFromIDE` 最终编译成功（XGE 未激活许可证，独立模式完成）；`Automation RunTests Delivery.Vehicle.HeldHorn+Delivery.NPC.PedestrianWalk+Delivery.Traffic.PedestrianYield+Delivery.Traffic.Horn+Delivery.Audio.Events+Delivery.Boxing.LowFrameRate+Delivery.UI.PersistentHint` 共 8 项通过，报告 `Saved/Automation/BikeCartoonFinalWithHint`。覆盖普通行人不响应、检测一人整线加速、重复鸣笛不叠乘、范围外与车后不触发、租约恢复、松开/失去控制停止、仅满速交通车响应、红灯/行人优先、路线重置清理及现有音效与提示。音效导入脚本成功执行并完成正式蓝图/配置回读；commandlet 启动仍记录 GEngine 未初始化时物理材质查询和本机 MCP 8000 端口占用错误，导致该导入进程返回 1，未将其视为干净通过。未做真实地图双人 PIE、手柄操作和音色试听验收；上车后提示的实际显示位置与一秒观感仍需场景检查。

玩家提示统一使用英文：交互、上下车、装卸货、召唤/冷却、视角、鸣笛、任务领取与交付、来电/通话及结算提示已更新；本地化键保留，源字符串改为英文。DefaultGame.ini 的 Internationalization.Culture=en 使运行时按键名称使用英文。Content/Python/setup_english_prompts.py 按 prompt_english_translations.json 精确匹配已有文字，同步蓝图覆盖、UMG 模板和正式地图中的同文案实例，保留其他参数；不翻译编辑器标签、开发日志和任务对白。代码编译通过，资源同步更新 26 项，UI/Task UI 图节点文字审计没有剩余中文常量，报告在 Saved/english_prompt_changes.json、Saved/english_graph_audit.json。
喇叭音色调整：SFX_Bike_HornLoop 改为固定音高 400/500 Hz 双音簧片，移除原来的调频颤音，增加泛音与柔和饱和；DA_VehicleAudio 的鸣笛音量设为 1.6，循环波形仍按 0.4 秒整周期闭合。资源与配置已在编辑器关闭后重新保存并回读通过；未做场景实际试听验收。

英文提示本次回归（2026-11-11）：最终编译成功；`Automation RunTests Delivery.UI+Delivery.Task+Delivery.Systems.InputLifecycle+Delivery.Vehicle.HeldHorn` 共 9 项通过，报告 `Saved/Automation/EnglishPrompts`。未做完整地图逐条提示截图验收，任务对白不在本次提示翻译范围内。
