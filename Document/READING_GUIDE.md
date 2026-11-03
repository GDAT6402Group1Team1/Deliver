# Delivery 项目实现

系统分工与阅读入口见 [系统设计](Architecture.md)，HTML 总文档包含当前实现的流程图和完整关键函数

系统分工与阅读入口见 [系统设计](Architecture.md)，HTML 总文档包含当前实现的流程图和完整关键函数

系统分工与阅读入口见 [系统设计](Architecture.md)，HTML 总文档包含当前实现的流程图和完整关键函数

系统分工与阅读入口见 [系统设计](Architecture.md)，HTML 总文档包含当前实现的流程图和完整关键函数

## 角色与组件

Delivery 的角色在行走、出拳、受击和倒地期间持续进行物理模拟。玩家输入提供移动方向和动作请求，布娃娃组件据此更新身体各部位的控制目标。角色最终的位置和姿态由物理计算产生，因此会受到碰撞、地形和外力影响。

DeliveryCharacter 创建并持有布娃娃、近战、抓取、交互、携带和背包组件，同时处理输入转发以及组件之间的调用。例如，攻击输入需要检查当前是否持有物品，再决定使用物品还是执行近战。角色通过交互、携带和战斗组件提供这些功能；站立计算、出拳时间和抓取检查分别在各自组件中实现。

BP_DeliveryMan 是角色的蓝图配置，指定网格、物理资产和组件参数。renwu_Physics1 这类物理资产定义骨骼对应的刚体、碰撞形状和关节限制。C++ 提供控制方法，资源提供身体结构与参数，两部分共同决定实际表现。蓝图和关卡实例可以覆盖代码默认值，调试时需要核对实际使用的配置。

对应文件：

- [Source/Delivery/Character/DeliveryCharacter.cpp](../Source/Delivery/Character/DeliveryCharacter.cpp)
- [Source/Delivery/Character/DeliveryCharacter.h](../Source/Delivery/Character/DeliveryCharacter.h)

## 物理移动

DeliveryActiveRagdollComponent 按物理更新阶段组织角色控制。物理计算前更新身体目标，物理计算后读取实际姿态、采集网络数据，并让角色胶囊跟随髋部。胶囊的位置用于镜头和其他角色逻辑，身体运动仍以骨骼刚体为准。

布娃娃代码按计算内容分在不同目录。Setup 创建物理控制对象并准备参考姿态；Movement 中的 Stance 计算髋部高度、朝向和身体旋转；Gait 管理左右脚的支撑与迈步；Ground 处理地面、坡度和恢复条件。这些算法仍由一个组件统一调度，运行状态分别保存在 Movement、Punch、Recovery 与 Network 中。蓝图参数和复制字段留在主组件，更新顺序与共享状态见 [布娃娃更新](RagdollUpdate.md)。

站立控制会根据地面位置、法线和角色状态计算髋部目标，再由物理驱动向目标施力。目标只是期望姿态，碰撞和关节限制会使实际姿态与它存在偏差。角色向前移动时，Gait 根据身体位置决定哪只脚迈步，记录落点并推进摆动，使双脚能够轮流提供支撑。

倒地和恢复沿用同一套刚体。倒地时减弱或关闭主动控制，让身体受到重力和碰撞影响；恢复时从当前姿态重新建立控制目标，逐步恢复支撑。斜坡翻滚与生命耗尽导致的晕倒分别管理，坡地恢复条件不能直接解除晕倒状态。

对应文件：

- [Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.cpp](../Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.cpp)
- [Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h](../Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h)
- [Source/Delivery/Ragdoll/Setup/DeliveryActiveRagdollSetup.cpp](../Source/Delivery/Ragdoll/Setup/DeliveryActiveRagdollSetup.cpp)
- [Source/Delivery/Ragdoll/Movement/DeliveryActiveRagdollStance.cpp](../Source/Delivery/Ragdoll/Movement/DeliveryActiveRagdollStance.cpp)
- [Source/Delivery/Ragdoll/Movement/DeliveryActiveRagdollGait.cpp](../Source/Delivery/Ragdoll/Movement/DeliveryActiveRagdollGait.cpp)
- [Source/Delivery/Ragdoll/Movement/DeliveryActiveRagdollGround.cpp](../Source/Delivery/Ragdoll/Movement/DeliveryActiveRagdollGround.cpp)

