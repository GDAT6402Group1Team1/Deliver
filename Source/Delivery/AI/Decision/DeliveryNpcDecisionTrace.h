#pragma once

#include "CoreMinimal.h"
#include "DeliveryNpcDecisionTrace.generated.h"

// 只展示最近一次真实请求与执行事件，不保存完整提示词或模型思考内容
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcDecisionTrace
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FGuid WorldEpoch;
    UPROPERTY(BlueprintReadOnly) FGuid RequestId;
    UPROPERTY(BlueprintReadOnly) FGuid ActionId;
    UPROPERTY(BlueprintReadOnly) int32 Revision = 0;
    // Stage 是最后发生的事件，模型结束与动作执行结果分别显示
    UPROPERTY(BlueprintReadOnly) FName Stage;
    UPROPERTY(BlueprintReadOnly) FString NpcName;
    UPROPERTY(BlueprintReadOnly) FString ActualModelName;
    UPROPERTY(BlueprintReadOnly) FString ModelProvider;
    UPROPERTY(BlueprintReadOnly) FString RequestSource;
    UPROPERTY(BlueprintReadOnly) FString Route;
    UPROPERTY(BlueprintReadOnly) FString FallbackReason;
    UPROPERTY(BlueprintReadOnly) FString InputSummary;
    UPROPERTY(BlueprintReadOnly) FString PlayerText;
    UPROPERTY(BlueprintReadOnly) TArray<FString> Observations;
    UPROPERTY(BlueprintReadOnly) TArray<FString> Subjects;
    UPROPERTY(BlueprintReadOnly) TArray<FString> Candidates;
    UPROPERTY(BlueprintReadOnly) TArray<FString> Memory;
    UPROPERTY(BlueprintReadOnly) FString CurrentActivity;
    UPROPERTY(BlueprintReadOnly) FString ProposedAction;
    UPROPERTY(BlueprintReadOnly) FString ProposedTarget;
    UPROPERTY(BlueprintReadOnly) FString ValidatedAction;
    UPROPERTY(BlueprintReadOnly) FString ExecutionResult;
    UPROPERTY(BlueprintReadOnly) FString RejectionReason;
    UPROPERTY(BlueprintReadOnly) FString ReplyPreview;
    UPROPERTY(BlueprintReadOnly) FName ReplyStage;
    UPROPERTY(BlueprintReadOnly) FString FirstReplyText;
    // 本机单调时钟实测：HTTP 发出到建议返回，入队到本次响应传输结束
    UPROPERTY(BlueprintReadOnly) double DecisionElapsedMs = -1;
    UPROPERTY(BlueprintReadOnly) double TotalElapsedMs = -1;
    // 后端明确提供才填写，不根据配置推测模型或耗时
    UPROPERTY(BlueprintReadOnly) double BackendQueueMs = -1;
    UPROPERTY(BlueprintReadOnly) double BackendDecisionMs = -1;
    UPROPERTY(BlueprintReadOnly) double BackendElapsedMs = -1;
    UPROPERTY(BlueprintReadOnly) double FirstSentenceMs = -1;
    UPROPERTY(BlueprintReadOnly) int32 InputTokens = -1;
    UPROPERTY(BlueprintReadOnly) int32 CachedInputTokens = -1;
    UPROPERTY(BlueprintReadOnly) int32 OutputTokens = -1;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDecisionTraceChanged, const FDeliveryNpcDecisionTrace&);
