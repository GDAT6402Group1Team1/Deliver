#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DeliveryHUD.generated.h"

class ADeliveryCharacter;
class UDeliveryHotbarWidget;
class UDeliveryInventoryComponent;

// 管理本机快捷栏的创建、背包绑定和销毁，界面布局仍由控件负责
UCLASS()
class DELIVERY_API ADeliveryHUD : public AHUD
{
    GENERATED_BODY()
public:
    ADeliveryHUD();
    virtual void Tick(float DeltaSeconds) override;
    // 骑车时读取驾驶员的背包，步行时读取当前角色的背包
    static ADeliveryCharacter* ResolveCharacter(APawn* Pawn);
protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    UPROPERTY(EditDefaultsOnly, Category="UI|Hotbar")
    TSoftClassPtr<UDeliveryHotbarWidget> HotbarWidgetClass;
private:
    void RefreshHotbar();
    void RemoveHotbar();
    UPROPERTY(Transient)
    TObjectPtr<UDeliveryHotbarWidget> HotbarWidget;
    TWeakObjectPtr<UDeliveryInventoryComponent> BoundInventory;
    TWeakObjectPtr<ADeliveryCharacter> BoundCharacter;
};
