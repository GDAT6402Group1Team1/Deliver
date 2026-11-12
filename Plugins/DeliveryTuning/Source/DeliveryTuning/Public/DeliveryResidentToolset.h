#pragma once
#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "DeliveryResidentToolset.generated.h"

// 编辑器专用 MCP 入口，创建可编辑居民蓝图与行为树，不进入运行时模块
UCLASS()
class UDeliveryResidentToolset : public UToolsetDefinition
{
    GENERATED_BODY()
public:
    UFUNCTION(meta=(AICallable)) static FString CreateResidentAssets();
    UFUNCTION(meta=(AICallable)) static FString PlaceResidentPair(FVector Location);
    UFUNCTION(meta=(AICallable)) static FString InspectResidentWorlds();
    UFUNCTION(meta=(AICallable)) static bool ConfigureResidentPIE(int32 Players);
    // 联调入口只发布实际台词，不伪造模型建议或绕过执行校验
    UFUNCTION(meta=(AICallable)) static FString SpeakInResidentPIE(FString Text);
    // 从指定本地玩家经过正式交谈入口发送文本，服务端仍校验距离和遮挡
    UFUNCTION(meta=(AICallable)) static FString TalkInResidentPIE(bool bClient, FString Text);
    // 只启动固定 Delivery 测试组，不接受任意控制台命令或进程参数
    UFUNCTION(meta=(AICallable)) static FString RunResidentValidation(bool bFull = true);
};
