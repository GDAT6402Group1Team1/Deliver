# 车辆

驾驶、骑手表现和后备箱装卸货

从 DeliveryMotorbike 开始查看对外方法，再进入对应实现

系统总览见 [系统设计](../../../Document/Architecture.md)

车辆事件由 Events 发布，事件声音与发动机由 Audio/DeliveryVehicleAudioComponent 处理，参数在 DA_VehicleAudio 中配置

车辆切换视角后发布 CameraChanged，提示组件订阅后更新文字
召唤组件保存冷却结束时间并发布 OnCooldownChanged，不再逐帧更新提示
## 文件分工

同一个 `ADeliveryMotorbike` 拆在三个 cpp 里，只是为了别让主文件继续变长：

| 文件 | 管什么 |
|---|---|
| `DeliveryMotorbike.cpp` | 上下车、速度转向、贴地越障、撞击、镜头、轮子 |
| `DeliveryMotorbikeRider.cpp` | 骑手姿态与接触点 |
| `DeliveryMotorbikeTrunk.cpp` | 整车视觉缩放（`VisualScale`）、后备箱装卸货 |

后备箱的完整背景、踩过的坑和回归测试见 [CLAUDE.md](../../../CLAUDE.md) 的「车上的货」一节。
