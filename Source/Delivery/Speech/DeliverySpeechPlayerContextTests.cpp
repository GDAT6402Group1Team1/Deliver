#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Speech/DeliverySpeechPlayerContext.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/UnrealType.h"

namespace
{
struct FSpeechPlayerContextWorld
{
    UWorld* World = nullptr;

    FSpeechPlayerContextWorld()
    {
        const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
            ERHIFeatureLevel::Num, &Init);
        if (World) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    }

    ~FSpeechPlayerContextWorld()
    {
        if (!World) return;
        World->EndPlay(EEndPlayReason::Quit);
        GEngine->DestroyWorldContext(World);
        World->DestroyWorld(false);
    }

    template<typename T> T* Spawn()
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return World->SpawnActor<T>(Params);
    }

    APlayerController* MakePlayer()
    {
        auto* Player = Spawn<APlayerController>();
        if (Player) Player->PlayerState = Spawn<APlayerState>();
        return Player;
    }
};

// 只构造服务器或复制后的身份链，驾驶权仍走真实 Possess，不启动身体物理或麦克风
bool SetTestDriver(ADeliveryMotorbike* Bike, ADeliveryCharacter* Character)
{
    const auto* Property = FindFProperty<FObjectPropertyBase>(ADeliveryMotorbike::StaticClass(), TEXT("Driver"));
    if (!Property) return false;
    Property->SetObjectPropertyValue_InContainer(Bike, Character);
    return true;
}

