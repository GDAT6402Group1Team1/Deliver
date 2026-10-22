# 红绿灯初始状态与朝向

初始颜色由蓝图类的 `Start Color` 决定，实例不可直接编辑：

- `/Game/trafficlight/trafficlight`：红（枚举值 2）。
- `/Game/trafficlight/trafficlight_green_start`：绿（枚举值 0）。
- `gen_traffic_lights.py` 按车道 `LightNumber` 选类；按2026-10-21用户定义，`GREEN_FIRST_GROUP = 0`，即组0／上下绿起始、组1／左右红起始。生成、调时和修复脚本采用同一规则。标签 `ClaudeGenTrafficLight:<组号>` 记录分组。
- 路口 `LightDuration = 6` 时，绿5秒、黄1秒、红6秒，周期12秒。黄灯从绿相位中扣除。

灯蓝图在 BeginPlay 启动自己的 Delay 循环，未持续读取路口相位。相同起始色和周期可保持对向配置一致，但这不等于已经验证游戏运行中的灯与车辆信号完全同步。

## 2026-10-21 朝向修复

实测灯的三个 SpotLight 面向 Actor 的局部 -Y（相对 Actor yaw 为 -90°）。旧生成器把模型正面当成 +X，造成灯面整体偏90°。`MODEL_YAW_OFFSET = 90` 后，灯头世界朝向才等于来车方向的反向。

`traffic_light_directions.py` 将0/2、1/3两对方向分别平均后设为精确相差180°，每对共用一个分组和周期。同一路口全部灯继续使用同一个世界坐标。

修复现有灯：编辑器控制台 `py fix_traffic_light_directions.py`。脚本先备份到 `Saved/traffic_light_direction_backup_*.json`，检查灯头方向与起始色，统一周期，只有起始类不符时才替换灯 Actor，最后保存当前地图。修复结果写入 `Saved/traffic_light_direction_fix_report.json`。

已在后台临时内存中检查保存地图的132盏灯、42个路口、55对对向灯：起始颜色、三个时长、对向灯头方向、同路口重合均通过；未保存或覆盖用户地图，未进行 PIE 动态验证。

原始车道数据在 Intersection_19、30、31、40 存在同方向混合 LightNumber；灯按该方向对的多数分组统一，报告明确列出，未修改车道数据。

## 四车道路口灯位

编辑器控制台运行 `py reposition_traffic_lights.py` 可重新摆放现有红绿灯。四车道路口只用同一条主路的 `-0180` 和 `+0180` 两条内车道计算灯位，再把该路口的所有灯放在同一个坐标；不使用 `-0540/-0180` 或 `+0180/+0540` 的同向车道间隙。如果两条内车道没有共同的安全位置，脚本会跳过该路口并在 `Saved/reposition_traffic_lights.json` 中记录原因。运行前停止 PIE / Simulate。脚本成功时保存当前关卡。
