"""创建 NPC 交谈姿态动画图，已有图保持当前编辑结果"""
import unreal

result = unreal.DeliveryNpcAnimationAssetTools.create_conversation_animation(False)
unreal.log(result)
if not result.startswith('NPC conversation animation saved;'):
    raise RuntimeError(result)
