# 交通

控制场景车辆沿路线行驶

从 DeliveryTrafficCarComponent 开始查看对外方法，再进入对应实现

靠近玩家鸣笛的距离、冷却和服务器通知在 DeliveryTrafficCarHorn.cpp；声音播放复用 Audio/DeliveryVehicleAudioComponent。多音效数组、手动接口和默认参数见 [事件与游戏音效](../../../Document/GameplayAudio.md#交通车靠近玩家鸣笛)

系统总览见 [系统设计](../../../Document/Architecture.md)
