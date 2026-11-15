#include "DeliveryHomeObjectAssetTools.h"
#include "World/DeliveryHome.h"
#include "World/DeliveryHomeObjectComponent.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "Grab/DeliveryGrabbableComponent.h"
#include "SmartObjectComponent.h"
#include "SmartObjectDefinition.h"
#include "ActorFactories/ActorFactoryBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Animation/AnimSequence.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"
#include "PhysicsEngine/BodySetup.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/PackageName.h"
#include "AssetCompilingManager.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "NavLinkCustomComponent.h"
#include "EngineUtils.h"
#include "Misc/ScopeExit.h"

namespace
{
const TCHAR* AssetRoot=TEXT("/Game/NPC/Companion/Home/");
const TCHAR* ToolKey=TEXT("DeliveryHomeAssetTool");
const TCHAR* ToolVersion=TEXT("1");

bool IsEditorActor(const AActor* Actor)
{
    return GEditor && !GEditor->PlayWorld && IsValid(Actor) && !Actor->IsTemplate() &&
        Actor->GetWorld()==GEditor->GetEditorWorldContext().World() &&
        Actor->GetWorld()->WorldType==EWorldType::Editor && !Actor->GetWorld()->HasBegunPlay();
}

FString Json(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object,TJsonWriterFactory<>::Create(&Result));
    return Result;
}

FString HomeToolError(const FString& Message)
{
    auto Result=MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("error"),Message);
    return Json(Result);
}

TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X),MakeShared<FJsonValueNumber>(Value.Y),MakeShared<FJsonValueNumber>(Value.Z)};
}

bool ReadMeshContactGeometry(const UStaticMesh& Mesh,TArray<FVector>& OutHull,FBox& OutFootprint)
{
    const auto* RenderData=Mesh.GetRenderData();
    if (!RenderData || RenderData->LODResources.IsEmpty()) return false;
    const auto& Positions=RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer;
    if (!Positions.GetVertexData() || Positions.GetNumVertices()<4 || Positions.GetNumVertices()>100000) return false;
    FBox RenderBounds(ForceInit);
    OutHull.Reserve(Positions.GetNumVertices());
    for (uint32 Index=0;Index<Positions.GetNumVertices();++Index)
    {
        const FVector Point(Positions.VertexPosition(Index));
        if (Point.ContainsNaN()) return false;
        RenderBounds+=Point;
        OutHull.Add(Point);
    }
    if (!RenderBounds.IsValid || RenderBounds.GetExtent().GetMin()<=.1) return false;
    const double BottomBand=RenderBounds.Min.Z+FMath::Clamp(RenderBounds.GetSize().Z*.02,.25,1.);
    for (const FVector& Vertex:OutHull)
        if (Vertex.Z<=BottomBand) OutFootprint+=Vertex;
    return OutFootprint.IsValid && OutFootprint.GetExtent().X>.1 && OutFootprint.GetExtent().Y>.1;
}

TSharedRef<FJsonObject> TransformJson(const FTransform& Value)
{
    auto Result=MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("location"),VectorJson(Value.GetLocation()));
    Result->SetArrayField(TEXT("rotation"),VectorJson(FVector(Value.Rotator().Pitch,Value.Rotator().Yaw,Value.Rotator().Roll)));
    Result->SetArrayField(TEXT("scale"),VectorJson(Value.GetScale3D()));
    return Result;
}

bool ReadVector(const TSharedPtr<FJsonObject>& Object,const TCHAR* Field,FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Array=nullptr;
    if (!Object->TryGetArrayField(Field,Array) || Array->Num()!=3) return false;
    double Values[3];
    for (int32 Index=0;Index<3;++Index)
        if (!(*Array)[Index]->TryGetNumber(Values[Index]) || !FMath::IsFinite(Values[Index])) return false;
    Out=FVector(Values[0],Values[1],Values[2]);
    return true;
}

bool ReadTransform(const TSharedPtr<FJsonObject>& Object,const TCHAR* Field,FTransform& Out)
{
    const TSharedPtr<FJsonObject>* Data=nullptr;
    FVector Location,Angles;
    if (!Object->TryGetObjectField(Field,Data) || !ReadVector(*Data,TEXT("location"),Location) ||
        !ReadVector(*Data,TEXT("rotation"),Angles)) return false;
    Out=FTransform(FRotator(Angles.X,Angles.Y,Angles.Z),Location,FVector::OneVector);
    return true;
}

bool ReadOperation(const FString& Name,EDeliveryHomeOperation& Out)
{
    const UEnum* Enum=StaticEnum<EDeliveryHomeOperation>();
    const int64 Value=Enum->GetValueByNameString(Name);
    if (Value==INDEX_NONE || Value==static_cast<int64>(EDeliveryHomeOperation::None)) return false;
    Out=static_cast<EDeliveryHomeOperation>(Value);
    return true;
}

bool IsAssetPath(const FString& Path)
{
    return Path.StartsWith(AssetRoot) && FPackageName::IsValidLongPackageName(Path) && !Path.Contains(TEXT("."));
}

bool IsOwnedAsset(UObject* Asset)
{
    return Asset && Asset->GetPackage()->GetMetaData().GetValue(Asset,ToolKey)==ToolVersion;
}

void MarkOwnedAsset(UObject* Asset,const FString& SourceMesh)
{
    Asset->GetPackage()->GetMetaData().SetValue(Asset,ToolKey,ToolVersion);
    Asset->GetPackage()->GetMetaData().SetValue(Asset,TEXT("DeliveryHomeSourceMesh"),*SourceMesh);
    Asset->MarkPackageDirty();
}

template<typename T> T* NewAsset(const FString& Path)
{
    auto* Asset=NewObject<T>(CreatePackage(*Path),*FPackageName::GetLongPackageAssetName(Path),RF_Public|RF_Standalone|RF_Transactional);
    FAssetRegistryModule::AssetCreated(Asset);
    return Asset;
}

template<typename T> T* EnsureComponent(UBlueprint* Blueprint,FName Name,const USceneComponent* Root=nullptr)
{
    USCS_Node* Node=nullptr;
    for (auto* Existing:Blueprint->SimpleConstructionScript->GetAllNodes())
        if (Existing->GetVariableName()==Name) { Node=Existing; break; }
    if (!Node)
    {
        Node=Blueprint->SimpleConstructionScript->CreateNode(T::StaticClass(),Name);
        Blueprint->SimpleConstructionScript->AddNode(Node);
        if (Root) Node->SetParent(Root);
    }
    return Cast<T>(Node->ComponentTemplate);
}

