# Movement：职责与入口

根据输入和地面计算身体、脚步目标，物理求解决定实际运动。

先读 [DeliveryActiveRagdollStance.cpp](DeliveryActiveRagdollStance.cpp)，再返回 [总阅读路线](../README.md)。

`DeliveryRagdollRecovery.h` 保存卡脚释放、交叉补步和起身进度。`UpdateFootRecovery` 在髋部更新前报告异常，`UpdateFeet` 执行单脚补位，详细流程见 [布娃娃更新与状态分工](../../../../Document/RagdollUpdate.md)。
