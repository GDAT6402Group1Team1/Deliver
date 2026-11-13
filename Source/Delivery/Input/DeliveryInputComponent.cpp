#include "Input/DeliveryInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
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
    // 这些动作先保存资源路径，绑定时再取得实际的 IA 对象
    SummonAction = TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_SummonVehicle.IA_SummonVehicle")));
    for (int32 Slot = 1; Slot <= 5; ++Slot)
    {
        const FString Name = FString::Printf(TEXT("IA_InventorySlot%d"), Slot);
        InventoryActions.Add(TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/") + Name + TEXT(".") + Name)));
    }
    InteractAction =
        TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Interact.IA_Interact")));
    PickupAction =
        TSoftObjectPtr<UInputAction>(FSoftObjectPath(TEXT("/Game/Input/Actions/IA_Pickup.IA_Pickup")));
    // 基础动作随组件加载，蓝图仍可覆盖这些默认配置
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
    // 同一个输入组件只绑定一次，避免按一次键触发多次操作
#if !UE_BUILD_SHIPPING
    // 同名不等于同一个对象，所以要打指针；"绑过了"这道早退一旦误判，
    // 表现就是整具身体一个键都不响应，而外部完全看不出来
    UE_LOG(LogDelivery, Log,
           TEXT("InputDiag %.3f Bind 请求 input=%s(%p) 已绑=%s(%p) 早退=%s owner=%s"),
           GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f, *GetNameSafe(PlayerInputComponent),
           PlayerInputComponent, *GetNameSafe(BoundInput.Get()), BoundInput.Get(),
           (PlayerInputComponent && BoundInput.Get() == PlayerInputComponent) ? TEXT("是！") : TEXT("否"),
           *GetNameSafe(GetOwner()));
#endif
    if (!PlayerInputComponent || BoundInput.Get() == PlayerInputComponent) return;
    Character = Cast<ADeliveryCharacter>(GetOwner());
    if (!Character) return;
    UEnhancedInputComponent *EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!EnhancedInputComponent)
    {
        UE_LOG(LogDelivery, Error, TEXT("%s：没有 Enhanced Input 组件。"), *GetNameSafe(this));
        return;
    }

    // 记住本次绑定对象，同一输入组件再次传入时不重复绑定
    BoundInput = PlayerInputComponent;
    // 视角持续接收输入，鼠标单独应用灵敏度
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
    // 松开移动键时也调用 Move，将移动方向清零
    if (MoveAction)
    {
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this,
                                           &UDeliveryInputComponent::Move);
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::Move);
    }
    // 跳跃只处理按下，不随持续按住重复发起
    if (JumpAction)
    {
        EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::JumpStarted);
    }
    // 左右键先判断组合抓取，右键拖动只转视角；输入取消时不补出拳
    if (AttackLeftAction)
    {
        EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::AttackLeftStarted);
        EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::AttackLeftEnded);
        EnhancedInputComponent->BindAction(AttackLeftAction, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::AttackLeftCanceled);
    }
    if (AttackRightAction)
    {
        EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::AttackRightStarted);
        EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::AttackRightEnded);
        EnhancedInputComponent->BindAction(AttackRightAction, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::AttackRightCanceled);
    }
    // 交互和拾取使用软引用，绑定前确保 IA 已加载；已在内存中则直接使用
    if (UInputAction *Interact = InteractAction.LoadSynchronous())
    {
        EnhancedInputComponent->BindAction(Interact, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::InteractStarted);
    }
    // 按下开始长按拾取，松开或取消时结束本次长按
    if (UInputAction *Pickup = PickupAction.LoadSynchronous())
    {
        EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::PickupStarted);
        EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::PickupEnded);
        EnhancedInputComponent->BindAction(Pickup, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::PickupEnded);
    }

    // 键盘和手柄映射到同一个动作，只绑定一次物品操作
    for (int32 Slot = 0; Slot < FMath::Min(InventoryActions.Num(), 5); ++Slot)
    {
        // 回调同时保存槽位索引，触发时直接交给背包
        if (UInputAction* Action = InventoryActions[Slot].LoadSynchronous())
        {
            EnhancedInputComponent->BindAction(Action, ETriggerEvent::Started, this,
                                               &UDeliveryInputComponent::InventorySlotSelected, Slot);
        }
    }
    // 召唤只发送操作请求，冷却和能否召唤由玩法组件检查
    if (UInputAction* Action = GetSummonAction())
    {
        EnhancedInputComponent->BindAction(Action, ETriggerEvent::Started, this,
                                           &UDeliveryInputComponent::SummonVehiclePressed);
    }

    // 刚接手的身体从"没有移动意图"开始。
    // 不能只靠失去控制权时的 ResetInput：那一帧 Enhanced Input 还会再派发一次
    // Triggered，把刚清掉的意图重新写回去（实测日志里 ResetInput 和 Move=(0,1)
    // 是同一个时间戳），而之后这具身体就被解除占有了，那次松开永远收不到。
    // 结果是上车时按着 W，下车后人就一直往前走；上车时没按，就定在原地。
    // 玩家真按着键的话，下一帧的 Triggered 自然会把意图补回来，不影响手感。
    Character->DoMove(0.f, 0.f);
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
    const FVector2D MouseDelta=Value.Get<FVector2D>();
    RightMouseGesture.AddMovement(MouseDelta,GFrameCounter);
    const FVector2D Axis = MouseDelta * MouseLookSensitivity;
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
    const auto* Player=Cast<APlayerController>(Character->GetController());
    MousePressed(false,Player && Player->IsInputKeyDown(EKeys::RightMouseButton));
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

void UDeliveryInputComponent::AttackLeftCanceled(const FInputActionValue &)
{
    MouseCanceled(true);
}

