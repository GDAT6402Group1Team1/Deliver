#include "DeliveryCompanionAnimationAssetTools.h"
#include "AI/Companion/Core/DeliveryCompanionBodyAnimInstance.h"
#include "AI/Companion/Core/DeliveryCompanionCharacter.h"
#include "AI/Companion/Core/DeliveryCompanionComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_BlendBoneByChannel.h"
#include "AnimPose.h"
#include "PreviewScene.h"
#include "Misc/ScopeExit.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Editor.h"

namespace
{
const TCHAR* CompanionPath=TEXT("/Game/NPC/Companion/BP_Companion_Ergou");
const TCHAR* AnimationFolder=TEXT("/Game/NPC/Companion/Animation");
const TCHAR* AnimationName=TEXT("ABP_CompanionBody");
const TCHAR* Marker=TEXT("DeliveryCompanionHomeBodyGraph");
const TCHAR* SourceKey=TEXT("DeliveryCompanionHomeBodySource");
const TCHAR* BasePoseKey=TEXT("DeliveryCompanionHomeBasePose");
const TCHAR* CarryAssetKey=TEXT("DeliveryCompanionHomeCarry");
const TCHAR* Version=TEXT("2");
const FName CarryBone(TEXT("Spine"));
const FName PelvisBone(TEXT("Hips"));

FString AnimationPath() { return FString(AnimationFolder)/AnimationName; }

FString Result(bool bSuccess,const FString& Reason,UAnimBlueprint* Blueprint=nullptr,bool bBound=false,
    const TSharedPtr<FJsonObject>& Validation=nullptr)
{
    auto Json=MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("success"),bSuccess);
    Json->SetStringField(TEXT("reason"),Reason);
    Json->SetStringField(TEXT("animation_blueprint"),AnimationPath());
    Json->SetBoolField(TEXT("bound_companion_defaults"),bBound);
    Json->SetBoolField(TEXT("map_saved"),false);
    if (Validation) Json->SetObjectField(TEXT("preview_validation"),Validation);
    if (Blueprint)
    {
        Json->SetStringField(TEXT("source_blueprint"),Blueprint->GetPackage()->GetMetaData().GetValue(Blueprint,SourceKey));
        Json->SetStringField(TEXT("skeleton"),GetPathNameSafe(Blueprint->TargetSkeleton));
        Json->SetStringField(TEXT("graph_version"),Blueprint->GetPackage()->GetMetaData().GetValue(Blueprint,Marker));
        Json->SetStringField(TEXT("upper_body_bone"),CarryBone.ToString());
        Json->SetNumberField(TEXT("upper_body_depth"),1);
        Json->SetStringField(TEXT("carry_animation"),Blueprint->GetPackage()->GetMetaData().GetValue(Blueprint,CarryAssetKey));
    }
    FString Text;
    FJsonSerializer::Serialize(Json,TJsonWriterFactory<>::Create(&Text));
    return Text;
}

UEdGraph* BodyGraph(UAnimBlueprint* Blueprint)
{
    UEdGraph* Found=nullptr;
    if (!Blueprint) return nullptr;
    for (UEdGraph* Graph:Blueprint->FunctionGraphs)
        if (Graph && Graph->GetFName()==TEXT("AnimGraph"))
        { if (Found) return nullptr; Found=Graph; }
    return Found;
}

UEdGraphPin* PosePin(UEdGraphNode* Node,EEdGraphPinDirection Direction)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin:Node->Pins)
        if (Pin && Pin->Direction==Direction && Pin->PinType.PinSubCategoryObject.IsValid() &&
            Pin->PinType.PinSubCategoryObject->GetName().Contains(TEXT("PoseLink"))) return Pin;
    return nullptr;
}

UEdGraphNode* LinkedNode(UEdGraphNode* Node,FName Input)
{
    const auto* Pin=Node?Node->FindPin(Input,EGPD_Input):nullptr;
    return Pin && Pin->LinkedTo.Num()==1?Pin->LinkedTo[0]->GetOwningNode():nullptr;
}

template<typename T> T* Role(UEdGraph* Graph,const TCHAR* Name)
{
    T* Found=nullptr;
    const FString Comment=FString(Marker)+TEXT(":")+Name;
    if (Graph) for (UEdGraphNode* Node:Graph->Nodes)
        if (Node && Node->NodeComment==Comment)
        { if (Found) return nullptr; Found=Cast<T>(Node); if (!Found) return nullptr; }
    return Found;
}

template<typename T> T* Add(UEdGraph* Graph,const TCHAR* Name,int32 X,int32 Y)
{
    auto* Node=NewObject<T>(Graph,NAME_None,RF_Transactional);
    Graph->AddNode(Node,false,false); Node->CreateNewGuid();
    Node->NodePosX=X; Node->NodePosY=Y;
    Node->NodeComment=FString(Marker)+TEXT(":")+Name;
    Node->PostPlacedNewNode(); Node->AllocateDefaultPins();
    return Node;
}

bool Connect(UEdGraph* Graph,UEdGraphNode* From,UEdGraphNode* To,FName Input)
{
    auto* Out=PosePin(From,EGPD_Output);
    auto* In=To?To->FindPin(Input,EGPD_Input):nullptr;
    return Out && In && Graph->GetSchema()->TryCreateConnection(Out,In);
}

bool Parameter(UEdGraph* Graph,UAnimGraphNode_Base* Node,FName PinName,FName Variable,const TCHAR* Name,int32 Y)
{
    for (int32 Index=0;Index<Node->ShowPinForProperties.Num();++Index)
        if (Node->ShowPinForProperties[Index].PropertyName==PinName)
        { Node->SetPinVisibility(true,Index); break; }
    auto* Pin=Node->FindPin(PinName,EGPD_Input);
    if (!Pin) return false;
    auto* Get=NewObject<UK2Node_VariableGet>(Graph,NAME_None,RF_Transactional);
    Get->VariableReference.SetSelfMember(Variable);
    Graph->AddNode(Get,false,false); Get->CreateNewGuid();
    Get->NodePosX=Node->NodePosX-220; Get->NodePosY=Y;
    Get->NodeComment=FString(Marker)+TEXT(":")+Name;
    Get->AllocateDefaultPins();
    return Get->GetValuePin() && Graph->GetSchema()->TryCreateConnection(Get->GetValuePin(),Pin);
}

bool ParameterMatches(UEdGraphNode* Node,FName Pin,FName Variable)
{
    const auto* Get=Cast<UK2Node_VariableGet>(LinkedNode(Node,Pin));
    return Get && Get->VariableReference.GetMemberName()==Variable;
}

bool HideConstantPin(UAnimGraphNode_Base* Node,FName Property)
{
    for (int32 Index=0;Index<Node->ShowPinForProperties.Num();++Index)
        if (Node->ShowPinForProperties[Index].PropertyName==Property)
        { Node->SetPinVisibility(false,Index); break; }
    return !Node->FindPin(Property,EGPD_Input);
}

