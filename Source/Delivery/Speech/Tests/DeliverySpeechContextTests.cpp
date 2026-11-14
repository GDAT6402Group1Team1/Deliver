#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Speech/DeliverySpeechInputComponent.h"
#include "Character/DeliveryCharacter.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySpeechVehicleContextTest, "Delivery.Speech.Input.VehicleContinuity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySpeechVehicleContextTest::RunTest(const FString&)
{
    using EContext = EDeliverySpeechPlayerContextState;
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    if (!TestNotNull(TEXT("建立无音频测试世界"), World)) return false;
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* PC = World->SpawnActor<APlayerController>(Spawn);
    auto* Character = World->SpawnActor<ADeliveryCharacter>(Spawn);
    auto* Bike = World->SpawnActor<ADeliveryMotorbike>(Spawn);
    auto* PlayerState = World->SpawnActor<APlayerState>(Spawn);
    if (!PC || !Character || !Bike || !PlayerState) return false;
    PC->PlayerState = PlayerState;
    PC->Possess(Character);
    auto* Speech = NewObject<UDeliverySpeechInputComponent>(PC);
    PC->AddInstanceComponent(Speech);
    if (!TestTrue(TEXT("步行时记录稳定玩家身份"), Speech->SessionContext.Capture(PC))) return false;
    Speech->bVoiceEnabled = Speech->bContinuousMode = true;
    const uint64 Generation = Speech->TurnState.BeginPreparation();
    Speech->TurnState.MarkReady(Generation);
    FDeliverySpeechTurnToken Token;
    Speech->TurnState.BeginCapture(100, Token);
    const double CaptureDeadline = Speech->TurnState.GetDeadline();
    Speech->ActiveNativeTurnId = 23;

    // 只装配车辆复制字段，不调用车辆玩法或打开实际麦克风
    const auto* DriverProperty = FindFProperty<FObjectPropertyBase>(ADeliveryMotorbike::StaticClass(), TEXT("Driver"));
    if (!TestNotNull(TEXT("车辆驾驶员复制字段"), DriverProperty)) return false;
    DriverProperty->SetObjectPropertyValue_InContainer(Bike, Character);
    if (!TestTrue(TEXT("原人物附着到车辆"), Character->AttachToActor(Bike, FAttachmentTransformRules::KeepWorldTransform))) return false;
    PC->Possess(Bike);
    TestTrue(TEXT("原人物驾驶车辆继续同一会话"), Speech->CheckSessionContext(100.1) == EContext::Valid);
    TestTrue(TEXT("上车不替换当前录音令牌"), Speech->TurnState.IsCurrent(Token));
    TestEqual(TEXT("上车不重建语音代次"), Speech->TurnState.GetGeneration(), Generation);
    TestEqual(TEXT("上车不更换识别句子编号"), Speech->ActiveNativeTurnId, uint64(23));

    DriverProperty->SetObjectPropertyValue_InContainer(Bike, nullptr);
    TestTrue(TEXT("驾驶员复制暂缺只等待"), Speech->CheckSessionContext(101) == EContext::Pending);
    TestTrue(TEXT("重复检查不会提前结束等待"), Speech->CheckSessionContext(102.5) == EContext::Pending);
    TestEqual(TEXT("等待不延长录音期限"), Speech->TurnState.GetDeadline(), CaptureDeadline);
    DriverProperty->SetObjectPropertyValue_InContainer(Bike, Character);
    TestTrue(TEXT("驾驶员复制到齐恢复"), Speech->CheckSessionContext(102.6) == EContext::Valid);
    TestTrue(TEXT("等待期间当前句子保持有效"), Speech->TurnState.IsCurrent(Token));
    Speech->TurnState.FinishCapture(Token, 102.7);
    const double FinalDeadline = Speech->TurnState.GetDeadline();

    PC->UnPossess();
    TestTrue(TEXT("下车控制对象空窗等待"), Speech->CheckSessionContext(103) == EContext::Pending);
    TestEqual(TEXT("空窗不会延长已有 final 期限"), Speech->TurnState.GetDeadline(), FinalDeadline);
    Character->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    DriverProperty->SetObjectPropertyValue_InContainer(Bike, nullptr);
    PC->Possess(Character);
    TestTrue(TEXT("下车恢复原人物可继续收尾"), Speech->CheckSessionContext(103.2) == EContext::Valid);
    FString FinalText;
    TestTrue(TEXT("原句在合法关系恢复后可接收"), Speech->TurnState.TryAcceptFinal(Token, TEXT("上车也继续说"), 103.3, FinalText));
    TestFalse(TEXT("同一 final 不会重复接收"), Speech->TurnState.TryAcceptFinal(Token, TEXT("上车也继续说"), 103.4, FinalText));
    TestEqual(TEXT("上下车全程会话代次保持"), Speech->TurnState.GetGeneration(), Generation);

    Speech->TurnState.BeginCapture(104, Token);
    PC->UnPossess();
    TestTrue(TEXT("下一次空窗有自己的首次时间"), Speech->CheckSessionContext(104) == EContext::Pending);
    TestTrue(TEXT("等待上限之前仍保留会话"), Speech->CheckSessionContext(105.99) == EContext::Pending);
    PC->Possess(Character);
    TestTrue(TEXT("长帧后控制字段已恢复完整"), Speech->SessionContext.Check(PC) == EContext::Valid);
    TestTrue(TEXT("恢复有效控制也不能绕过已开始的两秒上限"), Speech->CheckSessionContext(106) == EContext::Invalid);
    Speech->CloseVoice(TEXT("控制人物已切换，语音已关闭"));
    TestFalse(TEXT("无效控制关系关闭并清理身份"), Speech->SessionContext.IsCaptured());
    TestTrue(TEXT("无效控制关系作废旧句"), Speech->GetState() == EDeliverySpeechInputState::Off && !Speech->TurnState.IsCurrent(Token));
    return true;
}
#endif