void UDeliveryInputComponent::AttackRightCanceled(const FInputActionValue &)
{
    MouseCanceled(false);
}

// 等待短暂的组合键窗口，再决定出拳还是抓取
void UDeliveryInputComponent::MousePressed(bool bLeft,bool bMouseRight)
{
    if (!IsValid(Character)) return;
    UDeliveryGrabComponent* GrabComponent = Character->GetGrabComponent();
    bool &ThisDown = bLeft ? bLeftMouseDown : bRightMouseDown;
    // 同一只键已经按住时忽略重复通知
    if (ThisDown)
        return;
    const bool bWasEmpty = !bLeftMouseDown && !bRightMouseDown;
    ThisDown = true;
    if (!bLeft && bMouseRight) RightMouseGesture.Begin(FPlatformTime::Seconds());
    if (bWasEmpty)
    {
        bFirstMouseLeft = bLeft;
        bSingleMouseResolved = false;
        // 保留原来的双键组合时间，鼠标右键不会在计时结束时自动出拳
        GetWorld()->GetTimerManager().SetTimer(MouseChordTimer, this, &UDeliveryInputComponent::ResolveSingleMousePress,
                                        0.2f, false);
    }
    else if (!bSingleMouseResolved && !bGrabChordActive)
    {
        // 第二只键在等待时间内按下，取消单键出拳并开始抓取
        GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
        bGrabChordActive = true;
        RightMouseGesture.BlockClick();
        if (GrabComponent)
            GrabComponent->RequestBegin();
    }
    else if (!bGrabChordActive)
    {
        // 第一只键已经按单键处理，第二只键继续作为独立出拳
        if (bLeft)
            Character->DoAttackLeft();
        else if (!RightMouseGesture.IsActive())
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
    else if (!bFirstMouseLeft && bRightMouseDown && !RightMouseGesture.IsActive())
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
    EDeliveryRightMouseRelease RightRelease=EDeliveryRightMouseRelease::OtherInput;
    if (!bLeft)
    {
        // 松开与视角输入可能在同一帧到达，补查本帧鼠标移动，避免短拖动被判为点击
        if (RightMouseGesture.IsActive())
            if (const auto* Player=Cast<APlayerController>(Character->GetController()))
            {
                float MouseX=0,MouseY=0;
                Player->GetInputMouseDelta(MouseX,MouseY);
                RightMouseGesture.AddMovement(FVector2D(MouseX,MouseY),GFrameCounter);
            }
        RightRelease=RightMouseGesture.Finish(FPlatformTime::Seconds());
    }
    ThisDown = false;
    if (bGrabChordActive)
    {
        // 只松开对应的手，另一只键仍按住时继续保留抓取状态
        if (GrabComponent)
            GrabComponent->RequestReleaseHand(bLeft);
        if (!bLeftMouseDown && !bRightMouseDown)
            bGrabChordActive = false;
    }
    else if (RightRelease!=EDeliveryRightMouseRelease::OtherInput)
    {
        GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
        bSingleMouseResolved=true;
        if (RightRelease==EDeliveryRightMouseRelease::Click) Character->DoAttackRight();
    }
    // 左键和手柄保留短按出拳
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
        // 两只键都松开后，下一次按下重新判断单键或双键
        GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
        bSingleMouseResolved = false;
    }
}

void UDeliveryInputComponent::MouseCanceled(bool bLeft)
{
    if (!IsValid(Character) || !(bLeft?bLeftMouseDown:bRightMouseDown)) return;
    GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
    bSingleMouseResolved=true;
    if (!bLeft) RightMouseGesture.BlockClick();
    MouseReleased(bLeft);
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

// 把动作对应的槽位交给背包，拾取和丢弃条件仍由背包检查
void UDeliveryInputComponent::InventorySlotSelected(int32 Slot)
{
    if (!IsValid(Character)) return;
    if (UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent())
    {
        Inventory->RequestSlotAction(Slot);
    }
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

// 输入状态清理

// 清除未完成的输入，避免控制权切换后继续执行旧操作
void UDeliveryInputComponent::ResetInput()
{
#if !UE_BUILD_SHIPPING
    UE_LOG(LogDelivery, Log, TEXT("InputDiag %.3f ResetInput owner=%s"),
           GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f, *GetNameSafe(GetOwner()));
#endif
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
    // 先清掉组合键状态，后续松开事件不会再触发短按攻击
    bLeftMouseDown = bRightMouseDown = bGrabChordActive = false;
    bSingleMouseResolved = bFirstMouseLeft = false;
    RightMouseGesture.Reset();
    // 失去控制权时引擎会销毁这具身体的 InputComponent，绑定记录按定义当场作废。
    // 不清的话，下次重新占有时新建的组件可能复用同一个对象槽位，
    // Bind() 开头那道"已经绑过了"的早退就会误判，结果是一个键都不绑、
    // 整具身体彻底没反应——而且从外面看绑定关系是"对"的，极难查。
    BoundInput.Reset();
    ADeliveryCharacter* OwnerCharacter = Cast<ADeliveryCharacter>(GetOwner());
    if (!IsValid(OwnerCharacter)) return;
    // 远端角色没有本机按键状态，不从模拟代理发送操作请求
    if (!OwnerCharacter->HasAuthority() && !Character) return;
    // 先停止移动和长按拾取，再释放抓取并取消出拳
    OwnerCharacter->DoMove(0.f, 0.f);
    OwnerCharacter->DoPickupRelease();
    if (UDeliveryGrabComponent* Grab = OwnerCharacter->GetGrabComponent())
    {
        // 服务器直接释放，操作玩家的客户端分别请求松开两只手
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


UInputAction* UDeliveryInputComponent::GetSummonAction() const
{
    return SummonAction.LoadSynchronous();
}
