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

/**
 * @brief AI 观察面板的界面处理器，HUD 在本机创建；只给面板挑选要看的 NPC 和保存记录，不参与模型和玩法
 *
 * 玩家打开交谈面板或发出一句话时调用 ObserveNpc，交谈组件开始转发这位 NPC 的决策记录；记录到了调用 HandleTrace
 * 最近 8 轮记录存在 History 里，换 NPC、换角色或世界结束时清空
 */
UCLASS()
class DELIVERY_API UDeliveryAiInspectorController : public UObject
{
    GENERATED_BODY()
public:
    /// @brief 绑定本机玩家、HUD 和面板，订阅交谈组件的决策记录和发言通知，订阅面板的展开、历史和回到最新事件
    void Initialize(APlayerController* InPlayer,ADeliveryHUD* InHUD,UDeliveryAiInspectorWidget* InWidget);

    /// @brief 取消所有订阅，交谈组件停止转发记录，登记面板关闭，清空记录
    void Shutdown();

    /// @brief 换成看 Npc：清空面板和记录，让交谈组件开始转发这位 NPC 的决策记录；已经在看它时直接返回
    void ObserveNpc(ADeliveryNpcCharacter* Npc);

    /// @brief HUD 每 0.1 秒调用：玩家换了角色或看的 NPC 没了时清空面板，交谈组件停止转发
    void ValidateContext();

    /// @brief F6：让面板展开或收起
    void ToggleExpanded();

    /// @brief 玩家所在的世界
    virtual UWorld* GetWorld() const override;
private:
    /// @brief 收到一条决策记录：更新或插到 History 最前面（最多 8 条），没在看历史时显示它
    void HandleTrace(const FDeliveryNpcDecisionTrace& Trace);

    /// @brief 玩家对某位 NPC 说了一句话时调用 ObserveNpc 看那位 NPC
    void HandleSubmitted(ADeliveryNpcCharacter* Npc,FGuid MessageId,const FString& Text);

    /// @brief 面板展开或收起时登记面板开关，展开时显示鼠标
    void HandleExpanded(bool bExpanded);

    /// @brief 玩家点了第 Index 条历史，面板显示那一条
    void ShowHistory(int32 Index);

    /// @brief 玩家点了回到最新，面板显示 LatestTrace
    void ShowLatest();

    /// @brief 清空记录和面板
    void ResetView();

    /// 本机玩家
    TWeakObjectPtr<APlayerController> Player;

    /// 本机 HUD
    TWeakObjectPtr<ADeliveryHUD> HUD;

    /// 观察面板
    TWeakObjectPtr<UDeliveryAiInspectorWidget> Widget;

    /// 玩家控制器上的交谈组件，它转发 NPC 的决策记录
    TWeakObjectPtr<UDeliveryConversationComponent> Conversation;

    /// 正在看的 NPC
    TWeakObjectPtr<ADeliveryNpcCharacter> ObservedNpc;

    /// 开始看时玩家控制的角色；换了角色就清空
    TWeakObjectPtr<APawn> ObservedPawn;

    /// 最新的一条记录
    FDeliveryNpcDecisionTrace LatestTrace;

    /// 最近 8 轮记录，最新的在最前面
    TArray<FDeliveryNpcDecisionTrace> History;

    /// 面板正在显示历史记录，新记录到了不切换
    bool bShowingHistory=false;

    /// Initialize 成功后为 true
    bool bInitialized=false;
    friend class FDeliveryAiInspectorLifecycleTest;
};