bool OriginalGraph(UEdGraph* Graph,UAnimGraphNode_Root*& Root)
{
    Root=nullptr;
    UAnimGraphNode_BlendListByInt* Selector=nullptr;
    if (!Graph) return false;
    for (UEdGraphNode* Node:Graph->Nodes)
    {
        if (auto* Found=Cast<UAnimGraphNode_Root>(Node)) { if (Root) return false; Root=Found; }
        if (auto* Found=Cast<UAnimGraphNode_BlendListByInt>(Node)) { if (Selector) return false; Selector=Found; }
    }
    const auto* Times=FindFProperty<FArrayProperty>(FAnimNode_BlendListBase::StaticStruct(),TEXT("BlendTime"));
    if (!Root || !Selector || !Times) return false;
    const int32 Count=FScriptArrayHelper(Times,Times->ContainerPtrToValuePtr<void>(&Selector->Node)).Num();
    if (Count!=10 && Count!=static_cast<int32>(UDeliveryNpcBodyAnimInstance::EPose::JumpLand)+1) return false;
    for (int32 Index=0;Index<Count;++Index)
        if (!LinkedNode(Selector,FName(*FString::Printf(TEXT("BlendPose_%d"),Index)))) return false;
    const auto* Input=PosePin(Root,EGPD_Input);
    return Input && Input->LinkedTo.Num()==1;
}

bool ConfigureEvaluator(UEdGraph* Graph,UAnimGraphNode_SequenceEvaluator* Node,const TCHAR* AssetRole,const TCHAR* TimeRole)
{
    Node->Node.SetGroupName(NAME_None); Node->Node.SetGroupMethod(EAnimSyncMethod::DoNotSync);
    Node->Node.SetShouldUseExplicitFrame(false); Node->Node.SetShouldLoop(false);
    auto* Teleport=FindFProperty<FBoolProperty>(FAnimNode_SequenceEvaluator::StaticStruct(),TEXT("bTeleportToExplicitTime"));
    if (!Teleport) return false;
    Teleport->SetPropertyValue_InContainer(&Node->Node,true);
    return Parameter(Graph,Node,TEXT("Sequence"),TEXT("HomeAnimation"),AssetRole,Node->NodePosY) &&
        Parameter(Graph,Node,TEXT("ExplicitTime"),TEXT("HomeAnimationTime"),TimeRole,Node->NodePosY+90);
}

bool EvaluatorMatches(UAnimGraphNode_SequenceEvaluator* Node)
{
    if (!Node || !ParameterMatches(Node,TEXT("Sequence"),TEXT("HomeAnimation")) ||
        !ParameterMatches(Node,TEXT("ExplicitTime"),TEXT("HomeAnimationTime"))) return false;
    const auto* Loop=FindFProperty<FBoolProperty>(FAnimNode_SequenceEvaluator::StaticStruct(),TEXT("bShouldLoop"));
    const auto* Teleport=FindFProperty<FBoolProperty>(FAnimNode_SequenceEvaluator::StaticStruct(),TEXT("bTeleportToExplicitTime"));
    return Loop && Teleport && !Loop->GetPropertyValue_InContainer(&Node->Node) &&
        Teleport->GetPropertyValue_InContainer(&Node->Node) && Node->Node.GetGroupMethod()==EAnimSyncMethod::DoNotSync;
}

bool RootScaleConfiguration(USkeletalMesh* Mesh,FName& RootBone,FVector& Scale,FString& Reason)
{
    if (!Mesh) { Reason=TEXT("Actual companion mesh is unavailable"); return false; }
    const auto& Ref=Mesh->GetRefSkeleton();
    const int32 Pelvis=Ref.FindBoneIndex(PelvisBone);
    if (Ref.GetNum()<2 || Pelvis==INDEX_NONE || Ref.GetParentIndex(Pelvis)!=0)
    { Reason=TEXT("Expected pelvis directly below the reference root; inspect actual hierarchy before adapting"); return false; }
    RootBone=Ref.GetBoneName(0); Scale=Ref.GetRefBonePose()[0].GetScale3D();
    if (Scale.ContainsNaN() || Scale.GetMin()<=0 || !FMath::IsNearlyEqual(Scale.X,Scale.Y,1.e-4) ||
        !FMath::IsNearlyEqual(Scale.X,Scale.Z,1.e-4))
    { Reason=TEXT("Reference root scale must be positive and uniform"); return false; }
    for (int32 Index=1;Index<Ref.GetNum();++Index)
        if (!Ref.GetRefBonePose()[Index].GetScale3D().Equals(FVector::OneVector,1.e-4) ||
            (Ref.GetParentIndex(Index)==0 && Index!=Pelvis))
        { Reason=TEXT("Additional scaled bones or root branches need separate inspection; nothing changed"); return false; }
    return true;
}