## 近战攻击

近战从 GA_DeliverPunch 启动。技能检查角色是否允许攻击，通过后调用角色的 StartMeleeAttack，由 DeliveryRagdollCombatComponent 安排蓄力、释放、命中检测和结束。技能保存攻击规则与生命周期，战斗组件保存本次出拳的手、方向和计时状态。

释放时，战斗组件更新身体的出拳状态，并给手部、前臂和上臂施加不同强度的冲量。DeliveryBoxingPose 计算手臂目标姿态，Ragdoll/Combat 将这些目标应用到物理身体。冲量提供起始速度，姿态驱动维持拳路，实际手臂运动仍受到碰撞约束。

命中窗口只由服务器触发。GA_DeliverPunch 收到通知后收集目标，应用伤害效果，并请求受击者处理物理冲量。命中声音根据结算结果播放；本机出拳声音可以随操作提前播放，以减少输入反馈的等待。

攻击结束时，技能解除事件绑定。攻击取消时还会清除未触发的计时器，防止倒地或切换状态后继续执行旧攻击。当前已经晕倒的目标不再受到普通扣血，但仍可受到物理冲量，伤害计算与身体受力分别处理。

对应文件：

- [Source/Delivery/Combat/DeliveryRagdollCombatComponent.cpp](../Source/Delivery/Combat/DeliveryRagdollCombatComponent.cpp)
- [Source/Delivery/Combat/DeliveryBoxingPose.cpp](../Source/Delivery/Combat/DeliveryBoxingPose.cpp)
- [Source/Delivery/Ragdoll/Combat/DeliveryActiveRagdollCombat.cpp](../Source/Delivery/Ragdoll/Combat/DeliveryActiveRagdollCombat.cpp)
- [Source/Delivery/AbilitySystem/Abilities/GA_DeliverPunch.cpp](../Source/Delivery/AbilitySystem/Abilities/GA_DeliverPunch.cpp)

## 斧头下劈

DeliveryActiveRagdollAnimation.cpp 管理斧头动作的开始条件、同步时间和破门检测。下劈要求角色能够行动、当前物品支持斧头动作，并且没有与出拳、起身或抓取冲突。客户端发出请求后，服务器再次检查状态和动画有效性，再广播斧头对象及起始时间。

各端通过当前服务器时间减去起始时间，计算动作已经播放多久，并按播放速度转换为动画位置。GetAxeSwingWeight 根据当前物品、角色状态和动画进度计算混合权重。动作开始和结束时逐渐淡入、淡出；换掉斧头或进入不允许的状态时，权重归零。

DeliveryAnimationArmPose.cpp 负责动画采样。它累计骨骼层级变换，用参考姿势建立前、右、上三个方向，再提取双臂的方向信息。输出不包含源角色的手臂长度，身体控制可以使用这些方向驱动现有骨架，同时保留物理模拟。

服务器在动画下劈时刻执行一次球形扫掠，从右手向身体前方检测，并排除角色自身和手持斧头。检测只返回遇到的第一个阻挡物体。这个物体是 DeliveryBreakableDoor 时才执行破门；如果先碰到墙，后面的门不会被这次检测命中。门随后记录破坏状态，关闭完整门体的碰撞与显示，启用碎片物理。动作调度、姿态采样和门体表现分别由三个文件维护。

对应文件：

- [Source/Delivery/Ragdoll/Combat/DeliveryActiveRagdollAnimation.cpp](../Source/Delivery/Ragdoll/Combat/DeliveryActiveRagdollAnimation.cpp)
- [Source/Delivery/Combat/DeliveryAnimationArmPose.cpp](../Source/Delivery/Combat/DeliveryAnimationArmPose.cpp)
- [Source/Delivery/Task/World/DeliveryBreakableDoor.cpp](../Source/Delivery/Task/World/DeliveryBreakableDoor.cpp)

## 网络同步

