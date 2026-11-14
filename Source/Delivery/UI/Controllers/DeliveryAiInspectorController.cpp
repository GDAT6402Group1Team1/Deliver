#include "UI/Controllers/DeliveryAiInspectorController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "GameFramework/PlayerController.h"
#include "UI/DeliveryAiInspectorWidget.h"
#include "UI/DeliveryHUD.h"

void UDeliveryAiInspectorController::Initialize(APlayerController* InPlayer,ADeliveryHUD* InHUD,UDeliveryAiInspectorWidget* InWidget)
{
    Shutdown();
    if (!IsValid(InPlayer) || !InPlayer->IsLocalController() || !IsValid(InWidget) ||
        InWidget->GetOwningPlayer()!=InPlayer || InWidget->GetWorld()!=InPlayer->GetWorld()) return;
    auto* Source=InPlayer->FindComponentByClass<UDeliveryConversationComponent>();
    if (!Source) return;
    Player=InPlayer; HUD=InHUD; Widget=InWidget; Conversation=Source;
    ObservedPawn=InPlayer->GetPawn(); bInitialized=true;
    Source->OnDiagnosticsChanged.AddUObject(this,&UDeliveryAiInspectorController::HandleTrace);
    Source->OnSubmitted.AddUObject(this,&UDeliveryAiInspectorController::HandleSubmitted);
    InWidget->OnExpandedChanged.AddUObject(this,&UDeliveryAiInspectorController::HandleExpanded);
    InWidget->OnHistorySelected.AddUObject(this,&UDeliveryAiInspectorController::ShowHistory);
    InWidget->OnLatestRequested.AddUObject(this,&UDeliveryAiInspectorController::ShowLatest);
    ResetView();
}

UWorld* UDeliveryAiInspectorController::GetWorld() const
{
    return Player.IsValid()?Player->GetWorld():nullptr;
}

void UDeliveryAiInspectorController::Shutdown()
{
    if (auto* Source=Conversation.Get())
    {
        Source->OnDiagnosticsChanged.RemoveAll(this); Source->OnSubmitted.RemoveAll(this);
        Source->StopWatchingNpcDiagnostics();
    }
    if (auto* View=Widget.Get())
    {
        View->OnExpandedChanged.RemoveAll(this); View->OnHistorySelected.RemoveAll(this);
        View->OnLatestRequested.RemoveAll(this);
    }
    if (auto* OwnerHUD=HUD.Get()) OwnerHUD->SetPanelOpen(TEXT("AiInspector"),false);
    Player.Reset(); HUD.Reset(); Widget.Reset(); Conversation.Reset(); ObservedNpc.Reset(); ObservedPawn.Reset();
    LatestTrace={}; History.Reset(); bShowingHistory=false; bInitialized=false;
}

void UDeliveryAiInspectorController::ResetView()
{
    LatestTrace={}; History.Reset(); bShowingHistory=false;
    if (auto* View=Widget.Get()) { View->ClearTrace(); View->SetHistory(History); }
}

void UDeliveryAiInspectorController::ObserveNpc(ADeliveryNpcCharacter* Npc)
{
    auto* PC=Player.Get(); auto* Source=Conversation.Get();
    if (!bInitialized || !PC || !Source || !IsValid(Npc) || Npc->GetWorld()!=GetWorld()) return;
    if (ObservedNpc.Get()==Npc && ObservedPawn.Get()==PC->GetPawn()) return;
    ObservedNpc=Npc; ObservedPawn=PC->GetPawn(); ResetView();
    Source->WatchNpcDiagnostics(Npc);
}

void UDeliveryAiInspectorController::ValidateContext()
{
    const auto* PC=Player.Get();
    if (!bInitialized) return;
    if (!PC) { Shutdown(); return; }
    const bool bPawnChanged=ObservedPawn.IsStale() || ObservedPawn.Get()!=PC->GetPawn();
    const bool bTargetLost=ObservedNpc.IsStale() || (ObservedNpc.IsValid() && ObservedNpc->GetWorld()!=GetWorld());
    if (!bPawnChanged && !bTargetLost) return;
    ObservedNpc.Reset(); ObservedPawn=PC->GetPawn(); ResetView();
    if (auto* Source=Conversation.Get()) Source->StopWatchingNpcDiagnostics();
}

void UDeliveryAiInspectorController::HandleSubmitted(ADeliveryNpcCharacter* Npc,FGuid MessageId,const FString& Text)
{
    ObserveNpc(Npc);
}

void UDeliveryAiInspectorController::HandleTrace(const FDeliveryNpcDecisionTrace& Trace)
{
    if (!bInitialized) return;
    if (Trace.Stage==TEXT("Unavailable")) { ObservedNpc.Reset(); ResetView(); }
    LatestTrace=Trace;
    if (Trace.RequestId.IsValid())
    {
        const int32 Existing=History.IndexOfByPredicate([&](const auto& Item){ return Item.RequestId==Trace.RequestId && Item.WorldEpoch==Trace.WorldEpoch; });
        if (Existing==INDEX_NONE) History.Insert(Trace,0);
        else History[Existing]=Trace;
        // 历史只存在本机界面，切对象、换 Pawn、结束世界时全部释放
        if (History.Num()>8) History.SetNum(8);
    }
    if (auto* View=Widget.Get())
    {
        View->SetHistory(History);
        if (!bShowingHistory) View->SetTrace(LatestTrace,false);
    }
}

void UDeliveryAiInspectorController::ShowHistory(int32 Index)
{
    if (!History.IsValidIndex(Index)) return;
    bShowingHistory=true;
    if (auto* View=Widget.Get()) View->SetTrace(History[Index],true);
}

void UDeliveryAiInspectorController::ShowLatest()
{
    bShowingHistory=false;
    if (auto* View=Widget.Get()) View->SetTrace(LatestTrace,false);
}

void UDeliveryAiInspectorController::ToggleExpanded()
{
    if (auto* View=Widget.Get()) View->ToggleExpanded();
}

void UDeliveryAiInspectorController::HandleExpanded(bool bExpanded)
{
    if (auto* OwnerHUD=HUD.Get()) OwnerHUD->SetPanelOpen(TEXT("AiInspector"),bExpanded);
}
