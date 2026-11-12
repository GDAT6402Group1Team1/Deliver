#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DeliveryAiSettings.generated.h"

// 只保存连接和预算配置，运行中的请求由每个 World 的网关管理
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Delivery AI"))
class DELIVERY_API UDeliveryAiSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    // 默认关闭，现有关卡不会因加入基础设施自动调用模型
    UPROPERTY(Config, EditAnywhere, Category="连接") bool bEnabled = false;
    // 密钥只放在独立后端，不放进 UE 配置或客户端安装包
    UPROPERTY(Config, EditAnywhere, Category="连接") FString DecisionEndpoint = TEXT("http://127.0.0.1:8765/v1/decisions");
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="1", ClampMax="8")) int32 MaxConcurrentRequests = 2;
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="1", ClampMax="64")) int32 MaxPendingRequests = 32;
    // 包括排队和传输，使用单调真实时间，暂停游戏也不会无限保留 HTTP 请求
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="0.5", ClampMax="30", Units="s")) float RequestTimeout = 8.f;
};
