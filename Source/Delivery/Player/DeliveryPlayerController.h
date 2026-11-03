// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Task/DeliveryTaskTypes.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "DeliveryPlayerController.generated.h"

class ADeliveryCharacter;
class UInputAction;
class UInputMappingContext;
class UPostProcessComponent;
class UDeliveryDialogueVoiceComponent;
class UDeliveryMusicComponent;
class UAudioComponent;
class USoundBase;
class UDeliveryGroundArrowComponent;
class UDeliveryObjectiveRingComponent;
class UUserWidget;

/** 管理玩家输入、手机界面、任务指引和本机音频 */
UCLASS()

class ADeliveryPlayerController : public APlayerController
{
    GENERATED_BODY()

  public:
    ADeliveryPlayerController();

    // 步行与骑车共用的交互请求入口
    UFUNCTION(Server, Reliable)
    void ServerRequestInteraction(ADeliveryCharacter *ActingCharacter, EDeliveryInteractionRequest Request,
                                  AActor *Target);

    // 任务指引组件
    /** 当前任务的脚边方向箭头；上下摩托车时持续存在 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery|Guidance")
    TObjectPtr<UDeliveryGroundArrowComponent> GroundArrow;

    /** 当前任务目的地的地面圈；和脚边箭头同位置、同一个目的地来源 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery")
    TObjectPtr<UDeliveryObjectiveRingComponent> ObjectiveRing;

    /** 本机人物电子语音生成器；电话/NPC UI 都复用这一份 */
    UFUNCTION(BlueprintPure, Category = "Delivery|Voice")

    UDeliveryDialogueVoiceComponent *GetDialogueVoice() const
    {
        return DialogueVoice;
    }

    /** 背景音乐；和语音组件同一个位置：跨 Possess（上下摩托车）不断 */
    UDeliveryMusicComponent *GetMusic() const
    {
        return Music;
    }

    // 电话操作
    /**
     * 接听当前来电；客户端调用会自动转成 Server RPC
     *
     * 电话队列挂在 GameState 上，而 GameState 不属于任何客户端，
     * 客户端没法直接对它发 RPC——所以玩家的接听/挂断意图统一从这里中转
     */
    UFUNCTION(BlueprintCallable, Category = "Phone")
    void RequestAnswerCall();

    /** 挂断；响铃时是拒接，通话中是提前结束；两种都会影响所有玩家 */
    UFUNCTION(BlueprintCallable, Category = "Phone")
    void RequestHangUpCall();

    /**
     * 切换手机界面，由蓝图实现界面的显示和关闭
     * 控制器直接绑定手机按键，不依赖输入映射资源
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Phone")
    void TogglePhoneUI();

    /** 开关手机的按键；改这里提示里的键名会跟着变，不会说一套做一套 */
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    FKey TogglePhoneKey;

    // 界面状态与鼠标模式
    /**
     * 记录指定面板的开关状态，统一更新鼠标光标和输入模式
     * 只要还有面板打开，就保留鼠标操作
     * 面板自行关闭或销毁时，也必须报告关闭状态
     */
    UFUNCTION(BlueprintCallable, Category = "UI")
    void SetUIPanelOpen(FName PanelId, bool bOpen);

    /** 手机所有开关入口在实际改变面板后报告状态，包含来电自动展开和接听收起 */
    UFUNCTION(BlueprintCallable, Category = "Phone", meta = (DefaultToSelf = "PhoneWidget"))
    static void ReportPhoneUIOpen(UUserWidget *PhoneWidget, bool bOpen);

    /** 当前是否还有 UI 面板开着（= 该不该显示光标） */
    UFUNCTION(BlueprintPure, Category = "UI")
    bool IsAnyUIPanelOpen() const;

    /** 手机界面当前是开着的吗；状态由 C++ 持有，蓝图不要另存一份 */
    UFUNCTION(BlueprintPure, Category = "Phone")

    bool IsPhoneUIOpen() const
    {
        return bPhoneUIOpen;
    }

  protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // 服务器电话请求
    UFUNCTION(Server, Reliable)
    void ServerAnswerCall();

