# 玩家输入

DeliveryInputComponent 绑定步行操作，区分单键出拳与双键抓取，并调用角色和玩法组件
输入映射由 PlayerController 管理，车辆驾驶输入由 DeliveryMotorbike 绑定

先看 Bind，再看 MousePressed、ResolveSingleMousePress 和 MouseReleased
ResetInput 处理控制权交接，输入参数统一在 PlayerInput 组件中配置
DeliveryInputTests 检查重复绑定、组合键取消和骑车时的界面数据来源

鼠标右键在 0.2 秒内松开且累计移动不足 4 个输入单位时出拳；长按或拖动只转视角
左右键在原有 0.2 秒内组合仍用于抓取，抓取释放和输入取消不会补一次攻击
手柄继续使用原来的攻击与组合规则，鼠标灵敏度不影响右键拖动判断
DeliveryRightMouseGestureTests 检查短按、长按、拖动、抓取和取消后的点击结果；实际视角与手感仍需游戏中检查

召唤与五个物品栏动作统一通过 IA 绑定，手机由 PlayerController 绑定，驾驶视角由车辆绑定
按键在 IMC_Default 配置，提示由 UI/DeliveryHintComponent 根据实际映射生成