void CopyMeshSettings(UStaticMeshComponent* Source,UStaticMeshComponent* Target,UStaticMesh* Mesh,bool bProp,float PropMassKg)
{
    Target->SetStaticMesh(Mesh);
    Target->EmptyOverrideMaterials();
    for (int32 Index=0;Index<Source->GetNumOverrideMaterials();++Index)
        Target->SetMaterial(Index,Source->OverrideMaterials[Index]);
    Target->SetCastShadow(Source->CastShadow);
    Target->SetCollisionProfileName(bProp?FName(TEXT("PhysicsActor")):Source->GetCollisionProfileName());
    if (!bProp)
    {
        Target->SetCollisionEnabled(Source->GetCollisionEnabled());
        Target->SetCollisionResponseToChannels(Source->GetCollisionResponseToChannels());
    }
    Target->SetMobility(bProp?EComponentMobility::Movable:Source->Mobility.GetValue());
    Target->BodyInstance.bSimulatePhysics=bProp;
    if (bProp)
    {
        // CDO 只保存质量覆盖，创建真实刚体时再使用该值
        Target->BodyInstance.SetMassOverride(PropMassKg,true);
        Target->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Target->SetEnableGravity(true);
        Target->SetIsReplicated(true);
        Target->SetCanEverAffectNavigation(false);
    }
}

struct FPreparedSlot
{
    FTransform SlotTransform;
    TStrongObjectPtr<UDeliveryHomeBehaviorDefinition> Behavior;
};

bool PrepareSlots(const TSharedPtr<FJsonObject>& Config,TArray<FPreparedSlot>& Out,FString& OutError)
{
    const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
    if (!Config->TryGetArrayField(TEXT("slots"),Values)) { OutError=TEXT("slots array is required"); return false; }
    TSet<FName> Names;
    for (const auto& Value:*Values)
    {
        const auto Data=Value->AsObject();
        if (!Data) { OutError=TEXT("slot must be an object"); return false; }
        auto* Definition=NewObject<UDeliveryHomeBehaviorDefinition>();
        FString SlotId,UseId,Name,Operation;
        FPreparedSlot Slot;
        if (!Data->TryGetStringField(TEXT("slot_id"),SlotId) || !Data->TryGetStringField(TEXT("use_id"),UseId) ||
            !Data->TryGetStringField(TEXT("display_name"),Name) || !Data->TryGetStringField(TEXT("operation"),Operation) ||
            !ReadOperation(Operation,Definition->Operation) || !ReadTransform(Data,TEXT("slot"),Slot.SlotTransform) ||
            !ReadTransform(Data,TEXT("entrance"),Definition->EntranceOffset) || !ReadTransform(Data,TEXT("use"),Definition->UseOffset) ||
            !ReadTransform(Data,TEXT("exit"),Definition->ExitOffset) || !ReadVector(Data,TEXT("sight_offset"),Definition->SightOffset))
        { OutError=TEXT("slot identity, operation or calibrated transform is missing"); return false; }
        Definition->SlotId=FName(*SlotId); Definition->UseId=FName(*UseId); Definition->DisplayName=FText::FromString(Name);
        if (Names.Contains(Definition->SlotId)) { OutError=TEXT("duplicate slot_id"); return false; }
        Names.Add(Definition->SlotId);
        double Number;
        if (!Data->TryGetNumberField(TEXT("approach_radius"),Number)) { OutError=TEXT("approach_radius missing"); return false; }
        Definition->ApproachRadius=Number;
        if (!Data->TryGetNumberField(TEXT("use_radius"),Number)) { OutError=TEXT("use_radius missing"); return false; }
        Definition->UseRadius=Number;
        if (Data->TryGetNumberField(TEXT("use_seconds"),Number)) Definition->UseSeconds=Number;
        FString Path;
        if (Data->TryGetStringField(TEXT("enter_animation"),Path)) Definition->EnterAnimation=LoadObject<UAnimSequence>(nullptr,*Path);
        if (Data->TryGetStringField(TEXT("animation"),Path)) Definition->Animation=LoadObject<UAnimSequence>(nullptr,*Path);
        if (Data->TryGetStringField(TEXT("exit_animation"),Path)) Definition->ExitAnimation=LoadObject<UAnimSequence>(nullptr,*Path);
        if (Data->TryGetStringField(TEXT("seat_mount_animation"),Path)) Definition->SeatMountAnimation=LoadObject<UAnimSequence>(nullptr,*Path);
        if (Data->TryGetStringField(TEXT("seat_dismount_animation"),Path)) Definition->SeatDismountAnimation=LoadObject<UAnimSequence>(nullptr,*Path);
        if (Data->TryGetStringField(TEXT("sound"),Path)) Definition->Sound=LoadObject<USoundBase>(nullptr,*Path);
        Definition->bUsePropTransferPoint=false;
        Definition->PropTransferOffset=FVector::ZeroVector;
        if (Data->HasField(TEXT("use_prop_transfer_point")) &&
            (!Data->HasTypedField<EJson::Boolean>(TEXT("use_prop_transfer_point")) ||
             !Data->TryGetBoolField(TEXT("use_prop_transfer_point"),Definition->bUsePropTransferPoint)))
        { OutError=TEXT("use_prop_transfer_point must be a boolean"); return false; }
        if (Data->HasField(TEXT("prop_transfer_offset")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Offset=nullptr;
            if (!Data->TryGetArrayField(TEXT("prop_transfer_offset"),Offset) || Offset->Num()!=3)
            { OutError=TEXT("prop_transfer_offset requires three finite numbers"); return false; }
            // Json 的数值接口会接受布尔值和数字字符串，这里只允许真实数值
            for (const auto& Coordinate:*Offset)
                if (!Coordinate.IsValid() || Coordinate->Type!=EJson::Number)
                { OutError=TEXT("prop_transfer_offset requires three finite numbers"); return false; }
            if (!ReadVector(Data,TEXT("prop_transfer_offset"),Definition->PropTransferOffset))
            { OutError=TEXT("prop_transfer_offset requires three finite numbers"); return false; }
        }
        if (Definition->bUsePropTransferPoint && Definition->Operation!=EDeliveryHomeOperation::PlaceProp)
        { OutError=TEXT("prop transfer point is only supported by PlaceProp"); return false; }
        if (Definition->Operation==EDeliveryHomeOperation::PlaceProp)
        {
            if (!ReadVector(Data,TEXT("maximum_prop_half_extent"),Definition->MaximumPropHalfExtent))
            { OutError=TEXT("placement bounds are required"); return false; }
            if (Data->TryGetNumberField(TEXT("placement_tolerance"),Number)) Definition->PlacementTolerance=Number;
            if (Data->TryGetNumberField(TEXT("placement_angle_tolerance"),Number)) Definition->PlacementAngleTolerance=Number;
            if (Data->TryGetNumberField(TEXT("maximum_support_gap"),Number)) Definition->MaximumSupportGap=Number;
        }
        if (!Definition->IsUsableConfiguration()) { OutError=TEXT("slot configuration or required animation/sound is invalid"); return false; }
        Slot.Behavior=TStrongObjectPtr<UDeliveryHomeBehaviorDefinition>(Definition);
        Out.Add(MoveTemp(Slot));
    }
    return true;
}
}

