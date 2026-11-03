# 界面数据

提供任务文字、背包槽位和交互提示

从 DeliveryTaskController / DeliveryHotbarController / DeliveryInteractionController 开始查看对外方法，再进入对应实现

系统总览见 [系统设计](../../../../Document/Architecture.md)

DeliveryPhoneController 处理手机操作与字幕，由 HUD 创建和销毁，电话规则读取已有 PhoneCallQueue
