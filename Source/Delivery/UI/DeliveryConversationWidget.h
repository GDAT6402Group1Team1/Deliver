#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryConversationWidget.generated.h"
class ADeliveryResidentCharacter;
class UEditableTextBox;
class UTextBlock;

// 控件只显示和提交文本，HUD 管理面板，交谈组件负责网络与校验
UCLASS()
class DELIVERY_API UDeliveryConversationWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetResident(ADeliveryResidentCharacter* Value);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeTick(const FGeometry& Geometry,float Dt) override;
private:
    UFUNCTION() void Send();
    UFUNCTION() void Apologize();
    UFUNCTION() void Provoke();
    UFUNCTION() void Close();
    void SendText(const FString& Text);
    void RefreshReply();
    // 只记录当前面板的本机回显，关闭或换居民后不沿用上一轮等待
    FString LastSubmittedText;
    FString LastReplyText;
    FGuid SubmittedEventId;
    double ReplyWaitUntil=0;
    bool bAwaitingReply=false;
    bool bReplyTimedOut=false;
    TWeakObjectPtr<ADeliveryResidentCharacter> Resident;
    UPROPERTY() TObjectPtr<UEditableTextBox> Input;
    UPROPERTY() TObjectPtr<UTextBlock> Reply;
};
