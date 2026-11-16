#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Types/SlateEnums.h"
#include "Speech/Recognition/DeliverySpeechTurn.h"
#include "DeliveryConversationWidget.generated.h"
class ADeliveryNpcCharacter;
class UEditableTextBox;
class UMultiLineEditableTextBox;
class UTextBlock;
class UButton;
class UScrollBox;
class UBorder;
class USizeBox;
class UWidgetAnimation;
class UDeliveryConversationComponent;
class UDeliverySpeechInputComponent;

/**
 * @brief 和 NPC 聊天的对话框 WBP_Conversation 的父类：玩家在这里打字、开麦、看 NPC 的回话
 *
 * 玩家按 IA_Talk 后，服务器同意交谈，HUD 的 OpenResidentConversation 创建这个面板并调 SetResident。
 * 玩家打完字按回车或点发送，Send 取出文字交给 SendText，再交给交谈组件的 Submit 发往服务器。
 * 之后每帧 RefreshReply 读交谈组件里这句话有没有送到、NPC 有没有开口，把 NPC 的台词显示出来；
 * 等太久没回话会显示重试按钮。开麦按钮交给语音输入组件，识别出的文字也走交谈组件
 * 布局和样式都在蓝图里，这里只管操作和显示什么
 */
UCLASS()
class DELIVERY_API UDeliveryConversationWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    // ── 打开 ──
    // HUD 创建面板后依次调用：SetResident 指定聊天对象 → SetAvailableWidth 定宽 → FocusInput 把光标放进输入框

    /// @brief 指定和谁聊天；换人时清空上一位的聊天记录
    void SetResident(ADeliveryNpcCharacter* Value);

    /// @brief 例句列表是否展开
    bool IsSuggestionsVisible() const;

    /// @brief 设置面板宽度；HUD 传进来的已经按屏幕缩放换算好，高度随内容变
    void SetAvailableWidth(float Width);

    /// @brief 把键盘焦点放进输入框，玩家可以直接打字
    void FocusInput();
protected:
    // ── 生命周期 ──
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& KeyEvent) override;
    virtual void NativeTick(const FGeometry& Geometry,float Dt) override;

    // ── 控件 ──
    // WBP_Conversation 里同名的控件会自动绑到这些变量上；蓝图里没有的控件就是空，不影响其他功能

    /// 玩家打字的输入框
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UEditableTextBox> Input;

    /// 例句列表，点一句就能发出去
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UMultiLineEditableTextBox> Suggestions;

    /// NPC 的回话
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UTextBlock> Reply;

    /// NPC 的名字
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UTextBlock> SpeakerName;

    /// 玩家刚发出那句话的气泡
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UBorder> SubmittedLine;

    /// 控制面板宽度的外框
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<USizeBox> ConversationWidth;

    /// 回话太长时可以滚动
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UScrollBox> ReplyScroll;

    /// 发送按钮
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> SendButton;

    /// 展开或收起例句
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> SuggestionToggle;

    /// 关闭按钮
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> CloseButton;

    /// NPC 没回话时出现的重试按钮
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> RetryButton;

    /// 等 NPC 回话时转圈的提示
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UWidget> WaitingIndicator;

    /// 开麦按钮，点一下开、再点一下关
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> VoiceToggleButton;

    /// 按住说话的按钮
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UButton> VoiceHoldButton;

    /// 开麦按钮上的文字
    UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UTextBlock> VoiceToggleLabel;

    /// 发出一句时播的动画；动画本身做在蓝图里
    UPROPERTY(Transient,meta=(BindWidgetAnimOptional)) TObjectPtr<UWidgetAnimation> SendFeedback;

    /// NPC 回话出现时播的动画
    UPROPERTY(Transient,meta=(BindWidgetAnimOptional)) TObjectPtr<UWidgetAnimation> MessageArrive;

    /// 等回话时循环播的动画
    UPROPERTY(Transient,meta=(BindWidgetAnimOptional)) TObjectPtr<UWidgetAnimation> WaitingPulse;
