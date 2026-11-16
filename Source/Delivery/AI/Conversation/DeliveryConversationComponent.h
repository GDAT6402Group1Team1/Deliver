#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AI/Conversation/DeliveryConversationTurnSubsystem.h"
#include "Speech/Recognition/DeliverySpeechTurn.h"
#include "AI/Decision/DeliveryNpcDecisionTrace.h"
#include "TimerManager.h"
#include "DeliveryConversationComponent.generated.h"
class ADeliveryNpcCharacter;
class UDeliverySpeechInputComponent;
class UDeliveryNpcAgentComponent;

/// @brief 服务器为发言分配编号后，通知面板更新这句话的编号
DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryConversationSubmissionBound, FGuid /* 本机编号 */, FGuid /* 服务器编号 */);

/// @brief 本机发出文字后，通知面板显示原话并等待送达结果
DECLARE_MULTICAST_DELEGATE_ThreeParams(FDeliveryConversationSubmitted, ADeliveryNpcCharacter* /* 交谈对象 */, FGuid /* 发言编号 */, const FString& /* 原话 */);

/// @brief 语音输入失败时，通知面板显示原因
DECLARE_MULTICAST_DELEGATE_OneParam(FDeliveryConversationVoiceError, FName /* 失败原因 */);

/**
 * @brief 玩家跟 NPC 说话时，负责把这句话从玩家的电脑送到服务器上的 NPC
 *
 * 这个组件挂在玩家控制器上，每个玩家一份。玩家对着一位 NPC 按 IA_Talk 时，
 * OpenAimedConversation 在镜头前方挑出最近画面中心的那位，请服务器打开交谈；
 * 服务器在 ServerOpenConversation 里再查一遍玩家能不能跟他说话，同意后调用 ClientOpenConversation，
 * HUD 随即打开对话框。
 *
 * 玩家在对话框里打完字，对话框调用 Submit。Submit 先在本机检查是不是发得太快，
 * 然后调用 ServerSubmit 把文字发给服务器。服务器检查玩家没晕倒、离 NPC 不超过 600 cm、
 * 中间没有墙挡着、两句话之间隔够了 1 秒，都通过就调用 DeliverFinalText，
 * 它调用 NPC 的 HearSpeech，NPC 从这里开始想怎么回。最后服务器用 ClientResult
 * 告诉玩家这句话 NPC 听到了没有。
 *
 * 开麦说话只能对玩家自己的二狗。语音识别发现玩家开口时调用 HandleVoiceStarted，
 * 本机请服务器为这句话登记一轮；玩家说完、识别出整句文字后，SubmitReadyVoiceText 调用
 * ServerSubmitVoice 把文字发给服务器，服务器同样调用 DeliverFinalText 交给二狗。
 *
 * 另外还有两件小事：玩家走近一位正在说话的 NPC 时，向服务器要这句话已经播出的声音（补听）；
 * 开发时按键打开 AI 检查器，服务器把这位 NPC 每次问模型的过程推送过来显示
 */
