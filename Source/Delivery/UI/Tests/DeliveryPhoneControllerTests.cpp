#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UI/DeliveryHUD.h"
#include "UI/Controllers/DeliveryPhoneController.h"
#include "Player/DeliveryPlayerController.h"
#include "Audio/DeliveryDialogueVoiceComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPhoneControllerTest, "Delivery.UI.PhoneController",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryPhoneControllerTest::RunTest(const FString&)
{
    const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
        ERHIFeatureLevel::Num, &Init);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* Player = World->SpawnActor<ADeliveryPlayerController>();
    // 独立测试世界没有登录流程，需要显式提供本机玩家
    Player->Player = NewObject<ULocalPlayer>(GEngine);
    FActorSpawnParameters Params;
    Params.Owner = Player;
    auto* HUD = World->SpawnActor<ADeliveryHUD>(Params);
    HUD->PlayerOwner = Player;
    auto* Phone = HUD->GetPhoneController();
    if (TestNotNull(TEXT("HUD 创建手机处理器"), Phone))
    {
        TestEqual(TEXT("重复访问复用处理器"), HUD->GetPhoneController(), Phone);
        Phone->SetOpen(true);
        HUD->SetPanelOpen(TEXT("Inventory"), true);
        Phone->SetOpen(false);
        TestFalse(TEXT("手机已经关闭"), Phone->IsOpen());
        TestTrue(TEXT("其他面板打开时仍保留鼠标"), Player->bShowMouseCursor);
        HUD->SetPanelOpen(TEXT("Inventory"), false);
        TestFalse(TEXT("最后一个面板关闭后隐藏鼠标"), Player->bShowMouseCursor);

        // 同一通电话重复通知不能清空已经显示的字幕
        FDeliveryPhoneCall Call;
        Call.CallId = 7;
        Phone->HandleCallChanged(EDeliveryPhoneCallState::InCall, Call);
        Player->GetDialogueVoice()->OnGlyphRevealed.Broadcast(TEXT("你好"), 0);
        TestEqual(TEXT("逐字通知生成字幕"), Phone->Subtitle, FString(TEXT("你好")));
        Phone->HandleCallChanged(EDeliveryPhoneCallState::InCall, Call);
        TestEqual(TEXT("重复通知保留字幕进度"), Phone->Subtitle, FString(TEXT("你好")));
        Phone->HandleCallChanged(EDeliveryPhoneCallState::Idle, Call);
        TestTrue(TEXT("挂断清除字幕"), Phone->Subtitle.IsEmpty());

        // 重复初始化和销毁都必须解除旧的语音订阅
        Phone->Initialize(Player, HUD);
        Phone->HandleCallChanged(EDeliveryPhoneCallState::InCall, Call);
        Player->GetDialogueVoice()->OnGlyphRevealed.Broadcast(TEXT("好"), 0);
        TestEqual(TEXT("重新初始化没有重复订阅"), Phone->Subtitle, FString(TEXT("好")));
        Phone->Shutdown();
        Player->GetDialogueVoice()->OnGlyphRevealed.Broadcast(TEXT("旧通知"), 1);
        TestTrue(TEXT("关闭处理器后不再接收字幕"), Phone->Subtitle.IsEmpty());
        TestNull(TEXT("关闭后释放玩家引用"), Phone->Player.Get());
    }
    World->EndPlay(EEndPlayReason::Quit);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