bool GraphMatches(UAnimBlueprint* Blueprint,FString& Reason,USkeletalMesh* Mesh)
{
    auto* Graph=BodyGraph(Blueprint);
    UAnimGraphNode_Root* Root=nullptr;
    if (!OriginalGraph(Graph,Root)) { Reason=TEXT("Original ten-pose graph or output is missing"); return false; }
    auto* Cache=Role<UAnimGraphNode_SaveCachedPose>(Graph,TEXT("OriginalBody"));
    auto* Base=Role<UAnimGraphNode_UseCachedPose>(Graph,TEXT("CarryBase"));
    auto* Fallback=Role<UAnimGraphNode_UseCachedPose>(Graph,TEXT("CarryFallback"));
    auto* Layer=Role<UAnimGraphNode_LayeredBoneBlend>(Graph,TEXT("CarryLayer"));
    auto* Carry=Role<UAnimGraphNode_BlendListByBool>(Graph,TEXT("CarryEnabled"));
    auto* Full=Role<UAnimGraphNode_BlendListByBool>(Graph,TEXT("FullHomeEnabled"));
    auto* UpperPose=Role<UAnimGraphNode_SequenceEvaluator>(Graph,TEXT("CarryPose"));
    auto* FullPose=Role<UAnimGraphNode_SequenceEvaluator>(Graph,TEXT("FullHomePose"));
    auto* FullCache=Role<UAnimGraphNode_SaveCachedPose>(Graph,TEXT("UnscaledHome"));
    auto* ScaleInput=Role<UAnimGraphNode_UseCachedPose>(Graph,TEXT("HomeScaleInput"));
    auto* PositionInput=Role<UAnimGraphNode_UseCachedPose>(Graph,TEXT("HomePositionInput"));
    auto* ToComponent=Role<UAnimGraphNode_LocalToComponentSpace>(Graph,TEXT("HomeToComponent"));
    auto* ScaleNode=Role<UAnimGraphNode_ModifyBone>(Graph,TEXT("HomeReferenceScale"));
    auto* ToLocal=Role<UAnimGraphNode_ComponentToLocalSpace>(Graph,TEXT("HomeToLocal"));
    auto* PositionNode=Role<UAnimGraphNode_BlendBoneByChannel>(Graph,TEXT("HomePelvisPosition"));
    const auto* RootPin=PosePin(Root,EGPD_Input);
    const auto* CachePin=PosePin(Cache,EGPD_Input);
    FGuid OriginalOutput;
    const bool bHasOriginalOutput=FGuid::Parse(Blueprint->GetPackage()->GetMetaData().GetValue(Blueprint,BasePoseKey),OriginalOutput);
    if (!Cache || !Base || !Fallback || !Layer || !Carry || !Full || !RootPin || !CachePin ||
        CachePin->LinkedTo.Num()!=1 || !bHasOriginalOutput || CachePin->LinkedTo[0]->GetOwningNode()->NodeGuid!=OriginalOutput ||
        Base->SaveCachedPoseNode.Get()!=Cache || Fallback->SaveCachedPoseNode.Get()!=Cache ||
        RootPin->LinkedTo[0]->GetOwningNode()!=Full || LinkedNode(Full,TEXT("BlendPose_0"))!=PositionNode ||
        LinkedNode(Full,TEXT("BlendPose_1"))!=Carry || LinkedNode(Carry,TEXT("BlendPose_0"))!=Layer ||
        LinkedNode(Carry,TEXT("BlendPose_1"))!=Fallback || LinkedNode(Layer,TEXT("BasePose"))!=Base ||
        LinkedNode(Layer,TEXT("BlendPoses_0"))!=UpperPose ||
        !ParameterMatches(Carry,TEXT("bActiveValue"),TEXT("bHomeUpperBodyPose")) ||
        !ParameterMatches(Full,TEXT("bActiveValue"),TEXT("bHomePose")) ||
        !EvaluatorMatches(UpperPose) || !EvaluatorMatches(FullPose))
    { Reason=TEXT("Home wrapper wiring or explicit-time evaluator changed; existing graph preserved"); return false; }
    FName RootBone; FVector Scale;
    if (!RootScaleConfiguration(Mesh,RootBone,Scale,Reason)) return false;
    const auto* FullCachePin=PosePin(FullCache,EGPD_Input);
    if (!FullCache || !ScaleInput || !PositionInput || !ToComponent || !ScaleNode || !ToLocal || !PositionNode ||
        !FullCachePin || FullCachePin->LinkedTo.Num()!=1 || FullCachePin->LinkedTo[0]->GetOwningNode()!=FullPose ||
        ScaleInput->SaveCachedPoseNode.Get()!=FullCache || PositionInput->SaveCachedPoseNode.Get()!=FullCache ||
        LinkedNode(ToComponent,TEXT("LocalPose"))!=ScaleInput || LinkedNode(ScaleNode,TEXT("ComponentPose"))!=ToComponent ||
        LinkedNode(ToLocal,TEXT("ComponentPose"))!=ScaleNode || LinkedNode(PositionNode,TEXT("A"))!=ToLocal ||
        LinkedNode(PositionNode,TEXT("B"))!=PositionInput || ScaleNode->Node.BoneToModify.BoneName!=RootBone ||
        ScaleNode->Node.ScaleMode!=BMM_Replace || ScaleNode->Node.ScaleSpace!=BCS_ParentBoneSpace ||
        ScaleNode->Node.TranslationMode!=BMM_Ignore || ScaleNode->Node.RotationMode!=BMM_Ignore ||
        !FMath::IsNearlyEqual(ScaleNode->Node.Alpha,1.f) || ScaleNode->FindPin(TEXT("Scale"),EGPD_Input) ||
        ScaleNode->FindPin(TEXT("Alpha"),EGPD_Input) || PositionNode->FindPin(TEXT("Alpha"),EGPD_Input) ||
        !ScaleNode->Node.Scale.Equals(Scale,1.e-5) || PositionNode->BlendNode.TransformsSpace!=BCS_ComponentSpace ||
        !FMath::IsNearlyEqual(PositionNode->BlendNode.Alpha,1.f) || PositionNode->BlendNode.BoneDefinitions.Num()!=1)
    { Reason=TEXT("Reference scale restoration or original pelvis-position wiring changed"); return false; }
    const auto& Channel=PositionNode->BlendNode.BoneDefinitions[0];
    if (Channel.SourceBone.BoneName!=PelvisBone || Channel.TargetBone.BoneName!=PelvisBone ||
        !Channel.bBlendTranslation || Channel.bBlendRotation || Channel.bBlendScale)
    { Reason=TEXT("Home pelvis channel must preserve the original component-space translation only"); return false; }
    if (Layer->Node.BlendMode!=ELayeredBoneBlendMode::BranchFilter || Layer->Node.bMeshSpaceScaleBlend ||
        Layer->Node.bMeshSpaceRotationBlend || Layer->Node.LayerSetup.Num()!=1 ||
        Layer->Node.LayerSetup[0].BranchFilters.Num()!=1 || Layer->Node.LayerSetup[0].BranchFilters[0].BoneName!=CarryBone ||
        Layer->Node.LayerSetup[0].BranchFilters[0].BlendDepth!=1)
    { Reason=TEXT("Carry branch must start at Spine with depth 1"); return false; }
    return true;
}

bool Backup(UObject* Asset,const FString& Folder)
{
    if (!Asset) return true;
    const FString Base=FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName());
    for (const TCHAR* Extension:{TEXT(".uasset"),TEXT(".uexp"),TEXT(".ubulk")})
    {
        const FString Source=Base+Extension;
        if (!IFileManager::Get().FileExists(*Source)) continue;
        const FString Target=Folder/FPaths::GetCleanFilename(Source);
        if (!IFileManager::Get().MakeDirectory(*Folder,true) || IFileManager::Get().Copy(*Target,*Source)!=COPY_OK) return false;
    }
    return true;
}

bool Save(UObject* Asset)
{
    Asset->MarkPackageDirty();
    FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(Asset->GetPackage(),Asset,*FPackageName::LongPackageNameToFilename(
        Asset->GetPackage()->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}

ADeliveryCompanionCharacter* Defaults(UBlueprint* Blueprint)
{
    return Blueprint && Blueprint->GeneratedClass?Cast<ADeliveryCompanionCharacter>(Blueprint->GeneratedClass->GetDefaultObject()):nullptr;
}

TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X),MakeShared<FJsonValueNumber>(Value.Y),MakeShared<FJsonValueNumber>(Value.Z)};
}

TSharedRef<FJsonObject> TransformJson(const FTransform& Value)
{
    auto Json=MakeShared<FJsonObject>();
    Json->SetArrayField(TEXT("position"),VectorJson(Value.GetTranslation()));
    Json->SetArrayField(TEXT("scale"),VectorJson(Value.GetScale3D()));
    const FQuat Rotation=Value.GetRotation();
    Json->SetArrayField(TEXT("rotation_xyzw"),{MakeShared<FJsonValueNumber>(Rotation.X),MakeShared<FJsonValueNumber>(Rotation.Y),
        MakeShared<FJsonValueNumber>(Rotation.Z),MakeShared<FJsonValueNumber>(Rotation.W)});
    return Json;
}

TSharedRef<FJsonObject> PreviewPoseJson(USkeletalMeshComponent* Mesh)
{
    auto Json=MakeShared<FJsonObject>();
    const auto& Ref=Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
    const auto& Local=Mesh->GetBoneSpaceTransforms();
    const auto& World=Mesh->GetComponentSpaceTransforms();
    for (int32 Index=0;Index<Ref.GetNum() && Index<Local.Num() && Index<World.Num();++Index)
    {
        auto Bone=MakeShared<FJsonObject>();
        Bone->SetObjectField(TEXT("local"),TransformJson(Local[Index]));
        Bone->SetObjectField(TEXT("component"),TransformJson(World[Index]));
        Json->SetObjectField(Ref.GetBoneName(Index).ToString(),Bone);
    }
    return Json;
}

bool PreviewPoseHasSafeScale(USkeletalMeshComponent* Mesh,const FVector& RootScale,FString& Reason)
{
    const auto& Ref=Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
    const auto& Local=Mesh->GetBoneSpaceTransforms();
    const auto& Component=Mesh->GetComponentSpaceTransforms();
    if (Local.Num()!=Ref.GetNum() || Component.Num()!=Ref.GetNum())
    { Reason=TEXT("Preview did not evaluate the complete target skeleton"); return false; }
    for (int32 Index=0;Index<Local.Num();++Index)
    {
        const FVector ExpectedLocal=Index==0?RootScale:FVector::OneVector;
        if (Local[Index].ContainsNaN() || Component[Index].ContainsNaN() ||
            !Local[Index].GetScale3D().Equals(ExpectedLocal,.003) ||
            !Component[Index].GetScale3D().Equals(RootScale,.01) ||
            FVector::Dist(Component[Index].GetTranslation(),Component[0].GetTranslation())>500.)
        { Reason=FString::Printf(TEXT("Unsafe local/component scale or body extent at %s"),*Ref.GetBoneName(Index).ToString()); return false; }
    }
    return true;
}