物理角色的状态包含多块刚体的位置和旋转。仅同步角色整体位置，不能还原碰撞后的手臂、躯干和双脚姿态。DeliveryActiveRagdollNetwork.cpp 因此在服务器物理计算完成后采集刚体状态，附带时间、序号及速度，形成姿态快照。

远端角色使用前后两份快照平滑更新。位置进行插值，旋转采用四元数插值；后续数据暂未到达时，位置可以沿已有速度短时推算。推算时长设有上限，避免网络停顿时身体持续沿旧速度移动。旋转仍在快照之间插值，没有通过角速度推算未来旋转。

本机角色需要及时响应操作，因此保持本地物理控制，并根据服务器快照修正位置。正常控制期间，修正以髋部误差为依据，整体平移身体，尽量保持关节两端的相对关系。每个新快照只执行一次校正，普通偏差限制修正幅度，较大偏差采用更直接的纠正。

起身期间使用专门的交接流程。客户端等待恢复状态之后的新快照，应用完整姿态，再重新建立本地控制目标。该实现包含本机响应、服务器校正和远端快照播放，不包含保存历史输入后重新模拟过去帧的回滚流程。

对应文件：

- [Source/Delivery/Ragdoll/Network/DeliveryActiveRagdollNetwork.cpp](../Source/Delivery/Ragdoll/Network/DeliveryActiveRagdollNetwork.cpp)

## 抓取与任务

DeliveryGrabComponent 管理抓取候选、服务器检查、双手接触点和释放。客户端根据镜头方向寻找目标并显示操作反馈，服务器核对距离、朝向、遮挡和目标状态后确认抓取。这样，客户端的提示可以及时响应，实际抓取关系仍由服务器决定。

普通物品的托举与人物拖拽采用不同处理。普通物品可以按配置使用受控位置移动，以保持持物稳定；人物拖拽需要处理手部与目标物理身体之间的连接。接触点保存为局部坐标，使目标移动后仍可恢复正确的抓握位置。

任务系统维护配送规则。DeliveryTaskManagerComponent 挂在 GameState 上，保存全局任务状态；DeliveryTaskTrackerComponent 挂在 PlayerState 上，保存各玩家当前追踪的任务。接听任务电话后记录服务器起始时间，剩余时间根据时间差计算。物品掉落、转手或存入车辆不会重置计时。

交付时由任务目标组件检查条件并请求任务管理组件完成结算。抓取系统处理物体如何被拿起，任务系统处理物品属于哪项任务、是否可以领取以及是否完成交付，两者通过接口协作。

对应文件：

- [Source/Delivery/Grab/DeliveryGrabComponent.cpp](../Source/Delivery/Grab/DeliveryGrabComponent.cpp)
- [Source/Delivery/UI/DeliveryHotbarWidget.cpp](../Source/Delivery/UI/DeliveryHotbarWidget.cpp)

## 界面与资源

DeliveryHotbarWidget 显示背包槽位、物品图标和耐久。BuildWidgetTree 在 C++ 中创建控件树，SetInventory 接入背包数据，Refresh 更新显示。WBP_DeliveryHotbar 是对应的蓝图类，但当前快捷栏布局主要由原生控件树建立。

界面读取背包和任务状态，不保存另一套领取、计时或耐久规则。任务倒计时来自服务器时间与任务起始时间，物品耐久来自物品数据。关闭或重新创建界面不会改变这些玩法状态。

角色资源位于 BP_DeliveryMan，物理结构由角色使用的 Physics Asset 配置，快捷栏使用 WBP_DeliveryHotbar 及相关贴图。源码目录分别对应角色组装、Ragdoll 身体控制、Combat 动作安排、GAS 技能规则、Grab 交互和 UI 显示。资源引用与实例覆盖需要在演示关卡中确认。

对应文件：

- [Source/Delivery/Character/DeliveryCharacter.cpp](../Source/Delivery/Character/DeliveryCharacter.cpp)
- [Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h](../Source/Delivery/Ragdoll/DeliveryActiveRagdollComponent.h)
- [Source/Delivery/UI/DeliveryHotbarWidget.cpp](../Source/Delivery/UI/DeliveryHotbarWidget.cpp)
