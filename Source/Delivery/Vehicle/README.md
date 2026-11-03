# 车辆

驾驶、骑手表现和车辆货架

从 DeliveryMotorbike 开始查看对外方法，再进入对应实现

系统总览见 [系统设计](../../../Document/Architecture.md)

车辆事件由 Events 发布，事件声音与发动机由 Audio/DeliveryVehicleAudioComponent 处理，参数在 DA_VehicleAudio 中配置

车辆切换视角后发布 CameraChanged，提示组件订阅后更新文字
召唤组件保存冷却结束时间并发布 OnCooldownChanged，不再逐帧更新提示
