#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "AI/Decision/DeliveryNpcBehaviorCatalog.h"
#include "DeliveryAiGatewaySubsystem.generated.h"

class UDeliveryNpcAgentComponent;
class IHttpRequest;
struct FDeliveryNpcStreamBuffer;

/**
 * @brief 游戏和 AI 后端之间的邮差：把 NPC 的请求发给后端，再把回答一行行交回 NPC
 *
 * 发送：Agent 调 Enqueue 排队，Tick 按并发上限挑出来 Dispatch，POST 到 {Endpoint}/stream
 * 读流：后台线程收到的每行 JSON 放进缓冲，Tick 里交给 ReadStream，按类型转给 Agent 的对应函数
 * 结束：整轮收完 FinishStreamRequest，非流式或失败走 Complete；取消时 Remove 并通知后端 /cancel
 * 每个世界一份，换图后旧请求的回答一律丢掉
 */
UCLASS()
class DELIVERY_API UDeliveryAiGatewaySubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    // ── 发送 ──
    // Agent 调用；排进队列后由 Tick 发出

    /// @brief 把一次请求排进队列；回答玩家的请求插到前面
    /// 这位 NPC 还有请求在等模型时不接，每位 NPC 连同在播的回复最多同时占 3 份
    /// @return 排上了返回 true；AI 没开、队列满或这位 NPC 还在等时返回 false
    bool Enqueue(UDeliveryNpcAgentComponent* Agent, const FDeliveryNpcDecisionRequest& Request);

    /// @brief 取消这位 NPC 所有的请求
    void Cancel(UDeliveryNpcAgentComponent* Agent);

    /// @brief 只取消这一个请求
    void CancelRequest(UDeliveryNpcAgentComponent* Agent, FGuid RequestId);

    /// @brief 共享行为表，Agent 列候选动作时用
    const FDeliveryNpcBehaviorCatalog& GetBehaviorCatalog() const { return BehaviorCatalog; }

    /// @brief 世界编号
    FGuid GetWorldEpoch() const { return WorldEpoch; }

    /// @brief 还有多少请求没结束
    int32 GetPendingCount() const { return Pending.Num(); }

    // ── 生命周期 ──
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /// 世界结束时取消所有请求
    virtual void OnWorldEndPlay(UWorld& InWorld) override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

protected:
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
    // ── 读流 ──
    // 每个请求的接收进度，一轮最多 3 句，每句要么整句到、要么开头 → 分段 → 结尾

    /// 一句收到哪了：没收、整句到了、正在分段收、收完、失败
    enum class ESentenceReception : uint8 { Empty, Whole, Streaming, Complete, Failed };

    /// 一句的接收进度
    struct FSentenceReceipt
    {
        /// 收到哪了
        ESentenceReception State = ESentenceReception::Empty;

        /// 下一段的序号，乱序就判失败
        int32 NextChunkIndex = 0;

        /// 已收到的声音字节数
        int32 PcmBytes = 0;

        /// 这句不会再有数据了
        bool IsFinished() const
        {
            return State == ESentenceReception::Whole || State == ESentenceReception::Complete || State == ESentenceReception::Failed;
        }
    };

    /// 一个请求：谁发的、发了什么、HTTP 连接、收到哪了
    struct FPending
    {
        /// 发请求的 NPC
        TWeakObjectPtr<UDeliveryNpcAgentComponent> Agent;

        /// 请求内容
        FDeliveryNpcDecisionRequest Request;

        /// HTTP 连接
        TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Http;

        /// 超时时间
        double Deadline = 0;

        /// 发往的地址
        FString Endpoint;

        /// 后台线程收到的一行行 JSON 先放这里，游戏线程的 Tick 再逐行取出交给 ReadStream
        TSharedPtr<FDeliveryNpcStreamBuffer, ESPMode::ThreadSafe> Stream;

        /// HTTP 已经结束
        bool bHttpFinished = false;

        /// HTTP 成功结束
        bool bHttpSucceeded = false;

        /// 整轮回复已经交完
        bool bReplyFinished = false;

        /// 已经收到模型的建议
        bool bHasProposal = false;

        /// 模型已经说完
        bool bModelFinished = false;

        /// 模型说一共几句
        int32 SentenceCount = 0;

        /// 三句各自收到哪了；文字和声音本身直接交给 NPC 的表现组件
        FSentenceReceipt Sentences[3];
    };

    // ── 发送 ──

    /// 发出请求：组 JSON、POST、挂上逐行接收
    void Dispatch(FGuid RequestId);

    /// 非流式回答或失败：把结果一次交给 Agent
    void Complete(FGuid RequestId, const FString& Body /* 后端返回的完整内容 */, FName Error /* 失败原因，成功时为 None */);

    /// 删掉请求，还没结束的通知后端取消
    void Remove(FGuid RequestId);

    /// 关闭时取消全部请求
    void Shutdown();

    // ── 读流 ──

    /// 解析一行 JSON，按它的类型调用 Agent 对应的函数：模型建议、整句、一句声音的开头/分段/结尾、模型说完、诊断信息
    void ReadStream(FGuid RequestId, const FString& Line);

    /// 整轮收完，通知 Agent
    void FinishStreamRequest(FGuid RequestId);

    /// 数据不合规，这轮按失败结束
    void RejectStream(FGuid RequestId, const TCHAR* Reason);

    // ── 发送 ──
    // 所有请求和排队顺序

    /// 所有没结束的请求
    TMap<FGuid, FPending> Pending;

    /// 等着发出的顺序
    TArray<FGuid> Queue;

    /// 世界开始时读好的行为表
    FDeliveryNpcBehaviorCatalog BehaviorCatalog;

    /// 世界编号，换图或重开后，旧请求的回答按它丢掉
    FGuid WorldEpoch;

    /// 正在关闭，不再接新请求
    bool bShuttingDown = false;
    friend class FDeliveryAiInfrastructureTest;
    friend class FDeliveryAiBehaviorTreeTest;
    friend class FDeliveryNpcAudioStreamingTest;
    friend class FDeliveryNpcLiveAudioCommand;
    friend class FDeliveryNpcSentenceAudioProtocolTest;
    friend class FDeliveryNpcModelSpeechSeparationTest;
    friend class FDeliveryNpcDecisionTraceTest;
};
