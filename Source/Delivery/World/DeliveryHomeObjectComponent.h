#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "SmartObjectRuntime.h"
#include "SmartObjectRequestTypes.h"
#include "World/DeliveryHomeBehaviorDefinition.h"
#include "DeliveryHomeObjectComponent.generated.h"

class ADeliveryHome;
class ADeliveryCompanionCharacter;
class APlayerState;
class USmartObjectComponent;
class USmartObjectSubsystem;
class UAudioComponent;
class UPrimitiveComponent;
class UDeliveryHomeObjectComponent;

// 仅供服务器保存，协议层再映射为本次请求的 candidate_id
struct DELIVERY_API FDeliveryHomeUseCandidate
{
    TWeakObjectPtr<UDeliveryHomeObjectComponent> Object;
    TWeakObjectPtr<const UDeliveryHomeBehaviorDefinition> Definition;
    FGuid ObjectId;
    FName SlotId;
    FName UseId;
    EDeliveryHomeOperation Operation=EDeliveryHomeOperation::None;
    int32 Revision=0;
    FSmartObjectRequestResult Slot;
    FTransform EntranceTransform;
    FTransform UseTransform;
    FTransform ExitTransform;
};

// 调用方只保留此凭据，不能自行释放其他对象或其他动作领取的槽位
struct DELIVERY_API FDeliveryHomeUseClaim
{
    TWeakObjectPtr<UDeliveryHomeObjectComponent> Object;
    FGuid Token;
    FGuid ActionId;
    bool IsValid() const { return Object.IsValid() && Token.IsValid() && ActionId.IsValid(); }
};

// 只记录道具这次通过的家具位置，取走后不继续占用已经释放的摆放槽
struct DELIVERY_API FDeliveryHomePropTransferPoint
{
    bool bEnabled=false;
    TWeakObjectPtr<UDeliveryHomeObjectComponent> Object;
    TWeakObjectPtr<ADeliveryHome> Home;
    TWeakObjectPtr<const UDeliveryHomeBehaviorDefinition> Definition;
    FSmartObjectSlotHandle Slot;
    FTransform ActorTransform;
    FTransform SmartObjectTransform;
    FTransform SlotTransform;
    FTransform UseOffset;
    FVector LocalOffset=FVector::ZeroVector;
    FVector WorldPoint=FVector::ZeroVector;
};

// 把随机令牌写入 SO 自带的 UserData，旧失效回调不能误清同槽的新使用者
USTRUCT()
struct FDeliveryHomeSlotUserData : public FSmartObjectActorUserData
{
    GENERATED_BODY()
    FDeliveryHomeSlotUserData()=default;
    FDeliveryHomeSlotUserData(const AActor* Actor,FGuid InToken) : FSmartObjectActorUserData(Actor), Token(InToken) {}
    UPROPERTY() FGuid Token;
};

UENUM(BlueprintType)
enum class EDeliveryHomeUsePhase : uint8 { Approaching, Using, Placed };

UENUM(BlueprintType)
enum class EDeliveryHomeCarryScalePhase : uint8 { Original, CarriedSmall, WaitingToRestore };

// 原尺度在整次缩小期间保留，换搬运者也不会把小尺度重新记成原尺度
USTRUCT(BlueprintType)
struct FDeliveryHomeCarryScaleState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) EDeliveryHomeCarryScalePhase Phase=EDeliveryHomeCarryScalePhase::Original;
    UPROPERTY(BlueprintReadOnly) FVector OriginalActorScale=FVector::OneVector;
};

// 客户端只显示这个快照，占用判断始终读取服务器 Smart Objects
USTRUCT(BlueprintType)
struct FDeliveryHomeUseState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FName SlotId;
    UPROPERTY(BlueprintReadOnly) FName UseId;
    UPROPERTY(BlueprintReadOnly) FGuid ActionId;
    UPROPERTY(BlueprintReadOnly) EDeliveryHomeOperation Operation=EDeliveryHomeOperation::None;
    UPROPERTY(BlueprintReadOnly) EDeliveryHomeUsePhase Phase=EDeliveryHomeUsePhase::Approaching;
    UPROPERTY(BlueprintReadOnly) TObjectPtr<AActor> User;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FDeliveryHomeUseEnded,FGuid,FName);

