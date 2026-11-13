#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DeliveryAiSettings.generated.h"

// 保存后端地址、请求数量上限和超时时间，具体请求由 Gateway 处理
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Delivery AI"))
class DELIVERY_API UDeliveryAiSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    // 使用逐句回复接口，旧后端仍可关闭此选项继续返回完整结果
    UPROPERTY(Config, EditAnywhere, Category="交谈") bool bStreamReplies = true;
    // 默认使用完整句子音频；启用后还需要后端允许句内分段
    UPROPERTY(Config, EditAnywhere, Category="交谈", meta=(EditCondition="bStreamReplies")) bool bStreamSentenceAudio = false;
    // 类默认关闭，项目可在 DefaultGame.ini 中启用
    UPROPERTY(Config, EditAnywhere, Category="连接") bool bEnabled = false;
    // 密钥只放在独立后端，不放进 UE 配置或客户端安装包
    UPROPERTY(Config, EditAnywhere, Category="连接") FString DecisionEndpoint = TEXT("http://127.0.0.1:8765/v1/decisions");
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="1", ClampMax="8")) int32 MaxConcurrentRequests = 2;
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="1", ClampMax="64")) int32 MaxPendingRequests = 32;
    // 超时包含排队和网络等待，暂停游戏时也继续计时
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="0.5", ClampMax="30", Units="s")) float RequestTimeout = 8.f;
};
