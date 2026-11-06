// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DeliveryInteractionComponent.h"
#include "DeliveryPlayerController.generated.h"

class ADeliveryCharacter;
class UInputAction;
class UInputMappingContext;
class UPostProcessComponent;
class UDeliveryDialogueVoiceComponent;
class UDeliveryMusicComponent;
class UDeliveryGroundArrowComponent;
class UDeliveryObjectiveRingComponent;
class UDeliverySettlementComponent;
class UUserWidget;
class UDeliveryHintComponent;
class UDeliveryPhoneController;
class UDeliveryPhoneAudioComponent;

/**
 * 绑定跨步行和驾驶使用的手机输入，转发交互与电话的服务器请求
 * 持有跨角色使用的提示和声音组件，界面逻辑由 HUD 和界面处理器负责
 */
UCLASS()

class ADeliveryPlayerController : public APlayerController
{
    GENERATED_BODY()

  public:
    ADeliveryPlayerController();
    virtual bool InputKey(const FInputKeyEventArgs& Params) override;
    FText GetPhoneKeyText() const;
    UFUNCTION(BlueprintPure, Category="Phone")
    UDeliveryPhoneController* GetPhoneController() const;

    /** 监听控制对象、冷却、视角和按键变化，更新操作提示 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="提示")
    TObjectPtr<UDeliveryHintComponent> Hints;

    // 交互请求转发
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

    /** 交付成功后弹出本机结算界面；WBP 类填在 BP_DeliveryManPC 上 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Delivery")
    TObjectPtr<UDeliverySettlementComponent> Settlement;

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

    // 电话操作请求
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
     * 手机输入动作由控制器绑定，上下车时继续有效
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Phone")
    void TogglePhoneUI();

    /** 手机动作，按键在输入映射资源中配置 */
    UPROPERTY(EditDefaultsOnly, Category="Input|Input Mappings")
    TSoftObjectPtr<UInputAction> TogglePhoneAction;

    // 界面状态与鼠标模式
    /**
     * 兼容现有蓝图，将面板开关交给 HUD
     * 只要还有面板打开，就保留鼠标操作
     * 面板自行关闭或销毁时，也必须报告关闭状态
     */
    UFUNCTION(BlueprintCallable, Category = "UI")
    void SetUIPanelOpen(FName PanelId, bool bOpen);

    /** 手机所有开关入口在实际改变面板后报告状态，包含来电自动展开和接听收起 */
    UFUNCTION(BlueprintCallable, Category = "Phone", meta = (DefaultToSelf = "PhoneWidget"))
    static void ReportPhoneUIOpen(UUserWidget *PhoneWidget, bool bOpen);

    /** 查询 HUD 是否还有打开的面板 */
    UFUNCTION(BlueprintPure, Category = "UI")
    bool IsAnyUIPanelOpen() const;

    /** 查询 HUD 中的手机面板状态 */
    UFUNCTION(BlueprintPure, Category = "Phone")

    bool IsPhoneUIOpen() const;

    /** 只读当前开着的面板集合，供诊断命令列名字 */
    const TSet<FName> &GetOpenUIPanelsForDebug() const
    {
        return OpenUIPanels;
    }

  protected:
    virtual void BeginPlay() override;

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

    /** 电话铃声和操作音效，独立于手机界面的开关 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Audio")
    TObjectPtr<UDeliveryPhoneAudioComponent> PhoneAudio;

    // 控制器输入绑定
    virtual void SetupInputComponent() override;

    /** 将手机输入交给手机处理器 */
    void HandleTogglePhonePressed();

  private:
    // 旧资源缺少交互映射时补充临时映射
    void EnsureInteractMapping(class UEnhancedInputLocalPlayerSubsystem* Subsystem);
    UPROPERTY(Transient)
    TObjectPtr<UInputMappingContext> RuntimeInteractContext;
};