private:
    friend class FDeliveryConversationFeedbackTest;

    // ── 打字 ──
    // 按钮和输入框的回调：发送、重试、展开例句、关闭

    /// 点发送：取出输入框的文字发出去
    UFUNCTION() void Send();

    /// 点重试：把上一句再发一次
    UFUNCTION() void Retry();

    // ── 开麦 ──
    // 开麦按钮交给语音输入组件，识别出的文字由交谈组件提交，这里只更新按钮状态

    /// 点击开麦按钮：开或关
    UFUNCTION() void ToggleVoice();

    /// 按住说话按钮的回调；连麦现在一直开着，这个函数什么都不做，只为旧蓝图保留
    UFUNCTION() void BeginVoice();

    /// 松开说话按钮的回调；什么都不做，只为旧蓝图保留
    UFUNCTION() void FinishVoice();

    /// 按开麦状态刷新按钮文字
    UFUNCTION() void RefreshVoice();

    // ── 打字 ──

    /// 输入框里按回车时发送
    UFUNCTION() void CommitInput(const FText& Text,ETextCommit::Type Method);

    /// 展开或收起例句
    UFUNCTION() void ToggleSuggestions();

    /// 点关闭：请 HUD 关掉这个面板
    UFUNCTION() void Close();

    // ── 显示 ──
    // SendText 发出后，每帧 RefreshReply 刷新回话；交谈组件的通知更新这句话的编号和结果

    /// 交给交谈组件的 Submit；发得太快时在输入框下方提示
    void SendText(const FString& Text);

    /// 每帧：读这句话的送达结果、NPC 正在说的话，更新气泡、回话和等待提示
    void RefreshReply();

    /// 清空聊天记录，换人或关面板时调用
    void ResetConversation();

    /// 更新等待提示和重试按钮
    void RefreshFeedback(bool bCanRetry /* true 时显示重试按钮 */);

    /// 交谈组件通知：这句已经发出，记下编号开始等回话
    void HandleSubmitted(ADeliveryNpcCharacter* Npc,FGuid MessageId /* 本机给这句话的编号 */,const FString& Text);

    /// 交谈组件通知：服务器给了正式编号，改用它找回话
    void HandleSubmissionBound(FGuid ClientId /* 本机编号 */,FGuid MessageId /* 服务器编号 */);

    /// 交谈组件通知：开麦失败，在输入框下方显示 3 秒原因，比如二狗正在听别的玩家说话
    void HandleVoiceError(FName Reason);

    // ── 显示 ──
    // 当前这次聊天的记录，关面板或换人后清空

    /// 订阅了通知的交谈组件
    TWeakObjectPtr<UDeliveryConversationComponent> BoundConversation;

    /// 订阅了通知的语音输入组件
    TWeakObjectPtr<UDeliverySpeechInputComponent> BoundSpeech;

    /// 按住说话这一句的编号
    FDeliverySpeechTurnToken ButtonVoiceToken;

    /// 玩家刚发出的那句
    FString LastSubmittedText;

    /// NPC 上一次显示的回话
    FString LastReplyText;

    /// 玩家这句话的编号，靠它找到 NPC 回的是不是这句
    FGuid SubmittedEventId;

    /// 正在显示的 NPC 台词编号
    FGuid DisplayedTurnId;

    /// 这句话的送达结果
    FName SubmissionResult;

    /// 输入框下方的提示文字
    FString InputNotice;

    /// 提示什么时候消失
    double InputNoticeUntil=0;

    /// 最多等回话到什么时候，过了就显示重试
    double ReplyWaitUntil=0;

    /// 正在等回话
    bool bAwaitingReply=false;

    /// 等回话超时了
    bool bReplyTimedOut=false;

    /// 正在和谁聊天
    TWeakObjectPtr<ADeliveryNpcCharacter> Resident;
};
