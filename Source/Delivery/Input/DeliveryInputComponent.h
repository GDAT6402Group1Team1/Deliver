#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Input/DeliveryRightMouseGesture.h"
#include "DeliveryInputComponent.generated.h"

class ADeliveryCharacter;
class UInputAction;
class UInputComponent;
struct FInputActionValue;

/**
 * @brief 步行时的按键：把 IA 动作绑到角色的移动、视角、跳跃、出拳、抓取、交互、拾取、物品栏和召唤上
 *
 * 角色被玩家占有时 SetupPlayerInputComponent 调用 Bind；失去控制（上车、换角色）时调用 ResetInput 取消没做完的操作
 * 左右键的规则：按下一只键后等 0.2 秒，期间按下另一只键就是双手抓取（RequestBegin），否则是单键出拳
 * 鼠标右键按住拖动是转视角，只有单击才出右拳；这里只发请求，能不能做由角色和各玩法组件检查
 */
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInputComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    // ── 绑定 ──

    /// @brief 不需要每帧更新；构造时加载移动、视角、跳跃、左右拳的 IA，交互、拾取、召唤和五个物品栏 IA 只存路径，Bind 时再加载
    UDeliveryInputComponent();

    /// @brief 把所有动作绑到 PlayerInputComponent 上；同一个组件已经绑过时直接返回
    /// 最后调用 DoMove(0, 0)，刚接手的身体从没有移动意图开始
    void Bind(UInputComponent* PlayerInputComponent);

    /// @brief 失去控制时调用：清掉鼠标组合键状态和绑定记录 BoundInput，停止移动和长按拾取，松开抓取，取消出拳
    /// 已经按下但没松开的键不会补成一次出拳
    void ResetInput();

    /// @brief 召唤车辆的 IA，提示组件用它查当前映射的按键
    UInputAction* GetSummonAction() const;

    /// @brief 左右键都按住、而且已经判定为双手抓取时返回 true
    bool IsGrabChordHeld() const { return bGrabChordActive && bLeftMouseDown && bRightMouseDown; }
protected:
    // ── 生命周期 ──
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // ── 配置 ──
    // 各个 IA；按键映射在 IMC_Default 里配置，键盘和手柄映射到同一个 IA

    /// 跳跃
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> JumpAction;

    /// 移动，二维轴
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> MoveAction;

    /// 手柄右摇杆转视角
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> LookAction;

    /// 鼠标转视角，乘 MouseLookSensitivity
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> MouseLookAction;

    /**
     * 鼠标视角灵敏度，0.4 是原始输入的 40%（之前是 0.75，用户反馈转视角太快）；手柄右摇杆不受影响
     *
     * 摩托车 ADeliveryMotorbike 也有一个同名字段，两个要一起调：
     * 只改这一个的话走路正常、一上车视角就转得飞快，而两个数单看都像是对的
     */
    UPROPERTY(EditAnywhere, Category = "Input", meta = (ClampMin = "0.1", ClampMax = "2.0"))
    float MouseLookSensitivity = 0.4f;

    /// 左拳（鼠标左键、手柄 LT）
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> AttackLeftAction;

    /// 右拳（鼠标右键单击、手柄 RT）
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> AttackRightAction;

    /// 交互，默认 F；软引用，Bind 时才加载，编辑器里后建的资产也能用
    UPROPERTY(EditAnywhere, Category = "Input")
    TSoftObjectPtr<UInputAction> InteractAction;

    /// 拾取和长按交互，默认 E；按下调用 DoPickup，松开调用 DoPickupRelease
    UPROPERTY(EditAnywhere, Category = "Input")
    TSoftObjectPtr<UInputAction> PickupAction;

    /// 召唤车辆
    UPROPERTY(EditDefaultsOnly, Category="Input")
    TSoftObjectPtr<UInputAction> SummonAction;

    /// 物品栏 1 到 5 格，按顺序对应；按下时调用背包的 RequestSlotAction
    UPROPERTY(EditDefaultsOnly, EditFixedSize, Category="Input")
    TArray<TSoftObjectPtr<UInputAction>> InventoryActions;

public:
    /// @brief 调试命令用：当前绑到哪个 InputComponent；为空说明这具身体没接上按键
    const UInputComponent *GetBoundInputForDebug() const
    {
        return BoundInput.Get();
    }

