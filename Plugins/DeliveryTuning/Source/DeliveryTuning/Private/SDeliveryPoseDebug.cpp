#include "SDeliveryPoseDebug.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "Combat/DeliveryRagdollCombatComponent.h"
#include "Engine/Engine.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Components/LineBatchComponent.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"

SDeliveryPoseDebug::~SDeliveryPoseDebug()
{
    FEditorDelegates::EndPIE.Remove(EndPIEHandle);
    FWorldDelegates::OnWorldPostActorTick.Remove(WorldUpdateHandle);
    ClearDrawing();
}

void SDeliveryPoseDebug::ClearDrawing()
{
    // 使用自己的绘制组件，清理时不会擦掉其他诊断工具的标记
    if (IsValid(Lines.Get())) Lines->DestroyComponent();
    Lines.Reset();
    DrawingWorld.Reset();
}

void SDeliveryPoseDebug::StopDiagnostics()
{
    bEnabled = false;
    CrossTime = FootTime = ArmTime = SampleRemaining = 0.f;
    ClearDrawing();
    Status->SetText(FText::FromString(TEXT("诊断未开启")));
}

void SDeliveryPoseDebug::OnEndPIE(bool)
{
    // 页面被其他标签遮住时也清理，不能依赖下一次界面 Tick
    ClearDrawing();
    Selected.Reset();
    Targets.Reset();
    TargetBox->ClearSelection();
    TargetBox->RefreshOptions();
    Status->SetText(FText::FromString(TEXT("PIE 已结束，重新运行后刷新角色列表")));
}

void SDeliveryPoseDebug::Construct(const FArguments&)
{
    EndPIEHandle = FEditorDelegates::EndPIE.AddSP(this, &SDeliveryPoseDebug::OnEndPIE);
    // 采样跟随游戏世界，不依赖面板是否可见或获得焦点
    WorldUpdateHandle = FWorldDelegates::OnWorldPostActorTick.AddSP(this, &SDeliveryPoseDebug::OnWorldUpdated);
    ChildSlot
    [ SNew(SVerticalBox)
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("先运行 PIE，再刷新角色列表。多人时按世界编号和网络身份选择。绿球：实际位置；青球：目标；黄线：偏差；红色：持续异常。"))) ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SNew(SButton).Text(FText::FromString(TEXT("刷新运行中的角色"))).OnClicked(this, &SDeliveryPoseDebug::RefreshTargets) ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SAssignNew(TargetBox, SComboBox<FTargetPtr>).OptionsSource(&Targets)
        .OnGenerateWidget_Lambda([](FTargetPtr Item) { return SNew(STextBlock).Text(FText::FromString(Item->Label)); })
        .OnSelectionChanged_Lambda([this](FTargetPtr Item, ESelectInfo::Type) {
            ClearDrawing(); Selected = Item; CrossTime = FootTime = ArmTime = SampleRemaining = 0.f;
        })
        [ SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Selected ? Selected->Label : TEXT("尚未选择角色")); }) ] ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SNew(SCheckBox).IsChecked_Lambda([this] { return bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bEnabled = State == ECheckBoxState::Checked; if (!bEnabled) ClearDrawing(); CrossTime = FootTime = ArmTime = 0.f; })
        [ SNew(STextBlock).Text(FText::FromString(TEXT("开启实时诊断"))) ] ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowWalking = State == ECheckBoxState::Checked; })
        [ SNew(STextBlock).Text(FText::FromString(TEXT("走路：双脚、髋部目标、身体左右方向"))) ] ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowCombat = State == ECheckBoxState::Checked; })
        [ SNew(STextBlock).Text(FText::FromString(TEXT("战斗：手臂、拳头、服务器命中扫掠"))) ] ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SAssignNew(Status, STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("诊断未开启"))) ]
      + SVerticalBox::Slot().AutoHeight().Padding(8)
      [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("红色表示需要检查，不代表已确定故障。攻击被墙挡住也会产生姿态偏差。远端只播放快照，不用本机控制目标判错。关闭诊断或页面后标记自动消失；暂停 PIE 可观察当前姿态。"))) ]
    ];
}

