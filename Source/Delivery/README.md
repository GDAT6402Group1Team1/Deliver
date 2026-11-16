# Delivery 源码

先看 [系统设计](../../Document/Architecture.md)，再按需要进入对应系统

角色由 Character/DeliveryCharacter 创建，玩家控制与玩家状态放在 Player
交互的开始、取消和服务器检查在 Interaction，快递在背包和车辆之间的协调在 Carry
战斗过程在 Combat，身体运动与物理同步在 Ragdoll，抓取约束在 Grab
任务管理器保留在 Task 根目录，配置放在 Data，场景对象放在 World，追踪与引导放在 Tracking
电话队列在 Phone，地点注册在 World，车辆与场景交通分别在 Vehicle 和 Traffic
技能与伤害效果在 AbilitySystem，界面数据整理在 UI/Controllers

角色、玩家状态和游戏状态分别持有自己的组件，目录按功能分组，不改变网络状态归属
蓝图继续使用原类名和资源路径，角色原函数保留为兼容入口

输入规则位于 Input/DeliveryInputComponent，快捷栏的生命周期位于 UI/HUD/DeliveryHUD

## 子目录划分

文件多的模块按职责再分一层，类名和蓝图引用不变，`#include` 写相对 Source/Delivery 的完整路径
组件主体放在模块根目录，同一个类拆出的实现文件放进对应子目录，例如表现组件的语音播放在 AI/Presentation/Speech
测试统一放在模块下的 Tests，控制台命令放在 Debug

- AI/Companion：Core 放二狗角色、组件、调参和记忆，Encounter 放初遇，Home 放家居安排，Errand 放跑腿、物品和小绿，Travel 放骑车与追赶，Assist 放协战
- AI/Presentation：Speech 放句子播放、语音生成和音色，Animation 放身体与对话动画实例，Face 放表情、视线和受击反应
- AI/Tests：Resident、Companion、Conversation、Speech、Fight
- Vehicle：Motorbike 放摩托车主体，Rider 放骑手姿态，Trunk 放后备箱、货物和后座乘客，Companion 放二狗专属车，Summon 放召车
- UI：HUD 放 HUD、快捷栏、小地图和提示，Conversation 放对话、字幕和 AI 检查器
- Combat：Pose 放拳击与手臂姿势，Types 放接口、事件、类型和碰撞通道
- World：Home 放房屋与家居物件，Location 放地点注册
- Speech：Capture 放麦克风采集与音频处理，Recognition 放识别、分句和轮次
- Inventory：Items 放具体物品，NPC：Grandpa 和 Pedestrian 分开

整理由 Tools/Reorganize/reorganize_sources.py 完成，清单写在脚本里