bool CheckFullHomePose(USkeletalMeshComponent* Mesh,UAnimSequence* Clip,double Time,const FVector& RootScale,
    TSharedRef<FJsonObject> Sample,FString& Reason)
{
    if (!PreviewPoseHasSafeScale(Mesh,RootScale,Reason)) return false;
    FAnimPoseEvaluationOptions Options;
    Options.EvaluationType=EAnimDataEvalType::Compressed; Options.OptionalSkeletalMesh=Mesh->GetSkeletalMeshAsset();
    FAnimPose Raw;
    UAnimPoseExtensions::GetAnimPoseAtTime(Clip,Time,Options,Raw);
    if (!UAnimPoseExtensions::IsValid(Raw)) { Reason=TEXT("Downloaded clip could not be sampled"); return false; }
    const auto& Ref=Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
    const auto& Component=Mesh->GetComponentSpaceTransforms();
    const FTransform RawRoot=UAnimPoseExtensions::GetBonePose(Raw,Ref.GetBoneName(0),EAnimPoseSpaces::World);
    const FTransform RawHips=UAnimPoseExtensions::GetBonePose(Raw,PelvisBone,EAnimPoseSpaces::World);
    const FVector Factor=RootScale/RawRoot.GetScale3D();
    double MaximumPositionError=0,MaximumAngleError=0;
    for (int32 Index=1;Index<Ref.GetNum();++Index)
    {
        const FTransform Input=UAnimPoseExtensions::GetBonePose(Raw,Ref.GetBoneName(Index),EAnimPoseSpaces::World);
        const FVector Expected=RawHips.GetTranslation()+(Input.GetTranslation()-RawHips.GetTranslation())*Factor;
        MaximumPositionError=FMath::Max(MaximumPositionError,FVector::Dist(Component[Index].GetTranslation(),Expected));
        MaximumAngleError=FMath::Max(MaximumAngleError,FMath::RadiansToDegrees(Component[Index].GetRotation().AngularDistance(Input.GetRotation())));
    }
    Sample->SetNumberField(TEXT("maximum_position_error_cm"),MaximumPositionError);
    Sample->SetNumberField(TEXT("maximum_rotation_error_deg"),MaximumAngleError);
    Sample->SetObjectField(TEXT("raw_pelvis_component"),TransformJson(RawHips));
    if (MaximumPositionError>.1 || MaximumAngleError>.1)
    { Reason=TEXT("Actual graph did not preserve clip motion while restoring reference units"); return false; }
    return true;
}

bool CheckCarryPose(USkeletalMeshComponent* Mesh,UAnimSequence* Clip,double Time,TSharedRef<FJsonObject> Sample,FString& Reason)
{
    FAnimPoseEvaluationOptions Options;
    Options.EvaluationType=EAnimDataEvalType::Compressed; Options.OptionalSkeletalMesh=Mesh->GetSkeletalMeshAsset();
    FAnimPose Raw; UAnimPoseExtensions::GetAnimPoseAtTime(Clip,Time,Options,Raw);
    if (!UAnimPoseExtensions::IsValid(Raw)) { Reason=TEXT("Carry source pose could not be sampled"); return false; }
    const auto& Ref=Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
    const int32 Spine=Ref.FindBoneIndex(CarryBone);
    if (Spine==INDEX_NONE) { Reason=TEXT("Carry branch root no longer exists"); return false; }
    const auto& Local=Mesh->GetBoneSpaceTransforms();
    double MaximumPositionError=0,MaximumAngleError=0;
    for (int32 Index=0;Index<Ref.GetNum();++Index)
    {
        if (Index!=Spine && !Ref.BoneIsChildOf(Index,Spine)) continue;
        const FTransform Input=UAnimPoseExtensions::GetBonePose(Raw,Ref.GetBoneName(Index),EAnimPoseSpaces::Local);
        MaximumPositionError=FMath::Max(MaximumPositionError,FVector::Dist(Local[Index].GetTranslation(),Input.GetTranslation()));
        MaximumAngleError=FMath::Max(MaximumAngleError,FMath::RadiansToDegrees(Local[Index].GetRotation().AngularDistance(Input.GetRotation())));
    }
    Sample->SetNumberField(TEXT("maximum_upper_local_position_error"),MaximumPositionError);
    Sample->SetNumberField(TEXT("maximum_upper_rotation_error_deg"),MaximumAngleError);
    if (MaximumPositionError>.001 || MaximumAngleError>.1)
    { Reason=TEXT("Carry graph did not apply the downloaded upper-body pose"); return false; }
    return true;
}

float PreviewBlendSeconds(UAnimBlueprint* Blueprint,bool bUpper)
{
    auto* Node=Role<UAnimGraphNode_BlendListByBool>(BodyGraph(Blueprint),bUpper?TEXT("CarryEnabled"):TEXT("FullHomeEnabled"));
    const auto* Property=FindFProperty<FArrayProperty>(FAnimNode_BlendListBase::StaticStruct(),TEXT("BlendTime"));
    const auto* Inner=Property?CastField<FFloatProperty>(Property->Inner):nullptr;
    if (!Node || !Property || !Inner) return 0;
    FScriptArrayHelper Values(Property,Property->ContainerPtrToValuePtr<void>(&Node->Node));
    return Values.Num()>0?Inner->GetPropertyValue(Values.GetRawPtr(0)):0;
}

