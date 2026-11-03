#include "Input/DeliveryInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Vehicle/DeliveryVehicleSummonComponent.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Delivery.h"

UDeliveryInputComponent::UDeliveryInputComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SummonVehicleKey = EKeys::R;
    InteractAction =
        TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
    PickupAction =
        TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Pickup.IA_Pickup")));
    static ConstructorHelpers::FObjectFinder<UInputAction> LookActionFinder(
        TEXT("/Game/Input/Actions/IA_Look"));
    if (LookActionFinder.Succeeded())
    {
        LookAction = LookActionFinder.Object;
    }

    static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookActionFinder(
        TEXT("/Game/Input/Actions/IA_MouseLook"));
    if (MouseLookActionFinder.Succeeded())
    {
        MouseLookAction = MouseLookActionFinder.Object;
    }

    static ConstructorHelpers::FObjectFinder<UInputAction> MoveActionFinder(
        TEXT("/Game/Input/Actions/IA_Move"));
    if (MoveActionFinder.Succeeded())
    {
        MoveAction = MoveActionFinder.Object;
    }

    static ConstructorHelpers::FObjectFinder<UInputAction> JumpActionFinder(
        TEXT("/Game/Input/Actions/IA_Jump"));
    if (JumpActionFinder.Succeeded())
    {
        JumpAction = JumpActionFinder.Object;
    }

    static ConstructorHelpers::FObjectFinder<UInputAction> AttackLeftFinder(
        TEXT("/Game/Input/Actions/IA_AttackLeft"));
    if (AttackLeftFinder.Succeeded())
    {
        AttackLeftAction = AttackLeftFinder.Object;
    }

    static ConstructorHelpers::FObjectFinder<UInputAction> AttackRightFinder(
        TEXT("/Game/Input/Actions/IA_AttackRight"));
    if (AttackRightFinder.Succeeded())
    {
        AttackRightAction = AttackRightFinder.Object;
    }

}

// 输入动作绑定

// 绑定步行输入，玩法组件仍负责检查操作条件
void UDeliveryInputComponent::Bind(UInputComponent *PlayerInputComponent)
{
    if (!PlayerInputComponent || BoundInput.Get() == PlayerInputComponent) return;
    Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character) return;
    UDeliveryVehicleSummonComponent* VehicleSummon = Character->FindComponentByClass<UDeliveryVehicleSummonComponent>();
    UEnhancedInputComponent *EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!EnhancedInputComponent)
    {
        UE_LOG(LogDelivery, Error, TEXT("%s：没有 Enhanced Input 组件。"), *GetNameSafe(this));
        return;
    }

    // 记住本次绑定对象，同一输入组件再次传入时不重复绑定
    BoundInput = PlayerInputComponent;
    if (LookAction)
    {
        EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this,
                                           &UDeliveryInputComponent::Look);
    }
    if (MouseLookAction)
    {
        EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this,
                                           &UDeliveryInputComponent::MouseLook);
    }
    if (MoveAction)
    {
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this,
                                           &UDeliveryInputComponent::Move);
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::Move);
    }
    if (JumpAction)
    {
        EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::JumpStarted);
    }
    // 攻击完成和输入取消都结束蓄力，避免切换输入状态后仍保持出拳准备
    if (AttackLeftAction)
    {
        EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::AttackLeftStarted);
        EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::AttackLeftEnded);
        EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::AttackLeftEnded);
    }
    if (AttackRightAction)
    {
        EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::AttackRightStarted);
        EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::AttackRightEnded);
        EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::AttackRightEnded);
    }
    if (UInputAction *Interact = InteractAction.LoadSynchronous())
    {
        EnhancedInputComponent->BindAction(Interact, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::InteractStarted);
    }
    if (UInputAction *Pickup = PickupAction.LoadSynchronous())
    {
        EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::PickupStarted);
        EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::PickupEnded);
        EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::PickupEnded);
    }

    // 数字键 1 丢弃手中物品，2 至 5 将对应槽位与手中物品交换
    PlayerInputComponent->BindKey(EKeys::One, IE_Pressed, this, &UDeliveryInputComponent::InventorySlot1);
    PlayerInputComponent->BindKey(EKeys::Two, IE_Pressed, this, &UDeliveryInputComponent::InventorySlot2);
    PlayerInputComponent->BindKey(EKeys::Three, IE_Pressed, this, &UDeliveryInputComponent::InventorySlot3);
    PlayerInputComponent->BindKey(EKeys::Four, IE_Pressed, this, &UDeliveryInputComponent::InventorySlot4);
    PlayerInputComponent->BindKey(EKeys::Five, IE_Pressed, this, &UDeliveryInputComponent::InventorySlot5);
    // 为手柄绑定相同的物品操作，方向键上仍用于打开手机
    PlayerInputComponent->BindKey(EKeys::Gamepad_DPad_Down, IE_Pressed, this,
                                  &UDeliveryInputComponent::InventorySlot1);
    PlayerInputComponent->BindKey(EKeys::Gamepad_DPad_Left, IE_Pressed, this,
                                  &UDeliveryInputComponent::InventorySlot2);
    PlayerInputComponent->BindKey(EKeys::Gamepad_DPad_Right, IE_Pressed, this,
                                  &UDeliveryInputComponent::InventorySlot3);
    PlayerInputComponent->BindKey(EKeys::Gamepad_LeftShoulder, IE_Pressed, this,
                                  &UDeliveryInputComponent::InventorySlot4);
    PlayerInputComponent->BindKey(EKeys::Gamepad_RightShoulder, IE_Pressed, this,
                                  &UDeliveryInputComponent::InventorySlot5);

    if (SummonVehicleKey.IsValid())
    {
        PlayerInputComponent->BindKey(SummonVehicleKey, IE_Pressed, this,
                                      &UDeliveryInputComponent::SummonVehiclePressed);
        if (VehicleSummon)
        {
            // 使用实际绑定的键名显示提示，避免改键后提示仍显示旧按键
            VehicleSummon->SetDisplayKeyName(SummonVehicleKey.GetDisplayName(false));
        }
    }

}