FString UDeliveryHomeObjectAssetTools::InspectFurniturePlacement(AActor* Source,AActor* Support,const FTransform& Surface)
{
    if (!IsEditorActor(Source) || !IsEditorActor(Support) || Source==Support || Surface.ContainsNaN() ||
        !Surface.GetRotation().IsNormalized()) return HomeToolError(TEXT("InvalidEditorFurniturePlacement"));
    const auto* SourceObject=Source->FindComponentByClass<UDeliveryHomeObjectComponent>();
    const auto* SupportObject=Support->FindComponentByClass<UDeliveryHomeObjectComponent>();
    if (!SourceObject || !SupportObject || !SourceObject->Home || SourceObject->Home!=SupportObject->Home)
        return HomeToolError(TEXT("FurnitureMustBelongToSameHome"));
    const FTransform Orientation(Surface.GetRotation());
    const FBox FullSize=SourceObject->GetOriginalSizeBounds(Orientation);
    const FBox Footprint=SourceObject->GetPlacementFootprintBounds(Orientation);
    if (!FullSize.IsValid || !Footprint.IsValid) return HomeToolError(TEXT("MissingFurnitureBounds"));
    const FVector Offset(Footprint.GetCenter().X,Footprint.GetCenter().Y,FullSize.Min.Z);
    const FTransform Goal(Surface.GetRotation(),Surface.GetLocation()-Offset,SourceObject->GetOriginalActorScale());
    FName Reason;
    const bool bCanPlace=SourceObject->CanPlaceAtOriginalSize(Goal,Reason,SupportObject);
    auto Result=MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("can_place_original_size"),bCanPlace);
    Result->SetStringField(TEXT("reason"),Reason.ToString());
    Result->SetArrayField(TEXT("goal"),VectorJson(Goal.GetLocation()));
    Result->SetArrayField(TEXT("full_half_extent"),VectorJson(FullSize.GetExtent()));
    Result->SetArrayField(TEXT("footprint_half_extent"),VectorJson(Footprint.GetExtent()));
    return Json(Result);
}