    UFUNCTION(Server, Reliable)
    void ServerHangUpCall();

    // 输入映射
    /** 始终启用的输入映射 */
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

    /** 非触屏设备使用的额外视角输入映射 */
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    TArray<TObjectPtr<UInputMappingContext>> MobileExcludedMappingContexts;

    /**
     * 通用交互输入，默认使用 F 键
     * 已有输入映射未配置该操作时，在运行时补充绑定
     */
    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    TSoftObjectPtr<UInputAction> InteractAction;

    UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
    FKey InteractKey;

    // 本机高亮与声音
    /** 本机交互物和抓取候选使用的透视高亮后处理；任务地点不再高亮 */
    UPROPERTY(VisibleAnywhere, Category = "Delivery|Highlight")
    TObjectPtr<UPostProcessComponent> HighlightPostProcess;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery|Voice",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UDeliveryDialogueVoiceComponent> DialogueVoice;

    /** 背景音乐；轨道插槽填在 BP_DeliveryManPC 上，电话期间自动闪避 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Audio")
    TObjectPtr<UDeliveryMusicComponent> Music;

    /** 手机界面每次开/关时的按键反馈；需要区分开和关时可在 TogglePhoneUI 蓝图事件里另外播放 */
    UPROPERTY(EditDefaultsOnly, Category = "Delivery|Audio|Phone")
    TObjectPtr<USoundBase> PhoneToggleSound;

    /** 来电铃声；资产本身应设置循环；离开 Ringing 状态时 C++ 会主动停止 */
    UPROPERTY(EditDefaultsOnly, Category = "Delivery|Audio|Phone")
    TObjectPtr<USoundBase> IncomingCallSound;

    UPROPERTY(EditDefaultsOnly, Category = "Delivery|Audio|Phone")
    TObjectPtr<USoundBase> AnswerCallSound;

    UPROPERTY(EditDefaultsOnly, Category = "Delivery|Audio|Phone")
    TObjectPtr<USoundBase> HangUpCallSound;

    virtual void SetupInputComponent() override;

    /** TogglePhoneKey 按下时的转发；BindKey 需要一个无参成员函数，不能直接绑 BlueprintImplementableEvent */
    void HandleTogglePhonePressed();

    /** 按当前开着的面板集合算出光标和输入模式；只在本机控制器上生效 */
    void ApplyUIInputMode();

  private:
    // 界面运行状态
    /**
     * 当前开着的 UI 面板；只在本机维护、不复制
     *
     * 用集合而不是一个 bool：同时开手机和任务面板时，关掉一个不该把光标收走
     */
    TSet<FName> OpenUIPanels;

    /** 手机界面开没开；C++ 持有，这样光标状态不可能和它脱钩 */
    bool bPhoneUIOpen = false;

    // 电话事件与字幕
    /** GameState 可能比本机 PlayerController 晚就绪，绑定不到时短暂重试 */
    void BindPhoneVoice();

    /** 电话进入 InCall 时在本机播放任务台词；离开通话立即停声 */
    UFUNCTION()
    void HandlePhoneVoiceStateChanged(EDeliveryPhoneCallState NewState, const FDeliveryPhoneCall &Call);

    /** 程序化语音每揭示一个字符，就把它追加到屏幕下方字幕 */
    UFUNCTION()
    void HandlePhoneVoiceGlyphRevealed(const FString &Glyph, int32 GlyphIndex);

    /** 已有 IMC 里都没映射交互键时，建一份临时的补上 */
    void EnsureInteractMapping(class UEnhancedInputLocalPlayerSubsystem *Subsystem);

    // 临时输入与声音状态
    UPROPERTY(Transient)
    TObjectPtr<UInputMappingContext> RuntimeInteractContext;

    FTimerHandle PhoneVoiceBindRetryTimer;

    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> IncomingCallAudio;

    void StopIncomingCallAudio();
    bool bPhoneAudioStateInitialized = false;
    EDeliveryPhoneCallState LastPhoneAudioState = EDeliveryPhoneCallState::Idle;
    int32 ActiveVoiceCallId = 0;
    FText ActivePhoneCallerName;
    FString ActivePhoneSubtitle;
};