UCLASS(ClassGroup=(Delivery))
class DELIVERY_API UDeliveryConversationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryConversationComponent();

    // ── 选人 ──
    // 控制器创建时调用 BindInput 绑好按键；对话框打开时调用 SetConversationTarget 记下对象，关闭时清掉

    /// @brief 本机绑定 IA_Talk 和开麦按键
    void BindInput();

    /// @brief 记下交谈面板当前对着的 NPC
    void SetConversationTarget(ADeliveryNpcCharacter* Resident);

    /// @brief 对话框关闭或换人时清掉对象；只有传入的就是当前对象才清，避免旧对话框关闭时清掉新对象
    void ClearConversationTarget(ADeliveryNpcCharacter* Resident);

    /// @brief 取同一控制器上的语音输入组件
    UDeliverySpeechInputComponent* GetSpeechInput() const;

    // ── 开麦 ──
    // 开麦永远是对玩家自己的二狗说，对话框开着谁、镜头对着谁都不影响
    // 下面几个函数回答：二狗是谁、现在能不能对它开麦、某一轮话是不是开麦说的

    /// @brief 找到玩家自己的同伴
    /// @return 还没收服二狗，或二狗已经被销毁时返回空
    ADeliveryNpcCharacter* ResolveVoiceCompanion() const;

    /// @brief 这位 NPC 能不能开麦说话：必须是自家同伴、玩家没晕倒，不看距离和视线
    bool CanVoiceTalkTo(const ADeliveryNpcCharacter* Resident) const;

    /// @brief 上一句已经识别出文字、还没发给服务器时返回 true
    /// 语音识别看到它为 true 就先不开始识别下一句，免得两句话的文字串在一起
    bool HasPendingVoiceSubmission() const { return LocalVoiceToken.IsValid() && bVoiceTextReady; }

    /// @brief 服务器：这一轮是不是这个玩家开麦说的，而且 NPC 还没回完
    bool IsVoiceTurn(const FDeliveryConversationTurnHandle& Handle) const;

    /// @brief 这条 NPC 回复是不是在回答开麦说的那句话
    /// 是的话，二狗的声音直接传到主人耳边，主人离得再远也听得到
    bool IsVoiceReplyMessage(FGuid MessageId,const ADeliveryNpcCharacter* ExpectedNpc=nullptr) const;

    // ── 配置 ──

    /// 开麦说话时，玩家身边 900 cm 内最多再有几位 NPC 也能听到（只记住，不回话）
    UPROPERTY(EditDefaultsOnly,Category="Delivery|Voice",meta=(ClampMin="0",ClampMax="8"))
    int32 VoiceOverhearLimit=2;

    // ── 面板 ──
    // 对话框订阅下面三个通知：本机刚发出一句、服务器给了这句正式编号、开麦失败

    /// 服务器给这句话分配了正式编号
    FDeliveryConversationSubmissionBound OnSubmissionBound;

    /// 本机刚发出一句文字
    FDeliveryConversationSubmitted OnSubmitted;

    /// 开麦失败，带上原因
    FDeliveryConversationVoiceError OnVoiceInputError;

    // ── 检查器 ──
    // 开发用：调用 WatchNpcDiagnostics 后，服务器先发一份这位 NPC 最近一次问模型的记录，之后每次变化再发
    // 调用 StopWatchingNpcDiagnostics 后，本机把观察次数加一，之后迟到的旧记录对不上次数，直接丢掉

    /// @brief 开始观察这位 NPC 的决策记录
    void WatchNpcDiagnostics(ADeliveryNpcCharacter* Resident);

    /// @brief 停止观察
    void StopWatchingNpcDiagnostics();

    /// 收到新的决策记录时通知检查器界面
    FDeliveryNpcDecisionTraceChanged OnDiagnosticsChanged;

    /// @brief 最近一次开麦失败的原因，提示过期后返回 None
    FName GetVoiceInputError() const { return FPlatformTime::Seconds()<VoiceErrorExpiresAt ? LastVoiceInputError : NAME_None; }

    // ── 打字 ──
    // 打开对话框：OpenAimedConversation 选人 → ServerOpenConversation 服务器再查一遍 → ClientOpenConversation 打开
    // 发一句话：Submit 本机检查 → ServerSubmit 服务器检查 → DeliverFinalText 交给 NPC → ClientResult 告诉玩家结果

    /// @brief 提交玩家打出的文字
    /// @return 已经发给服务器时返回 true；NPC 听没听到要等服务器的 ClientResult
    UFUNCTION(BlueprintCallable) bool Submit(ADeliveryNpcCharacter* Resident,const FString& Text);

    /// @brief 客户端：服务器同意交谈后，清空上次结果，由 HUD 打开交谈面板
    UFUNCTION(Client,Reliable) void ClientOpenConversation(ADeliveryNpcCharacter* Resident);

    /// @brief 服务器：再查一遍玩家能不能跟这位 NPC 说话，能就让玩家打开对话框，并让主角转身面向 NPC
    UFUNCTION(Server,Reliable) void ServerOpenConversation(ADeliveryNpcCharacter* Resident,uint64 FacingSequence=0 /* 本机转身序号 */,APawn* FacingPawn=nullptr /* 要转身的角色 */);

    /// @brief 服务器：检查两句话间隔够不够 1 秒、文字是不是太长、玩家还能不能跟 NPC 说话
    /// 都通过就向轮次子系统登记这一轮，再调用 DeliverFinalText；不通过就用 ClientResult 把原因发回玩家
    UFUNCTION(Server,Reliable) void ServerSubmit(ADeliveryNpcCharacter* Resident,const FString& Text,FGuid SpeechId /* 本机给这句话的编号 */,uint64 InputSequence /* 这个玩家第几次输入，旧的作废 */,uint64 FacingSequence=0 /* 本机转身序号 */,APawn* FacingPawn=nullptr /* 要转身的角色 */);

    /// @brief 客户端：服务器给这句话分配了正式编号；本机之后用这个编号认出 NPC 的回复是不是在回答这句
    UFUNCTION(Client,Reliable) void ClientSubmissionBound(FGuid ClientToken /* 本机编号 */,FGuid MessageId /* 服务器编号 */);

    /// @brief 客户端：收到这句话的结果，Heard 表示 NPC 听到了，TooFast、Unavailable 等表示没送到
    UFUNCTION(Client,Reliable) void ClientResult(FName Result,FGuid SpeechId);

    // ── 补听 ──
    // NPC 的声音只发给当时在附近的玩家；玩家后来才走近时，这句话开头那部分他没收到
    // 于是本机调用 ServerRequestNpcAudio 向服务器要，服务器分段发回 ClientReceiveNpcAudio，
    // 每 0.25 秒最多发一段、每段最多 2 KB，免得占满网络；收到的声音交给 NPC 的表现组件接着播

    /// @brief 服务器：发回某句 NPC 语音从 Offset 开始的一段，每 0.25 秒最多一段
    UFUNCTION(Server,Reliable) void ServerRequestNpcAudio(ADeliveryNpcCharacter* Npc,FGuid TurnId /* 要哪一句 */,int32 Offset=0 /* 从第几个字节开始 */);

    /// @brief 客户端：收到一段声音，交给 NPC 的表现组件接到已有的部分后面
    /// @param Total 整句压缩后有多少字节，用来判断收全没有
    UFUNCTION(Client,Reliable) void ClientReceiveNpcAudio(ADeliveryNpcCharacter* Npc,FGuid TurnId /* 哪一句 */,int32 Offset /* 这段从第几个字节开始 */,int32 Total /* 整句多少字节 */,const TArray<uint8>& Data);

    // ── 面板 ──
    // 交谈面板每帧调用下面几个函数，按 LastResult（Sending、Heard、TooFast 等）决定显示哪条提示

    /// @brief 最近一句的结果，提示显示 2.5 秒后返回 None
    UFUNCTION(BlueprintPure) FName GetLastResult() const { return FPlatformTime::Seconds()<ResultExpiresAt ? LastResult : NAME_None; }

    /// @brief 最近一句的编号
    FGuid GetLastSubmissionId() const { return LastSubmissionId; }

    /// @brief 按编号取这句话的结果；提示消失后对话框仍能据此显示这句话有没有送到
    FName GetSubmissionResult(FGuid SpeechId) const { return SpeechId.IsValid() && SpeechId==LastSubmissionId ? LastResult : NAME_None; }

    /// @brief 离上一句已经过了 1.1 秒时返回 true；对话框据此决定发送按钮能不能点
    bool CanSendNow() const { return FPlatformTime::Seconds()>=NextLocalSendAt; }

    /// @brief 能不能跟这位 NPC 交谈：NPC 没倒地、600 cm 内、玩家没晕倒、中间无遮挡
    /// 本机选人和服务器检查调用的是同一个函数，所以本机能选中的 NPC，服务器检查时条件也一样
    bool CanTalkTo(const ADeliveryNpcCharacter* Resident) const;

    /// @brief 选本机镜头前方可交谈的 NPC，越靠画面中心越优先
    /// @return 范围内没人时返回空
    ADeliveryNpcCharacter* GetAimedNpc() const;

    /// @brief 生成一句拱火的例句放进对话框，比如提到附近 800 cm 内的另一位 NPC；NPC 听了怎么反应由模型决定
    FString GetProvokeText(const ADeliveryNpcCharacter* Npc) const;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime,ELevelTick TickType,FActorComponentTickFunction* ThisTickFunction) override;
