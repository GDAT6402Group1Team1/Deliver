#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryConversationComponent.generated.h"
class ADeliveryNpcCharacter;

// 玩家控制器持有这个组件，负责将玩家的话发送到服务器
// 服务器检查玩家与 NPC 的距离、遮挡和发送间隔，通过后才交给 NPC
UCLASS(ClassGroup=(Delivery))
class DELIVERY_API UDeliveryConversationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryConversationComponent();
    void BindInput();
    // 返回 true 表示消息已发出，接收结果由服务器另行通知
    UFUNCTION(BlueprintCallable) bool Submit(ADeliveryNpcCharacter* Resident,const FString& Text);
    UFUNCTION(Client,Reliable) void ClientOpenConversation(ADeliveryNpcCharacter* Resident);
    UFUNCTION(Server,Reliable) void ServerOpenConversation(ADeliveryNpcCharacter* Resident);
    UFUNCTION(Server,Reliable) void ServerSubmit(ADeliveryNpcCharacter* Resident,const FString& Text,FGuid SpeechId);
    UFUNCTION(Client,Reliable) void ClientResult(FName Result,FGuid SpeechId);
    // 玩家刚走到 NPC 附近时，通过自己的控制器请求正在播放的语音
    UFUNCTION(Server,Reliable) void ServerRequestNpcAudio(ADeliveryNpcCharacter* Npc,FGuid TurnId,int32 Offset=0);
    UFUNCTION(Client,Reliable) void ClientReceiveNpcAudio(ADeliveryNpcCharacter* Npc,FGuid TurnId,int32 Offset,int32 Total,const TArray<uint8>& Data);
    UFUNCTION(BlueprintPure) FName GetLastResult() const { return FPlatformTime::Seconds()<ResultExpiresAt ? LastResult : NAME_None; }
    FGuid GetLastSubmissionId() const { return LastSubmissionId; }
    // 交谈窗口按发言编号读取结果，短提示消失后仍能说明这句话有没有送到
    FName GetSubmissionResult(FGuid SpeechId) const { return SpeechId.IsValid() && SpeechId==LastSubmissionId ? LastResult : NAME_None; }
    bool CanSendNow() const { return FPlatformTime::Seconds()>=NextLocalSendAt; }
    bool CanTalkTo(const ADeliveryNpcCharacter* Resident) const;
    // 选择本机镜头前方可交谈的人物，优先靠近画面中心的人；范围内无人时返回空
    ADeliveryNpcCharacter* GetAimedNpc() const;
    // 快捷发言点明现场另一位 NPC，具体反应仍交给模型决定
    FString GetProvokeText(const ADeliveryNpcCharacter* Npc) const;
private:
    void OpenAimedConversation();
    ADeliveryNpcCharacter* FindNpcInView(const FVector& ViewLocation,const FVector& ViewDirection) const;
    friend class FDeliveryNpcConversationAimTest;
    friend class FDeliveryConversationFeedbackTest;
    double NextSendAt=0;
    double NextAudioRequestAt=0;
    // 快捷按钮和文本框使用相同的发送间隔，服务器再检查一次
    double NextLocalSendAt=0;
    // 用发言编号找到对应回复，交谈距离和遮挡由服务器检查
    FGuid LastSubmissionId;
    FName LastResult;
    // 按现实经过的时间关闭提示，游戏暂停时继续计时
    double ResultExpiresAt=0;
};