FString UDeliveryHomeObjectAssetTools::InspectFurniture(AActor* Actor)
{
    if (!IsEditorActor(Actor)) return HomeToolError(TEXT("EditorWorldRequired_NoPIE_NoBeginPlay"));
    auto* StaticActor=Cast<AStaticMeshActor>(Actor);
    auto* Mesh=StaticActor?StaticActor->GetStaticMeshComponent():nullptr;
    if (!Mesh || !Mesh->GetStaticMesh()) return HomeToolError(TEXT("ExistingStaticMeshActorRequired"));
    auto Result=MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor"),Actor->GetPathName());
    Result->SetStringField(TEXT("label"),Actor->GetActorLabel());
    Result->SetStringField(TEXT("class"),Actor->GetClass()->GetPathName());
    Result->SetObjectField(TEXT("transform"),TransformJson(Actor->GetActorTransform()));
    Result->SetStringField(TEXT("mesh"),Mesh->GetStaticMesh()->GetPathName());
    Result->SetArrayField(TEXT("mesh_bounds_origin"),VectorJson(Mesh->GetStaticMesh()->GetBoundingBox().GetCenter()));
    Result->SetArrayField(TEXT("mesh_bounds_extent"),VectorJson(Mesh->GetStaticMesh()->GetBoundingBox().GetExtent()));
    Result->SetArrayField(TEXT("world_bounds_origin"),VectorJson(Mesh->Bounds.Origin));
    Result->SetArrayField(TEXT("world_bounds_extent"),VectorJson(Mesh->Bounds.BoxExtent));
    Result->SetBoolField(TEXT("movable"),Mesh->Mobility==EComponentMobility::Movable);
    Result->SetBoolField(TEXT("simulate_physics_configured"),Mesh->BodyInstance.bSimulatePhysics);
    Result->SetBoolField(TEXT("prop_start_awake"),Mesh->BodyInstance.bStartAwake);
    Result->SetBoolField(TEXT("mass_override_enabled"),Mesh->BodyInstance.bOverrideMass);
    Result->SetNumberField(TEXT("prop_mass_kg"),Mesh->BodyInstance.GetMassOverride());
    Result->SetBoolField(TEXT("replicates"),Actor->GetIsReplicated());
    Result->SetBoolField(TEXT("replicate_movement"),Actor->IsReplicatingMovement());
    Result->SetBoolField(TEXT("static_mesh_replicate_movement"),StaticActor->bStaticMeshReplicateMovement);
    Result->SetBoolField(TEXT("root_component_replicates"),Mesh->GetIsReplicated());
    Result->SetStringField(TEXT("collision_profile"),Mesh->GetCollisionProfileName().ToString());
    // 从引擎已加载的映射读取对象查询索引，包含项目自定义通道
    TArray<TSharedPtr<FJsonValue>> ObjectChannels;
    const auto* CollisionProfile=UCollisionProfile::Get();
    for (int32 Index=0;Index<static_cast<int32>(ObjectTypeQuery_MAX);++Index)
    {
        const ECollisionChannel Channel=CollisionProfile->ConvertToCollisionChannel(false,Index);
        if (Channel==ECC_MAX) continue;
        auto Entry=MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("query_index"),Index);
        Entry->SetNumberField(TEXT("channel"),static_cast<int32>(Channel));
        Entry->SetStringField(TEXT("channel_name"),CollisionProfile->ReturnChannelNameFromContainerIndex(static_cast<int32>(Channel)).ToString());
        ObjectChannels.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("collision_object_channels"),ObjectChannels);
    Result->SetBoolField(TEXT("query_and_physics"),Mesh->GetCollisionEnabled()==ECollisionEnabled::QueryAndPhysics);
    Result->SetBoolField(TEXT("static_mesh_is_root"),Actor->GetRootComponent()==Mesh);
    const auto* Grab=Actor->FindComponentByClass<UDeliveryGrabbableComponent>();
    Result->SetBoolField(TEXT("grabbable_uses_root"),Grab && Grab->BodyComponentName==Mesh->GetFName());
    Result->SetBoolField(TEXT("npc_edge_pickup"),Grab && Grab->bAllowNpcEdgePickup);
    const auto* BodySetup=Mesh->GetStaticMesh()->GetBodySetup();
    Result->SetNumberField(TEXT("convex_shape_count"),BodySetup?BodySetup->AggGeom.ConvexElems.Num():0);
    TArray<TSharedPtr<FJsonValue>> Materials,Overrides;
    for (auto* Material:Mesh->GetMaterials()) Materials.Add(MakeShared<FJsonValueString>(Material?Material->GetPathName():TEXT("")));
    for (const auto& Material:Mesh->OverrideMaterials) Overrides.Add(MakeShared<FJsonValueString>(Material?Material->GetPathName():TEXT("")));
    Result->SetArrayField(TEXT("materials"),Materials); Result->SetArrayField(TEXT("overrides"),Overrides);
    auto* Metadata=Actor->FindComponentByClass<UDeliveryHomeObjectComponent>();
    if (Metadata)
    {
        Result->SetBoolField(TEXT("complex_placement_support"),Metadata->bTracePlacementSupportComplex);
        Result->SetArrayField(TEXT("placement_footprint_center"),VectorJson(Metadata->PlacementFootprintCenter));
        Result->SetArrayField(TEXT("placement_footprint_half_extent"),VectorJson(FVector(Metadata->PlacementFootprintHalfExtent,0)));
        Result->SetStringField(TEXT("object_id"),Metadata->ObjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
        Result->SetStringField(TEXT("home"),Metadata->Home?Metadata->Home->GetPathName():TEXT(""));
        Result->SetStringField(TEXT("display_name"),Metadata->DisplayName.ToString());
        Result->SetStringField(TEXT("description"),Metadata->Description.ToString());
        TArray<TSharedPtr<FJsonValue>> AllowedUses;
        for (const auto Operation:Metadata->AllowedUses)
            AllowedUses.Add(MakeShared<FJsonValueString>(StaticEnum<EDeliveryHomeOperation>()->GetNameStringByValue(static_cast<int64>(Operation))));
        Result->SetArrayField(TEXT("allowed_uses"),AllowedUses);
        // 按运行时配置解析引用，不能仅找到一个同类组件就认为接线有效
        const auto* ReferencedSmart=Cast<USmartObjectComponent>(Metadata->SmartObjectReference.GetComponent(Actor));
        const bool bSmartReferenceValid=IsValid(ReferencedSmart) && ReferencedSmart->GetOwner()==Actor && ReferencedSmart->GetWorld()==Actor->GetWorld();
        Result->SetBoolField(TEXT("smart_object_reference_valid"),bSmartReferenceValid);
        Result->SetStringField(TEXT("smart_object_reference"),bSmartReferenceValid?ReferencedSmart->GetPathName():TEXT(""));
        const auto* Audio=Cast<UAudioComponent>(Metadata->AudioReference.GetComponent(Actor));
        const bool bAudioReferenceValid=IsValid(Audio) && Audio->GetOwner()==Actor && Audio->GetWorld()==Actor->GetWorld();
        Result->SetBoolField(TEXT("audio_reference_valid"),bAudioReferenceValid);
        Result->SetStringField(TEXT("audio_reference"),bAudioReferenceValid?Audio->GetPathName():TEXT(""));
        if (bAudioReferenceValid)
        {
            auto Settings=MakeShared<FJsonObject>();
            Settings->SetNumberField(TEXT("volume"),Audio->VolumeMultiplier);
            Settings->SetBoolField(TEXT("auto_activate"),Audio->bAutoActivate);
            Settings->SetBoolField(TEXT("auto_destroy"),Audio->bAutoDestroy);
            Settings->SetBoolField(TEXT("override_attenuation"),Audio->bOverrideAttenuation);
            Settings->SetBoolField(TEXT("attenuate"),Audio->AttenuationOverrides.bAttenuate);
            Settings->SetBoolField(TEXT("spatialize"),Audio->AttenuationOverrides.bSpatialize);
            Settings->SetBoolField(TEXT("sphere"),Audio->AttenuationOverrides.AttenuationShape==EAttenuationShape::Sphere);
            Settings->SetNumberField(TEXT("inner_radius"),Audio->AttenuationOverrides.AttenuationShapeExtents.X);
            Settings->SetNumberField(TEXT("falloff"),Audio->AttenuationOverrides.FalloffDistance);
            Result->SetObjectField(TEXT("audio_settings"),Settings);
        }
        Result->SetNumberField(TEXT("carry_scale_multiplier"),Metadata->CarryScaleMultiplier);
        Result->SetBoolField(TEXT("restore_scale_from_support_bottom"),Metadata->bRestoreScaleFromSupportBottom);
        Result->SetBoolField(TEXT("carry_scale_active"),Metadata->IsCarryScaleActive());
        Result->SetStringField(TEXT("carry_scale_phase"),StaticEnum<EDeliveryHomeCarryScalePhase>()->GetNameStringByValue(
            static_cast<int64>(Metadata->GetCarryScaleState().Phase)));
        Result->SetArrayField(TEXT("original_actor_scale"),VectorJson(Metadata->GetOriginalActorScale()));
        Result->SetBoolField(TEXT("carry_scale_replication_ready"),Actor->GetIsReplicated() &&
            Actor->IsReplicatingMovement() && Mesh->GetIsReplicated());
    }
    if (const auto* Smart=Actor->FindComponentByClass<USmartObjectComponent>())
        if (const auto* Definition=Smart->GetDefinition())
        {
            Result->SetStringField(TEXT("definition"),Definition->GetPathName());
            Result->SetStringField(TEXT("smart_object_component"),Smart->GetPathName());
            TArray<TSharedPtr<FJsonValue>> Slots;
            for (int32 Index=0;Index<Definition->GetSlots().Num();++Index)
            {
                const auto* Use=Cast<UDeliveryHomeBehaviorDefinition>(Definition->GetBehaviorDefinition(Index,UDeliveryHomeBehaviorDefinition::StaticClass()));
                if (!Use) continue;
                auto Entry=MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("slot_id"),Use->SlotId.ToString());
                Entry->SetStringField(TEXT("use_id"),Use->UseId.ToString());
                Entry->SetStringField(TEXT("operation"),StaticEnum<EDeliveryHomeOperation>()->GetNameStringByValue(static_cast<int64>(Use->Operation)));
                const auto& Slot=Definition->GetSlot(Index);
                Entry->SetObjectField(TEXT("slot"),TransformJson(FTransform(FRotator(Slot.Rotation),FVector(Slot.Offset))));
                Entry->SetObjectField(TEXT("entrance"),TransformJson(Use->EntranceOffset));
                Entry->SetObjectField(TEXT("use"),TransformJson(Use->UseOffset));
                Entry->SetStringField(TEXT("use_transform_semantics"),Use->Operation==EDeliveryHomeOperation::PlaceProp?
                    TEXT("support_surface_center"):Use->Operation==EDeliveryHomeOperation::Sit?
                    TEXT("character_foot_origin"):TEXT("interaction_point"));
                Entry->SetObjectField(TEXT("exit"),TransformJson(Use->ExitOffset));
                Entry->SetArrayField(TEXT("sight_offset"),VectorJson(Use->SightOffset));
                Entry->SetStringField(TEXT("enter_animation"),Use->EnterAnimation?Use->EnterAnimation->GetPathName():TEXT(""));
                Entry->SetStringField(TEXT("animation"),Use->Animation?Use->Animation->GetPathName():TEXT(""));
                Entry->SetStringField(TEXT("exit_animation"),Use->ExitAnimation?Use->ExitAnimation->GetPathName():TEXT(""));
                Entry->SetStringField(TEXT("seat_mount_animation"),Use->SeatMountAnimation?Use->SeatMountAnimation->GetPathName():TEXT(""));
                Entry->SetStringField(TEXT("seat_dismount_animation"),Use->SeatDismountAnimation?Use->SeatDismountAnimation->GetPathName():TEXT(""));
                Entry->SetStringField(TEXT("sound"),Use->Sound?Use->Sound->GetPathName():TEXT(""));
                Entry->SetNumberField(TEXT("approach_radius"),Use->ApproachRadius);
                Entry->SetNumberField(TEXT("use_radius"),Use->UseRadius);
                Entry->SetNumberField(TEXT("use_seconds"),Use->UseSeconds);
                Entry->SetArrayField(TEXT("maximum_prop_half_extent"),VectorJson(Use->MaximumPropHalfExtent));
                Entry->SetNumberField(TEXT("placement_tolerance"),Use->PlacementTolerance);
                Entry->SetNumberField(TEXT("placement_angle_tolerance"),Use->PlacementAngleTolerance);
                Entry->SetNumberField(TEXT("maximum_support_gap"),Use->MaximumSupportGap);
                Entry->SetBoolField(TEXT("use_prop_transfer_point"),Use->bUsePropTransferPoint);
                Entry->SetArrayField(TEXT("prop_transfer_offset"),VectorJson(Use->PropTransferOffset));
                Entry->SetBoolField(TEXT("configuration_valid"),Use->IsUsableConfiguration());
                Slots.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Result->SetArrayField(TEXT("slots"),Slots);
        }
    return Json(Result);
}

