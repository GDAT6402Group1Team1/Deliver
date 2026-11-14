#include "Input/DeliveryInputComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "EnhancedInputComponent.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "UObject/UnrealType.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Player/DeliveryPlayerController.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "UI/DeliveryHUD.h"
#include "Game/DeliveryGameMode.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryInputLifecycleTest, "Delivery.Systems.InputLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryMoveInputSafetyTest, "Delivery.Systems.MoveInputSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryMoveInputSafetyTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("World"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Character = World->SpawnActor<ADeliveryCharacter>(Spawn);
    auto* Player = World->SpawnActor<ADeliveryPlayerController>(Spawn);
    auto* Bike = World->SpawnActor<ADeliveryMotorbike>(Spawn);
    if (!Character || !Player || !Bike) return false;
    // 独立世界没有 NetDriver/ULocalPlayer，显式建立本机输入的前提
    Player->SetAsLocalPlayerController();
    Player->Possess(Character);
    if (!TestTrue(TEXT("测试角色由本机控制器持有"), Character->IsLocallyControlled())) return false;
    auto* Input = Character->GetPlayerInput();
    auto* Body = Character->GetActiveRagdoll();
    auto* Bindings = NewObject<UEnhancedInputComponent>(Character);
    Input->Bind(Bindings);

    // 使用正式绑定派发两个结束事件，不能只检查函数存在
    for (const ETriggerEvent Event : {ETriggerEvent::Completed, ETriggerEvent::Canceled})
    {
        Character->DoMove(0.0f, 1.0f);
        bool bDispatched = false;
        for (const auto& Binding : Bindings->GetActionEventBindings())
        {
            if (Binding->GetAction() == Input->MoveAction && Binding->GetTriggerEvent() == Event)
            {
                Binding->Execute(FInputActionInstance(Input->MoveAction));
                bDispatched = true;
            }
        }
        TestTrue(TEXT("移动结束事件实际绑定并派发"), bDispatched);
        TestTrue(TEXT("结束和取消均停止移动"), Body->GetMoveInputForDebug().IsNearlyZero());
    }
    Character->DoMove(0.0f, 1.0f);
    Input->MoveEnded(FInputActionValue(FVector2D(0.0f, 1.0f)));
    TestTrue(TEXT("结束回调即使携带旧轴值也归零"), Body->GetMoveInputForDebug().IsNearlyZero());

    Character->DoMove(0.3f, 0.6f);
    Player->FlushPressedKeys();
    TestTrue(TEXT("引擎失焦清键同时停止身体"), Body->GetMoveInputForDebug().IsNearlyZero());
    TestEqual(TEXT("清键不破坏现有输入绑定"), Input->GetBoundInputForDebug(), static_cast<const UInputComponent*>(Bindings));
    Player->UpdateWalkingInputFocus(false);
    Input->Move(FInputActionValue(FVector2D(0.0f, 1.0f)));
    TestTrue(TEXT("UI取得焦点后旧Triggered不能重新驱动身体"), Body->GetMoveInputForDebug().IsNearlyZero());
    Player->UpdateWalkingInputFocus(true);
    Input->Move(FInputActionValue(FVector2D(0.3f, 0.6f)));
    TestTrue(TEXT("重新输入保持手柄模拟轴幅度"), Body->GetMoveInputForDebug().Equals(FVector2D(0.3f, 0.6f)));

    // 保留旧签名，同时给无松键包的请求设置有界有效期
    Character->ServerSetMoveInput_Implementation(FVector2D(0.0f, 1.0f), 30.0f);
    TestFalse(TEXT("旧两参数入口仍接受移动"), Body->GetMoveInputForDebug().IsNearlyZero());
    TestTrue(TEXT("远端输入设置过期计时器"), World->GetTimerManager().IsTimerActive(Character->MoveInputExpiryTimer));
    Character->LastServerMoveInputTime = World->GetTimeSeconds() - 2.0;
    Character->ExpireServerMoveInput();
    TestTrue(TEXT("丢失松键包后过期归零"), Body->GetMoveInputForDebug().IsNearlyZero());

    Character->ServerSetMoveInputSequenced_Implementation(FVector2D(0.0f, 1.0f), 0.0f, 10);
    Character->LastServerMoveInputTime = World->GetTimeSeconds() - 2.0;
    Character->ServerSetMoveInputSequenced_Implementation(FVector2D(0.25f, 0.75f), 0.0f, 11);
    Character->ExpireServerMoveInput();
    TestTrue(TEXT("持续新输入续期且保持轴值"), Body->GetMoveInputForDebug().Equals(FVector2D(0.25f, 0.75f)));
    Character->ServerStopMoveInput_Implementation(12);
    TestTrue(TEXT("可靠停止立即清零"), Body->GetMoveInputForDebug().IsNearlyZero());
    Character->ServerSetMoveInputSequenced_Implementation(FVector2D(0.0f, 1.0f), 0.0f, 11);
    TestTrue(TEXT("迟到移动不能越过较新的停止"), Body->GetMoveInputForDebug().IsNearlyZero());
    Character->ServerSetMoveInputSequenced_Implementation(FVector2D(0.5f, 0.5f), 0.0f, 14);
    Character->ServerStopMoveInput_Implementation(13);
    TestTrue(TEXT("迟到可靠停止不能覆盖更新的移动"), Body->GetMoveInputForDebug().Equals(FVector2D(0.5f, 0.5f)));
    Character->ServerSetMoveInput_Implementation(FVector2D::ZeroVector, 0.0f);
    TestTrue(TEXT("启用序号后拒绝混入无序旧请求"), Body->GetMoveInputForDebug().Equals(FVector2D(0.5f, 0.5f)));
    Character->LastServerMoveInputSequence = MAX_uint32;
    Character->ServerStopMoveInput_Implementation(0);
    TestTrue(TEXT("序号绕回仍可停止"), Body->GetMoveInputForDebug().IsNearlyZero());
    Character->ServerSetMoveInputSequenced_Implementation(FVector2D(0.0f, 1.0f), 0.0f, MAX_uint32);
    TestTrue(TEXT("绕回前的旧移动仍被拒绝"), Body->GetMoveInputForDebug().IsNearlyZero());

    Player->Possess(Bike);
    Input->Move(FInputActionValue(FVector2D(0.0f, 1.0f)));
    Character->ServerSetMoveInputSequenced_Implementation(FVector2D(0.0f, 1.0f), 0.0f, 15);
    TestTrue(TEXT("上车后的旧本机事件和旧角色请求均不能恢复移动"), Body->GetMoveInputForDebug().IsNearlyZero());
    TestFalse(TEXT("控制权交接清理移动计时器"), World->GetTimerManager().IsTimerActive(Character->MoveInputExpiryTimer));
    Player->Possess(Character);
    Input->Bind(NewObject<UEnhancedInputComponent>(Character));
    Input->Move(FInputActionValue(FVector2D(-0.5f, 0.4f)));
    TestTrue(TEXT("下车重新输入仍正常"), Body->GetMoveInputForDebug().Equals(FVector2D(-0.5f, 0.4f)));
    return true;
}