// 家具的服务器规则与 SO 句柄清理，NPC 导航、动画和物体移动由现有执行者负责
UCLASS(ClassGroup=(Delivery),meta=(BlueprintSpawnableComponent))
class DELIVERY_API UDeliveryHomeObjectComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UDeliveryHomeObjectComponent();
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Home|身份") TObjectPtr<ADeliveryHome> Home;
    UPROPERTY(EditInstanceOnly,BlueprintReadOnly,Category="Home|身份") FGuid ObjectId;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|身份") FText DisplayName;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|身份",meta=(MultiLine=true)) FText Description;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|身份") FGameplayTagContainer ObjectTags;
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|用途") TArray<EDeliveryHomeOperation> AllowedUses;
    UPROPERTY(EditAnywhere,Category="Home|组件",meta=(UseComponentPicker,AllowedClasses="/Script/SmartObjectsModule.SmartObjectComponent")) FComponentReference SmartObjectReference;
    UPROPERTY(EditAnywhere,Category="Home|组件",meta=(UseComponentPicker,AllowedClasses="/Script/Engine.AudioComponent")) FComponentReference AudioReference;
    // 只对明确配置 TakeProp 的实物生效，任务包裹与人物不能使用
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|搬运缩放",meta=(ClampMin="0.1",ClampMax="1")) float CarryScaleMultiplier=.35f;
    // 取消后落地展开时保持真实底面，只允许中心沿世界 Z 轴上移
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Home|搬运缩放") bool bRestoreScaleFromSupportBottom=true;

    // 只返回当前真实可见、允许接近的用途，不领取槽位
    void FindAvailableUses(ADeliveryCompanionCharacter* Companion,APlayerState* Requester,TArray<FDeliveryHomeUseCandidate>& OutUses) const;
    // 领取只表示准备接近，尚未到达或完成用途
    bool TryClaimUse(ADeliveryCompanionCharacter* Companion,APlayerState* Requester,FGuid ActionId,
        const FDeliveryHomeUseCandidate& Candidate,FDeliveryHomeUseClaim& OutClaim,FName& OutReason);
    bool ValidateClaim(const FDeliveryHomeUseClaim& Claim,bool bRequireInteractionRange,FName& OutReason) const;
    bool GetUseTransforms(const FDeliveryHomeUseClaim& Claim,FTransform& OutEntrance,FTransform& OutUse,FTransform& OutExit) const;
    // Place 的 Use 是支撑面中心；按源物件原尺寸求 root 目标，成功仅表示查询有效，返回尺度为 1
    bool GetPlacementTransform(const FDeliveryHomeUseClaim& Claim,const UDeliveryHomeObjectComponent* Source,
        FTransform& OutTransform,FName& OutReason) const;
    // Take 在登记搬运前读取唯一有效摆放记录；没有配置中转点时返回空快照
    bool CapturePropTransferPoint(const FDeliveryHomeUseClaim& Claim,FDeliveryHomePropTransferPoint& OutPoint,FName& OutReason) const;
    // 不要求旧摆放仍占槽，但家具、槽定义、位置或开关变化都会使本次路线失效
    bool ValidatePropTransferPoint(const FDeliveryHomePropTransferPoint& Point,FName& OutReason) const;
    const UDeliveryHomeBehaviorDefinition* GetUseDefinition(const FDeliveryHomeUseClaim& Claim) const;
    // 实际走到交互点后才将 SO 从 Claimed 改成 Occupied
    bool BeginUse(const FDeliveryHomeUseClaim& Claim,FName& OutReason);
    // 开关成功后释放短使用，实际声音状态继续保留
    bool CommitAudioState(const FDeliveryHomeUseClaim& Claim,FName& OutReason);
    // 只核验已经移动到位并释放搬运的真实道具，不移动道具
    bool CanCommitPlacement(const FDeliveryHomeUseClaim& Claim,AActor* Prop,FName& OutReason) const;
    bool CommitPlacement(const FDeliveryHomeUseClaim& Claim,AActor* Prop,FName& OutReason);
    // 真实搬运登记成功后缩小，返回 true 只表示本次搬运仍有效
    bool BeginCarryScale(const FDeliveryHomeUseClaim& TakeClaim,UActorComponent* Provider,FGuid CarryId,FName& OutReason);
    FVector GetOriginalActorScale() const;
    // 输入的位置和朝向不变，尺寸按保存的原尺度计算，查询不会移动实物
    FBox GetOriginalSizeBounds(const FTransform& ActorTransform) const;
    bool CanRestoreScaleAt(const FTransform& FullSizeGoal,FName& OutReason) const;
    // 用实际当前位置恢复，必须先释放搬运且已到指定目标附近，不会横移到目标
    bool TryRestoreScaleAt(const FTransform& FullSizeGoal,FName& OutReason);
    // 只登记等待，下次元数据更新才按实物当前位置检查展开
    void QueueScaleRestore();
    UFUNCTION(BlueprintPure,Category="Home|状态") bool IsCarryScaleActive() const { return CarryScaleState.Phase!=EDeliveryHomeCarryScalePhase::Original; }
    UFUNCTION(BlueprintPure,Category="Home|状态") FDeliveryHomeCarryScaleState GetCarryScaleState() const { return CarryScaleState; }
    // 已提交的摆放由实物持有槽位，旧执行者的 ReleaseUse 不会把它释放
    void ReleaseUse(const FDeliveryHomeUseClaim& Claim,FName Reason);
    FDeliveryHomeUseEnded OnUseEnded;
    UFUNCTION(BlueprintPure,Category="Home|状态") int32 GetRevision() const { return Revision; }
    UFUNCTION(BlueprintPure,Category="Home|状态") bool IsAudioPlaying() const { return bAudioPlaying; }
    UFUNCTION(BlueprintPure,Category="Home|状态") TArray<FDeliveryHomeUseState> GetUseStates() const { return UseStates; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime,ELevelTick Type,FActorComponentTickFunction* Function) override;
