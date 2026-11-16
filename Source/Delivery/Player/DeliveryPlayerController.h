// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "DeliveryPlayerController.generated.h"

class ADeliveryCharacter;
class ADeliveryCompanionCharacter;
class UDeliveryConversationComponent;
class UInputAction;
class UInputMappingContext;
class UPostProcessComponent;
class UDeliveryDialogueVoiceComponent;
class UDeliverySpeechInputComponent;
class UDeliveryMusicComponent;
class UDeliveryGroundArrowComponent;
class UDeliveryObjectiveRingComponent;
class UDeliverySettlementComponent;
class UUserWidget;
class UDeliveryHintComponent;
class UDeliveryPhoneController;
class UDeliveryPhoneAudioComponent;
class ADeliveryStartupFlow;

/**
 * @brief 玩家控制器：步行和骑车都在的那部分，上下车时它不换
 *
 * 绑定手机键和补上交互键映射；转发需要服务器做的请求：交互、接听挂断电话、加载完成报告、二狗补位时的视野回答
 * 挂着上下车都不能断的本机组件：提示、脚边箭头、目的地圈、结算、电子语音、麦克风、居民交谈、背景音乐、电话音效
 * 界面怎么显示归 HUD 和各界面处理器，这里只保留入口
 */
UCLASS()

class ADeliveryPlayerController : public APlayerController
{
    GENERATED_BODY()

  public:
    // ── 生命周期 ──

    /// @brief 创建上面那些本机组件和高亮后处理，设置交互 IA 的软引用
    ADeliveryPlayerController();

    // ── 开局 ──

    /// @brief 客户端加载完成后通过自己的控制器报告给服务器，服务器转给 Flow 的 ReportPlayerReady；客户端不能直接对世界里的 Actor 发 RPC
    UFUNCTION(Server, Reliable)
    void ServerReportStartupReady(ADeliveryStartupFlow* Flow);

    // ── 交付 ──

    /// @brief 服务器确认交付成功后调用：本机在 Recipient（交货的那具身体）上发布 DeliverySucceeded 事件，播放音效
    /// 玩家正在骑车时也能收到，因为这个 RPC 发给控制器而不是角色
    UFUNCTION(Client, Reliable)
    void ClientConfirmDelivery(ADeliveryCharacter* Recipient);

    // ── 二狗补位 ──
    // 二狗跟丢主人想瞬移过去时，服务器先问每个玩家能不能看见起点和终点；所有人都看不见才瞬移

    /// @brief 服务器问本机：StartCenter 和 EndCenter 两个盒子在本机镜头里是不是都看不见，算完调用 ServerReportCompanionCatchUpView 回答
    UFUNCTION(Client, Unreliable)
    void ClientCheckCompanionCatchUp(ADeliveryCompanionCharacter* Companion, FGuid RequestId,
                                    FVector_NetQuantize StartCenter, FVector_NetQuantize EndCenter,
                                    FVector_NetQuantize Extent);

    /// @brief 本机回答服务器：bUnseen 为 true 表示两个盒子都看不见；服务器转给二狗组件的 AcceptCatchUpVisibility 核对
    UFUNCTION(Server, Unreliable)
    void ServerReportCompanionCatchUpView(ADeliveryCompanionCharacter* Companion, FGuid RequestId,
                                        APawn* ViewPawn, AActor* ViewTarget, bool bUnseen);

    /// @brief 镜头版本号；换控制对象或视角时加一，服务器拿它拒绝换之前发出的旧回答
    uint64 GetCompanionViewRevision() const { return CompanionViewRevision; }

    /// @brief 换镜头时调用 InvalidateCompanionView，旧的补位回答作废
    virtual void SetViewTarget(AActor* NewViewTarget,
                               FViewTargetTransitionParams TransitionParams = FViewTargetTransitionParams()) override;

    // ── 输入 ──

    /// @brief 每个按键都经过这里：真按下了手柄键或键盘鼠标键时，告诉提示组件换成对应设备的键名；摇杆轻微漂移不算
    virtual bool InputKey(const FInputKeyEventArgs& Params) override;

    /// @brief 引擎清空按键时（例如窗口失去焦点），顺带把角色的移动意图清零，布娃娃另外存着移动意图
    virtual void FlushPressedKeys() override;

    /// @brief 每帧处理输入之前：步行时检查游戏窗口是否有键盘焦点，没有就调用 UpdateWalkingInputFocus(false)
    virtual void PreProcessInput(float DeltaTime, bool bGamePaused) override;

