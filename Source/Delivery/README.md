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

输入规则位于 Input/DeliveryInputComponent，快捷栏的生命周期位于 UI/DeliveryHUD