private:
    struct FOwnedUse
    {
        FDeliveryHomeUseClaim Ticket;
        FSmartObjectClaimHandle Handle;
        TWeakObjectPtr<ADeliveryCompanionCharacter> Companion;
        TWeakObjectPtr<APlayerState> Requester;
        TWeakObjectPtr<const UDeliveryHomeBehaviorDefinition> Definition;
        TWeakObjectPtr<AActor> PlacedProp;
        bool bPlacementCommitted=false;
        bool bExecutionEnded=false;
    };
    USmartObjectComponent* ResolveSmartObject() const;
    UAudioComponent* ResolveAudio() const;
    bool ValidateHome(FName& OutReason) const;
    bool ValidateUser(ADeliveryCompanionCharacter* Companion,APlayerState* Requester,FName& OutReason) const;
    bool ValidateUse(ADeliveryCompanionCharacter* Companion,const UDeliveryHomeBehaviorDefinition* Definition,
        const FSmartObjectSlotHandle& Slot,bool bClose,FName& OutReason) const;
    // 候选、领取复核和执行读取同一站位，Take 入口不跟着物件高度悬空
    bool ResolveUseTransforms(ADeliveryCompanionCharacter* Companion,const UDeliveryHomeBehaviorDefinition* Definition,
        const FSmartObjectSlotHandle& Slot,FTransform& OutEntrance,FTransform& OutUse,FTransform& OutExit,FName& OutReason) const;
    bool ResolveTakeStandingPoint(ADeliveryCompanionCharacter* Companion,const UDeliveryHomeBehaviorDefinition& Definition,
        const FTransform& Authored,const FTransform& Use,bool bProjectNearby,FTransform& OutStanding,FName& OutReason) const;
    bool FindCommittedPlacement(FDeliveryHomeUseClaim& OutClaim,FName& OutReason) const;
    bool ReadPropTransferPoint(const FOwnedUse& Use,FDeliveryHomePropTransferPoint& OutPoint,FName& OutReason) const;
    bool IsOperationAvailable(const UDeliveryHomeBehaviorDefinition* Definition) const;
    bool IsCurrentSmartObjectClaim(const FOwnedUse& Use) const;
    bool IsPlacementValid(const FOwnedUse& Use,AActor* Prop,FName& OutReason) const;
    bool ValidateScaleObject(FName& OutReason) const;
    FBox CalculateSizeBounds(const FTransform& ActorTransform) const;
    bool HasRestoreSpaceAndSupport(const FTransform& Target,bool bCheckExpansion,FName& OutReason) const;
    bool RestoreScale(const FTransform& Target,FName& OutReason);
    void UpdateScaleRestore();
    void SetCarryScalePhase(EDeliveryHomeCarryScalePhase Phase);
    FOwnedUse* FindOwned(const FDeliveryHomeUseClaim& Claim);
    const FOwnedUse* FindOwned(const FDeliveryHomeUseClaim& Claim) const;
    void ReleaseOwned(FGuid Token,FName Reason,bool bSlotAlreadyInvalid=false);
    void HandleSlotInvalidated(FSmartObjectClaimHandle Handle);
    void RefreshReplicatedState();
    UFUNCTION() void OnRep_Audio();
    void RetryAudioWhenReady();
    void StopFinishedAudio(int32 ExpectedRevision);
    double GetServerTime() const;
    // 这里只保存本组件亲自领取的句柄，不保存另一份座位可用状态
    TMap<FGuid,FOwnedUse> OwnedUses;
    UPROPERTY(Replicated) int32 Revision=0;
    UPROPERTY(Replicated) TArray<FDeliveryHomeUseState> UseStates;
    // 尺度只由本组件在服务器修改，视觉与碰撞沿根组件原生 RelativeScale3D 复制
    UPROPERTY(Replicated) FDeliveryHomeCarryScaleState CarryScaleState;
    TWeakObjectPtr<UPrimitiveComponent> ScaleRoot;
    uint32 ScaleGeneration=0;
    bool bChangingCarryScale=false;
    UPROPERTY(ReplicatedUsing=OnRep_Audio) bool bAudioPlaying=false;
    UPROPERTY(ReplicatedUsing=OnRep_Audio) TObjectPtr<USoundBase> ActiveSound;
    UPROPERTY(ReplicatedUsing=OnRep_Audio) double AudioStartedServerTime=0;
    FTimerHandle AudioFinishTimer;
    // 客户端等待服务器时钟、声音引用或本地组件就绪，停播和退出时清理
    FTimerHandle AudioReadyTimer;
    int32 AudioGeneration=0;
    FTransform LastSmartObjectTransform;
    FSmartObjectHandle LastSmartObjectHandle;
    TWeakObjectPtr<USoundBase> PresentedSound;
    double PresentedStartTime=-1;
    bool bEndingPlay=false;
};