FReply SDeliveryPoseDebug::RefreshTargets()
{
    ClearDrawing();
    const TWeakObjectPtr<UDeliveryActiveRagdollComponent> Previous = Selected ? Selected->Component : nullptr;
    Selected.Reset();
    Targets.Reset();
    // 逐个 PIE 世界收集，不把服务器与客户端的同名角色合并
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        UWorld* World = Context.World();
        if (!World || World->WorldType != EWorldType::PIE || World->bIsTearingDown) continue;
        for (TActorIterator<APawn> It(World); It; ++It)
        {
            auto* Component = It->FindComponentByClass<UDeliveryActiveRagdollComponent>();
            if (!Component) continue;
            FTargetPtr Item = MakeShared<FTarget>();
            Item->Component = Component;
            const TCHAR* Role = World->GetNetMode() == NM_DedicatedServer ? TEXT("专用服务器（仅数据）") : It->HasAuthority() ? TEXT("服务器") : It->IsLocallyControlled() ? TEXT("本机") : TEXT("远端");
            Item->Label = FString::Printf(TEXT("PIE %d | %s | %s"), Context.PIEInstance, Role, *It->GetActorLabel());
            Targets.Add(Item);
            if (Previous.Get() == Component) Selected = Item;
        }
    }
    if (!Selected && !Targets.IsEmpty()) Selected = Targets[0];
    TargetBox->RefreshOptions();
    TargetBox->SetSelectedItem(Selected);
    CrossTime = FootTime = ArmTime = 0.f;
    Status->SetText(FText::FromString(Targets.IsEmpty() ? TEXT("没有运行中的角色，请先开始 PIE") : TEXT("选择角色后开启诊断")));
    return FReply::Handled();
}

void SDeliveryPoseDebug::OnWorldUpdated(UWorld* UpdatedWorld, ELevelTick, float DeltaTime)
{
    if (!bEnabled) return;
    UDeliveryActiveRagdollComponent* Component = Selected ? Selected->Component.Get() : nullptr;
    UWorld* World = Component ? Component->GetWorld() : nullptr;
    if (!World || World->WorldType != EWorldType::PIE || World->bIsTearingDown)
    {
        ClearDrawing();
        CrossTime = FootTime = ArmTime = 0.f;
        Status->SetText(FText::FromString(TEXT("角色已退出，重新运行后刷新列表")));
        return;
    }
    // 多人 PIE 每个世界都会通知，只处理所选角色所在的世界
    if (UpdatedWorld != World) return;
    FDeliveryPoseDebug Pose;
    if (!Component->ReadPoseDebug(Pose))
    {
        ClearDrawing();
        CrossTime = FootTime = ArmTime = 0.f;
        Status->SetText(FText::FromString(TEXT("布娃娃未启用，暂无姿态数据")));
        return;
    }
    // 暂停期间继续显示，但不累计异常持续时间
    const float Step = World->IsPaused() ? 0.f : FMath::Min(DeltaTime, .1f);
    UpdateStatus(Pose, Step);
    DrawPose(World, Pose);
}

void SDeliveryPoseDebug::UpdateStatus(const FDeliveryPoseDebug& Pose, float DeltaTime)
{
    const bool bRemote = Selected->Component->GetOwner()->GetLocalRole() == ROLE_SimulatedProxy;
    const bool bStanding = Pose.bGrounded && !Pose.bLimp && !Pose.bRising;
    CrossTime = !bRemote && bStanding && Pose.bCrossed ? CrossTime + DeltaTime : 0.f;
    FootTime = !bRemote && bStanding && Pose.bFootLate ? FootTime + DeltaTime : 0.f;
    const bool bArmError = (Pose.bPunching || Pose.bReturning) && !Pose.bLimp &&
        ((Pose.ArmValid[0] && Pose.ArmErrors[0] > 60.f) || (Pose.ArmValid[1] && Pose.ArmErrors[1] > 60.f));
    ArmTime = !bRemote && bArmError ? ArmTime + DeltaTime : 0.f;
    SampleRemaining -= DeltaTime;
    if (SampleRemaining > 0.f) return;
    SampleRemaining = .1f;
    FString Issues;
    if (CrossTime >= .2f) Issues += TEXT("双脚持续交叉  ");
    if (FootTime >= .2f) Issues += TEXT("脚未到达目标  ");
    if (ArmTime >= .25f) Issues += TEXT("手臂持续偏离目标  ");
    if (!bRemote && bStanding && Pose.Upright < Pose.MinimumUpright) Issues += TEXT("身体过度倾斜  ");
    if (Issues.IsEmpty()) Issues = bRemote ? TEXT("远端快照：仅观察实际姿态") : TEXT("当前未达到诊断阈值");
    const TCHAR* State = Pose.bLimp ? TEXT("倒地") : Pose.bRising ? TEXT("起身") : Pose.bFootRecovery ? TEXT("脚步恢复") : TEXT("正常控制");
    const TCHAR* Attack = Pose.bPunching ? (Pose.bReleased ? TEXT("释放") : TEXT("蓄力")) : Pose.bArmRecovery ? TEXT("手臂异常恢复") : Pose.bReturning ? TEXT("收拳") : TEXT("无出拳");
    Status->SetText(FText::FromString(FString::Printf(
        TEXT("%s\n身体：%s | 战斗：%s\n双脚左右投影：%.1f cm | 直立值：%.2f\n脚部目标偏差 左 / 右：%.1f / %.1f cm\n手臂旋转偏差 左 / 右：%.1f / %.1f 度"),
        *Issues, State, Attack, Pose.Separation, Pose.Upright, Pose.FootErrors[0], Pose.FootErrors[1], Pose.ArmErrors[0], Pose.ArmErrors[1])));
}

