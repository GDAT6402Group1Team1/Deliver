"""创建正式交谈界面；已有蓝图保持当前编辑结果"""
import unreal

result = unreal.DeliveryConversationAssetTools.create_conversation_widget(False)
unreal.log(result)
if 'saved:' not in result and 'already exists' not in result:
    raise RuntimeError(result)
