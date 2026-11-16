#pragma once

#include "CoreMinimal.h"
#include "DeliveryNpcDecisionTrace.generated.h"

/**
 * @brief AI 检查器显示的一次请求的全过程：给了模型什么、模型选了什么、检查结果、执行结果、耗时
 *
 * Agent 在请求、检查、执行各阶段填写，交谈组件推送给本机的检查器
 * 只记最近一次真实请求，不存完整提示词，也不存模型的思考过程
 */
USTRUCT(BlueprintType)
struct DELIVERY_API FDeliveryNpcDecisionTrace
{
    GENERATED_BODY()

    /// 哪个世界的请求，换图后旧记录不再显示
    UPROPERTY(BlueprintReadOnly) FGuid WorldEpoch;

    /// 请求编号
    UPROPERTY(BlueprintReadOnly) FGuid RequestId;

    /// 采纳后的动作编号
    UPROPERTY(BlueprintReadOnly) FGuid ActionId;

    /// 每改一次加一，检查器只显示最新的
    UPROPERTY(BlueprintReadOnly) int32 Revision = 0;

    /// 最后发生的一步，模型结束和动作执行结果分开显示
    UPROPERTY(BlueprintReadOnly) FName Stage;

    /// NPC 名字
    UPROPERTY(BlueprintReadOnly) FString NpcName;

    /// 后端实际用的模型
    UPROPERTY(BlueprintReadOnly) FString ActualModelName;

    /// 模型来自哪家服务
    UPROPERTY(BlueprintReadOnly) FString ModelProvider;

    /// 是谁触发的这次请求：玩家说话、挨打、闲着等
    UPROPERTY(BlueprintReadOnly) FString RequestSource;

    /// 后端走了哪条处理流程，比如普通对话流程或交战专用流程
    UPROPERTY(BlueprintReadOnly) FString Route;

    /// 没用完整模型时的原因
    UPROPERTY(BlueprintReadOnly) FString FallbackReason;

    /// 给模型的输入摘要
    UPROPERTY(BlueprintReadOnly) FString InputSummary;

    /// 玩家说的那句话
    UPROPERTY(BlueprintReadOnly) FString PlayerText;

    /// NPC 看到听到的事
    UPROPERTY(BlueprintReadOnly) TArray<FString> Observations;

    /// 模型能指代的人和物
    UPROPERTY(BlueprintReadOnly) TArray<FString> Subjects;

    /// 列给模型的候选动作
    UPROPERTY(BlueprintReadOnly) TArray<FString> Candidates;

    /// 记忆里的往事
    UPROPERTY(BlueprintReadOnly) TArray<FString> Memory;

    /// NPC 当时正在做什么
    UPROPERTY(BlueprintReadOnly) FString CurrentActivity;

    /// 模型选的动作
    UPROPERTY(BlueprintReadOnly) FString ProposedAction;

    /// 模型选的目标
    UPROPERTY(BlueprintReadOnly) FString ProposedTarget;

    /// 检查通过后实际采纳的动作
    UPROPERTY(BlueprintReadOnly) FString ValidatedAction;

    /// 动作做完的结果
    UPROPERTY(BlueprintReadOnly) FString ExecutionResult;

    /// 检查没通过的原因
    UPROPERTY(BlueprintReadOnly) FString RejectionReason;

    /// 台词预览
    UPROPERTY(BlueprintReadOnly) FString ReplyPreview;

    /// 台词播到哪一步
    UPROPERTY(BlueprintReadOnly) FName ReplyStage;

    /// 第一句台词
    UPROPERTY(BlueprintReadOnly) FString FirstReplyText;

    // ── 耗时 ──

    /// 本机实测：从 HTTP 发出到收到模型建议，毫秒
    UPROPERTY(BlueprintReadOnly) double DecisionElapsedMs = -1;

    /// 从排队到这次回答全部收完，毫秒
    UPROPERTY(BlueprintReadOnly) double TotalElapsedMs = -1;

    /// 后端报的排队毫秒数；后端没给就是 -1，不靠配置猜
    UPROPERTY(BlueprintReadOnly) double BackendQueueMs = -1;

    /// 后端报的模型思考毫秒数
    UPROPERTY(BlueprintReadOnly) double BackendDecisionMs = -1;

    /// 后端报的总毫秒数
    UPROPERTY(BlueprintReadOnly) double BackendElapsedMs = -1;

    /// 到第一句台词出来用了多少毫秒，决定玩家要等多久才听到回话
    UPROPERTY(BlueprintReadOnly) double FirstSentenceMs = -1;

    /// 输入用了多少 token（模型计费和计长度的单位）
    UPROPERTY(BlueprintReadOnly) int32 InputTokens = -1;

    /// 其中多少 token 命中了缓存，命中越多越快越便宜
    UPROPERTY(BlueprintReadOnly) int32 CachedInputTokens = -1;

    /// 模型输出了多少 token
    UPROPERTY(BlueprintReadOnly) int32 OutputTokens = -1;
};

/// @brief NPC 的决策记录更新时通知交谈组件，它再发给开着检查器的玩家
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryNpcDecisionTraceChanged, const FDeliveryNpcDecisionTrace& /* 更新后的记录 */);