    /// @brief 游戏窗口有焦点、没暂停时返回 true；输入组件的 Move 读它，焦点在界面上时不走路
    bool CanAcceptWalkingInput() const { return bWalkingInputAllowed; }

    /// @brief 手机键当前映射的键名，按当前设备显示
    FText GetPhoneKeyText() const;

    /// @brief HUD 上的手机处理器
    UFUNCTION(BlueprintPure, Category="Phone")
    UDeliveryPhoneController* GetPhoneController() const;

    // ── 本机组件 ──

    /// 左下角操作提示，监听控制对象、冷却、视角和按键映射的变化
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="提示")
    TObjectPtr<UDeliveryHintComponent> Hints;

    // ── 交互 ──

    /// @brief 步行和骑车共用的交互请求：服务器确认 ActingCharacter 是自己控制的角色、或自己骑的车上的驾驶员，再交给角色的交互组件 HandleRequest
    UFUNCTION(Server, Reliable)
    void ServerRequestInteraction(ADeliveryCharacter *ActingCharacter, EDeliveryInteractionRequest Request,
                                  AActor *Target);

    // ── 任务指引 ──

    /// 当前任务的脚边方向箭头；挂在控制器上，上下摩托车时一直在
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery|Guidance")
    TObjectPtr<UDeliveryGroundArrowComponent> GroundArrow;

    /// 当前任务目的地的地面圈，和脚边箭头读同一个目的地
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery")
    TObjectPtr<UDeliveryObjectiveRingComponent> ObjectiveRing;

    /// 交付成功后弹出本机结算界面；WBP 类填在 BP_DeliveryManPC 上
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery")
    TObjectPtr<UDeliverySettlementComponent> Settlement;

    // ── 声音 ──

    /// @brief 本机人物电子语音组件，电话和 NPC 界面都用这一个
    UFUNCTION(BlueprintPure, Category = "Delivery|Voice")

    UDeliveryDialogueVoiceComponent *GetDialogueVoice() const
    {
        return DialogueVoice;
    }

    /// @brief 麦克风输入组件；挂在控制器上，上下车时不断，由本机界面开启
    UFUNCTION(BlueprintPure, Category = "Delivery|Speech")
    UDeliverySpeechInputComponent* GetSpeechInput() const { return SpeechInput; }

    /// @brief 背景音乐组件；挂在控制器上，上下摩托车时音乐不断
    UDeliveryMusicComponent *GetMusic() const
    {
        return Music;
    }

    // ── 电话 ──
    // 电话队列挂在 GameState 上，GameState 不属于任何客户端、客户端不能直接对它发 RPC，所以接听和挂断从控制器的 Server RPC 转过去

    /// @brief 接听当前来电；服务器上直接调用电话队列的 AnswerCurrentCall，客户端发 ServerAnswerCall
    UFUNCTION(BlueprintCallable, Category = "Phone")
    void RequestAnswerCall();

    /// @brief 挂断：响铃时是拒接，通话中是提前结束；电话所有玩家共用，挂断后所有人都停
    UFUNCTION(BlueprintCallable, Category = "Phone")
    void RequestHangUpCall();

    /// @brief 蓝图事件：显示或收起手机面板；手机处理器决定开关后调用
    UFUNCTION(BlueprintImplementableEvent, Category = "Phone")
    void TogglePhoneUI();

    /// 手机键的 IA，按键在输入映射资源里配置；控制器绑定它，上下车时都有效
    UPROPERTY(EditDefaultsOnly, Category="Input|Input Mappings")
    TSoftObjectPtr<UInputAction> TogglePhoneAction;

    // ── 界面与鼠标 ──
    // 打开着哪些面板由 HUD 记着，有面板开着时显示鼠标；这几个函数是给旧蓝图留的入口，转给 HUD

    /// @brief 报告某个面板打开或关闭，转给 HUD；面板自己关掉或被销毁时也要调用一次 false
    UFUNCTION(BlueprintCallable, Category = "UI")
    void SetUIPanelOpen(FName PanelId, bool bOpen);

    /// @brief 手机面板在实际打开或收起后调用（包括来电自动展开、接听后收起），通过 PhoneWidget 找到控制器再转给手机处理器
    UFUNCTION(BlueprintCallable, Category = "Phone", meta = (DefaultToSelf = "PhoneWidget"))
    static void ReportPhoneUIOpen(UUserWidget *PhoneWidget, bool bOpen);

    /// @brief HUD 上还有没有打开的面板
    UFUNCTION(BlueprintPure, Category = "UI")
    bool IsAnyUIPanelOpen() const;

    /// @brief 手机面板是否打开
    UFUNCTION(BlueprintPure, Category = "Phone")
    bool IsPhoneUIOpen() const;

    /// @brief 调试命令用：当前打开的面板名字
    const TSet<FName> &GetOpenUIPanelsForDebug() const;

  protected:
    // ── 生命周期 ──

    /// @brief 只在本机控制器上开启高亮后处理
    virtual void BeginPlay() override;

    /// @brief 换控制对象时调用 InvalidateCompanionView
    virtual void OnPossess(APawn* InPawn) override;

    /// @brief 失去控制对象时调用 InvalidateCompanionView
    virtual void OnUnPossess() override;

    // ── 电话 ──

    /// @brief 客户端接听请求到了服务器，调用 RequestAnswerCall
    UFUNCTION(Server, Reliable)
    void ServerAnswerCall();

    /// @brief 客户端挂断请求到了服务器，调用 RequestHangUpCall
    UFUNCTION(Server, Reliable)
    void ServerHangUpCall();

    // ── 输入映射 ──

    /// 一直启用的输入映射
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

    /// 不是触屏设备时额外启用的视角映射
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    TArray<TObjectPtr<UInputMappingContext>> MobileExcludedMappingContexts;

    /// 交互 IA，默认 F；输入映射里没有它时 EnsureInteractMapping 在运行时补上
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    TSoftObjectPtr<UInputAction> InteractAction;

    /// 补映射时用的键
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    FKey InteractKey;

    // ── 本机组件 ──

    /// 交互物和抓取候选的透视高亮后处理，只在本机开启；任务地点不再高亮
    UPROPERTY(VisibleAnywhere, Category = "Delivery|Highlight")
    TObjectPtr<UPostProcessComponent> HighlightPostProcess;

    /// 电子语音组件
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery|Voice",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryDialogueVoiceComponent> DialogueVoice;

    /// 麦克风输入组件
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Delivery|Speech")
    TObjectPtr<UDeliverySpeechInputComponent> SpeechInput;

    /// 和居民 NPC 交谈的组件
    UPROPERTY(VisibleAnywhere, Category="Delivery|居民") TObjectPtr<UDeliveryConversationComponent> ResidentConversation;

    /// 背景音乐；轨道填在 BP_DeliveryManPC 上，电话期间自动压低音量
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Audio")
    TObjectPtr<UDeliveryMusicComponent> Music;

    /// 电话铃声和接听挂断的音效，和手机面板开没开无关
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Audio")
    TObjectPtr<UDeliveryPhoneAudioComponent> PhoneAudio;

    // ── 输入 ──

    /// @brief 本机：添加输入映射、补上交互键，绑定手机键到 HandleTogglePhonePressed
    virtual void SetupInputComponent() override;

    /// @brief 手机键按下，交给手机处理器决定开还是关
    void HandleTogglePhonePressed();

  private:
    friend class FDeliveryMoveInputSafetyTest;

    /// @brief 游戏窗口在前台、键盘焦点在游戏视口上时返回 true；焦点在输入框或按钮上时返回 false
    bool HasWalkingInputFocus() const;

    /// @brief 记下能否步行输入；从能变成不能时调用 FlushPressedKeys，按着的移动键不再生效
    void UpdateWalkingInputFocus(bool bAllowed);

    /// 能否接受步行输入
    bool bWalkingInputAllowed = true;

    // ── 二狗补位 ──

    /// @brief 按本机镜头的视锥判断两个盒子是否都看不见；旁观、过渡或镜头还没准备好时一律算看得见
    bool AreCompanionCatchUpBoxesUnseen(APawn* ViewPawn, AActor* ViewTarget,
                                       const FVector& StartCenter, const FVector& EndCenter,
                                       const FVector& Extent) const;

    /// @brief 服务器上镜头版本号加一；本机记下换视角的时间，之后至少等一次镜头更新再回答
    void InvalidateCompanionView();

    /// 镜头版本号，只在服务器上改，不同步
    uint64 CompanionViewRevision = 1;

    /// 本机最近一次换视角的时间；新镜头至少更新一次之前，不用旧的镜头数据回答
    float LocalCompanionViewChangedAt = -1.0f;

    // ── 输入映射 ──

    /// @brief 已有的输入映射里没有交互 IA 时，新建一个运行时映射 RuntimeInteractContext 把 InteractKey 绑上
    void EnsureInteractMapping(class UEnhancedInputLocalPlayerSubsystem* Subsystem);

    /// 运行时补的交互映射
    UPROPERTY(Transient)
    TObjectPtr<UInputMappingContext> RuntimeInteractContext;
};