TSharedRef<FJsonObject> ValidateGraphPreview(UAnimBlueprint* Blueprint,ADeliveryCompanionCharacter* Defaults,
    const TArray<UAnimSequence*>& Clips,UAnimSequence* Carry,bool& bPassed)
{
    auto Json=MakeShared<FJsonObject>(); bPassed=false;
    Json->SetBoolField(TEXT("begin_play_called"),false); Json->SetBoolField(TEXT("world_ticked"),false);
    Json->SetBoolField(TEXT("assets_saved"),false); Json->SetStringField(TEXT("scope"),TEXT("EditorPreview animation graph only; not gameplay acceptance"));
    FString Reason;
    auto Fail=[&](const FString& Why) { Json->SetBoolField(TEXT("success"),false); Json->SetStringField(TEXT("reason"),Why); return Json; };
    USkeletalMesh* Asset=Defaults && Defaults->GetMesh()?Defaults->GetMesh()->GetSkeletalMeshAsset():nullptr;
    FName RootBone; FVector RootScale;
    if (!Blueprint || !Blueprint->GeneratedClass || !Defaults || !Defaults->Presentation || Clips.IsEmpty())
        return Fail(TEXT("Actual graph, character animation defaults or requested clips are missing"));
    if (
        !RootScaleConfiguration(Asset,RootBone,RootScale,Reason) || !GraphMatches(Blueprint,Reason,Asset)) return Fail(Reason);
    auto* Source=LoadObject<UAnimBlueprint>(nullptr,Blueprint->GetPackage()->GetMetaData().GetValue(Blueprint,SourceKey));
    if (!Source || !Source->GeneratedClass) return Fail(TEXT("Original body animation is unavailable for comparison"));
    const auto* StateProperty=FindFProperty<FStructProperty>(UDeliveryCompanionComponent::StaticClass(),TEXT("HomeState"));
    if (!StateProperty || StateProperty->Struct!=FDeliveryCompanionHomeState::StaticStruct()) return Fail(TEXT("Transient preview pose input is unavailable"));
    const auto* EncounterProperty=FindFProperty<FStructProperty>(ADeliveryCompanionCharacter::StaticClass(),TEXT("Encounter"));
    if (!EncounterProperty || EncounterProperty->Struct!=FDeliveryCompanionEncounterState::StaticStruct())
        return Fail(TEXT("Transient preview encounter condition is unavailable"));
    FPreviewScene Scene(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false)
        .SetForceMipsResident(false).SetTransactional(false).AllowAudioPlayback(false));
    UWorld* World=Scene.GetWorld();
    if (!World || World->WorldType!=EWorldType::EditorPreview || World->HasBegunPlay()) return Fail(TEXT("Preview world is not a non-playing EditorPreview"));
    FActorSpawnParameters Spawn; Spawn.ObjectFlags=RF_Transient; Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Preview=World->SpawnActor<ADeliveryCompanionCharacter>(ADeliveryCompanionCharacter::StaticClass(),FTransform::Identity,Spawn);
    if (!Preview) return Fail(TEXT("Could not create transient preview actor"));
    ON_SCOPE_EXIT { if (IsValid(Preview)) Preview->Destroy(); };
    if (Preview->HasActorBegunPlay() || World->HasBegunPlay()) return Fail(TEXT("Preview unexpectedly entered BeginPlay"));
    // 预览角色须处于已加入状态，正式 AnimInstance 会拒绝初遇尚未完成的角色
    // 这里只设置瞬态预览数据，不调用招募、阶段事件或服务器玩法接口
    auto* Encounter=EncounterProperty->ContainerPtrToValuePtr<FDeliveryCompanionEncounterState>(Preview);
    Encounter->Phase=EDeliveryCompanionEncounterPhase::Joined;
    Json->SetStringField(TEXT("preview_encounter_phase"),TEXT("Joined (transient preview input only)"));
    auto* Mesh=Preview->GetMesh();
    if (!Mesh || !Preview->Companion || !Preview->Presentation) return Fail(TEXT("Preview components unavailable"));
    Preview->SetActorEnableCollision(false);
    // 只复制正式角色的动画资源配置，不执行初遇、任务、移动或任何游戏接口
    for (TFieldIterator<FObjectPropertyBase> It(UDeliveryNpcPresentationComponent::StaticClass());It;++It)
        if (It->PropertyClass && It->PropertyClass->IsChildOf(UAnimSequence::StaticClass()))
            It->SetObjectPropertyValue_InContainer(Preview->Presentation,It->GetObjectPropertyValue_InContainer(Defaults->Presentation));
    Mesh->SetSkeletalMesh(Asset); Mesh->SetRelativeTransform(FTransform::Identity);
    Mesh->SetDisablePostProcessBlueprint(true); Mesh->SetUpdateAnimationInEditor(true);
    Mesh->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Mesh->SetForcedLOD(1);
    auto* State=StateProperty->ContainerPtrToValuePtr<FDeliveryCompanionHomeState>(Preview->Companion);
    TArray<TSharedPtr<FJsonValue>> AnimationInputs;
    auto Tick=[&](float Seconds)
    {
        Mesh->TickAnimation(Seconds,false); Mesh->RefreshBoneTransforms();
        auto Input=MakeShared<FJsonObject>();
        Input->SetNumberField(TEXT("tick_seconds"),Seconds);
        Input->SetBoolField(TEXT("is_knocked_down"),Preview->IsKnockedDown());
        Input->SetBoolField(TEXT("combat_down"),Preview->Combat && Preview->Combat->IsDown());
        Input->SetBoolField(TEXT("recovering"),Preview->Combat && Preview->Combat->IsRecovering());
        Input->SetBoolField(TEXT("reacting_to_hit"),Preview->Combat && Preview->Combat->IsReactingToHit());
        Input->SetStringField(TEXT("requested_animation"),GetPathNameSafe(State->PoseAnimation));
        bool bInputMatches=true;
        if (const auto* Instance=Cast<UDeliveryCompanionBodyAnimInstance>(Mesh->GetAnimInstance()))
        {
            Input->SetStringField(TEXT("animation"),GetPathNameSafe(Instance->HomeAnimation));
            Input->SetNumberField(TEXT("animation_time"),Instance->HomeAnimationTime);
            Input->SetBoolField(TEXT("full_home"),Instance->bHomePose);
            Input->SetBoolField(TEXT("upper_home"),Instance->bHomeUpperBodyPose);
            const bool bRequested=IsValid(State->PoseAnimation);
            const double ExpectedTime=bRequested?FMath::Clamp(World->GetTimeSeconds()-State->PoseStartedServerTime,
                0.,static_cast<double>(State->PoseAnimation->GetPlayLength())):0.;
            bInputMatches=Instance->HomeAnimation==State->PoseAnimation &&
                Instance->bHomePose==(bRequested && !State->bUpperBodyPose) &&
                Instance->bHomeUpperBodyPose==(bRequested && State->bUpperBodyPose) &&
                FMath::IsNearlyEqual(static_cast<double>(Instance->HomeAnimationTime),ExpectedTime,1.e-4);
            Input->SetBoolField(TEXT("matches_requested_input"),bInputMatches);
        }
        AnimationInputs.Add(MakeShared<FJsonValueObject>(Input)); Json->SetArrayField(TEXT("animation_inputs"),AnimationInputs);
        if (!bInputMatches) Json->SetStringField(TEXT("input_error"),TEXT("Actual AnimInstance did not accept transient preview pose input"));
        return bInputMatches && !World->HasBegunPlay() && !Preview->HasActorBegunPlay();
    };
    auto Initialize=[&](UClass* Class)
    {
        *State=FDeliveryCompanionHomeState();
        Mesh->SetAnimInstanceClass(Class); Mesh->InitAnim(true);
        return Tick(0.f) && Tick(.25f);
    };
    if (!Initialize(Source->GeneratedClass)) return Fail(TEXT("Original preview initialization failed"));
    const TArray<FTransform> OriginalLocal=Mesh->GetBoneSpaceTransforms();
    if (!Initialize(Blueprint->GeneratedClass)) return Fail(TEXT("Independent preview initialization failed"));
    const TArray<FTransform> WrappedLocal=Mesh->GetBoneSpaceTransforms();
    if (OriginalLocal.Num()!=WrappedLocal.Num()) return Fail(TEXT("Original body pose size changed"));
    for (int32 Index=0;Index<OriginalLocal.Num();++Index)
        if (!OriginalLocal[Index].Equals(WrappedLocal[Index],.001)) return Fail(TEXT("Disabled home wrapper changed the original body pose"));
    Json->SetBoolField(TEXT("original_body_preserved"),true);
    Json->SetStringField(TEXT("scale_bone"),RootBone.ToString()); Json->SetArrayField(TEXT("reference_root_scale"),VectorJson(RootScale));
    TArray<TSharedPtr<FJsonValue>> Samples;
    for (UAnimSequence* Clip:Clips)
    {
        if (!Clip || Clip->GetSkeleton()!=Asset->GetSkeleton()) return Fail(TEXT("Preview clip skeleton mismatch"));
        const bool bUpper=Clip==Carry;
        const float BlendTime=PreviewBlendSeconds(Blueprint,bUpper);
        if (!FMath::IsFinite(BlendTime) || BlendTime<=0 || BlendTime>2) return Fail(TEXT("Home blend duration needs explicit valid configuration"));
        for (const double Fraction:{0.,.5,1.})
        {
            if (!Initialize(Blueprint->GeneratedClass)) return Fail(TEXT("Could not reset graph preview"));
            const TArray<FTransform> BaseComponent=Mesh->GetComponentSpaceTransforms();
            const double Time=Clip->GetPlayLength()*Fraction;
            State->PoseAnimation=Clip; State->PoseStartedServerTime=World->GetTimeSeconds()-Time;
            State->bUpperBodyPose=bUpper;
            if (!Tick(0.f)) return Fail(TEXT("Preview entered game state"));
            // 在真实 Bool blend 的半时刻和终点读回，防止两处祖先缩放相乘造成巨型角色
            float BlendElapsed=0;
            for (const float Step:{.5f,.5f,1.f})
            {
                if (!Tick(BlendTime*Step)) return Fail(TEXT("Preview entered game state"));
                BlendElapsed+=BlendTime*Step;
                auto Sample=MakeShared<FJsonObject>();
                Sample->SetStringField(TEXT("animation"),Clip->GetPathName()); Sample->SetNumberField(TEXT("clip_time"),Time);
                Sample->SetBoolField(TEXT("upper_body"),bUpper); Sample->SetNumberField(TEXT("blend_elapsed_seconds"),BlendElapsed);
                Sample->SetNumberField(TEXT("configured_blend_seconds"),BlendTime);
                Sample->SetObjectField(TEXT("bones"),PreviewPoseJson(Mesh));
                Samples.Add(MakeShared<FJsonValueObject>(Sample));
                Json->SetArrayField(TEXT("samples"),Samples);
                if (!PreviewPoseHasSafeScale(Mesh,RootScale,Reason)) return Fail(Reason);
            }
            auto Stable=MakeShared<FJsonObject>();
            Stable->SetStringField(TEXT("animation"),Clip->GetPathName()); Stable->SetNumberField(TEXT("clip_time"),Time);
            Stable->SetBoolField(TEXT("upper_body"),bUpper); Stable->SetObjectField(TEXT("bones"),PreviewPoseJson(Mesh));
            Samples.Add(MakeShared<FJsonValueObject>(Stable)); Json->SetArrayField(TEXT("samples"),Samples);
            if (!bUpper && !CheckFullHomePose(Mesh,Clip,Time,RootScale,Stable,Reason)) return Fail(Reason);
            if (bUpper)
            {
                const int32 Pelvis=Asset->GetRefSkeleton().FindBoneIndex(PelvisBone);
                if (!Mesh->GetComponentSpaceTransforms()[Pelvis].GetScale3D().Equals(BaseComponent[Pelvis].GetScale3D(),.01))
                    return Fail(TEXT("Carry layer changed the base pelvis scale"));
                if (!CheckCarryPose(Mesh,Clip,Time,Stable,Reason)) return Fail(Reason);
            }
            State->PoseAnimation=nullptr;
            if (!Tick(0.f)) return Fail(TEXT("Preview entered game state"));
            for (int32 Step=1;Step<=2;++Step)
            {
                if (!Tick(BlendTime*.5f)) return Fail(TEXT("Preview entered game state"));
                auto Exit=MakeShared<FJsonObject>();
                Exit->SetStringField(TEXT("animation"),Clip->GetPathName()); Exit->SetNumberField(TEXT("clip_time"),Time);
                Exit->SetStringField(TEXT("transition"),TEXT("home_to_original_body"));
                Exit->SetNumberField(TEXT("blend_elapsed_seconds"),BlendTime*.5f*Step);
                Exit->SetObjectField(TEXT("bones"),PreviewPoseJson(Mesh));
                Samples.Add(MakeShared<FJsonValueObject>(Exit)); Json->SetArrayField(TEXT("samples"),Samples);
                if (!PreviewPoseHasSafeScale(Mesh,RootScale,Reason)) return Fail(Reason);
            }
        }
    }
    Json->SetArrayField(TEXT("samples"),Samples);
    Json->SetBoolField(TEXT("world_has_begun_play"),World->HasBegunPlay());
    bPassed=true; Json->SetBoolField(TEXT("success"),true);
    Json->SetStringField(TEXT("reason"),TEXT("Actual graph preserved base poses, clip motion and reference scale through home/carry transitions"));
    return Json;
}
}

