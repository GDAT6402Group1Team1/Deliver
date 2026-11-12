// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInteractableComponent.generated.h"

class APawn;
class USceneComponent;

UENUM(BlueprintType)
enum class EDeliveryInteractionKey : uint8
{
    GeneralF UMETA(DisplayName = "F - General Interaction"),
    PickupE UMETA(DisplayName = "E - 物品操作"),
    /**
     * 左键一次性操作（摩托车后备箱装卸货）
     *
     * 左键平时是出拳，所以这类目标**只在 CanUse 通过时**才抢走左键：
     * 手上没货、箱子也空着的时候站在车尾照样能挥拳。
     */
    ActionLeft UMETA(DisplayName = "左键 - 就地操作")
};

DECLARE_DELEGATE_RetVal_TwoParams(bool, FDeliveryCanUse, APawn *, bool);
DECLARE_DELEGATE_RetVal_OneParam(bool, FDeliveryUse, APawn *);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDeliveryInteractSignature, APawn *, Interactor);

/**
 * 配置物体的按键交互，包括提示、操作条件和交互事件
 * 物体通过绑定交互事件实现具体行为，持续抓取由抓取组件处理
 */
UCLASS(ClassGroup = (Delivery), meta = (BlueprintSpawnableComponent))

class DELIVERY_API UDeliveryInteractableComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    UDeliveryInteractableComponent();

    // 目标提供操作条件和执行方法，交互流程不识别具体目标类型
    FDeliveryCanUse CanUse;
    FDeliveryUse Use;

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // 提示与交互配置
    /** 浮窗上显示的字；摩托车默认是"按 F 驾驶" */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    FText PromptText;

    /**
     * 玩家离得多近才能交互；从下面那个"测量点"算起，不一定是 Actor 原点
     *
     * 测量点的优先级见 `GetInteractionPoint()`：锚点 > 根碰撞表面 > Actor 原点
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction",
              meta = (ClampMin = "0.0", Units = "cm"))
    float InteractRadius = 300.0f;

    /**
     * 指定交互距离的测量起点
     * 大型模型的原点可能远离门口，因此取货点和收件点默认使用地点组件的位置
     * 手动指定后优先使用该组件
     */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Interaction")
    TObjectPtr<USceneComponent> InteractAnchor;

    /** 浮窗吊在宿主 Actor 原点往上多高 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    FVector PromptOffset = FVector(0.0f, 0.0f, 180.0f);

    // 开启后提示跟随交互目标，默认保留主角头顶提示
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    bool bPromptAtTarget = false;

    // 仅改变提示外观，不改变交互按键与服务器校验
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    bool bUseConversationIcon = false;

    // 临时隐藏提示时仍允许交互，例如为正在播放的对白字幕让位
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    bool bShowPrompt = true;

    /**
     * 关掉之后既不提示也不能交互
     * 摩托车在有人骑的时候把它关掉——不然别人站旁边还能"上车"
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    bool bInteractEnabled = true;

    // 仅显式支持拖拽交接的目标开启，服务器仍执行目标自己的校验
    UPROPERTY(EditDefaultsOnly, Category="Interaction")
    bool bAllowWhileGrabbing = false;

    /** 区分 E 键物品操作和 F 键通用交互，默认使用 F 键 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
    EDeliveryInteractionKey InteractionKey = EDeliveryInteractionKey::GeneralF;

    /** 长按时长，单位为秒；零表示立即触发，快递取件和交付使用半秒 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction",
              meta = (ClampMin = "0.0", Units = "s"))
    float HoldDuration = 0.0f;

    // 交互通知
    UPROPERTY(BlueprintAssignable, Category = "Interaction")
    FDeliveryInteractSignature OnInteract;

    // 条件检查与执行
    UFUNCTION(BlueprintPure, Category = "Interaction")
    bool CanInteract(const APawn *Interactor) const;

    /**
     * 距离从哪个点算；锚点 > 根组件碰撞表面（只对 E 目标）> Actor 原点
     *
     * 公开出来是为了让诊断命令能打这个点——"按不动"最常见的原因就是
     * 它和玩家以为的位置不是一回事，而这件事在游戏里完全看不见
     */
    UFUNCTION(BlueprintPure, Category = "Interaction")
    FVector GetInteractionPoint(const FVector &FromLocation) const;

    /** 玩家到测量点的实际距离；负数表示拿不到（没有 Owner）；诊断用 */
    UFUNCTION(BlueprintPure, Category = "Interaction")
    float GetInteractionDistance(const APawn *Interactor) const;

    /** 服务器复核通过之后真正执行一次交互 */
    void Execute(APawn *Interactor);

    UFUNCTION(BlueprintPure, Category = "Interaction")
    FVector GetPromptLocation() const;

    /** 从指定物体取得交互组件，用于服务器复核目标 */
    static UDeliveryInteractableComponent *FindOn(const AActor *Actor);

    /**
     * 按交互键在物体上找交互组件
     *
     * 一个 Actor 可以挂多个交互组件（摩托车就是"按 F 驾驶"+"左键装卸货"两个），
     * 这时不带键的那个重载只会拿到第一个，服务器复核就会复核错对象——
     * 表现是"提示是装货、按下去却上了车"。凡是已经知道自己在处理哪个键的
     * 代码路径，都该走这个重载。
     */
    static UDeliveryInteractableComponent *FindOn(const AActor *Actor, EDeliveryInteractionKey Key);
};
