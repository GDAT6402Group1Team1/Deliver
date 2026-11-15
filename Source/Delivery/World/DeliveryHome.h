#pragma once

#include "CoreMinimal.h"
#include "World/DeliveryLocationMarker.h"
#include "DeliveryHome.generated.h"

class ADeliveryGrandpa;
class USceneComponent;
class UNavLinkCustomComponent;
class ADeliveryBreakableDoor;
class UDeliveryHomeObjectComponent;
class APlayerState;
class UTexture2D;
class USoundBase;
class SWidget;
class UGameViewportClient;

// 解锁反馈控件每帧读取的一份只读状态
struct FDeliveryHomeClaimFeedback
{
    bool bMarker=false;
    bool bMine=true;
    // 距离本机开始播放的秒数，负数表示这台机器没有播放这次解锁
    double Elapsed=-1;
    float StarSeconds=1;
    float BannerSeconds=2.5f;
    float MarkerDistance=6000;
    FVector FrameBase=FVector::ZeroVector;
    FVector FrameRight=FVector::RightVector;
    float FrameWidth=120;
    float FrameHeight=215;
    UTexture2D* Star=nullptr;
    UTexture2D* Spark=nullptr;
    UTexture2D* Marker=nullptr;
};

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
    UPROPERTY(ReplicatedUsing=OnRep_ClaimEventId,BlueprintReadOnly,Category="Delivery|Home") FGuid ClaimEventId;
    // 催爷爷搬走的玩家，他看到「新家到手！」并听到确认音，其他人看到「街坊小屋已解锁」；没有记录时所有人按完成者处理
    UPROPERTY(Replicated,BlueprintReadOnly,Category="Delivery|Home") TObjectPtr<APlayerState> ClaimedBy;
    // 解锁提示停留的时间，星光、确认音和提示都按 ClaimEventId 去重
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈",meta=(ClampMin="0.5",ClampMax="8",Units="s")) float ClaimHintSeconds=2.5f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈",meta=(ClampMin="0.3",ClampMax="4",Units="s")) float ClaimStarSeconds=1.f;
    // 门框尺寸和位置微调；X 沿门宽，Y 朝屋外，Z 向上，原点在门洞屋外一端的地面
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈",meta=(ClampMin="40",ClampMax="400",Units="cm")) float ClaimFrameWidth=120.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈",meta=(ClampMin="80",ClampMax="500",Units="cm")) float ClaimFrameHeight=215.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈") FVector ClaimFrameOffset=FVector::ZeroVector;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈",meta=(ClampMin="500",ClampMax="30000",Units="cm")) float ClaimMarkerDistance=6000.f;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈") TSoftObjectPtr<UTexture2D> ClaimStarTexture;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈") TSoftObjectPtr<UTexture2D> ClaimSparkTexture;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈") TSoftObjectPtr<UTexture2D> ClaimMarkerTexture;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈") TSoftObjectPtr<USoundBase> ClaimSound;
    // 确认音资产还没导入时的替代音
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Delivery|Home|解锁反馈") TSoftObjectPtr<USoundBase> ClaimFallbackSound;
    bool GetClaimFeedback(FDeliveryHomeClaimFeedback& Out) const;
    UPROPERTY(Replicated,BlueprintReadOnly,Category="Delivery|Home") double ClaimedServerTime=-1;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UFUNCTION() void RefreshDoorwayNavigation();
    UFUNCTION() void OnRep_ClaimEventId();
    // 每台机器对同一次解锁只提示一次，晚加入的玩家只看到已解锁状态
    void ShowClaimHint();
    // 本机视口上的反馈层：门框星光、门口小房子标记和解锁提示
    void EnsureClaimOverlay();
    void RemoveClaimOverlay();
    bool IsClaimedByLocalPlayer() const;
    FGuid ShownClaimEvent;
    double ClaimFeedbackStartedAt=-1;
    bool bClaimFeedbackMine=true;
    FTimerHandle ClaimOverlayTimer;
    TSharedPtr<SWidget> ClaimOverlay;
    TWeakObjectPtr<UGameViewportClient> ClaimOverlayViewport;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> LoadedClaimStar;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> LoadedClaimSpark;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> LoadedClaimMarker;
    UFUNCTION() void OnDoorDestroyed(AActor* DestroyedActor);
    TWeakObjectPtr<ADeliveryBreakableDoor> BoundDoor;
};
