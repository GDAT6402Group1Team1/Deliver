# 布娃娃

DeliveryActiveRagdollComponent 统一安排物理前后的更新，蓝图继续配置同一个组件
Movement 保存步态数据，Punch 保存出拳指令，Recovery 保存恢复进度，Network 管理快照与交接
这些内部对象不单独 Tick，身体模式切换和物理写入仍由主组件协调

先看 TickComponent、UpdateBeforePhysics 和 UpdateAfterPhysics，再按玩法进入具体计算

| 目录 | 内容 |
| --- | --- |
| Setup | 创建控制器、参考姿态与物理质量修正 |
| Movement | 步态数据、身体目标、脚步与恢复进度 |
| Combat | 出拳指令、受击和动画转换 |
| Network | 快照格式、缓存、播放和本机校正 |
| Debug | 只读诊断，默认关闭 |
| Tests | 物理步态与网络交接回归检查 |

完整更新流程与状态归属见 [布娃娃更新](../../../Document/RagdollUpdate.md)
