#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/LineBatchComponent.h"
#include "Widgets/Input/SComboBox.h"
#include "Ragdoll/Debug/DeliveryPoseDebug.h"

class UDeliveryActiveRagdollComponent;
class STextBlock;
class ULineBatchComponent;

// 选择 PIE 世界中的角色，读取姿态快照并绘制临时标记
// 只保存弱引用，关闭页面后停止采样，不写角色参数或资源
class SDeliveryPoseDebug : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDeliveryPoseDebug) {}
    SLATE_END_ARGS()
    virtual ~SDeliveryPoseDebug() override;
    void ClearDrawing();
    void StopDiagnostics();
    void Construct(const FArguments& Args);

private:
    friend class FDeliveryPoseDebugTest;
    struct FTarget
    {
        TWeakObjectPtr<UDeliveryActiveRagdollComponent> Component;
        FString Label;
    };
    using FTargetPtr = TSharedPtr<FTarget>;
    FReply RefreshTargets();
    void OnEndPIE(bool bSimulating);
    void OnWorldUpdated(UWorld* World, ELevelTick TickType, float DeltaTime);
    void DrawPose(UWorld* World, const FDeliveryPoseDebug& Pose);
    void UpdateStatus(const FDeliveryPoseDebug& Pose, float DeltaTime);

    FDelegateHandle EndPIEHandle;
    FDelegateHandle WorldUpdateHandle;
    TArray<FTargetPtr> Targets;
    FTargetPtr Selected;
    TSharedPtr<SComboBox<FTargetPtr>> TargetBox;
    TSharedPtr<STextBlock> Status;
    TStrongObjectPtr<ULineBatchComponent> Lines;
    TWeakObjectPtr<UWorld> DrawingWorld;
    bool bEnabled = false;
    bool bShowWalking = true;
    bool bShowCombat = true;
    float SampleRemaining = 0.f;
    float CrossTime = 0.f;
    float FootTime = 0.f;
    float ArmTime = 0.f;
};