FString UDeliveryCompanionAnimationAssetTools::SetupCompanionBodyAnimation(const FString& SitDownPath,const FString& SitPath,
    const FString& StandUpPath,const FString& CarryPath)
{
    if (GEditor && GEditor->PlayWorld) return Result(false,TEXT("Stop PIE before editing companion animation assets"));
    auto* NpcBP=LoadObject<UBlueprint>(nullptr,CompanionPath);
    auto* Npc=Defaults(NpcBP);
    auto* Mesh=Npc?Npc->GetMesh():nullptr;
    auto* SkeletalMesh=Mesh?Mesh->GetSkeletalMeshAsset():nullptr;
    auto* CurrentClass=Mesh?Mesh->GetAnimClass():nullptr;
    auto* Current=CurrentClass?Cast<UAnimBlueprint>(CurrentClass->ClassGeneratedBy):nullptr;
    auto* Target=LoadObject<UAnimBlueprint>(nullptr,*AnimationPath());
    if (!Npc || !Npc->Companion || !SkeletalMesh || !Current || !Current->ParentClass ||
        !Current->ParentClass->IsChildOf(UDeliveryNpcBodyAnimInstance::StaticClass()) ||
        Current->TargetSkeleton!=SkeletalMesh->GetSkeleton() || SkeletalMesh->GetRefSkeleton().FindBoneIndex(CarryBone)==INDEX_NONE)
        return Result(false,TEXT("Companion mesh, current body graph, shared skeleton or Spine bone is unavailable"));
    auto* CarryAnimation=LoadObject<UAnimSequence>(nullptr,*CarryPath);
    TArray<UAnimSequence*> HomeClips;
    for (const FString& Path:{SitDownPath,SitPath,StandUpPath,CarryPath})
    {
        auto* Animation=LoadObject<UAnimSequence>(nullptr,*Path);
        if (!Animation || Animation->GetSkeleton()!=SkeletalMesh->GetSkeleton() ||
            !FMath::IsFinite(Animation->GetPlayLength()) || Animation->GetPlayLength()<=0)
            return Result(false,FString::Printf(TEXT("Import a finished animation on the actual companion skeleton first: %s"),*Path));
        HomeClips.Add(Animation);
    }
    if (Target && (Target->GetPackage()->GetMetaData().GetValue(Target,Marker)!=Version ||
        Target->ParentClass!=UDeliveryCompanionBodyAnimInstance::StaticClass() || Target->TargetSkeleton!=SkeletalMesh->GetSkeleton()))
        return Result(false,TEXT("Existing independent animation has unknown ownership, parent or skeleton; nothing changed"),Target);
    if (Npc->Companion->PropCarryAnimation && Npc->Companion->PropCarryAnimation!=CarryAnimation &&
        (!Target || Target->GetPackage()->GetMetaData().GetValue(Target,CarryAssetKey)!=Npc->Companion->PropCarryAnimation->GetPathName()))
        return Result(false,TEXT("Unknown manual carry animation override; nothing changed"));
    if (Target && Current!=Target && Target->GetPackage()->GetMetaData().GetValue(Target,SourceKey)!=Current->GetPathName())
        return Result(false,TEXT("Current companion animation is an unknown override; nothing changed"),Target);
    FString Reason;
    FName RootBone; FVector ReferenceScale;
    if (!RootScaleConfiguration(SkeletalMesh,RootBone,ReferenceScale,Reason)) return Result(false,Reason,Target);
    if (Target && !GraphMatches(Target,Reason,SkeletalMesh)) return Result(false,Reason,Target);
    if (Target && Current==Target && Npc->Companion->PropCarryAnimation==CarryAnimation &&
        Mesh->GetAnimationMode()==EAnimationMode::AnimationBlueprint && Target->Status==BS_UpToDate && NpcBP->Status==BS_UpToDate)
    {
        bool bPassed=false; auto Validation=ValidateGraphPreview(Target,Npc,HomeClips,CarryAnimation,bPassed);
        return Result(bPassed,bPassed?TEXT("Existing wrapper preserved and preview evaluated; no assets saved"):TEXT("Existing wrapper preview failed"),Target,true,Validation);
    }
    UAnimGraphNode_Root* OriginalRoot=nullptr;
    if (!Target && !OriginalGraph(BodyGraph(Current),OriginalRoot))
        return Result(false,TEXT("Current animation must retain its connected base poses; nothing changed"));
    const FString BackupFolder=FPaths::ProjectSavedDir()/TEXT("AIResearch/HomeObjects/AnimationBackups")/FGuid::NewGuid().ToString();
    if (!Backup(NpcBP,BackupFolder) || !Backup(Target,BackupFolder)) return Result(false,TEXT("Animation asset backup failed; nothing changed"));
    if (!Target)
    {
        Target=Cast<UAnimBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().DuplicateAsset(
            AnimationName,AnimationFolder,Current));
        if (!Target) return Result(false,TEXT("Could not duplicate current companion body animation"));
        Target->ParentClass=UDeliveryCompanionBodyAnimInstance::StaticClass();
        FBlueprintEditorUtils::RefreshAllNodes(Target);
        auto* Graph=BodyGraph(Target);
        UAnimGraphNode_Root* Root=nullptr;
        if (!OriginalGraph(Graph,Root)) return Result(false,TEXT("Duplicated base animation could not be resolved; not saved"),Target);
        auto* RootPin=PosePin(Root,EGPD_Input);
        auto* OriginalPose=RootPin->LinkedTo[0];
        Target->GetPackage()->GetMetaData().SetValue(Target,BasePoseKey,*OriginalPose->GetOwningNode()->NodeGuid.ToString());
        const int32 X=Root->NodePosX+300,Y=Root->NodePosY;
        auto* Cache=Add<UAnimGraphNode_SaveCachedPose>(Graph,TEXT("OriginalBody"),X,Y);
        Cache->CacheName=TEXT("ErgouOriginalBody");
        auto* Base=Add<UAnimGraphNode_UseCachedPose>(Graph,TEXT("CarryBase"),X+300,Y+180);
        auto* Fallback=Add<UAnimGraphNode_UseCachedPose>(Graph,TEXT("CarryFallback"),X+600,Y+460);
        Base->SaveCachedPoseNode=Cache; Fallback->SaveCachedPoseNode=Cache;
        auto* UpperPose=Add<UAnimGraphNode_SequenceEvaluator>(Graph,TEXT("CarryPose"),X+300,Y-100);
        auto* FullPose=Add<UAnimGraphNode_SequenceEvaluator>(Graph,TEXT("FullHomePose"),X+900,Y-220);
        auto* FullCache=Add<UAnimGraphNode_SaveCachedPose>(Graph,TEXT("UnscaledHome"),X+1200,Y-400);
        FullCache->CacheName=TEXT("ErgouUnscaledHome");
        auto* ScaleInput=Add<UAnimGraphNode_UseCachedPose>(Graph,TEXT("HomeScaleInput"),X+1450,Y-600);
        auto* PositionInput=Add<UAnimGraphNode_UseCachedPose>(Graph,TEXT("HomePositionInput"),X+2050,Y-300);
        ScaleInput->SaveCachedPoseNode=FullCache; PositionInput->SaveCachedPoseNode=FullCache;
        auto* ToComponent=Add<UAnimGraphNode_LocalToComponentSpace>(Graph,TEXT("HomeToComponent"),X+1700,Y-600);
        auto* ScaleNode=Add<UAnimGraphNode_ModifyBone>(Graph,TEXT("HomeReferenceScale"),X+1950,Y-600);
        ScaleNode->Node.BoneToModify.BoneName=RootBone;
        ScaleNode->Node.Scale=ReferenceScale; ScaleNode->Node.ScaleMode=BMM_Replace;
        ScaleNode->Node.ScaleSpace=BCS_ParentBoneSpace;
        ScaleNode->Node.TranslationMode=BMM_Ignore; ScaleNode->Node.RotationMode=BMM_Ignore;
        ScaleNode->ReconstructNode();
        if (!HideConstantPin(ScaleNode,TEXT("Scale")) || !HideConstantPin(ScaleNode,TEXT("Alpha")))
            return Result(false,TEXT("Could not store scale restoration as editable node constants"),Target);
        ScaleNode->Node.Scale=ReferenceScale; ScaleNode->Node.Alpha=1.f;
        auto* ToLocal=Add<UAnimGraphNode_ComponentToLocalSpace>(Graph,TEXT("HomeToLocal"),X+2250,Y-600);
        auto* PositionNode=Add<UAnimGraphNode_BlendBoneByChannel>(Graph,TEXT("HomePelvisPosition"),X+2500,Y-450);
        PositionNode->BlendNode.Alpha=1.f; PositionNode->BlendNode.TransformsSpace=BCS_ComponentSpace;
        FBlendBoneByChannelEntry PelvisChannel;
        PelvisChannel.SourceBone.BoneName=PelvisBone; PelvisChannel.TargetBone.BoneName=PelvisBone;
        PelvisChannel.bBlendTranslation=true; PelvisChannel.bBlendRotation=false; PelvisChannel.bBlendScale=false;
        PositionNode->BlendNode.BoneDefinitions={PelvisChannel}; PositionNode->ReconstructNode();
        if (!HideConstantPin(PositionNode,TEXT("Alpha"))) return Result(false,TEXT("Could not store pelvis copy weight"),Target);
        PositionNode->BlendNode.Alpha=1.f;
        auto* Layer=Add<UAnimGraphNode_LayeredBoneBlend>(Graph,TEXT("CarryLayer"),X+650,Y+100);
        Layer->Node.BlendMode=ELayeredBoneBlendMode::BranchFilter;
        Layer->Node.bMeshSpaceRotationBlend=false; Layer->Node.bMeshSpaceScaleBlend=false;
        Layer->Node.BlendPoses.Reset(); Layer->Node.BlendWeights.Reset(); Layer->Node.LayerSetup.Reset(); Layer->Node.BlendMasks.Reset();
        Layer->Node.AddPose();
        FBranchFilter Filter; Filter.BoneName=CarryBone; Filter.BlendDepth=1;
        Layer->Node.LayerSetup[0].BranchFilters.Add(Filter);
        Layer->ReconstructNode();
        auto* Carry=Add<UAnimGraphNode_BlendListByBool>(Graph,TEXT("CarryEnabled"),X+1000,Y+200);
        auto* Full=Add<UAnimGraphNode_BlendListByBool>(Graph,TEXT("FullHomeEnabled"),X+2800,Y);
        RootPin->BreakAllPinLinks();
        auto* CacheInput=PosePin(Cache,EGPD_Input);
        auto* FullCacheInput=PosePin(FullCache,EGPD_Input);
        const bool bConnected=CacheInput && FullCacheInput && Graph->GetSchema()->TryCreateConnection(OriginalPose,CacheInput) &&
            ConfigureEvaluator(Graph,UpperPose,TEXT("CarryAsset"),TEXT("CarryTime")) &&
            ConfigureEvaluator(Graph,FullPose,TEXT("FullAsset"),TEXT("FullTime")) &&
            Graph->GetSchema()->TryCreateConnection(PosePin(FullPose,EGPD_Output),FullCacheInput) &&
            Connect(Graph,ScaleInput,ToComponent,TEXT("LocalPose")) &&
            Connect(Graph,ToComponent,ScaleNode,TEXT("ComponentPose")) &&
            Connect(Graph,ScaleNode,ToLocal,TEXT("ComponentPose")) &&
            Connect(Graph,ToLocal,PositionNode,TEXT("A")) && Connect(Graph,PositionInput,PositionNode,TEXT("B")) &&
            Connect(Graph,Base,Layer,TEXT("BasePose")) && Connect(Graph,UpperPose,Layer,TEXT("BlendPoses_0")) &&
            Connect(Graph,Layer,Carry,TEXT("BlendPose_0")) && Connect(Graph,Fallback,Carry,TEXT("BlendPose_1")) &&
            Connect(Graph,PositionNode,Full,TEXT("BlendPose_0")) && Connect(Graph,Carry,Full,TEXT("BlendPose_1")) &&
            Parameter(Graph,Carry,TEXT("bActiveValue"),TEXT("bHomeUpperBodyPose"),TEXT("CarryFlag"),Y+380) &&
            Parameter(Graph,Full,TEXT("bActiveValue"),TEXT("bHomePose"),TEXT("FullFlag"),Y+180) &&
            Connect(Graph,Full,Root,RootPin->PinName);
        if (!bConnected || !GraphMatches(Target,Reason,SkeletalMesh)) return Result(false,TEXT("Could not build independent home wrapper: ")+Reason,Target);
        Target->GetPackage()->GetMetaData().SetValue(Target,Marker,Version);
        Target->GetPackage()->GetMetaData().SetValue(Target,SourceKey,*Current->GetPathName());
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
    }
    FKismetEditorUtilities::CompileBlueprint(Target,EBlueprintCompileOptions::SkipSave);
    if (Target->Status==BS_Error || !Target->GeneratedClass || !GraphMatches(Target,Reason,SkeletalMesh))
        return Result(false,TEXT("Independent body animation did not compile or retain its wrapper: ")+Reason,Target);
    bool bPreviewPassed=false;
    auto Validation=ValidateGraphPreview(Target,Npc,HomeClips,CarryAnimation,bPreviewPassed);
    if (!bPreviewPassed) return Result(false,TEXT("Preview evaluation failed; independent graph and companion defaults not saved"),Target,false,Validation);
    Target->GetPackage()->GetMetaData().SetValue(Target,CarryAssetKey,*CarryAnimation->GetPathName());
    const auto OriginalTransform=Mesh->GetRelativeTransform();
    const auto OriginalMaterials=Mesh->GetMaterials();
    const auto OriginalPostProcess=SkeletalMesh->GetPostProcessAnimBlueprint();
    auto* Mode=FindFProperty<FByteProperty>(USkeletalMeshComponent::StaticClass(),TEXT("AnimationMode"));
    auto* Class=FindFProperty<FObjectPropertyBase>(USkeletalMeshComponent::StaticClass(),TEXT("AnimClass"));
    if (!Mode || !Class) return Result(false,TEXT("Mesh animation template properties unavailable"),Target);
    NpcBP->Modify(); Npc->Modify(); Mesh->Modify(); Npc->Companion->Modify();
    Mode->SetPropertyValue_InContainer(Mesh,EAnimationMode::AnimationBlueprint);
    Class->SetObjectPropertyValue_InContainer(Mesh,Target->GeneratedClass);
    Npc->Companion->PropCarryAnimation=CarryAnimation;
    FBlueprintEditorUtils::MarkBlueprintAsModified(NpcBP);
    FKismetEditorUtilities::CompileBlueprint(NpcBP,EBlueprintCompileOptions::SkipSave);
    Npc=Defaults(NpcBP); Mesh=Npc?Npc->GetMesh():nullptr;
    if (NpcBP->Status==BS_Error || !Npc || !Npc->Companion || !Mesh || Mesh->GetAnimClass()!=Target->GeneratedClass ||
        Npc->Companion->PropCarryAnimation!=CarryAnimation || Mesh->GetSkeletalMeshAsset()!=SkeletalMesh ||
        !Mesh->GetRelativeTransform().Equals(OriginalTransform) || Mesh->GetMaterials()!=OriginalMaterials ||
        SkeletalMesh->GetPostProcessAnimBlueprint()!=OriginalPostProcess)
        return Result(false,TEXT("Companion binding or preserved mesh settings did not match; blueprint not saved"),Target);
    if (!Save(Target) || !Save(NpcBP)) return Result(false,TEXT("Independent body animation or companion blueprint save failed"),Target);
    return Result(true,TEXT("Independent body and scale-corrected home wrapper saved after preview evaluation; map instances require separate binding"),Target,true,Validation);
}