bool FDeliveryInputLifecycleTest::RunTest(const FString&)
{
    const auto Settings = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Settings);
    if (!TestNotNull(TEXT("World"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); };
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADeliveryCharacter* Character = World->SpawnActor<ADeliveryCharacter>(Spawn);
    ADeliveryPlayerController* Player = World->SpawnActor<ADeliveryPlayerController>(Spawn);
    ADeliveryMotorbike* Bike = World->SpawnActor<ADeliveryMotorbike>(Spawn);
    if (!Character || !Player || !Bike) return false;
    Player->Possess(Character);
    UDeliveryInputComponent* Input = Character->GetPlayerInput();
    UEnhancedInputComponent* Bindings = NewObject<UEnhancedInputComponent>(Character);
    Input->Bind(Bindings);
    const int32 BindingCount = Bindings->GetActionEventBindings().Num();
    TestTrue(TEXT("Gameplay actions bound"), BindingCount > 0);
    TestEqual(TEXT("快捷栏和召唤不再直接绑定按键"), Bindings->KeyBindings.Num(), 0);
    TestEqual(TEXT("五个快捷栏动作"), Input->InventoryActions.Num(), 5);
    TestNotNull(TEXT("召唤动作加载成功"), Input->GetSummonAction());
    TestNotNull(TEXT("车辆视角动作加载成功"), Bike->GetCameraToggleAction());
    UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, TEXT("/Game/Input/IMC_Default.IMC_Default"));
    if (TestNotNull(TEXT("默认输入映射"), Context))
    {
        const auto HasMapping = [Context](const TCHAR* Name, FKey Key)
        {
            return Context->GetMappings().ContainsByPredicate([&](const FEnhancedActionKeyMapping& Mapping)
            {
                return Mapping.Action && Mapping.Action->GetFName() == Name && Mapping.Key == Key;
            });
        };
        TestTrue(TEXT("R 召唤"), HasMapping(TEXT("IA_SummonVehicle"), EKeys::R));
        TestTrue(TEXT("P 切换视角"), HasMapping(TEXT("IA_ToggleCamera"), EKeys::P));
        TestTrue(TEXT("J 手机"), HasMapping(TEXT("IA_TogglePhone"), EKeys::J));
        TestTrue(TEXT("手柄手机"), HasMapping(TEXT("IA_TogglePhone"), EKeys::Gamepad_DPad_Up));
        const FKey Keyboard[] = {EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five};
        const FKey Gamepad[] = {EKeys::Gamepad_DPad_Down, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_DPad_Right,
                               EKeys::Gamepad_LeftShoulder, EKeys::Gamepad_RightShoulder};
        for (int32 Index = 0; Index < 5; ++Index)
        {
            const FString Name = FString::Printf(TEXT("IA_InventorySlot%d"), Index + 1);
            TestTrue(*Name, HasMapping(*Name, Keyboard[Index]) && HasMapping(*Name, Gamepad[Index]));
        }
    }
    Input->Bind(Bindings);
    TestEqual(TEXT("Repeated setup does not duplicate bindings"), Bindings->GetActionEventBindings().Num(), BindingCount);

    // 角色失去控制权时，尚未判定的单键不能在车辆上继续触发
    Input->MousePressed(true);
    TestTrue(TEXT("Chord wait started"), World->GetTimerManager().IsTimerActive(Input->MouseChordTimer));
    Player->Possess(Bike);
    TestFalse(TEXT("Possession clears chord timer"), World->GetTimerManager().IsTimerActive(Input->MouseChordTimer));
    TestFalse(TEXT("Possession clears held buttons"), Input->bLeftMouseDown || Input->bRightMouseDown);
    Input->MouseReleased(true);
    TestFalse(TEXT("Release after reset does not resolve an attack"), Input->bSingleMouseResolved);
    Player->Possess(Character);
    Input->MousePressed(true);
    Input->MousePressed(false);
    TestTrue(TEXT("Chord works after returning to character"), Character->IsGrabChordHeld());
    Input->ResetInput();
    TestFalse(TEXT("Reset releases chord"), Character->IsGrabChordHeld());

    // 右键拖动先转视角，在组合时间内补按左键仍可以抓取
    Input->MousePressed(false,true);
    Input->MouseLook(FInputActionValue(FVector2D(10,0)));
    Input->MousePressed(true);
    TestTrue(TEXT("右键转视角后及时补按左键仍能抓取"),Character->IsGrabChordHeld());
    Input->MouseReleased(false);
    TestTrue(TEXT("松开右手后左手保持按住"),Input->bLeftMouseDown && !Input->bRightMouseDown);
    TestFalse(TEXT("松开右手后不遗留鼠标点击"),Input->RightMouseGesture.IsActive());
    Input->MouseReleased(true);
    TestFalse(TEXT("双手松开后结束抓取输入"),Input->bGrabChordActive);

    Input->MousePressed(true);
    Input->MousePressed(false,true);
    TestTrue(TEXT("先左键后右键仍能抓取"),Character->IsGrabChordHeld());
    Input->MouseCanceled(false);
    TestTrue(TEXT("取消右键只释放右手"),Input->bLeftMouseDown && !Input->bRightMouseDown);
    Input->MouseReleased(true);
    TestFalse(TEXT("抓取取消后的松开不遗留待处理攻击"),Input->bSingleMouseResolved);

    Input->MousePressed(false,true);
    Input->MouseCanceled(false);
    TestFalse(TEXT("取消右键清除手势与按住状态"),Input->RightMouseGesture.IsActive() || Input->bRightMouseDown);
    TestFalse(TEXT("取消右键清除组合计时"),World->GetTimerManager().IsTimerActive(Input->MouseChordTimer));
    Input->MouseReleased(false);
    TestFalse(TEXT("取消后迟到的松开不会重新处理攻击"),Input->bSingleMouseResolved);

    Input->MousePressed(false,true);
    TestTrue(TEXT("鼠标右键记录单独的点击判断"),Input->RightMouseGesture.IsActive());
    Player->Possess(Bike);
    TestFalse(TEXT("上车时清除右键点击判断"),Input->RightMouseGesture.IsActive());
    Input->MouseReleased(false);
    TestFalse(TEXT("上车后松开右键不会残留单键输入"),Input->bSingleMouseResolved);
    Player->Possess(Character);
    Input->MousePressed(false);
    TestFalse(TEXT("手柄右攻击沿用原规则"),Input->RightMouseGesture.IsActive());
    Input->ResolveSingleMousePress();
    TestTrue(TEXT("手柄按住超过组合时间后照常处理攻击"),Input->bSingleMouseResolved);
    Input->MouseReleased(false);

    // 上下车的输入交接。真实流程里引擎会在 UnPossessed 时销毁这具身体的
    // InputComponent，重新占有时再新建一个——而新建的那个有可能复用同一个
    // 对象槽位。Bind() 开头"已经绑过了"的早退因此会误判成不用再绑，
    // 结果整具身体一个键都不响应，且从外面看绑定关系是"对"的。
    // 这里直接钉住那个不变量：失去控制权之后，绑定记录必须是空的。
    UEnhancedInputComponent* FirstStack = NewObject<UEnhancedInputComponent>(Character);
    Input->Bind(FirstStack);
    TestEqual(TEXT("Bound to the component just handed over"),
              Input->GetBoundInputForDebug(), static_cast<const UInputComponent*>(FirstStack));
    Player->Possess(Bike);
    TestNull(TEXT("Losing control clears the binding record"),
             Input->GetBoundInputForDebug());

    // 重新占有必须真的绑上，而不是被早退吃掉
    Player->Possess(Character);
    UEnhancedInputComponent* SecondStack = NewObject<UEnhancedInputComponent>(Character);
    Input->Bind(SecondStack);
    TestTrue(TEXT("Re-possession binds the new component"),
             SecondStack->GetActionEventBindings().Num() > 0);
    TestEqual(TEXT("Binding record follows the new component"),
              Input->GetBoundInputForDebug(), static_cast<const UInputComponent*>(SecondStack));

    // 刚接手的身体不能带着上一任留下的移动意图：实测日志里 ResetInput 和
    // Move=(0,1) 是同一个时间戳，清完又被当帧的 Triggered 写了回去，
    // 于是下车后人一直往前走
    if (UDeliveryActiveRagdollComponent* Body = Character->GetActiveRagdoll())
    {
        Character->DoMove(0.f, 1.f);
        TestFalse(TEXT("Move intent is set before the handover"),
                  Body->GetMoveInputForDebug().IsNearlyZero());
        UEnhancedInputComponent* ThirdStack = NewObject<UEnhancedInputComponent>(Character);
        Input->Bind(ThirdStack);
        TestTrue(TEXT("A freshly bound body starts with no move intent"),
                 Body->GetMoveInputForDebug().IsNearlyZero());
    }

    // HUD 在步行和驾驶时必须找到同一个角色，避免切换到车辆背包
    TestEqual(TEXT("Walking HUD source"), ADeliveryHUD::ResolveCharacter(Character), Character);
    const FObjectPropertyBase* Driver = FindFProperty<FObjectPropertyBase>(ADeliveryMotorbike::StaticClass(), TEXT("Driver"));
    if (!TestNotNull(TEXT("Driver property"), Driver)) return false;
    Driver->SetObjectPropertyValue_InContainer(Bike, Character);
    TestEqual(TEXT("Riding HUD source"), ADeliveryHUD::ResolveCharacter(Bike), Character);
    Driver->SetObjectPropertyValue_InContainer(Bike, nullptr);
    TestNull(TEXT("No stale HUD source on empty bike"), ADeliveryHUD::ResolveCharacter(Bike));

    UClass* GameModeClass = LoadClass<ADeliveryGameMode>(nullptr,
        TEXT("/Game/Blueprint/GameMode/BP_DeliverGameMode.BP_DeliverGameMode_C"));
    // 同时检查正式蓝图，防止蓝图中保留旧 HUD 配置
    TestTrue(TEXT("Native game mode uses Delivery HUD"), GetDefault<ADeliveryGameMode>()->HUDClass->IsChildOf(ADeliveryHUD::StaticClass()));
    TestNotNull(TEXT("Production game mode loads"), GameModeClass);
    if (GameModeClass)
        TestTrue(TEXT("Blueprint game mode uses Delivery HUD"), GameModeClass->GetDefaultObject<ADeliveryGameMode>()->HUDClass->IsChildOf(ADeliveryHUD::StaticClass()));
    return true;
}
#endif