// 移动与视角

void UDeliveryInputComponent::Move(const FInputActionValue &Value)
{
    if (!IsValid(Character)) return;
    const FVector2D Axis = Value.Get<FVector2D>();
    Character->DoMove(Axis.X, Axis.Y);
}

void UDeliveryInputComponent::Look(const FInputActionValue &Value)
{
    if (!IsValid(Character)) return;
    const FVector2D Axis = Value.Get<FVector2D>();
    Character->DoLook(Axis.X, Axis.Y);
}

void UDeliveryInputComponent::MouseLook(const FInputActionValue &Value)
{
    if (!IsValid(Character)) return;
    const FVector2D Axis = Value.Get<FVector2D>() * MouseLookSensitivity;
    Character->DoLook(Axis.X, Axis.Y);
}

void UDeliveryInputComponent::JumpStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoJumpStart();
}

// 左右拳与双键抓取

void UDeliveryInputComponent::AttackLeftStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MousePressed(true);
}

void UDeliveryInputComponent::AttackRightStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MousePressed(false);
}

void UDeliveryInputComponent::AttackLeftEnded(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MouseReleased(true);
}

void UDeliveryInputComponent::AttackRightEnded(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MouseReleased(false);
}

// 等待短暂的组合键窗口，再决定出拳还是抓取
void UDeliveryInputComponent::MousePressed(bool bLeft)
{
    if (!IsValid(Character)) return;
    UDeliveryGrabComponent* GrabComponent = Character->GetGrabComponent();
    bool &ThisDown = bLeft ? bLeftMouseDown : bRightMouseDown;
    if (ThisDown)
        return;
    const bool bWasEmpty = !bLeftMouseDown && !bRightMouseDown;
    ThisDown = true;
    if (bWasEmpty)
    {
        bFirstMouseLeft = bLeft;
        bSingleMouseResolved = false;
        // 先等待另一只鼠标键；等待期间双键按下则抓取，否则按单键出拳
        GetWorld()->GetTimerManager().SetTimer(MouseChordTimer, this, &UDeliveryInputComponent::ResolveSingleMousePress,
                                        0.2f, false);
    }
    else if (!bSingleMouseResolved && !bGrabChordActive)
    {
        GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
        bGrabChordActive = true;
        if (GrabComponent)
            GrabComponent->RequestBegin();
    }
    else if (!bGrabChordActive)
    {
        if (bLeft)
            Character->DoAttackLeft();
        else
            Character->DoAttackRight();
    }
}

// 等待结束后只处理第一次按下的键
void UDeliveryInputComponent::ResolveSingleMousePress()
{
    if (!IsValid(Character)) return;
    if (bGrabChordActive || bSingleMouseResolved)
        return;
    bSingleMouseResolved = true;
    if (bFirstMouseLeft && bLeftMouseDown)
        Character->DoAttackLeft();
    else if (!bFirstMouseLeft && bRightMouseDown)
        Character->DoAttackRight();
}