FString UDeliveryCompanionAnimationAssetTools::InspectCompanionBodyAnimation()
{
    auto* Target=LoadObject<UAnimBlueprint>(nullptr,*AnimationPath());
    auto* Npc=Defaults(LoadObject<UBlueprint>(nullptr,CompanionPath));
    FString Reason;
    if (!Target) return Result(false,TEXT("Independent companion body animation not created"));
    const bool bGraph=Target->GetPackage()->GetMetaData().GetValue(Target,Marker)==Version &&
        Target->ParentClass==UDeliveryCompanionBodyAnimInstance::StaticClass() &&
        GraphMatches(Target,Reason,Npc && Npc->GetMesh()?Npc->GetMesh()->GetSkeletalMeshAsset():nullptr);
    const bool bBound=Npc && Npc->GetMesh() && Npc->GetMesh()->GetAnimClass()==Target->GeneratedClass &&
        Npc->GetMesh()->GetAnimationMode()==EAnimationMode::AnimationBlueprint &&
        Npc->GetMesh()->GetSkeletalMeshAsset() && Npc->GetMesh()->GetSkeletalMeshAsset()->GetSkeleton()==Target->TargetSkeleton &&
        Npc->Companion && Npc->Companion->PropCarryAnimation &&
        Npc->Companion->PropCarryAnimation->GetPathName()==Target->GetPackage()->GetMetaData().GetValue(Target,CarryAssetKey);
    return Result(bGraph && bBound,bGraph?TEXT("Independent home wrapper inspected; map instances were not checked"):Reason,Target,bBound);
}