FString UDeliveryHomeObjectAssetTools::BuildFurnitureAssets(AActor* SourceActor,const FString& Configuration,bool bReplaceOwnedAssets)
{
    if (!IsEditorActor(SourceActor)) return HomeToolError(TEXT("EditorWorldRequired_NoPIE_NoBeginPlay"));
    auto* Source=Cast<AStaticMeshActor>(SourceActor);
    auto* SourceMesh=Source?Source->GetStaticMeshComponent():nullptr;
    if (!SourceMesh || !SourceMesh->GetStaticMesh()) return HomeToolError(TEXT("ExistingStaticMeshActorRequired"));
    TSharedPtr<FJsonObject> Config;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Configuration),Config) || !Config) return HomeToolError(TEXT("InvalidJSON"));
    FString BlueprintPath,DefinitionPath,CollisionPath,Name,Description;
    bool bProp=false;
    double CarryScaleMultiplier=.35;
    double PropMassKg=0;
    bool bRestoreScaleFromSupportBottom=true;
    bool bPropStartAwake=true;
    if (!Config->TryGetStringField(TEXT("blueprint_path"),BlueprintPath) || !IsAssetPath(BlueprintPath) ||
        !Config->TryGetStringField(TEXT("display_name"),Name) || Name.IsEmpty() ||
        !Config->TryGetStringField(TEXT("description"),Description) || !Config->TryGetBoolField(TEXT("physical_prop"),bProp))
        return HomeToolError(TEXT("InvalidBlueprintIdentity"));
    if (bProp && (!Config->TryGetNumberField(TEXT("prop_mass_kg"),PropMassKg) ||
        !FMath::IsFinite(PropMassKg) || PropMassKg<=0 ||
        !FMath::IsFinite(static_cast<float>(PropMassKg)) || static_cast<float>(PropMassKg)<=0))
        return HomeToolError(TEXT("InvalidPropMassKg_ExplicitFinitePositiveValueRequired"));
    if (Config->HasField(TEXT("carry_scale_multiplier")) &&
        !Config->TryGetNumberField(TEXT("carry_scale_multiplier"),CarryScaleMultiplier))
        return HomeToolError(TEXT("InvalidCarryScaleMultiplier"));
    if (!FMath::IsFinite(CarryScaleMultiplier) || CarryScaleMultiplier<.1 || CarryScaleMultiplier>1)
        return HomeToolError(TEXT("InvalidCarryScaleMultiplier"));
    if (Config->HasField(TEXT("restore_scale_from_support_bottom")) &&
        !Config->TryGetBoolField(TEXT("restore_scale_from_support_bottom"),bRestoreScaleFromSupportBottom))
        return HomeToolError(TEXT("InvalidRestoreScaleFromSupportBottom"));
    if (Config->HasField(TEXT("prop_start_awake")) && !Config->TryGetBoolField(TEXT("prop_start_awake"),bPropStartAwake))
        return HomeToolError(TEXT("InvalidPropStartAwake"));
    TArray<FPreparedSlot> Slots;
    FString Why;
    if (!PrepareSlots(Config,Slots,Why)) return HomeToolError(Why);
    if (!Slots.IsEmpty() && (!Config->TryGetStringField(TEXT("definition_path"),DefinitionPath) || !IsAssetPath(DefinitionPath)))
        return HomeToolError(TEXT("DefinitionPathRequired"));
    if (bProp && (!Config->TryGetStringField(TEXT("collision_mesh_path"),CollisionPath) || !IsAssetPath(CollisionPath)))
        return HomeToolError(TEXT("IndependentPropCollisionMeshRequired"));
    for (const FString& Path:{BlueprintPath,DefinitionPath,CollisionPath})
        if (!Path.IsEmpty())
            if (auto* Existing=LoadObject<UObject>(nullptr,*Path); Existing && (!IsOwnedAsset(Existing) || !bReplaceOwnedAssets))
                return HomeToolError(TEXT("ExistingAssetKept_UnownedOrReplaceNotExplicit: ")+Path);
    auto* Blueprint=LoadObject<UBlueprint>(nullptr,*BlueprintPath);
    auto* Definition=DefinitionPath.IsEmpty()?nullptr:LoadObject<USmartObjectDefinition>(nullptr,*DefinitionPath);
    auto* Collision=CollisionPath.IsEmpty()?nullptr:LoadObject<UStaticMesh>(nullptr,*CollisionPath);
    if (Blueprint && Blueprint->ParentClass!=AStaticMeshActor::StaticClass()) return HomeToolError(TEXT("BlueprintParentChanged"));
    const FString SourceMeshPath=SourceMesh->GetStaticMesh()->GetPathName();
    bool bMeshHull=false,bMeshFootprint=false,bComplexSupport=false,bEdgePickup=false;
    if ((Config->HasField(TEXT("prop_mesh_hull")) && !Config->TryGetBoolField(TEXT("prop_mesh_hull"),bMeshHull)) ||
        (Config->HasField(TEXT("prop_mesh_footprint")) && !Config->TryGetBoolField(TEXT("prop_mesh_footprint"),bMeshFootprint)) ||
        (Config->HasField(TEXT("complex_placement_support")) && !Config->TryGetBoolField(TEXT("complex_placement_support"),bComplexSupport)) ||
        (Config->HasField(TEXT("npc_edge_pickup")) && !Config->TryGetBoolField(TEXT("npc_edge_pickup"),bEdgePickup)) ||
        ((bMeshHull || bEdgePickup) && !bProp) || (bMeshFootprint && !bMeshHull)) return HomeToolError(TEXT("InvalidPropContactConfiguration"));
    TArray<FVector> HullVertices;
    FBox ContactFootprint(ForceInit);
    if (bMeshHull && !ReadMeshContactGeometry(*SourceMesh->GetStaticMesh(),HullVertices,ContactFootprint))
        return HomeToolError(TEXT("FurnitureMeshContactGeometryUnavailable: ")+SourceMeshPath);
    TArray<TSharedPtr<FJsonValue>> Assets;
    if (bProp)
    {
        if (!Collision)
        {
            Collision=Cast<UStaticMesh>(StaticDuplicateObject(SourceMesh->GetStaticMesh(),CreatePackage(*CollisionPath),*FPackageName::GetLongPackageAssetName(CollisionPath)));
            if (!Collision) return HomeToolError(TEXT("CollisionMeshDuplicateFailed"));
            Collision->SetFlags(RF_Public|RF_Standalone); FAssetRegistryModule::AssetCreated(Collision);
        }
        Collision->CreateBodySetup();
        auto* Body=Collision->GetBodySetup();
        if (!Body || Body->GetOuter()!=Collision) return HomeToolError(TEXT("IndependentBodySetupRequired"));
        Body->Modify(); Body->AggGeom.EmptyElements();
        const FBox Bounds=Collision->GetBoundingBox();
        if (!Bounds.IsValid || Bounds.GetExtent().GetMin()<=0) return HomeToolError(TEXT("InvalidPropBounds"));
        if (bMeshHull)
        {
            FKConvexElem Hull;
            Hull.VertexData=MoveTemp(HullVertices);
            Hull.UpdateElemBox();
            Body->AggGeom.ConvexElems.Add(MoveTemp(Hull));
        }
        else
        {
            FKBoxElem Box; Box.Center=Bounds.GetCenter();
            const FVector Size=Bounds.GetSize(); Box.X=Size.X; Box.Y=Size.Y; Box.Z=Size.Z;
            Body->AggGeom.BoxElems.Add(Box);
        }
        Body->CollisionTraceFlag=CTF_UseSimpleAndComplex;
        Body->InvalidatePhysicsData(); Body->CreatePhysicsMeshes();
        MarkOwnedAsset(Collision,SourceMeshPath); Assets.Add(MakeShared<FJsonValueString>(CollisionPath));
    }
    if (!Slots.IsEmpty())
    {
        if (!Definition) Definition=NewAsset<USmartObjectDefinition>(DefinitionPath);
        TMap<FName,FGuid> ExistingIds;
        for (const auto& Slot:Definition->GetSlots()) ExistingIds.Add(Slot.Name,Slot.ID);
        auto* Property=FindFProperty<FArrayProperty>(USmartObjectDefinition::StaticClass(),TEXT("Slots"));
        if (!Property) return HomeToolError(TEXT("EngineSmartObjectSlotsPropertyMissing"));
        Definition->Modify();
        FScriptArrayHelper Array(Property,Property->ContainerPtrToValuePtr<void>(Definition));
        Array.EmptyValues(); Array.AddValues(Slots.Num());
        for (int32 Index=0;Index<Slots.Num();++Index)
        {
            auto& Slot=*reinterpret_cast<FSmartObjectSlotDefinition*>(Array.GetRawPtr(Index));
            const auto& Prepared=Slots[Index];
            Slot.Name=Prepared.Behavior->SlotId;
            const auto* PreviousId=ExistingIds.Find(Slot.Name);
            Slot.ID=PreviousId && PreviousId->IsValid()?*PreviousId:FGuid::NewGuid();
            Slot.Offset=FVector3f(Prepared.SlotTransform.GetLocation());
            Slot.Rotation=FRotator3f(Prepared.SlotTransform.Rotator());
            Slot.BehaviorDefinitions.Add(DuplicateObject<UDeliveryHomeBehaviorDefinition>(Prepared.Behavior.Get(),Definition));
        }
        if (!Definition->Validate()) return HomeToolError(TEXT("SmartObjectDefinitionValidationFailed"));
        MarkOwnedAsset(Definition,SourceMeshPath); Assets.Add(MakeShared<FJsonValueString>(DefinitionPath));
    }
    if (!Blueprint)
    {
        Blueprint=FKismetEditorUtilities::CreateBlueprint(AStaticMeshActor::StaticClass(),CreatePackage(*BlueprintPath),
            *FPackageName::GetLongPackageAssetName(BlueprintPath),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
        if (!Blueprint) return HomeToolError(TEXT("BlueprintCreateFailed"));
        FAssetRegistryModule::AssetCreated(Blueprint);
    }
    auto* Defaults=Blueprint->GeneratedClass?Blueprint->GeneratedClass->GetDefaultObject<AStaticMeshActor>():nullptr;
    if (!Defaults || !Blueprint->SimpleConstructionScript) return HomeToolError(TEXT("BlueprintDefaultsMissing"));
    auto* Root=Defaults->GetStaticMeshComponent();
    Blueprint->Modify(); Root->Modify();
    CopyMeshSettings(SourceMesh,Root,bProp?Collision:SourceMesh->GetStaticMesh().Get(),bProp,static_cast<float>(PropMassKg));
    if (bProp) Root->BodyInstance.bStartAwake=bPropStartAwake;
    Root->SetRelativeTransform(FTransform::Identity);
    Defaults->SetReplicates(true); Defaults->SetReplicateMovement(bProp);
    Defaults->bStaticMeshReplicateMovement=bProp;
    auto* Metadata=EnsureComponent<UDeliveryHomeObjectComponent>(Blueprint,TEXT("HomeObject"));
    if (!Metadata) return HomeToolError(TEXT("HomeObjectComponentClassChanged"));
    Metadata->DisplayName=FText::FromString(Name); Metadata->Description=FText::FromString(Description);
    Metadata->CarryScaleMultiplier=static_cast<float>(CarryScaleMultiplier);
    Metadata->bRestoreScaleFromSupportBottom=bRestoreScaleFromSupportBottom;
    Metadata->bTracePlacementSupportComplex=bComplexSupport;
    Metadata->PlacementFootprintCenter=bMeshFootprint?ContactFootprint.GetCenter():FVector::ZeroVector;
    Metadata->PlacementFootprintHalfExtent=bMeshFootprint?FVector2D(ContactFootprint.GetExtent()):FVector2D::ZeroVector;
    Metadata->ObjectId.Invalidate(); Metadata->Home=nullptr; Metadata->AllowedUses.Reset();
    for (const auto& Slot:Slots) Metadata->AllowedUses.AddUnique(Slot.Behavior->Operation);
    if (Definition)
    {
        auto* Smart=EnsureComponent<USmartObjectComponent>(Blueprint,TEXT("HomeSmartObject"),Root);
        if (!Smart) return HomeToolError(TEXT("SmartObjectComponentClassChanged"));
        Smart->SetDefinition(Definition); Smart->SetRelativeTransform(FTransform::Identity);
        Metadata->SmartObjectReference.ComponentProperty=TEXT("HomeSmartObject");
    }
    const bool bAudio=Metadata->AllowedUses.Contains(EDeliveryHomeOperation::PlayAudio) || Metadata->AllowedUses.Contains(EDeliveryHomeOperation::StopAudio);
    if (bAudio)
    {
        auto* Audio=EnsureComponent<UAudioComponent>(Blueprint,TEXT("HomeAudio"),Root);
        if (!Audio) return HomeToolError(TEXT("AudioComponentClassChanged"));
        Audio->bAutoActivate=false; Audio->bAutoDestroy=false;
        double Volume=.45;
        Config->TryGetNumberField(TEXT("audio_volume"),Volume);
        if (!FMath::IsFinite(Volume) || Volume<0 || Volume>1) return HomeToolError(TEXT("InvalidAudioVolume"));
        Audio->SetVolumeMultiplier(Volume);
        double Radius=200,Falloff=1200;
        Config->TryGetNumberField(TEXT("audio_inner_radius"),Radius);
        Config->TryGetNumberField(TEXT("audio_falloff"),Falloff);
        if (!FMath::IsFinite(Radius) || !FMath::IsFinite(Falloff) || Radius<=0 || Falloff<=0)
            return HomeToolError(TEXT("InvalidAudioAttenuation"));
        Audio->bOverrideAttenuation=true;
        Audio->AttenuationOverrides.bAttenuate=true; Audio->AttenuationOverrides.bSpatialize=true;
        Audio->AttenuationOverrides.AttenuationShape=EAttenuationShape::Sphere;
        Audio->AttenuationOverrides.AttenuationShapeExtents=FVector(Radius,0,0);
        Audio->AttenuationOverrides.FalloffDistance=Falloff;
        Metadata->AudioReference.ComponentProperty=TEXT("HomeAudio");
    }
    if (bProp)
    {
        auto* Grab=EnsureComponent<UDeliveryGrabbableComponent>(Blueprint,TEXT("HomeGrabbable"));
        if (!Grab) return HomeToolError(TEXT("GrabbableComponentClassChanged"));
        Grab->BodyComponentName=Root->GetFName();
        Grab->bAllowNpcEdgePickup=bEdgePickup;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status==BS_Error || !Blueprint->GeneratedClass) return HomeToolError(TEXT("BlueprintCompileFailed_NoMapChange"));
    MarkOwnedAsset(Blueprint,SourceMeshPath); Assets.Add(MakeShared<FJsonValueString>(BlueprintPath));
    auto Result=MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("assets_to_save"),Assets);
    Result->SetBoolField(TEXT("compiled"),true); Result->SetBoolField(TEXT("saved"),false);
    Result->SetStringField(TEXT("blueprint"),BlueprintPath);
    return Json(Result);
}

