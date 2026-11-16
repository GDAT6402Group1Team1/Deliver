#include "Input/DeliveryInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "GameFramework/PlayerController.h"
#include "Player/DeliveryPlayerController.h"
#include "InputCoreTypes.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Grab/DeliveryGrabComponent.h"
#include "Inventory/DeliveryInventoryComponent.h"
#include "Vehicle/Summon/DeliveryVehicleSummonComponent.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Delivery.h"

// ── 生命周期 ──
// 构造函数加载 IA；组件结束时 EndPlay 清掉组合键计时器

/// @brief 不需要每帧更新；构造时加载移动、视角、跳跃、左右拳的 IA，交互、拾取、召唤和五个物品栏 IA 只存路径，Bind 时再加载
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

// ── 绑定 ──
// 角色被玩家占有时 SetupPlayerInputComponent 调用 Bind；失去控制时角色调用 ResetInput

/// @brief 把所有步行动作绑到 PlayerInputComponent 上，同一个组件已经绑过时直接返回；最后调用 DoMove(0, 0) 清掉移动意图
/// 这里只绑定，按键时能不能做由角色和各玩法组件检查
void UDeliveryInputComponent::Bind(UInputComponent *PlayerInputComponent)
{
    // 同一个输入组件只绑定一次，避免按一次键触发多次操作
#if !UE_BUILD_SHIPPING
    // 日志打出两个组件的指针：同名的组件不一定是同一个对象
    // 下面这行判断误以为已经绑过时，整具身体一个键都不响应，从外面完全看不出来
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
    // 松开和取消都显式清零，不沿用结束事件可能携带的旧轴值
    if (MoveAction)
    {
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this,
                                           &UDeliveryInputComponent::Move);
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this,
                                           &UDeliveryInputComponent::MoveEnded);
        EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Canceled, this,
                                           &UDeliveryInputComponent::MoveEnded);
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

    // 刚接手的身体先把移动意图清零
    // 失去控制时 ResetInput 虽然清过一次，但同一帧 Enhanced Input 还会再派发一次 Triggered，把意图写回去
    // （日志里 ResetInput 和 Move=(0,1) 时间戳相同）；之后这具身体不再被控制，松开键的事件收不到
    // 不在这里清零的话，上车时按着 W，下车后人会一直往前走
    // 玩家真按着键时，下一帧的 Triggered 会重新写入意图
    Character->DoMove(0.f, 0.f);
}

// ── 移动 ──
// Enhanced Input 每帧派发 Triggered 时调用 Move、Look、MouseLook，按下跳跃键时调用 JumpStarted

/// @brief 移动轴；本机不再控制这具身体、或玩家控制器不接受步行输入时按松开处理
void UDeliveryInputComponent::Move(const FInputActionValue &Value)
{
    // 交接控制权当帧仍可能收到旧输入栈的 Triggered
    if (!IsValid(Character) || !Character->IsLocallyControlled()) return;
    const auto* Player = Cast<ADeliveryPlayerController>(Character->GetController());
    if (Player && (!Player->CanAcceptWalkingInput() || Player->IsMoveInputIgnored()))
    {
        MoveEnded(Value);
        return;
    }
    const FVector2D Axis = Value.Get<FVector2D>();
    Character->DoMove(Axis.X, Axis.Y);
}

/// @brief 移动键松开或取消：移动意图清零
void UDeliveryInputComponent::MoveEnded(const FInputActionValue &)
{
    if (IsValid(Character)) Character->DoMove(0.0f, 0.0f);
}

/// @brief 手柄右摇杆转视角
void UDeliveryInputComponent::Look(const FInputActionValue &Value)
{
    if (!IsValid(Character)) return;
    const FVector2D Axis = Value.Get<FVector2D>();
    Character->DoLook(Axis.X, Axis.Y);
}

/// @brief 鼠标转视角；同时记进 RightMouseGesture，用来区分右键单击和拖动
void UDeliveryInputComponent::MouseLook(const FInputActionValue &Value)
{
    if (!IsValid(Character)) return;
    const FVector2D MouseDelta=Value.Get<FVector2D>();
    RightMouseGesture.AddMovement(MouseDelta,GFrameCounter);
    const FVector2D Axis = MouseDelta * MouseLookSensitivity;
    Character->DoLook(Axis.X, Axis.Y);
}

/// @brief 跳跃键按下
void UDeliveryInputComponent::JumpStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoJumpStart();
}

// ── 出拳与抓取 ──
// 左右键按下调用 MousePressed，开始等 0.2 秒；0.2 秒内按下另一只键开始双手抓取，到时调用 ResolveSingleMousePress 出单拳
// 松开调用 MouseReleased，被取消调用 MouseCanceled

