#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DeliveryInteractionRegistry.generated.h"

class APawn;
class UDeliveryInteractableComponent;
enum class EDeliveryInteractionKey : uint8;

// 保存当前世界的交互组件，组件的生命周期仍由所属物体管理
UCLASS()
class DELIVERY_API UDeliveryInteractionRegistry : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    void Register(UDeliveryInteractableComponent* Component);
    void Unregister(UDeliveryInteractableComponent* Component);
    virtual void Deinitialize() override;

    // 收集范围内允许交互的目标，准星排序由玩家探测器负责
    void GetCandidates(const APawn* Seeker, EDeliveryInteractionKey Key,
        TArray<UDeliveryInteractableComponent*>& OutCandidates);
    UDeliveryInteractableComponent* FindNearest(const APawn* Seeker, EDeliveryInteractionKey Key);

private:
    TArray<TWeakObjectPtr<UDeliveryInteractableComponent>> Entries;
};