FString UDeliveryCompanionAnimationAssetTools::ValidateCompanionBodyAnimation(const FString& SitDownPath,const FString& SitPath,
    const FString& StandUpPath,const FString& CarryPath,const FString& PickUpPath)
{
    if (GEditor && GEditor->PlayWorld) return Result(false,TEXT("Stop PIE before static animation preview"));
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*AnimationPath());
    auto* Npc=Defaults(LoadObject<UBlueprint>(nullptr,CompanionPath));
    TArray<UAnimSequence*> Clips;
    for (const FString& Path:{SitDownPath,SitPath,StandUpPath,CarryPath,PickUpPath})
    {
        if (Path.IsEmpty()) continue;
        auto* Clip=LoadObject<UAnimSequence>(nullptr,*Path);
        if (!Clip || Clip->GetPlayLength()<=0) return Result(false,TEXT("Requested finished clip is missing: ")+Path,Blueprint);
        Clips.Add(Clip);
    }
    bool bPassed=false;
    auto Validation=ValidateGraphPreview(Blueprint,Npc,Clips,LoadObject<UAnimSequence>(nullptr,*CarryPath),bPassed);
    return Result(bPassed,bPassed?TEXT("Actual independent graph evaluated in EditorPreview without BeginPlay"):TEXT("Static animation preview failed"),Blueprint,false,Validation);
}
