#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DeliveryInputComponent.generated.h"

class ADeliveryCharacter;
class UInputAction;
class UInputComponent;
struct FInputActionValue;

// 处理步行按键和双键组合，将操作交给角色及玩法组件
UCLASS(ClassGroup=(Delivery), meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryInputComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryInputComponent();
    // 使用角色当前的输入组件绑定，控制车辆时这些绑定会随角色退出输入栈
    void Bind(UInputComponent* PlayerInputComponent);
    // 失去控制权时取消未完成的操作，不将松开补判为一次攻击
    void ResetInput();
    bool IsGrabChordHeld() const { return bGrabChordActive && bLeftMouseDown && bRightMouseDown; }
protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // 移动与视角动作
    // 输入配置
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> JumpAction;

    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> LookAction;

    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> MouseLookAction;

    /** 鼠标视角灵敏度；手柄右摇杆不受影响 */
    UPROPERTY(EditAnywhere, Category = "Input", meta = (ClampMin = "0.1", ClampMax = "2.0"))
    float MouseLookSensitivity = 0.75f;

    // 战斗动作
    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> AttackLeftAction;

    UPROPERTY(EditAnywhere, Category = "Input")
    TObjectPtr<UInputAction> AttackRightAction;

    // 交互与物品动作
    /** 交互输入，默认按 F 触发；绑定输入时再加载资源，允许在编辑器中稍后创建该资源 */
    UPROPERTY(EditAnywhere, Category = "Input")
    TSoftObjectPtr<UInputAction> InteractAction;

    /** 拾取输入，默认按 E 触发 */
    UPROPERTY(EditAnywhere, Category = "Input")
    TSoftObjectPtr<UInputAction> PickupAction;

    /** 召唤载具的按键，默认 R；直接绑定按键，不依赖输入映射资源 */
    UPROPERTY(EditAnywhere, Category = "Input")
    FKey SummonVehicleKey;

private:
    friend class FDeliveryInputLifecycleTest;

    // 当前绑定对象
    TWeakObjectPtr<UInputComponent> BoundInput;
    UPROPERTY(Transient)
    TObjectPtr<ADeliveryCharacter> Character;

    // 移动输入回调
    void Move(const FInputActionValue &Value);
    void Look(const FInputActionValue &Value);
    void MouseLook(const FInputActionValue &Value);
    void JumpStarted(const FInputActionValue &Value);

    // 出拳与双键抓取回调
    void AttackLeftStarted(const FInputActionValue &Value);
    void AttackRightStarted(const FInputActionValue &Value);
    void AttackLeftEnded(const FInputActionValue &Value);
    void AttackRightEnded(const FInputActionValue &Value);
    /** 等待双键组合的时间结束后，将首次按下的鼠标键作为出拳操作 */
    void ResolveSingleMousePress();
    /** 记录鼠标按下状态，区分单键出拳和双键抓取 */
    void MousePressed(bool bLeft);
    /** 松开对应的抓取手；短按未触发抓取时执行一次出拳 */
    void MouseReleased(bool bLeft);

    // 交互与物品回调
    void InteractStarted(const FInputActionValue &Value);
    void PickupStarted(const FInputActionValue &Value);
    void PickupEnded(const FInputActionValue &Value);
    void InventorySlot1();
    void InventorySlot2();
    void InventorySlot3();
    void InventorySlot4();
    void InventorySlot5();
    void SummonVehiclePressed();

    // 鼠标组合键状态
    FTimerHandle MouseChordTimer;
    bool bLeftMouseDown = false;
    bool bRightMouseDown = false;
    bool bGrabChordActive = false;
    /** 此次鼠标按下是否已按单键处理，防止重复出拳或改判为抓取 */
    bool bSingleMouseResolved = false;
    /** 首次按下的是否为左键，用于确定等待结束后打哪一拳 */
    bool bFirstMouseLeft = false;

};
