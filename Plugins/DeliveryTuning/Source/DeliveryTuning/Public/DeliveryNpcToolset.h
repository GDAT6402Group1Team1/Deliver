#pragma once
#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "DeliveryNpcToolset.generated.h"

class USkeletalMesh;

// 编辑器专用 MCP 入口，创建可编辑居民蓝图与行为树，不进入运行时模块
UCLASS()
class UDeliveryNpcToolset : public UToolsetDefinition
{
    GENERATED_BODY()
public:
    UFUNCTION(meta=(AICallable)) static FString CreateResidentAssets();
    // 只补充倾听分支，保留居民模型、关卡位置与已有配置
    UFUNCTION(meta=(AICallable)) static FString UpdateNpcConversationAssets();
    // 将聊天布局保存到正式 UMG 资产，仅在退出 PIE 后按明确请求重建
    UFUNCTION(meta=(AICallable)) static FString RebuildConversationWidget();
    UFUNCTION(meta=(AICallable)) static FString PlaceResidentPair(FVector Location);
    // 默认只检查导航；修复时只补全空体积，保留已有的有效范围
    UFUNCTION(BlueprintCallable, Category="Delivery|NPC", meta=(AICallable)) static FString CheckNpcNavigation(bool bRepairEmpty = false);
    // 读取已导入表情的顶点位移，供无渲染的资源检查使用；不修改或保存模型
    UFUNCTION(BlueprintCallable, Category="Delivery|NPC", meta=(AICallable)) static FString GetNpcMorphStats(USkeletalMesh* Mesh, FName MorphName, int32 LODIndex = 0);
    UFUNCTION(meta=(AICallable)) static FString InspectResidentWorlds();
    UFUNCTION(meta=(AICallable)) static bool ConfigureResidentPIE(int32 Players);
    // 联调入口只发布实际台词，不伪造模型建议或绕过执行校验
    UFUNCTION(meta=(AICallable)) static FString SpeakInResidentPIE(FString Text);
    // 从指定本地玩家经过正式交谈入口发送文本，服务端仍校验距离和遮挡
    UFUNCTION(meta=(AICallable)) static FString TalkInResidentPIE(bool bClient, FString Text);
    // 只启动固定 Delivery 测试组，不接受任意控制台命令或进程参数
    UFUNCTION(meta=(AICallable)) static FString RunResidentValidation(bool bFull = true);
};