/// @brief 左键按下，调用 MousePressed(true)
void UDeliveryInputComponent::AttackLeftStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MousePressed(true);
}

/// @brief 右键按下，调用 MousePressed(false)；是鼠标右键（不是手柄）时开始记录拖动
void UDeliveryInputComponent::AttackRightStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    const auto* Player=Cast<APlayerController>(Character->GetController());
    MousePressed(false,Player && Player->IsInputKeyDown(EKeys::RightMouseButton));
}

/// @brief 左键松开，调用 MouseReleased(true)
void UDeliveryInputComponent::AttackLeftEnded(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MouseReleased(true);
}

/// @brief 右键松开，调用 MouseReleased(false)
void UDeliveryInputComponent::AttackRightEnded(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    MouseReleased(false);
}

/// @brief 左键被取消，调用 MouseCanceled(true)
void UDeliveryInputComponent::AttackLeftCanceled(const FInputActionValue &)
{
    MouseCanceled(true);
}

/// @brief 右键被取消，调用 MouseCanceled(false)
void UDeliveryInputComponent::AttackRightCanceled(const FInputActionValue &)
{
    MouseCanceled(false);
}

/// @brief 记下这只键按下：第一只键开始等 0.2 秒；等待期间按下第二只键就调用 GrabComponent->RequestBegin 开始抓取
/// 第一只键已经按单键出过拳时，第二只键也单独出拳
void UDeliveryInputComponent::MousePressed(bool bLeft,bool bMouseRight)
{
    if (!IsValid(Character)) return;
    if (auto* Body=Character->GetActiveRagdoll()) Body->CancelConversationFacing();
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

/// @brief 0.2 秒到了还没按第二只键：第一只是左键就出左拳，是手柄右键就出右拳；鼠标右键等松开时再判断单击还是拖动
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

/// @brief 这只键松开：在抓取时只松开这只手；鼠标右键按单击还是拖动判断要不要出右拳；还没出过拳的短按补出一拳
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

/// @brief 这只键被取消（例如打开界面）：不出拳，只按松开处理抓取
void UDeliveryInputComponent::MouseCanceled(bool bLeft)
{
    if (!IsValid(Character) || !(bLeft?bLeftMouseDown:bRightMouseDown)) return;
    GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
    bSingleMouseResolved=true;
    if (!bLeft) RightMouseGesture.BlockClick();
    MouseReleased(bLeft);
}

// ── 交互与物品 ──
// 都转给角色或背包、召唤组件，能不能做由它们检查

/// @brief 交互键按下，调用角色的 DoInteract
void UDeliveryInputComponent::InteractStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoInteract();
}

/// @brief 拾取键按下，调用角色的 DoPickup 开始长按
void UDeliveryInputComponent::PickupStarted(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoPickup();
}

/// @brief 拾取键松开或取消，调用角色的 DoPickupRelease
void UDeliveryInputComponent::PickupEnded(const FInputActionValue & /* 未使用 */)
{
    if (!IsValid(Character)) return;
    Character->DoPickupRelease();
}

/// @brief 物品栏键按下，调用背包的 RequestSlotAction(Slot)；能不能拿出或丢下由背包检查
void UDeliveryInputComponent::InventorySlotSelected(int32 Slot)
{
    if (!IsValid(Character)) return;
    if (UDeliveryInventoryComponent* Inventory = Character->GetInventoryComponent())
    {
        Inventory->RequestSlotAction(Slot);
    }
}

/// @brief 召唤键按下；没晕倒时调用召唤组件的 RequestSummon
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

// ── 绑定 ──

/// @brief 失去控制时调用：清掉鼠标组合键状态和 BoundInput，停止移动和长按拾取，松开抓取，取消出拳
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
    // 失去控制时引擎会销毁这具身体的 InputComponent，BoundInput 记的那个组件已经没了，这里清掉
    // 不清的话，下次重新占有时新建的组件可能占用旧组件的对象槽位，Bind 开头的判断会以为已经绑过而直接返回，一个键都不绑
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

// ── 生命周期 ──

/// @brief 组件结束时清掉组合键计时器；网络和玩法状态由各组件自己的 EndPlay 清理
void UDeliveryInputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 销毁时只清本机计时器，网络和玩法状态由各组件自己的结束流程清理
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(MouseChordTimer);
    Super::EndPlay(EndPlayReason);
}

// ── 绑定 ──

/// @brief 召唤车辆的 IA，提示组件用它查当前映射的按键
UInputAction* UDeliveryInputComponent::GetSummonAction() const
{
    return SummonAction.LoadSynchronous();
}