private:
    // ── 检查器 ──
    // 按检查器键 → ToggleInspector 让 HUD 打开界面；服务器订阅这位 NPC 的决策记录，
    // 记录一变就由 HandleInspectorTrace 调用 ClientNpcDiagnostics 发给玩家；
    // 客户端收到后，观察次数、世界编号或修订号对不上的旧记录直接丢掉

    /// 本机绑定打开检查器的按键，Shipping 版本没有
    void BindInspectorInput();

    /// 按键回调，让 HUD 打开或关闭检查器
    void ToggleInspector();

    /// 组件结束时解除检查器的所有绑定
    void ShutdownInspector();

    /// 服务器：不再订阅原来那位 NPC 的决策记录
    void UnbindServerInspector();

    /// 这位 NPC 能不能看：30 米内就行，倒地和跑动中的也可以
    bool CanInspectNpc(const ADeliveryNpcCharacter* Resident) const;

    /// 服务器：NPC 决策记录变了就发给玩家；玩家走出 30 米就不再发
    void HandleInspectorTrace(const FDeliveryNpcDecisionTrace& Trace);

    /// 服务器：订阅这位 NPC 的决策记录，先发一份当前的
    UFUNCTION(Server,Reliable) void ServerWatchNpcDiagnostics(ADeliveryNpcCharacter* Resident,uint64 Generation /* 本机第几次开始观察 */);

    /// 客户端：收到记录，丢掉旧版本号、旧世界和旧修订号的，再交给检查器界面
    UFUNCTION(Client,Reliable) void ClientNpcDiagnostics(uint64 Generation /* 这份记录属于第几次观察 */,const FDeliveryNpcDecisionTrace& Trace);

    /// 本机第几次开始观察；收到的记录带着这个次数，对不上说明是上一次观察的，直接丢掉
    uint64 LocalInspectorGeneration=0;

    /// 服务器记下的观察版本号
    uint64 ServerInspectorGeneration=0;

    /// 本机正在看的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> LocalInspectorTarget;

    /// 服务器正在推送的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> ServerInspectorTarget;

    /// 开始观察时本机控制的角色，换角色后重新开始
    TWeakObjectPtr<APawn> LocalInspectorPawn;

    /// 服务器记下的观察者角色
    TWeakObjectPtr<APawn> ServerInspectorPawn;

    /// 收到的记录属于哪个世界
    FGuid InspectorWorldEpoch;

    /// 收到的最新修订号，更旧的丢掉
    int32 InspectorRevision=-1;

    /// 服务器订阅决策记录的句柄
    FDelegateHandle InspectorTraceHandle;

    /// 检查器按键绑在哪个输入组件上
    TWeakObjectPtr<class UEnhancedInputComponent> BoundInspectorInput;

    /// 加映射用的输入子系统
    TWeakObjectPtr<class UEnhancedInputLocalPlayerSubsystem> InspectorInputSubsystem;

    /// 检查器按键的映射
    TWeakObjectPtr<class UInputMappingContext> InspectorMapping;

    /// 检查器按键的绑定句柄
    uint32 InspectorInputHandle=0;

    /// 映射是不是这个组件加的，是的话结束时由它移除
    bool bOwnsInspectorMapping=false;

    // ── 开麦 ──
    // 一句话按这个顺序走：
    // 1 语音识别发现玩家开口，调用 HandleVoiceStarted，本机调用 ServerBeginVoice；
    //   服务器确认说话对象是这位玩家的二狗，向轮次子系统登记一轮，用 ClientVoiceGranted 把这轮的编号发回来
    // 2 玩家停顿，HandleVoiceReleased 调用 ServerFinishVoice，服务器开始最多等 5 秒识别文字
    // 3 识别出整句文字，HandleVoiceText 存下来，SubmitReadyVoiceText 等编号和文字都齐了，调用 ServerSubmitVoice
    // 4 服务器调用 DeliverFinalText 把文字交给二狗
    // 玩家开口时如果正在上下车，服务器暂时认不出说话的是不是原来那个角色；
    // 这时文字先存进 Deferred 开头的变量，每 0.05 秒调用 FlushDeferredVoiceSubmission 再试，最多等 2 秒

    /// 服务器：确认说话对象是这位玩家的二狗，向轮次子系统登记一轮，成功就调用 ClientVoiceGranted
    UFUNCTION(Server,Reliable) void ServerBeginVoice(ADeliveryNpcCharacter* Resident,FGuid ClientToken /* 本机给这句话的编号 */,uint64 InputSequence /* 这个玩家第几次输入 */,uint64 FacingSequence=0 /* 本机转身序号 */,APawn* FacingPawn=nullptr /* 要转身的角色 */);

    /// 服务器：玩家说完了，开始计时等识别文字，最多 5 秒
    UFUNCTION(Server,Reliable) void ServerFinishVoice(FGuid ClientToken /* 本机编号 */,uint64 InputSequence /* 输入序号 */);

    /// 服务器：取消这一轮开麦
    UFUNCTION(Server,Reliable) void ServerCancelVoice(FGuid ClientToken /* 本机编号 */,uint64 InputSequence /* 输入序号 */);

    /// 服务器：收到识别出的文字
    /// 玩家正在上下车、认不出是不是原来的角色时，先把文字存进 Deferred 变量稍后再试；
    /// 离上一句不到 1.1 秒时也先存着，等够间隔再交；都没问题就调用 DeliverFinalText
    UFUNCTION(Server,Reliable) void ServerSubmitVoice(ADeliveryNpcCharacter* Resident,const FString& Text,FDeliveryConversationTurnHandle Handle);

    /// 本机：服务器同意这一轮，Handle 存进 LocalVoiceHandle，再调用 SubmitReadyVoiceText（文字已经识别出来就马上提交）
    UFUNCTION(Client,Reliable) void ClientVoiceGranted(FGuid ClientToken /* 本机编号 */,uint64 InputSequence /* 输入序号 */,FDeliveryConversationTurnHandle Handle /* 服务器发的编号牌 */);

    /// 客户端：服务器拒绝这一轮，显示原因
    UFUNCTION(Client,Reliable) void ClientVoiceRejected(FGuid ClientToken /* 本机编号 */,uint64 InputSequence /* 输入序号 */,FName Reason);

    /// 本机订阅语音输入组件的事件，并绑定开麦按键
    void BindSpeechInput();

    // 开麦按键是开关：按一下打开连续开麦（同时关掉对话框），再按一下关闭；松开按键什么都不做
    void StartVoiceFromKey();

    /// 松开开麦键
    void FinishVoiceFromKey();

    /// 按键事件被取消时只清掉按键记录，正在识别的这句照常进行
    void CancelVoiceKeyHold();

    /// 主动放弃当前这句：通知服务器取消这一轮，清空本机这句的文字和编号
    void CancelVoiceFromKey();

    /// 语音识别发现玩家开口：记下这句的编号和对象，调用 ServerBeginVoice 请服务器登记一轮
    void HandleVoiceStarted(const FDeliverySpeechTurnToken& Token,ADeliveryNpcCharacter* Resident);

    /// 玩家停顿，这句说完了：调用 ServerFinishVoice
    void HandleVoiceReleased(const FDeliverySpeechTurnToken& Token);

    /// 语音识别取消了这句：调用 ServerCancelVoice，清空本机这句
    void HandleVoiceCancelled(const FDeliverySpeechTurnToken& Token);

    /// 识别出整句文字：存进 FinalVoiceText，调用 SubmitReadyVoiceText
    void HandleVoiceText(const FDeliverySpeechTurnToken& Token,ADeliveryNpcCharacter* Resident,const FString& Text);

    /// FinalVoiceText 和 LocalVoiceHandle 都有了才调用 ServerSubmitVoice
    /// 已经过了截止时间，或玩家上下车超过 2 秒还认不出角色，就放弃这句并提示玩家
    void SubmitReadyVoiceText();

    /// 清空本机这句的编号、文字和各个标记，之后迟到的识别结果对不上编号，不会被提交
    void ResetLocalVoice();

    /// 本机放弃这句：bNotifyServer 时调用 ServerCancelVoice，然后清空这句，并让对话框显示 3 秒原因
    void RejectLocalVoice(FName Reason,bool bNotifyServer /* true 时通知服务器取消这一轮 */);

    /// 服务器：ExpectedHandle 等于 ServerVoiceHandle 时清空服务器上记着的这句；不相等说明已经换了新的一句，不清
    void ResetServerVoice(const FDeliveryConversationTurnHandle& ExpectedHandle);

    /// 定时器到点：取出 Deferred 变量里的文字，再调用一次 ServerSubmitVoice 的服务器实现
    void FlushDeferredVoiceSubmission();

    /// 清空 Deferred 变量并停掉定时器
    void ClearDeferredVoiceSubmission();

    // ── 朝向 ──
    // 玩家开始跟 NPC 说话时，主角要转身面向他
    // 本机 PrepareConversationFacingInput 生成一个转身序号；服务器 RegisterConversationFacingInput 只记下最新序号，
    // 防止网络里先发后到的旧请求让主角转回去；服务器同意交谈后 ConfirmConversationFacingInput 才真的转身
    // 转身序号和说话序号分开计数，所以只是打开对话框不会影响开麦和回复

    /// 下一个输入序号，打字和开麦共用，回绕时跳过 0
    uint64 NextInputSequence();

    /// 客户端：生成新的朝向序号，让主角准备转身
    uint64 PrepareConversationFacingInput();

    /// 服务器：登记最新的朝向请求，更旧的序号直接忽略
    void RegisterConversationFacingInput(ADeliveryNpcCharacter* Resident,FGuid ClientToken /* 本机编号 */,uint64 Sequence /* 转身序号 */,APawn* PawnAtInput /* 按键时玩家控制的角色 */);

    /// 服务器：交谈被接受后，主角转身面向这位 NPC
    void ConfirmConversationFacingInput(ADeliveryNpcCharacter* Resident,FGuid ClientToken /* 本机编号 */,uint64 Sequence=0 /* 转身序号，0 表示用最新的 */);

    /// 取消两边的转身，组件结束时调用
    void CancelConversationFacingInput();

    /// 本机最近一次朝向序号
    uint64 LastLocalFacingSequence=0;

    /// 服务器收到的最新朝向序号
    uint64 LastServerFacingSequence=0;

    /// 服务器记下的这次转身属于哪句话
    FGuid ServerFacingClientToken;

    /// 本机准备转身的角色
    TWeakObjectPtr<class APawn> LocalFacingPawn;

    /// 服务器上要转身的角色
    TWeakObjectPtr<class APawn> ServerFacingPawn;

    /// 要面向的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> ServerFacingTarget;

    // ── 交付 ──
    // 打字和开麦最后都调用 DeliverFinalText，它调用 NPC 的 HearSpeech
    // 之后 WatchReplies 订阅 NPC 的回复结束通知，NPC 回完话就通知轮次子系统：这位 NPC 可以听别人说了
    // 玩家在 NPC 回话前又说了一句，旧的那轮被新的顶掉，CancelReplacedLeases 让 NPC 停止回答旧的那句
    // 一轮超时没完成时，TurnSubsystem 调用 HandleLeaseExpired，它调用 InterruptPlayerReply 打断 NPC 的回复，再调用 ClientResult 发回 ReplyFailed

    /// 服务器：订阅这位 NPC 回复结束的通知；回完时通知轮次子系统，这位 NPC 不再被这一轮占着
    void WatchReplies(ADeliveryNpcCharacter* Resident);

    /// 轮次子系统通知某一轮超时或 NPC 不在了：清空服务器上这句的开麦记录；
    /// 已经在等 NPC 回话的，让 NPC 停止回答，并用 ClientResult 告诉玩家 ReplyFailed；还在录音的，告诉玩家 Expired
    void HandleLeaseExpired(const FDeliveryConversationTurnLease& Lease,EDeliveryConversationLeaseEnd Reason);

    /// 玩家说了新的一句，旧的一轮被顶掉：让 NPC 停止回答旧的那句；旧那轮还在录音的，告诉玩家 Cancelled
    void CancelReplacedLeases(const TArray<FDeliveryConversationTurnLease>& Leases);

    /// 玩家开口时，取消这位 NPC 正在和别的 NPC 接的话
    void CancelNpcMessageTurns(ADeliveryNpcCharacter* Resident);

    /// @brief 服务器：把检查过的一句话交给 NPC
    ///
    /// 打字的话调用对象 NPC 的 HearSpeech；开麦的话调用二狗的 HearCompanionVoice。
    /// 然后让旁边的 NPC 也听到：打字时其他 NPC 都会听到，并且知道这句话是对谁说的；
    /// 开麦时只有玩家身边 900 cm 内最近的几位（VoiceOverhearLimit）听到，而且只记住、不回话。
    /// 最后写一条 ResidentConversation accepted 日志，用 ClientResult 告诉玩家 Heard
    /// @return NPC 听到了返回 true；NPC 不肯听时返回 false，并告诉玩家 NotHeard
    bool DeliverFinalText(ADeliveryNpcCharacter* Resident,const FString& Text,
        const FDeliveryConversationTurnHandle& Handle);

    // ── 选人 ──

    /// @brief 选镜头前 30 度内可交谈、没被挡住的 NPC，请求服务器打开交谈
    void OpenAimedConversation();

    /// 从镜头位置和方向找 30 度内、可交谈、没被挡住的一位
    ADeliveryNpcCharacter* FindNpcInView(const FVector& ViewLocation,const FVector& ViewDirection) const;
    friend class FDeliveryNpcConversationAimTest;
    friend class FDeliveryConversationFeedbackTest;
    friend class FDeliveryConversationVoiceBoundaryTest;
    friend class FDeliveryConversationVoiceKeyTest;

    // ── 打字 ──
    // 两句话的间隔和最近一句的结果

    /// 服务器：这个玩家下次能发言的时间
    double NextSendAt=0;

    /// 服务器：下次能补发声音的时间，每 0.25 秒最多一段
    double NextAudioRequestAt=0;

    /// 本机：下次能发的时间；服务器另有 NextSendAt 再查一次，不能只信客户端
    double NextLocalSendAt=0;

    /// 最近一句的编号，对话框按它找 NPC 的回复
    FGuid LastSubmissionId;

    /// 最近一句的结果
    FName LastResult;

    /// 结果提示什么时候消失；用现实时间，游戏暂停时也照样倒数
    double ResultExpiresAt=0;

    // ── 选人 ──
    // 绑定的对象和输入

    /// 交谈面板当前对着的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> ConversationTarget;

    /// 已订阅的语音输入组件
    TWeakObjectPtr<UDeliverySpeechInputComponent> BoundSpeech;

    /// 服务器正在等回复结束的 NPC
    TSet<TWeakObjectPtr<UDeliveryNpcAgentComponent>> WatchedAgents;

    /// 已订阅超时通知的轮次子系统
    TWeakObjectPtr<UDeliveryConversationTurnSubsystem> BoundTurns;

    /// 开麦按键的绑定句柄
    TArray<uint32> VoiceInputBindingHandles;

    /// 开麦按键绑在哪个输入组件上
    TWeakObjectPtr<class UEnhancedInputComponent> BoundVoiceInput;

    /// 开麦按键的映射
    TWeakObjectPtr<class UInputMappingContext> VoiceMapping;

    /// 加映射用的输入子系统
    TWeakObjectPtr<class UEnhancedInputLocalPlayerSubsystem> VoiceInputSubsystem;

    /// IA_Talk 绑在哪个输入组件上
    TWeakObjectPtr<class UEnhancedInputComponent> BoundTalkInput;

    /// IA_Talk 的绑定句柄
    uint32 TalkInputBindingHandle=0;

    /// 开麦映射是不是这个组件加的
    bool bOwnsVoiceMapping=false;

    /// 组件正在结束；这时检查器、转身、开麦等函数一进来就返回，不再绑按键，也不再向服务器发请求
    bool bEndingPlay=false;

    /// 本机最近用过的输入序号
    uint64 LastLocalInputSequence=0;

    // ── 开麦 ──
    // 本机正在说的这句：从开口到文字发给服务器，保存编号、对象、识别文字和截止时间

    /// 识别给这句话的编号
    FDeliverySpeechTurnToken LocalVoiceToken;

    /// 按键开始的那句的编号
    FDeliverySpeechTurnToken KeyVoiceToken;

    /// 这句的输入序号
    uint64 LocalVoiceSequence=0;

    /// 服务器发回的发言编号；ClientVoiceGranted 收到之前是空的，这时就算识别出文字也先不发
    FDeliveryConversationTurnHandle LocalVoiceHandle;

    /// 这句说给谁
    TWeakObjectPtr<ADeliveryNpcCharacter> LocalVoiceTarget;

    /// 开口时玩家控制的角色
    FDeliverySpeechPlayerContext LocalVoiceContext;

    /// 从什么时候开始认不出原来的角色（通常是正在上下车）；超过 2 秒就放弃这句
    double LocalVoiceContextPendingSince=-1;

    /// 识别出的最终文字
    FString FinalVoiceText;

    /// 文字已经识别好
    bool bVoiceTextReady=false;

    /// 文字已经发给服务器
    bool bVoiceSubmissionSent=false;

    /// 玩家已经说完
    bool bVoiceReleased=false;

    /// 这句最晚的提交时间
    double LocalVoiceDeadline=0;

    /// 最近一次开麦失败的原因
    FName LastVoiceInputError;

    /// 失败提示什么时候消失
    double VoiceErrorExpiresAt=0;

    // 服务器正在收的这句：每个玩家只记最新一轮；收到松键、取消请求时，客户端编号和序号对不上就不理

    /// 服务器记下的客户端编号
    FGuid ServerVoiceClientToken;

    /// 服务器为当前开麦这一轮生成的发言编号
    FDeliveryConversationTurnHandle ServerVoiceHandle;

    /// 服务器记下的对象二狗
    TWeakObjectPtr<ADeliveryNpcCharacter> ServerVoiceTarget;

    /// 文字暂存后，到点调用 FlushDeferredVoiceSubmission 的定时器
    FTimerHandle VoiceSubmitTimer;

    /// 暂存的那句的发言编号
    FDeliveryConversationTurnHandle DeferredVoiceHandle;

    /// 暂存的那句说给谁
    TWeakObjectPtr<ADeliveryNpcCharacter> DeferredVoiceTarget;

    /// 暂存的文字
    FString DeferredVoiceText;

    // 开麦说过的几轮：二狗回完话前一直记着，最多 8 轮；IsVoiceTurn 和 IsVoiceReplyMessage 查的就是这里

    /// 一轮开麦：发言编号和说给哪位 NPC
    struct FVoiceTurn
    {
        FDeliveryConversationTurnHandle Handle;
        TWeakObjectPtr<ADeliveryNpcCharacter> Npc;
    };

    /// 按消息编号存的开麦轮次
    TMap<FGuid,FVoiceTurn> VoiceTurns;

    /// 上次找到的自家二狗；它被销毁后下次调用 ResolveVoiceCompanion 会重新找
    mutable TWeakObjectPtr<ADeliveryNpcCharacter> CachedVoiceCompanion;

    /// 删掉二狗已经回完话的那几轮
    void PruneVoiceTurns();

    /// 最多记 8 轮
    static constexpr int32 MaxVoiceTurns=8;
    friend class FDeliveryConversationVoiceTargetTest;
    friend class FDeliveryConversationVehicleVoiceTest;
    friend class FDeliveryNpcVoiceLinkTest;
    friend class FDeliveryAiInspectorLifecycleTest;
};
