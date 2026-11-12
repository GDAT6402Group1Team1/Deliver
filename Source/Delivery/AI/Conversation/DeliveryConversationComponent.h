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
    bool CanTalkTo(const ADeliveryNpcCharacter* Resident) const;
private:
    void OpenNearest();
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
