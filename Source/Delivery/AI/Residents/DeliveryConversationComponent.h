#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryConversationComponent.generated.h"
class ADeliveryResidentCharacter;

// 玩家控制器持有交谈入口，步行和驾驶共用 RPC；服务器复核距离、遮挡、频率和对象
UCLASS(ClassGroup=(Delivery))
class DELIVERY_API UDeliveryConversationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryConversationComponent();
    void BindInput();
    UFUNCTION(BlueprintCallable) void Submit(ADeliveryResidentCharacter* Resident,const FString& Text);
    UFUNCTION(Client,Reliable) void ClientOpenConversation(ADeliveryResidentCharacter* Resident);
    UFUNCTION(Server,Reliable) void ServerOpenConversation(ADeliveryResidentCharacter* Resident);
    UFUNCTION(Server,Reliable) void ServerSubmit(ADeliveryResidentCharacter* Resident,const FString& Text);
    UFUNCTION(Client,Reliable) void ClientResult(FName Result);
    UFUNCTION(BlueprintPure) FName GetLastResult() const { return FPlatformTime::Seconds()<ResultExpiresAt ? LastResult : NAME_None; }
    bool CanTalkTo(const ADeliveryResidentCharacter* Resident) const;
private:
    void OpenNearest();
    double NextSendAt=0;
    // 快捷按钮和文本发送共用本机防连点，服务器仍独立验证
    double NextLocalSendAt=0;
    FName LastResult;
    // 本机提示使用真实时间到期，不随游戏暂停永久保留
    double ResultExpiresAt=0;
};