bool AttachTestDriver(ADeliveryCharacter* Character, ADeliveryMotorbike* Bike)
{
    return Character->AttachToComponent(Bike->GetRootComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechPlayerContextTransitionsTest, "Delivery.Speech.PlayerContext.Transitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechPlayerContextTransitionsTest::RunTest(const FString&)
{
    using EState = EDeliverySpeechPlayerContextState;
    FSpeechPlayerContextWorld Fixture;
    if (!TestNotNull(TEXT("身份切换测试世界"), Fixture.World)) return false;
    auto* Player = Fixture.MakePlayer();
    auto* Character = Fixture.Spawn<ADeliveryCharacter>();
    auto* Bike = Fixture.Spawn<ADeliveryMotorbike>();
    if (!TestTrue(TEXT("玩家、人物和车辆创建成功"), Player && Player->PlayerState && Character && Bike)) return false;
    Player->Possess(Character);
    FDeliverySpeechPlayerContext Context;
    if (!TestTrue(TEXT("步行时捕获完整身份"), Context.Capture(Player))) return false;
    TestEqual(TEXT("会话保存原人物"), Context.GetCharacter(), Character);
    TestEqual(TEXT("步行身份有效"), Context.Check(Player), EState::Valid);

    Player->UnPossess();
    TestEqual(TEXT("控制对象暂空时保留会话等待复制"), Context.Check(Player), EState::Pending);
    FDeliverySpeechPlayerContext Incomplete;
    TestFalse(TEXT("控制对象未到齐时不能新开会话"), Incomplete.Capture(Player));
    if (!TestTrue(TEXT("构造车辆驾驶员字段"), SetTestDriver(Bike, Character)) ||
        !TestTrue(TEXT("原人物附着到车辆"), AttachTestDriver(Character, Bike))) return false;
    Player->Possess(Bike);
    TestNull(TEXT("真实换乘会清掉原人物的控制器"), Character->GetController());
    TestNull(TEXT("真实换乘会清掉原人物的玩家状态"), Character->GetPlayerState());
    TestEqual(TEXT("换成车辆后仍是同一连麦身份"), Context.Check(Player), EState::Valid);

    SetTestDriver(Bike, nullptr);
    TestEqual(TEXT("驾驶员字段暂未复制时等待"), Context.Check(Player), EState::Pending);
    TestFalse(TEXT("驾驶员字段未到齐时不能新开会话"), Incomplete.Capture(Player));
    SetTestDriver(Bike, Character);
    Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    TestEqual(TEXT("附着关系暂未复制时等待"), Context.Check(Player), EState::Pending);
    TestFalse(TEXT("附着关系未到齐时不能新开会话"), Incomplete.Capture(Player));
    if (!TestTrue(TEXT("附着关系恢复"), AttachTestDriver(Character, Bike))) return false;
    TestEqual(TEXT("复制补齐后原会话恢复有效"), Context.Check(Player), EState::Valid);
    FDeliverySpeechPlayerContext MountedContext;
    if (!TestTrue(TEXT("首次在车上开启也能捕获身份"), MountedContext.Capture(Player))) return false;
    TestEqual(TEXT("车上首次开启保存人物而非车辆"), MountedContext.GetCharacter(), Character);

    SetTestDriver(Bike, nullptr);
    Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    Player->Possess(Character);
    TestEqual(TEXT("下车后步行开启的会话仍有效"), Context.Check(Player), EState::Valid);
    TestEqual(TEXT("下车后车上开启的会话仍有效"), MountedContext.Check(Player), EState::Valid);
    Context.Reset();
    TestFalse(TEXT("关闭后不再保存身份"), Context.IsCaptured());
    TestNull(TEXT("关闭后不留下原人物"), Context.GetCharacter());
    TestEqual(TEXT("关闭后的旧身份不能继续使用"), Context.Check(Player), EState::Invalid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechPlayerContextBoundariesTest, "Delivery.Speech.PlayerContext.IdentityBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechPlayerContextBoundariesTest::RunTest(const FString&)
{
    using EState = EDeliverySpeechPlayerContextState;
    FSpeechPlayerContextWorld Fixture;
    if (!TestNotNull(TEXT("身份边界测试世界"), Fixture.World)) return false;
    auto* Player = Fixture.MakePlayer();
    auto* OtherPlayer = Fixture.MakePlayer();
    auto* Character = Fixture.Spawn<ADeliveryCharacter>();
    auto* OtherCharacter = Fixture.Spawn<ADeliveryCharacter>();
    auto* Bike = Fixture.Spawn<ADeliveryMotorbike>();
    auto* OtherBike = Fixture.Spawn<ADeliveryMotorbike>();
    if (!TestTrue(TEXT("边界测试对象创建成功"), Player && Player->PlayerState && OtherPlayer &&
        OtherPlayer->PlayerState && Character && OtherCharacter && Bike && OtherBike)) return false;
    Player->Possess(Character);
    FDeliverySpeechPlayerContext Context;
    if (!TestTrue(TEXT("捕获原玩家身份"), Context.Capture(Player))) return false;
    TestEqual(TEXT("其他控制器不能沿用会话"), Context.Check(OtherPlayer), EState::Invalid);
    APlayerState* OriginalPlayerState = Player->PlayerState;
    Player->PlayerState = OtherPlayer->PlayerState;
    TestEqual(TEXT("同一控制器换玩家状态也使旧会话失效"), Context.Check(Player), EState::Invalid);
    Player->PlayerState = OriginalPlayerState;
    Player->Possess(OtherCharacter);
    TestEqual(TEXT("明确换成另一人物不能作为短暂空窗等待"), Context.Check(Player), EState::Invalid);
    Player->Possess(Character);
    if (!TestTrue(TEXT("设置原驾驶员"), SetTestDriver(Bike, Character)) ||
        !TestTrue(TEXT("建立驾驶员附着关系"), AttachTestDriver(Character, Bike))) return false;
    Player->Possess(Bike);
    OtherPlayer->Possess(Character);
    TestEqual(TEXT("原人物被别人控制时拒绝仍保留的车辆字段"), Context.Check(Player), EState::Invalid);
    OtherPlayer->UnPossess();
    SetTestDriver(Bike, OtherCharacter);
    TestEqual(TEXT("车辆记录另一驾驶员时拒绝旧会话"), Context.Check(Player), EState::Invalid);
    SetTestDriver(Bike, Character);
    SetTestDriver(OtherBike, Character);
    if (!TestTrue(TEXT("构造另一辆车的完整附着关系"), AttachTestDriver(Character, OtherBike))) return false;
    TestEqual(TEXT("人物实际附着另一辆车时拒绝控制关系冲突"), Context.Check(Player), EState::Invalid);
    SetTestDriver(OtherBike, nullptr);
    if (!TestTrue(TEXT("恢复原车辆附着关系"), AttachTestDriver(Character, Bike))) return false;
    TestEqual(TEXT("纯查询不改写原身份"), Context.Check(Player), EState::Valid);

    FSpeechPlayerContextWorld OtherWorld;
    if (!TestNotNull(TEXT("独立的第二个世界"), OtherWorld.World)) return false;
    auto* ForeignPlayer = OtherWorld.MakePlayer();
    if (!TestNotNull(TEXT("另一个世界的玩家"), ForeignPlayer)) return false;
    TestEqual(TEXT("其他世界不能使用旧会话"), Context.Check(ForeignPlayer), EState::Invalid);
    SetTestDriver(Bike, nullptr);
    Character->Destroy();
    TestEqual(TEXT("原人物销毁后不能继续等待或更换人物"), Context.Check(Player), EState::Invalid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechPlayerContextLifetimeTest, "Delivery.Speech.PlayerContext.Lifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechPlayerContextLifetimeTest::RunTest(const FString&)
{
    using EState = EDeliverySpeechPlayerContextState;
    FSpeechPlayerContextWorld Fixture;
    if (!TestNotNull(TEXT("生命周期测试世界"), Fixture.World)) return false;
    auto* Player = Fixture.MakePlayer();
    auto* Pawn = Fixture.Spawn<APawn>();
    auto* OtherPawn = Fixture.Spawn<APawn>();
    if (!TestTrue(TEXT("普通 Pawn 测试对象创建成功"), Player && Player->PlayerState && Pawn && OtherPawn)) return false;
    FDeliverySpeechPlayerContext Context;
    TestFalse(TEXT("空玩家不能捕获身份"), Context.Capture(nullptr));
    Player->Possess(Pawn);
    if (!TestTrue(TEXT("普通 Pawn 可建立只绑定自身的会话"), Context.Capture(Player))) return false;
    TestNull(TEXT("普通 Pawn 不冒充步行人物"), Context.GetCharacter());
    TestEqual(TEXT("普通 Pawn 保持原对象时有效"), Context.Check(Player), EState::Valid);
    Player->Possess(OtherPawn);
    TestEqual(TEXT("普通 Pawn 不允许任意换乘"), Context.Check(Player), EState::Invalid);
    Player->Possess(Pawn);
    Fixture.World->EndPlay(EEndPlayReason::Quit);
    TestEqual(TEXT("世界结束后即使 UObject 尚存也不能继续连麦"), Context.Check(Player), EState::Invalid);
    TestFalse(TEXT("正在清理的世界不能新开会话"), Context.Capture(Player));
    return true;
}
#endif
