# 玩家输入

DeliveryInputComponent 绑定步行操作，区分单键出拳与双键抓取，并调用角色和玩法组件
输入映射由 PlayerController 管理，车辆驾驶输入由 DeliveryMotorbike 绑定

先看 Bind，再看 MousePressed、ResolveSingleMousePress 和 MouseReleased
ResetInput 处理控制权交接，输入参数统一在 PlayerInput 组件中配置
DeliveryInputTests 检查重复绑定、组合键取消和骑车时的界面数据来源

召唤与五个物品栏动作统一通过 IA 绑定，手机由 PlayerController 绑定，驾驶视角由车辆绑定
按键在 IMC_Default 配置，提示由 UI/DeliveryHintComponent 根据实际映射生成