// 释放抓取，或将尚未处理的短按交给攻击入口
void UDeliveryInputComponent::MouseReleased(bool bLeft)
{
    if (!IsValid(Character)) return;
    UDeliveryGrabComponent* GrabComponent = Character->GetGrabComponent();
    bool &ThisDown = bLeft ? bLeftMouseDown : bRightMouseDown;
    if (!ThisDown)
        return;
    ThisDown = false;
    if (bGrabChordActive)
    {
        if (GrabComponent)
            GrabComponent->RequestReleaseHand(bLeft);
        if (!bLeftMouseDown && !bRightMouseDown)
            bGrabChordActive = false;
    }
    // 短按在等待结束前松开，也应执行一次攻击
    else if (!bSingleMouseResolved)
    {
        GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
        bSingleMouseResolved = true;
        if (bLeft)
            Character->DoAttackLeft();
        else
            Character->DoAttackRight();
    }
    if (!bLeftMouseDown && !bRightMouseDown)
    {
        GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
        bSingleMouseResolved = false;
    }
}

// 交互与物品栏

void UDeliveryInputComponent::InteractStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoInteract();
}

void UDeliveryInputComponent::PickupStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoPickup();
}

void UDeliveryInputComponent::PickupEnded(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoPickupRelease();
}

void UDeliveryInputComponent::InventorySlot1()
{
    if (!IsValid(Character)) return;
    UDeliveryInventoryComponent* InventoryComponent = Character->GetInventoryComponent();
    if (InventoryComponent)
        InventoryComponent->RequestSlotAction(0);
}

void UDeliveryInputComponent::InventorySlot2()
{
    if (!IsValid(Character)) return;
    UDeliveryInventoryComponent* InventoryComponent = Character->GetInventoryComponent();
    if (InventoryComponent)
        InventoryComponent->RequestSlotAction(1);
}

void UDeliveryInputComponent::InventorySlot3()
{
    if (!IsValid(Character)) return;
    UDeliveryInventoryComponent* InventoryComponent = Character->GetInventoryComponent();
    if (InventoryComponent)
        InventoryComponent->RequestSlotAction(2);
}

// 输入状态清理

void UDeliveryInputComponent::InventorySlot4()
{
    if (!IsValid(Character)) return;
    UDeliveryInventoryComponent* InventoryComponent = Character->GetInventoryComponent();
    if (InventoryComponent)
        InventoryComponent->RequestSlotAction(3);
}

void UDeliveryInputComponent::InventorySlot5()
{
    if (!IsValid(Character)) return;
    UDeliveryInventoryComponent* InventoryComponent = Character->GetInventoryComponent();
    if (InventoryComponent)
        InventoryComponent->RequestSlotAction(4);
}

void UDeliveryInputComponent::SummonVehiclePressed()
{
    if (!IsValid(Character)) return;
    UDeliveryVehicleSummonComponent* VehicleSummon = Character->FindComponentByClass<UDeliveryVehicleSummonComponent>();
    // 晕倒期间禁止召唤车辆
    if (VehicleSummon && !Character->IsIncapacitated())
    {
        VehicleSummon->RequestSummon();
    }
}

// 清除未完成的输入，避免控制权切换后继续执行旧操作
void UDeliveryInputComponent::ResetInput()
{
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
    // 先清掉组合键状态，后续松开事件不会再触发短按攻击
    bLeftMouseDown = bRightMouseDown = bGrabChordActive = false;
    bSingleMouseResolved = bFirstMouseLeft = false;
    ADeliveryCharacter* OwnerCharacter = Cast<ADeliveryCharacter>(GetOwner());
    if (!IsValid(OwnerCharacter)) return;
    // 远端角色没有本机按键状态，不从模拟代理发送操作请求
    if (!OwnerCharacter->HasAuthority() && !Character) return;
    OwnerCharacter->DoMove(0.f, 0.f);
    OwnerCharacter->DoPickupRelease();
    if (UDeliveryGrabComponent* Grab = OwnerCharacter->GetGrabComponent())
    {
        if (OwnerCharacter->HasAuthority()) Grab->ForceRelease();
        else { Grab->RequestReleaseHand(true); Grab->RequestReleaseHand(false); }
    }
    if (UDeliveryRagdollCombatComponent* Combat = OwnerCharacter->GetRagdollCombat())
        Combat->CancelPunch();
}

void UDeliveryInputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 销毁时只清本机计时器，网络和玩法状态由各组件自己的结束流程清理
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
    Super::EndPlay(EndPlayReason);
}

