#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AI/Decision/DeliveryNpcDecisionTrace.h"
#include "DeliveryAiInspectorController.generated.h"

class ADeliveryHUD;
class APlayerController;
class APawn;
class ADeliveryNpcCharacter;
class UDeliveryAiInspectorWidget;
class UDeliveryConversationComponent;

// 本机演示界面处理器，选择观察对象并保存有上限的请求记录，不参与模型选择和玩法执行
UCLASS()
class DELIVERY_API UDeliveryAiInspectorController : public UObject
{
    GENERATED_BODY()
public:
    void Initialize(APlayerController* InPlayer,ADeliveryHUD* InHUD,UDeliveryAiInspectorWidget* InWidget);
    void Shutdown();
    void ObserveNpc(ADeliveryNpcCharacter* Npc);
    void ValidateContext();
    void ToggleExpanded();
    virtual UWorld* GetWorld() const override;
private:
    void HandleTrace(const FDeliveryNpcDecisionTrace& Trace);
    void HandleSubmitted(ADeliveryNpcCharacter* Npc,FGuid MessageId,const FString& Text);
    void HandleExpanded(bool bExpanded);
    void ShowHistory(int32 Index);
    void ShowLatest();
    void ResetView();
    TWeakObjectPtr<APlayerController> Player;
    TWeakObjectPtr<ADeliveryHUD> HUD;
    TWeakObjectPtr<UDeliveryAiInspectorWidget> Widget;
    TWeakObjectPtr<UDeliveryConversationComponent> Conversation;
    TWeakObjectPtr<ADeliveryNpcCharacter> ObservedNpc;
    TWeakObjectPtr<APawn> ObservedPawn;
    FDeliveryNpcDecisionTrace LatestTrace;
    TArray<FDeliveryNpcDecisionTrace> History;
    bool bShowingHistory=false;
    bool bInitialized=false;
    friend class FDeliveryAiInspectorLifecycleTest;
};
