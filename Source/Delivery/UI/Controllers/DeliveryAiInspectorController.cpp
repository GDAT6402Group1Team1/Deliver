#include "UI/Controllers/DeliveryAiInspectorController.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "GameFramework/PlayerController.h"
#include "UI/Conversation/DeliveryAiInspectorWidget.h"
#include "UI/HUD/DeliveryHUD.h"

// ── 生命周期 ──

/// @brief 绑定本机玩家、HUD 和面板，订阅交谈组件的决策记录和发言通知，订阅面板的展开、历史和回到最新事件
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

/// @brief 玩家所在的世界
UWorld* UDeliveryAiInspectorController::GetWorld() const
{
    return Player.IsValid()?Player->GetWorld():nullptr;
}

/// @brief 取消所有订阅，交谈组件停止转发记录，登记面板关闭，清空记录
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

/// @brief 清空记录和面板
void UDeliveryAiInspectorController::ResetView()
{
    LatestTrace={}; History.Reset(); bShowingHistory=false;
    if (auto* View=Widget.Get()) { View->ClearTrace(); View->SetHistory(History); }
}

/// @brief 换成看 Npc：清空面板和记录，让交谈组件开始转发这位 NPC 的决策记录；已经在看它时直接返回
void UDeliveryAiInspectorController::ObserveNpc(ADeliveryNpcCharacter* Npc)
{
    auto* PC=Player.Get(); auto* Source=Conversation.Get();
    if (!bInitialized || !PC || !Source || !IsValid(Npc) || Npc->GetWorld()!=GetWorld()) return;
    if (ObservedNpc.Get()==Npc && ObservedPawn.Get()==PC->GetPawn()) return;
    ObservedNpc=Npc; ObservedPawn=PC->GetPawn(); ResetView();
    Source->WatchNpcDiagnostics(Npc);
}

/// @brief HUD 每 0.1 秒调用：玩家换了角色或看的 NPC 没了时清空面板，交谈组件停止转发
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

/// @brief 玩家对某位 NPC 说了一句话时调用 ObserveNpc 看那位 NPC
void UDeliveryAiInspectorController::HandleSubmitted(ADeliveryNpcCharacter* Npc,FGuid MessageId,const FString& Text)
{
    ObserveNpc(Npc);
}

/// @brief 收到一条决策记录：更新或插到 History 最前面（最多 8 条），没在看历史时显示它
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

/// @brief 玩家点了第 Index 条历史，面板显示那一条
void UDeliveryAiInspectorController::ShowHistory(int32 Index)
{
    if (!History.IsValidIndex(Index)) return;
    bShowingHistory=true;
    if (auto* View=Widget.Get()) View->SetTrace(History[Index],true);
}

/// @brief 玩家点了回到最新，面板显示 LatestTrace
void UDeliveryAiInspectorController::ShowLatest()
{
    bShowingHistory=false;
    if (auto* View=Widget.Get()) View->SetTrace(LatestTrace,false);
}

/// @brief F6：让面板展开或收起
void UDeliveryAiInspectorController::ToggleExpanded()
{
    if (auto* View=Widget.Get()) View->ToggleExpanded();
}

/// @brief 面板展开或收起时登记面板开关，展开时显示鼠标
void UDeliveryAiInspectorController::HandleExpanded(bool bExpanded)
{
    if (auto* OwnerHUD=HUD.Get()) OwnerHUD->SetPanelOpen(TEXT("AiInspector"),bExpanded);
}
