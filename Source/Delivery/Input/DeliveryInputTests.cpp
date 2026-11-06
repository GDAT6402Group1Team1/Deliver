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
#include "UObject/UnrealType.h"
#include "Character/DeliveryCharacter.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Player/DeliveryPlayerController.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "UI/DeliveryHUD.h"
#include "Game/DeliveryGameMode.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryInputLifecycleTest, "Delivery.Systems.InputLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