AActor* UDeliveryHomeObjectAssetTools::InstallFurniture(AActor* SourceActor,UBlueprint* Blueprint,ADeliveryHome* Home,FGuid ObjectId,
    bool bRefreshExistingPortableProp)
{
    if (!IsEditorActor(SourceActor) || !IsEditorActor(Home) || SourceActor->GetWorld()!=Home->GetWorld() || !ObjectId.IsValid() ||
        !Blueprint || !IsOwnedAsset(Blueprint) || Blueprint->Status==BS_Error || !Blueprint->GeneratedClass ||
        !Blueprint->GeneratedClass->IsChildOf(AStaticMeshActor::StaticClass())) return nullptr;
    auto* Source=Cast<AStaticMeshActor>(SourceActor);
    if (!Source || !Source->GetStaticMeshComponent()->GetStaticMesh()) return nullptr;
    const FTransform OriginalTransform=Source->GetActorTransform();
    auto* Existing=Source->FindComponentByClass<UDeliveryHomeObjectComponent>();
    const auto* Defaults=Blueprint->GeneratedClass->GetDefaultObject<AStaticMeshActor>();
    auto* Template=Defaults->GetStaticMeshComponent();
    if (Existing)
    {
        if (Source->GetClass()!=Blueprint->GeneratedClass || Existing->Home!=Home || Existing->ObjectId!=ObjectId) return nullptr;
        if (!bRefreshExistingPortableProp) return Source;
        if (!Blueprint->SimpleConstructionScript) return nullptr;
        auto* Root=Source->GetStaticMeshComponent();
        auto* Grab=Source->FindComponentByClass<UDeliveryGrabbableComponent>();
        auto* Smart=Source->FindComponentByClass<USmartObjectComponent>();
        const UDeliveryHomeObjectComponent* MetadataTemplate=nullptr;
        const USmartObjectComponent* SmartTemplate=nullptr;
        const UDeliveryGrabbableComponent* GrabTemplate=nullptr;
        for (const auto* Node:Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node->GetVariableName()==TEXT("HomeObject")) MetadataTemplate=Cast<UDeliveryHomeObjectComponent>(Node->ComponentTemplate);
            if (Node->GetVariableName()==TEXT("HomeSmartObject")) SmartTemplate=Cast<USmartObjectComponent>(Node->ComponentTemplate);
            if (Node->GetVariableName()==TEXT("HomeGrabbable")) GrabTemplate=Cast<UDeliveryGrabbableComponent>(Node->ComponentTemplate);
        }
        if (!Root || Root!=Source->GetRootComponent() || !Grab || !Smart || !MetadataTemplate || !SmartTemplate || !GrabTemplate ||
            !Template->BodyInstance.bSimulatePhysics || !MetadataTemplate->AllowedUses.Contains(EDeliveryHomeOperation::TakeProp)) return nullptr;
        // 组件模板只提供只读定义，关卡实例需要同一份可编辑资产
        auto* Definition=LoadObject<USmartObjectDefinition>(nullptr,*GetPathNameSafe(SmartTemplate->GetDefinition()));
        if (!Definition) return nullptr;
        Source->Modify(); Root->Modify(); Existing->Modify(); Smart->Modify(); Grab->Modify();
        Root->SetStaticMesh(Template->GetStaticMesh());
        Root->SetMobility(EComponentMobility::Movable);
        Root->SetCollisionProfileName(Template->GetCollisionProfileName());
        Root->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Root->BodyInstance.bStartAwake=Template->BodyInstance.bStartAwake;
        Root->BodyInstance.bSimulatePhysics=true;
        Root->BodyInstance.SetMassOverride(Template->BodyInstance.GetMassOverride(),Template->BodyInstance.bOverrideMass);
        Root->SetCanEverAffectNavigation(false); Root->SetIsReplicated(true);
        Source->bStaticMeshReplicateMovement=true;
        Source->SetReplicates(true); Source->SetReplicateMovement(true);
        Grab->BodyComponentName=Root->GetFName();
        Grab->bAllowNpcEdgePickup=GrabTemplate->bAllowNpcEdgePickup;
        Existing->AllowedUses=MetadataTemplate->AllowedUses;
        Existing->SmartObjectReference=MetadataTemplate->SmartObjectReference;
        Existing->CarryScaleMultiplier=MetadataTemplate->CarryScaleMultiplier;
        Existing->bRestoreScaleFromSupportBottom=MetadataTemplate->bRestoreScaleFromSupportBottom;
        Existing->bTracePlacementSupportComplex=MetadataTemplate->bTracePlacementSupportComplex;
        Existing->PlacementFootprintCenter=MetadataTemplate->PlacementFootprintCenter;
        Existing->PlacementFootprintHalfExtent=MetadataTemplate->PlacementFootprintHalfExtent;
        Smart->SetDefinition(Definition);
        Source->MarkPackageDirty();
        return Source->GetActorTransform().Equals(OriginalTransform,.001)?Source:nullptr;
    }
    if (Source->GetClass()!=AStaticMeshActor::StaticClass()) return nullptr;
    auto* Factory=NewObject<UActorFactoryBlueprint>();
    TArray<AActor*> NewActors;
    // UE 标准替换保留 Actor 名字、GUID、层级及指向旧家具的关卡引用
    UEditorActorSubsystem::ReplaceActors(Factory,FAssetData(Blueprint),{Source},&NewActors,true);
    if (NewActors.Num()!=1) return nullptr;
    auto* NewActor=Cast<AStaticMeshActor>(NewActors[0]);
    auto* Metadata=NewActor?NewActor->FindComponentByClass<UDeliveryHomeObjectComponent>():nullptr;
    if (!Metadata || !NewActor->GetActorTransform().Equals(OriginalTransform,.001)) return nullptr;
    auto* Root=NewActor->GetStaticMeshComponent();
    // 标准替换会复制旧 StaticMeshComponent，独立枕头只覆盖明确新增的物理配置
    const bool bProp=NewActor->FindComponentByClass<UDeliveryGrabbableComponent>()!=nullptr;
    NewActor->bStaticMeshReplicateMovement=bProp;
    if (bProp)
    {
        // 替换组件后恢复蓝图明确配置的质量，不更新编辑器中的 live 刚体
        Root->BodyInstance.SetMassOverride(Template->BodyInstance.GetMassOverride(),Template->BodyInstance.bOverrideMass);
        Root->SetStaticMesh(Template->GetStaticMesh());
        Root->SetMobility(EComponentMobility::Movable);
        Root->SetCollisionProfileName(Template->GetCollisionProfileName());
        Root->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Root->SetCanEverAffectNavigation(false); Root->BodyInstance.bSimulatePhysics=true;
        Root->BodyInstance.bStartAwake=Template->BodyInstance.bStartAwake;
        Root->SetIsReplicated(true); NewActor->SetReplicateMovement(true);
    }
    NewActor->SetReplicates(true);
    Metadata->Modify(); Metadata->Home=Home; Metadata->ObjectId=ObjectId;
    NewActor->MarkPackageDirty();
    return NewActor;
}

