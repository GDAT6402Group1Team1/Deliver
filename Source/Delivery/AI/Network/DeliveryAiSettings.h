#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DeliveryAiSettings.generated.h"

class UDataTable;

/**
 * @brief AI 后端的项目设置：开关、地址、同时发几个请求、多久超时
 *
 * 在 项目设置 → Delivery AI 里改，存进 Config/DefaultGame.ini；网关每次发请求时读这里
 */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Delivery AI"))
class DELIVERY_API UDeliveryAiSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    // ── 交谈 ──

    /// 后端一句一句地返回回复；旧后端可以关掉，一次返回完整结果
    UPROPERTY(Config, EditAnywhere, Category="交谈") bool bStreamReplies = true;

    /// 一句话的声音也分段发，边收边播；默认整句发完，开启后后端也要允许
    UPROPERTY(Config, EditAnywhere, Category="交谈", meta=(EditCondition="bStreamReplies")) bool bStreamSentenceAudio = false;

    // ── 行为 ──

    /// 共享行为表，世界开始时读一次；留空沿用角色自己的规则
    UPROPERTY(Config, EditAnywhere, Category="行为", meta=(RequiredAssetDataTags="RowStructure=/Script/Delivery.DeliveryNpcBehaviorCatalogRow"))
    TSoftObjectPtr<UDataTable> NpcBehaviorCatalog;

    // ── 连接 ──

    /// 是否连 AI 后端；默认关，在 DefaultGame.ini 里打开
    UPROPERTY(Config, EditAnywhere, Category="连接") bool bEnabled = false;

    /// 后端地址；模型密钥只放在后端，不放进 UE 配置或安装包
    UPROPERTY(Config, EditAnywhere, Category="连接") FString DecisionEndpoint = TEXT("http://127.0.0.1:8765/v1/decisions");

    // ── 预算 ──

    /// 同时最多发几个请求
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="1", ClampMax="8")) int32 MaxConcurrentRequests = 2;

    /// 最多排多少个请求
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="1", ClampMax="64")) int32 MaxPendingRequests = 32;

    /// 一个请求最多等多久，从排队算起，暂停游戏也照样计时
    UPROPERTY(Config, EditAnywhere, Category="预算", meta=(ClampMin="0.5", ClampMax="30", Units="s")) float RequestTimeout = 8.f;
};
