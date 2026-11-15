#pragma once

#include "CoreMinimal.h"
#include "World/DeliveryLocationMarker.h"
#include "DeliveryHome.generated.h"

class ADeliveryGrandpa;
class USceneComponent;
class UNavLinkCustomComponent;
class ADeliveryBreakableDoor;
class UDeliveryHomeObjectComponent;

// 现有爷爷房屋的共享地点状态，按 World 只放一份，不归某个玩家所有
UCLASS()
class DELIVERY_API ADeliveryHome : public ADeliveryLocationMarker
{
    GENERATED_BODY()
public:
    ADeliveryHome();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;

    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Delivery|Home") TObjectPtr<AActor> ExistingHouse;
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Delivery|Home") TObjectPtr<ADeliveryGrandpa> FormerResident;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|Home") TObjectPtr<USceneComponent> ReturnAnchor;
    // 车辆在屋外结束行程，人物走到这个屋内地面点才算回到家
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|Home") TObjectPtr<USceneComponent> InteriorAnchor;
    // 只登记关卡中明确配置的实物，不按房屋范围或网格名称推断所有权
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Delivery|Home") TArray<TObjectPtr<AActor>> Furnishings;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home",meta=(ClampMin="100",ClampMax="3000",Units="cm")) float ObjectSightRadius=1600;
    // 门洞的两端在组件中编辑，完整门禁用，破门后由普通路径跟随步行通过
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Delivery|Home") TObjectPtr<UNavLinkCustomComponent> DoorwayLink;
    // 地面点相对本标记保存，包含门洞内外及屋外终点；不随爷爷移动
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Delivery|Home",meta=(MakeEditWidget=true)) TArray<FVector> DepartureRoute;

    UFUNCTION(BlueprintPure,Category="Delivery|Home",meta=(WorldContext="WorldContextObject"))
    static ADeliveryHome* FindSharedHome(const UObject* WorldContextObject);
    UFUNCTION(BlueprintPure,Category="Delivery|Home") bool IsClaimed() const { return bClaimed; }
    UFUNCTION(BlueprintPure,Category="Delivery|Home") bool TryGetReturnLocation(FVector& OutLocation) const;
    UFUNCTION(BlueprintPure,Category="Delivery|Home") bool TryGetInteriorLocation(FVector& OutLocation) const;
    // 按同层门叶横截面判断两点是否从门的一侧跨到另一侧
    bool CrossesPedestrianDoorway(const FVector& FromFeet,const FVector& ToFeet) const;
    void GetFurnitureObjects(TArray<UDeliveryHomeObjectComponent*>& OutObjects) const;
    bool ContainsFurniture(const UDeliveryHomeObjectComponent* Object) const;
    bool GetDeparturePoint(int32 Index,FVector& OutLocation) const;
    bool TryClaimAfterDeparture(ADeliveryGrandpa* Resident);

    UPROPERTY(Replicated,BlueprintReadOnly,Category="Delivery|Home") bool bClaimed=false;
    UPROPERTY(Replicated,BlueprintReadOnly,Category="Delivery|Home") FGuid ClaimEventId;
    UPROPERTY(Replicated,BlueprintReadOnly,Category="Delivery|Home") double ClaimedServerTime=-1;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UFUNCTION() void RefreshDoorwayNavigation();
    UFUNCTION() void OnDoorDestroyed(AActor* DestroyedActor);
    TWeakObjectPtr<ADeliveryBreakableDoor> BoundDoor;
};