bool UDeliveryHomeObjectAssetTools::FinishPendingEditorAssets()
{
    if (!GEditor || GEditor->PlayWorld) return false;
    UWorld* World=GEditor->GetEditorWorldContext().World();
    if (!World || World->HasBegunPlay() || World->WorldType!=EWorldType::Editor) return false;
    FlushAsyncLoading();
    FAssetCompilingManager::Get().FinishAllCompilation();
    return !IsAsyncLoading() && FAssetCompilingManager::Get().GetNumRemainingAssets()==0;
}

FString UDeliveryHomeObjectAssetTools::InspectInteriorAnchor(ADeliveryHome* Home,FVector ProposedWorldLocation)
{
    if (!IsEditorActor(Home) || !Home->ReturnAnchor || !Home->InteriorAnchor || !Home->DoorwayLink || ProposedWorldLocation.ContainsNaN())
        return HomeToolError(TEXT("EditorHomeAndFiniteAnchorRequired"));
    if (IsAsyncLoading() || FAssetCompilingManager::Get().GetNumRemainingAssets()!=0)
        return HomeToolError(TEXT("AssetsNotReady_RetryAfterCompilation_NoClearanceClaim"));
    ADeliveryCompanionCharacter* Npc=nullptr;
    for (TActorIterator<ADeliveryCompanionCharacter> It(Home->GetWorld());It;++It)
    {
        if (Npc) return HomeToolError(TEXT("CompanionNotUnique"));
        Npc=*It;
    }
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(Home->GetWorld());
    if (!Npc || !Nav || !Npc->GetCapsuleComponent()) return HomeToolError(TEXT("CompanionOrNavigationMissing"));
    FNavLocation Outside,Inside;
    if (!Nav->ProjectPointToNavigation(Home->ReturnAnchor->GetComponentLocation(),Outside,FVector(100,100,150),&Npc->GetNavAgentPropertiesRef()) ||
        !Nav->ProjectPointToNavigation(ProposedWorldLocation,Inside,FVector(60,60,100),&Npc->GetNavAgentPropertiesRef()))
        return HomeToolError(TEXT("InteriorOrReturnAnchorNotOnNavigation"));
    auto Result=MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("configured"),VectorJson(Home->InteriorAnchor->GetComponentLocation()));
    Result->SetArrayField(TEXT("proposed"),VectorJson(ProposedWorldLocation));
    Result->SetArrayField(TEXT("projected"),VectorJson(Inside.Location));
    FCollisionQueryParams Params(SCENE_QUERY_STAT(HomeInteriorAnchor),false,Npc);
    FHitResult Floor;
    const bool bFloor=Home->GetWorld()->LineTraceSingleByChannel(Floor,Inside.Location+FVector(0,0,50),
        Inside.Location-FVector(0,0,100),ECC_Visibility,Params) && Floor.ImpactNormal.Z>=.7f && Floor.GetActor()==Home->ExistingHouse;
    const float HalfHeight=Npc->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const float Radius=Npc->GetCapsuleComponent()->GetScaledCapsuleRadius();
    const bool bClear=bFloor && !Home->GetWorld()->OverlapBlockingTestByChannel(
        Floor.ImpactPoint+FVector(0,0,HalfHeight+1),FQuat::Identity,ECC_Pawn,
        FCollisionShape::MakeCapsule(Radius,HalfHeight),Params);
    Result->SetBoolField(TEXT("floor_found"),bFloor);
    Result->SetBoolField(TEXT("capsule_clear"),bClear);
    Result->SetArrayField(TEXT("floor"),VectorJson(Floor.ImpactPoint));
    Result->SetStringField(TEXT("floor_actor"),Floor.GetActor()?Floor.GetActor()->GetPathName():TEXT(""));
    const bool bPrevious=Home->DoorwayLink->IsEnabled();
    ON_SCOPE_EXIT { Home->DoorwayLink->SetEnabled(bPrevious); };
    bool bDoorwayComplete=false;
    for (const bool bOpen:{false,true})
    {
        Home->DoorwayLink->SetEnabled(bOpen);
        auto* Path=Nav->FindPathToLocationSynchronously(Home->GetWorld(),Outside.Location,Inside.Location,Npc);
        auto Entry=MakeShared<FJsonObject>();
        const bool bComplete=Path && Path->IsValid() && !Path->IsPartial();
        const bool bUsesDoor=Path && Path->GetPath().IsValid() && Path->GetPath()->ContainsCustomLink(Home->DoorwayLink->GetId());
        Entry->SetBoolField(TEXT("complete"),bComplete); Entry->SetBoolField(TEXT("uses_doorway"),bUsesDoor);
        Entry->SetNumberField(TEXT("length_cm"),Path && Path->IsValid()?Path->GetPathLength():0);
        Result->SetObjectField(bOpen?TEXT("doorway_open_preview"):TEXT("doorway_closed_preview"),Entry);
        if (bOpen) bDoorwayComplete=bComplete && bUsesDoor;
    }
    Home->DoorwayLink->SetEnabled(bPrevious);
    Result->SetBoolField(TEXT("doorway_state_restored"),Home->DoorwayLink->IsEnabled()==bPrevious);
    Result->SetBoolField(TEXT("valid"),bClear && bDoorwayComplete && FVector::Dist(ProposedWorldLocation,Inside.Location)<=50 &&
        FMath::Abs(Inside.Location.Z-Floor.ImpactPoint.Z)<=15);
    Result->SetStringField(TEXT("limitation"),TEXT("Static floor/capsule and pedestrian path only; no door breaking, BeginPlay or furniture-use acceptance"));
    return Json(Result);
}
