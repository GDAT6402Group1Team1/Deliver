# 交互

DeliveryInteractableComponent 配置每个物体的交互条件与提示，开始运行时登记到本世界的 DeliveryInteractionRegistry
DeliveryInteractionRegistry 保存弱引用，提供范围查询和最近目标查询，不处理任务或背包行为
DeliveryInteractionProbeComponent 选择当前目标并更新提示和描边
DeliveryInteractionComponent 接收按键请求，管理长按、取消和服务器检查，再调用目标的 CanUse 与 Use

普通物品和取货点提供拾取行为，收件点提供 CanDeliver 与 Deliver
角色原有蓝图入口继续转交请求，公共过程不再把交付称作拾取

DeliveryInteractionRegistryTests 检查双世界隔离、重复登记、销毁注销和锚点距离排序
DeliveryInteractionTests 检查操作权限、长按时间、骑车取件和交付

系统总览见 [系统设计](../../../Document/Architecture.md)