private:
    friend class FDeliveryInputLifecycleTest;
    friend class FDeliveryMoveInputSafetyTest;

    // ── 绑定状态 ──

    /// 已经绑过的 InputComponent；ResetInput 时清掉，因为引擎会在失去控制时销毁它
    TWeakObjectPtr<UInputComponent> BoundInput;

    /// 组件所在的角色，Bind 时记下
    UPROPERTY(Transient)
    TObjectPtr<ADeliveryCharacter> Character;

    // ── 移动 ──
    // 都转给角色的 DoMove、DoLook、DoJumpStart

    /// @brief 移动轴；本机不再控制这具身体、或玩家控制器不接受步行输入时按松开处理
    void Move(const FInputActionValue &Value);

    /// @brief 移动键松开或取消：移动意图清零
    void MoveEnded(const FInputActionValue &Value);

    /// @brief 手柄右摇杆转视角
    void Look(const FInputActionValue &Value);

    /// @brief 鼠标转视角；同时记进 RightMouseGesture，用来区分右键单击和拖动
    void MouseLook(const FInputActionValue &Value);

    /// @brief 跳跃键按下
    void JumpStarted(const FInputActionValue &Value);

    // ── 出拳与抓取 ──
    // 左右键按下都调用 MousePressed，松开调用 MouseReleased，被取消调用 MouseCanceled；0.2 秒到时调用 ResolveSingleMousePress

    /// @brief 左键按下，调用 MousePressed(true)
    void AttackLeftStarted(const FInputActionValue &Value);

    /// @brief 右键按下，调用 MousePressed(false)；是鼠标右键（不是手柄）时开始记录拖动
    void AttackRightStarted(const FInputActionValue &Value);

    /// @brief 左键松开，调用 MouseReleased(true)
    void AttackLeftEnded(const FInputActionValue &Value);

    /// @brief 右键松开，调用 MouseReleased(false)
    void AttackRightEnded(const FInputActionValue &Value);

    /// @brief 左键被取消，调用 MouseCanceled(true)
    void AttackLeftCanceled(const FInputActionValue &Value);

    /// @brief 右键被取消，调用 MouseCanceled(false)
    void AttackRightCanceled(const FInputActionValue &Value);

    /// @brief 0.2 秒到了还没按第二只键：第一只是左键就出左拳，是手柄右键就出右拳；鼠标右键等松开时再判断
    void ResolveSingleMousePress();

    /// @brief 记下这只键按下：第一只键开始等 0.2 秒；等待期间按下第二只键就调用 GrabComponent->RequestBegin 开始抓取
    /// 第一只键已经按单键出过拳时，第二只键也单独出拳
    /// @param bMouseRight true 表示是鼠标右键，要区分单击和拖动
    void MousePressed(bool bLeft,bool bMouseRight=false);

    /// @brief 这只键松开：在抓取时只松开这只手；鼠标右键按单击还是拖动判断要不要出右拳；还没出过拳的短按补出一拳
    void MouseReleased(bool bLeft);

    /// @brief 这只键被取消（例如打开界面）：不出拳，只按松开处理抓取
    void MouseCanceled(bool bLeft);

    // ── 交互与物品 ──

    /// @brief 交互键按下，调用角色的 DoInteract
    void InteractStarted(const FInputActionValue &Value);

    /// @brief 拾取键按下，调用角色的 DoPickup 开始长按
    void PickupStarted(const FInputActionValue &Value);

    /// @brief 拾取键松开或取消，调用角色的 DoPickupRelease
    void PickupEnded(const FInputActionValue &Value);

    /// @brief 物品栏键按下，调用背包的 RequestSlotAction(Slot)
    void InventorySlotSelected(int32 Slot);

    /// @brief 召唤键按下；没晕倒时调用召唤组件的 RequestSummon
    void SummonVehiclePressed();

    // ── 鼠标组合键状态 ──

    /// 第一只键按下后的 0.2 秒等待，到时调用 ResolveSingleMousePress
    FTimerHandle MouseChordTimer;

    /// 左键（或 LT）按着
    bool bLeftMouseDown = false;

    /// 右键（或 RT）按着
    bool bRightMouseDown = false;

    /// 这次按键已经判定为双手抓取
    bool bGrabChordActive = false;

    /// 这次按键已经按单键出过拳，之后不再出拳也不改判为抓取
    bool bSingleMouseResolved = false;

    /// 第一只按下的是不是左键，0.2 秒到时出哪只拳看它
    bool bFirstMouseLeft = false;

    /// 记录鼠标右键按下后的移动，松开时判断是单击还是拖动
    FDeliveryRightMouseGesture RightMouseGesture;

};
