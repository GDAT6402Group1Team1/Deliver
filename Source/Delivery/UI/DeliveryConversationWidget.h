#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DeliveryConversationWidget.generated.h"
class ADeliveryNpcCharacter;
class UEditableTextBox;
class UTextBlock;
class UButton;
class UScrollBox;

// 显示输入和回复，将玩家消息交给交谈组件发送，面板的打开和关闭由 HUD 处理
UCLASS()
class DELIVERY_API UDeliveryConversationWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetResident(ADeliveryNpcCharacter* Value);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& KeyEvent) override;
    virtual void NativeTick(const FGeometry& Geometry,float Dt) override;
    // 蓝图保存布局和样式，这些名称把界面控件与交谈操作对应起来
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UEditableTextBox> Input;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UTextBlock> Reply;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UTextBlock> SpeakerName;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UScrollBox> ReplyScroll;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> SendButton;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> ApologizeButton;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> ProvokeButton;
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> CloseButton;
private:
    friend class FDeliveryConversationFeedbackTest;
    UFUNCTION() void Send();
    UFUNCTION() void Apologize();
    UFUNCTION() void Provoke();
    UFUNCTION() void Close();
    void SendText(const FString& Text);
    void RefreshReply();
    // 保存当前交谈的输入和回复，关闭面板或切换 NPC 后重新开始记录
    FString LastSubmittedText;
    FString LastReplyText;
    FGuid SubmittedEventId;
    FGuid DisplayedTurnId;
    FName SubmissionResult;
    FString InputNotice;
    double InputNoticeUntil=0;
    double ReplyWaitUntil=0;
    bool bAwaitingReply=false;
    bool bReplyTimedOut=false;
    TWeakObjectPtr<ADeliveryNpcCharacter> Resident;
};