void SDeliveryPoseDebug::DrawPose(UWorld* World, const FDeliveryPoseDebug& Pose)
{
    const bool bRemote = Selected->Component->GetOwner()->GetLocalRole() == ROLE_SimulatedProxy;
    if (World->GetNetMode() == NM_DedicatedServer) { ClearDrawing(); return; }
    if (!Lines.IsValid() || DrawingWorld.Get() != World)
    {
        ClearDrawing();
        Lines.Reset(NewObject<ULineBatchComponent>(GetTransientPackage(), NAME_None, RF_Transient));
        Lines->RegisterComponentWithWorld(World);
        DrawingWorld = World;
    }
    // 暂停时也替换上一帧标记，不在世界中累计球和线
    Lines->Flush();
    auto Sphere = [this](FVector Position, FColor Color, float Radius = 5.f)
    {
        Lines->DrawSphere(Position, Radius, 12, FLinearColor(Color), 0.f, 0, 1.f);
    };
    auto Line = [this](FVector Start, FVector End, FColor Color)
    {
        TArray<FBatchedLine> Batch;
        Batch.Emplace(Start, End, FLinearColor(Color), 0.f, 1.f, 0);
        Lines->DrawLines(Batch);
    };
    if (bShowWalking)
    {
        Sphere(Pose.Hips, FColor::Green, 7.f);
        Lines->DrawDirectionalArrow(Pose.Hips, Pose.Hips + Pose.Right * 60.f, 12.f, FLinearColor::White, 0.f, 0, 1.f);
        for (int32 Index = 0; Index < 2; ++Index)
        {
            Sphere(Pose.Feet[Index], CrossTime >= .2f ? FColor::Red : FColor::Green);
            if (!bRemote && Pose.FootDriven[Index])
            {
                Sphere(Pose.FootTargets[Index], FColor::Cyan);
                Line(Pose.Feet[Index], Pose.FootTargets[Index], FootTime >= .2f ? FColor::Red : FColor::Yellow);
            }
        }
        if (!bRemote && !Pose.bLimp)
        {
            Sphere(Pose.HipTarget, FColor::Cyan, 7.f);
            Line(Pose.Hips, Pose.HipTarget, FColor::Yellow);
        }
    }
    if (bShowCombat)
    {
        for (int32 Index = 0; Index < 2; ++Index)
        {
            if (!Pose.ArmValid[Index]) continue;
            const FColor Color = ArmTime >= .25f && Pose.ArmErrors[Index] > 60.f ? FColor::Red : FColor::Green;
            Sphere(Pose.Hands[Index], Color, 6.f);
            Line(Pose.Shoulders[Index], Pose.Elbows[Index], Color);
            Line(Pose.Elbows[Index], Pose.Hands[Index], Color);
        }
        const auto* Combat = Selected->Component->GetOwner()->FindComponentByClass<UDeliveryRagdollCombatComponent>();
        FVector Start, End;
        if (Combat && Combat->ReadHitDebug(Start, End))
        {
            Sphere(Start, FColor::Orange, Combat->GetHitRadius());
            Sphere(End, FColor::Orange, Combat->GetHitRadius());
            Line(Start, End, FColor::Orange);
        }
    }
    Lines->UpdateBounds();
    Lines->MarkRenderTransformDirty();

}
