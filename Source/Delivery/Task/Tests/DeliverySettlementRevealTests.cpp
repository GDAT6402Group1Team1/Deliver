#if WITH_DEV_AUTOMATION_TESTS

#include "DeliverySettlementTestWidget.h"
#include "Task/DeliverySettlementComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliverySettlementRevealTest, "Delivery.UI.SettlementReveal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliverySettlementRevealTest::RunTest(const FString&)
{
	const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false)
		.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
		ERHIFeatureLevel::Num, &Init);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Player = World->SpawnActor<APlayerController>();
	Player->Player = NewObject<ULocalPlayer>(GEngine);
	auto* Widget = CreateWidget<UDeliverySettlementTestWidget>(Player);
	if (!TestNotNull(TEXT("创建结算测试控件"), Widget))
	{
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		return false;
	}
	if (!Widget->WidgetTree)
	{
		Widget->WidgetTree = NewObject<UWidgetTree>(Widget);
	}
	auto* Root = Widget->WidgetTree->ConstructWidget<UVerticalBox>();
	Widget->WidgetTree->RootWidget = Root;
	auto AddText = [&](const TCHAR* Name)
	{
		auto* Text = Widget->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(Name));
		Root->AddChild(Text);
		return Text;
	};
	Widget->TaskNameText = AddText(TEXT("TaskNameText"));
	Widget->TimeGradeText = AddText(TEXT("TimeGradeText"));
	Widget->ElapsedText = AddText(TEXT("ElapsedText"));
	Widget->BaseRewardText = AddText(TEXT("BaseRewardText"));
	Widget->TimeMultiplierText = AddText(TEXT("TimeMultiplierText"));
	Widget->SpecialMultiplierText = AddText(TEXT("SpecialMultiplierText"));
	Widget->FinalRewardText = AddText(TEXT("FinalRewardText"));
	Widget->TimeGradeText->SetRenderOpacity(0.8f);
	TestNotNull(TEXT("普通行默认提示音存在"), Widget->RowRevealSound.Get());
	TestNotNull(TEXT("总收入默认提示音存在"), Widget->FinalRevealSound.Get());
	if (Widget->RowRevealSound)
	{
		TestEqual(TEXT("默认使用合成音效"), Widget->RowRevealSound->GetName(), FString(TEXT("SFX_Settlement_Tone")));
		TestFalse(TEXT("合成提示音不循环"), Widget->RowRevealSound->IsLooping());
	}
	TestTrue(TEXT("总收入使用独立和弦音效"), Widget->FinalRevealSound != Widget->RowRevealSound);
	if (Widget->FinalRevealSound)
	{
		TestEqual(TEXT("总收入默认和弦资源"), Widget->FinalRevealSound->GetName(), FString(TEXT("SFX_Settlement_Finale")));
		TestFalse(TEXT("收尾和弦不循环"), Widget->FinalRevealSound->IsLooping());
		TestTrue(TEXT("总收入音效留足和声延音"), Widget->FinalRevealSound->GetDuration() > 1.5f);
	}
	const float SemitoneRatio = 1.059463094f;
	for (int32 Row = 1; Row < 6; ++Row)
	{
		const float Previous = Widget->GetRevealPitch(false, Row - 1);
		const float Current = Widget->GetRevealPitch(Row == 5, Row);
		TestTrue(TEXT("含总收入在内的相邻行严格相差半音"),
			FMath::IsNearlyEqual(Current / Previous, SemitoneRatio, 0.00001f));
	}
	TestEqual(TEXT("十二个半音为一个八度"), Widget->GetRevealPitch(false, 12), 2.f);
	TestEqual(TEXT("只有总额时从原音开始"), Widget->GetRevealPitch(true, 0), 1.f);
	Widget->RowPitchStep = 0.f;
	TestEqual(TEXT("可关闭逐行升调"), Widget->GetRevealPitch(false, 4), 1.f);
	Widget->RowPitchStep = 2.f;
	TestTrue(TEXT("可改为每行一个全音"), FMath::IsNearlyEqual(Widget->GetRevealPitch(false, 1), 1.122462048f));
	Widget->RowPitchStep = 1.f;
	Widget->FinalRevealPitch = 1.1f;
	TestTrue(TEXT("总收入额外倍率叠在逐行音高上"),
		FMath::IsNearlyEqual(Widget->GetRevealPitch(true, 5) / Widget->GetRevealPitch(false, 5), 1.1f));
	Widget->FinalRevealPitch = 1.f;
	Widget->RowRevealSound = nullptr;
	Widget->FinalRevealSound = nullptr;

	auto Advance = [&](float Seconds)
	{
		// 在独立世界推进真实 TimerManager，不依赖测试执行机器的实际帧率
		++GFrameCounter;
		World->GetTimerManager().Tick(Seconds);
	};
	auto* Component = NewObject<UDeliverySettlementComponent>(Player);
	Component->AutoHideSeconds = 0.5f;
	Component->ActiveWidget = Widget;
	Widget->OnRevealFinished.AddUObject(Component, &UDeliverySettlementComponent::HandleRevealFinished);
	int32 FinishedCount = 0;
	Widget->OnRevealFinished.AddLambda([&](UDeliverySettlementWidget*) { ++FinishedCount; });
	FDeliveryRewardBreakdown Reward;
	Reward.BaseReward = 120;
	Reward.FinalReward = 165;
	Reward.ElapsedSeconds = 155;
	Reward.TimeGradeName = FText::FromString(TEXT("On Time"));

	Widget->ApplyReward(nullptr, Reward);
	Advance(0.f);
	TestEqual(TEXT("标题立即显示"), Widget->TaskNameText->GetRenderOpacity(), 1.f);
	TestEqual(TEXT("第一行先隐藏"), Widget->TimeGradeText->GetRenderOpacity(), 0.f);
	TestEqual(TEXT("总额先隐藏"), Widget->FinalRewardText->GetRenderOpacity(), 0.f);
	TestFalse(TEXT("明细未完成时不倒计时关闭"), World->GetTimerManager().TimerExists(Component->AutoHideTimer));
	Advance(0.1f);
	TestEqual(TEXT("首行等待期间保持隐藏"), Widget->TimeGradeText->GetRenderOpacity(), 0.f);
	Advance(0.11f);
	TestEqual(TEXT("第一行恢复设计透明度"), Widget->TimeGradeText->GetRenderOpacity(), 0.8f);
	TestEqual(TEXT("第二行还没出现"), Widget->ElapsedText->GetRenderOpacity(), 0.f);
	for (UTextBlock* Text : {Widget->ElapsedText.Get(), Widget->BaseRewardText.Get(),
		Widget->TimeMultiplierText.Get(), Widget->SpecialMultiplierText.Get()})
	{
		Advance(0.36f);
		TestEqual(TEXT("下一行按序显示"), Text->GetRenderOpacity(), 1.f);
	}
	Advance(0.3f);
	TestEqual(TEXT("总额前保留额外停顿"), Widget->FinalRewardText->GetRenderOpacity(), 0.f);
	Advance(0.31f);
	TestEqual(TEXT("最后显示总额"), Widget->FinalRewardText->GetRenderOpacity(), 1.f);
	TestEqual(TEXT("只广播一次完成"), FinishedCount, 1);
	TestTrue(TEXT("全部显示后才启动关闭计时器"), World->GetTimerManager().TimerExists(Component->AutoHideTimer));
	Advance(0.49f);
	TestTrue(TEXT("完成后仍保留阅读时间"), Component->IsSettlementOpen());
	Advance(0.02f);
	TestFalse(TEXT("阅读时间结束后关闭"), Component->IsSettlementOpen());

	// 新订单替换进行中的旧明细时，应从首行重来，并丢弃旧计时器
	Widget->ApplyReward(nullptr, Reward);
	Advance(0.21f);
	Reward.FinalReward = 79;
	Widget->ApplyReward(nullptr, Reward);
	TestEqual(TEXT("替换订单重新隐藏明细"), Widget->TimeGradeText->GetRenderOpacity(), 0.f);
	TestTrue(TEXT("替换订单更新总金额"), Widget->FinalRewardText->GetText().ToString().Contains(TEXT("79")));
	Widget->CancelReveal();
	Widget->CancelReveal();
	Advance(10.f);
	TestEqual(TEXT("取消后不再广播完成"), FinishedCount, 1);
	TestFalse(TEXT("取消清除逐行计时器"), World->GetTimerManager().TimerExists(Widget->RevealTimer));
	TestNull(TEXT("取消不保留音频实例"), Widget->RevealAudio.Get());

	// 只摆必需控件时没有空行等待，关闭动画时立即显示并通知组件
	Widget->TimeGradeText = nullptr;
	Widget->ElapsedText = nullptr;
	Widget->BaseRewardText = nullptr;
	Widget->TimeMultiplierText = nullptr;
	Widget->SpecialMultiplierText = nullptr;
	Widget->ApplyReward(nullptr, Reward);
	Advance(0.21f);
	TestEqual(TEXT("无可选行时直接显示总额"), Widget->FinalRewardText->GetRenderOpacity(), 1.f);
	TestEqual(TEXT("无可选行仍正常完成"), FinishedCount, 2);
	Widget->bRevealRows = false;
	Widget->ApplyReward(nullptr, Reward);
	TestEqual(TEXT("关闭逐行时立即完成"), FinishedCount, 3);
	TestFalse(TEXT("关闭逐行时不启动计时器"), World->GetTimerManager().TimerExists(Widget->RevealTimer));
	Widget->OnRevealFinished.Clear();
	Widget->NativeDestruct();
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}

#endif
