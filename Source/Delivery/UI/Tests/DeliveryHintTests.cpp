#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Interaction/DeliveryPromptSubsystem.h"
#include "UI/DeliveryHintComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPersistentHintTest, "Delivery.UI.PersistentHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryPersistentHintTest::RunTest(const FString&)
{
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* Prompt = World->GetSubsystem<UDeliveryPromptSubsystem>();
    UObject* OldSource = NewObject<UDeliveryHintComponent>(World);
    UObject* NewSource = NewObject<UDeliveryHintComponent>(World);
    Prompt->SetCornerHint(OldSource, FText::FromString(TEXT("召唤")));
    World->Tick(LEVELTICK_All, 1.0f);
    TestTrue(TEXT("不持续推送也保留提示"), Prompt->HasCornerHint());
    Prompt->SetCornerHint(NewSource, FText::FromString(TEXT("切换视角")));
    Prompt->ClearCornerHint(OldSource);
    TestEqual(TEXT("旧来源不能删除新提示"), Prompt->CornerText.ToString(), FString(TEXT("切换视角")));
    Prompt->ClearCornerHint(NewSource);
    TestFalse(TEXT("当前来源可以主动隐藏提示"), Prompt->HasCornerHint());
    Prompt->SetCornerHint(NewSource, FText::FromString(TEXT("冷却")), true);
    NewSource->MarkAsGarbage();
    TestFalse(TEXT("来源失效后不留下提示"), Prompt->HasCornerHint());
    World->EndPlay(EEndPlayReason::Quit);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
