#include "DeliveryNpcAnimationAssetTools.h"
#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_BlendBoneByChannel.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_LookAt.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
bool SaveNpcAnimationAsset(UObject* Asset)
{
    Asset->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags=RF_Public|RF_Standalone;
    const FString Filename=FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(),FPackageName::GetAssetPackageExtension());
    return UPackage::SavePackage(Asset->GetPackage(),Asset,*Filename,Args);
}

template<typename T>
T* AddAnimationNode(UEdGraph* Graph,int32 X,int32 Y,const TCHAR* Comment)
{
    auto* Node=NewObject<T>(Graph,NAME_None,RF_Transactional);
    Graph->AddNode(Node,false,false);
    Node->CreateNewGuid();
    Node->NodePosX=X;
    Node->NodePosY=Y;
    Node->NodeComment=Comment;
    Node->bCommentBubbleVisible=true;
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    return Node;
}

UEdGraphPin* FindPosePin(UEdGraphNode* Node,EEdGraphPinDirection Direction)
{
    for (auto* Pin:Node->Pins)
        if (Pin->Direction==Direction && Pin->PinType.PinSubCategoryObject.IsValid() &&
            Pin->PinType.PinSubCategoryObject->GetName().Contains(TEXT("PoseLink"))) return Pin;
    return nullptr;
}

bool ConnectPose(UEdGraph* Graph,UEdGraphNode* From,UEdGraphNode* To)
{
    auto* Output=FindPosePin(From,EGPD_Output);
    auto* Input=FindPosePin(To,EGPD_Input);
    return Output && Input && Graph->GetSchema()->TryCreateConnection(Output,Input);
}

bool ConnectParameter(UEdGraph* Graph,UAnimGraphNode_Base* Node,FName PinName,FName Property,int32 Y)
{
    for (int32 Index=0;Index<Node->ShowPinForProperties.Num();++Index)
        if (Node->ShowPinForProperties[Index].PropertyName==PinName)
        {
            Node->SetPinVisibility(true,Index);
            break;
        }
    auto* Pin=Node->FindPin(PinName,EGPD_Input);
    if (!Pin) return false;
    auto* Get=NewObject<UK2Node_VariableGet>(Graph,NAME_None,RF_Transactional);
    Get->VariableReference.SetSelfMember(Property);
    Graph->AddNode(Get,false,false);
    Get->CreateNewGuid();
    Get->NodePosX=Node->NodePosX-170;
    Get->NodePosY=Y;
    Get->AllocateDefaultPins();
    return Get->GetValuePin() && Graph->GetSchema()->TryCreateConnection(Get->GetValuePin(),Pin);
}

bool GetHeadAxes(USkeletalMesh* Mesh,const FTransform& MeshRelative,FVector& Front,FVector& Up)
{
    const auto& Skeleton=Mesh->GetRefSkeleton();
    const int32 Head=Skeleton.FindBoneIndex(TEXT("Head"));
    if (Head==INDEX_NONE || Skeleton.FindBoneIndex(TEXT("Spine2"))==INDEX_NONE) return false;
    FTransform HeadInMesh=Skeleton.GetRefBonePose()[Head];
    for (int32 Parent=Skeleton.GetParentIndex(Head);Parent!=INDEX_NONE;Parent=Skeleton.GetParentIndex(Parent))
        HeadInMesh=HeadInMesh*Skeleton.GetRefBonePose()[Parent];
    const FVector MeshFront=MeshRelative.InverseTransformVectorNoScale(FVector::ForwardVector);
    const FVector MeshUp=MeshRelative.InverseTransformVectorNoScale(FVector::UpVector);
    Front=HeadInMesh.InverseTransformVectorNoScale(MeshFront).GetSafeNormal();
    Up=HeadInMesh.InverseTransformVectorNoScale(MeshUp).GetSafeNormal();
    return !Front.IsNearlyZero() && !Up.IsNearlyZero() && FMath::Abs(FVector::DotProduct(Front,Up))<.01f;
}

bool SetNpcAnimationBool(UScriptStruct* Type,void* Data,FName Name,bool Value)
{
    auto* Property=FindFProperty<FBoolProperty>(Type,Name);
    if (!Property) return false;
    Property->SetPropertyValue_InContainer(Data,Value);
    return true;
}

bool ConnectNpcBodyPose(UEdGraph* Graph,UEdGraphNode* From,UAnimGraphNode_BlendListByInt* Blend,int32 Index)
{
    auto* Output=FindPosePin(From,EGPD_Output);
    auto* Input=Blend->FindPin(FName(*FString::Printf(TEXT("BlendPose_%d"),Index)),EGPD_Input);
    return Output && Input && Graph->GetSchema()->TryCreateConnection(Output,Input);
}

constexpr int32 FirstJumpPose=static_cast<int32>(UDeliveryNpcBodyAnimInstance::EPose::JumpStart);
constexpr int32 LastJumpPose=static_cast<int32>(UDeliveryNpcBodyAnimInstance::EPose::JumpLand);
const FName JumpAssetProperties[]={TEXT("JumpStartAnimation"),TEXT("JumpLoopAnimation"),TEXT("JumpLandAnimation")};
const FName JumpTimeProperties[]={TEXT("JumpStartTime"),TEXT("JumpLoopTime"),TEXT("JumpLandTime")};
const FName JumpPelvisBone(TEXT("Hips"));

UEdGraphNode* LinkedNpcBodyInput(const UEdGraphNode* Node,FName PinName)
{
    const auto* Pin=Node?Node->FindPin(PinName,EGPD_Input):nullptr;
    return Pin && Pin->LinkedTo.Num()==1?Pin->LinkedTo[0]->GetOwningNode():nullptr;
}

FName NpcBodyInputProperty(const UEdGraphNode* Node,FName PinName)
{
    const auto* Get=Cast<UK2Node_VariableGet>(LinkedNpcBodyInput(Node,PinName));
    return Get?Get->VariableReference.GetMemberName():NAME_None;
}

FName BlendPosePin(int32 Index)
{
    return FName(*FString::Printf(TEXT("BlendPose_%d"),Index));
}

UAnimGraphNode_BlendListByInt* FindNativeBodyPoseSelector(const UAnimBlueprint* Blueprint)
{
    UAnimGraphNode_BlendListByInt* Result=nullptr;
    for (const UEdGraph* Graph:Blueprint->FunctionGraphs)
    {
        if (!Graph || Graph->GetFName()!=TEXT("AnimGraph")) continue;
        for (UEdGraphNode* Node:Graph->Nodes)
        {
            auto* Selector=Cast<UAnimGraphNode_BlendListByInt>(Node);
            if (!Selector || NpcBodyInputProperty(Selector,TEXT("ActiveChildIndex"))!=TEXT("PoseIndex")) continue;
            if (Result) return nullptr;
            Result=Selector;
        }
    }
    return Result;
}

bool IsNpcJumpBodyPath(const FString& BodyPath)
{
    return BodyPath==TEXT("/Game/NPC/Residents/Animation/ABP_NpcBody") ||
        BodyPath==TEXT("/Game/NPC/Companion/Animation/ABP_CompanionBody");
}

bool IsValidNpcJumpClip(const UAnimSequence* Clip,const USkeleton* Skeleton)
{
    // 零位根锁保留成品原有单位姿势，蓝图单独恢复根缩放和骨盆位置
    return IsValid(Clip) && Clip->GetSkeleton()==Skeleton && !Clip->HasRootMotion() &&
        Clip->GetAdditiveAnimType()==AAT_None && Clip->bForceRootLock && Clip->RootMotionRootLock==ERootMotionRootLock::Zero &&
        FMath::IsFinite(Clip->GetPlayLength()) && Clip->GetPlayLength()>0;
}

bool JumpReferenceScale(const FReferenceSkeleton& Ref,FName& Root,FVector& Scale)
{
    const int32 Hips=Ref.FindBoneIndex(JumpPelvisBone);
    if (Ref.GetNum()<2 || Hips==INDEX_NONE || Ref.GetParentIndex(Hips)!=0) return false;
    Root=Ref.GetBoneName(0);
    Scale=Ref.GetRefBonePose()[0].GetScale3D();
    if (Scale.ContainsNaN() || Scale.GetMin()<=0 || !FMath::IsNearlyEqual(Scale.X,Scale.Y,1.e-4) ||
        !FMath::IsNearlyEqual(Scale.X,Scale.Z,1.e-4)) return false;
    for (int32 Index=1;Index<Ref.GetNum();++Index)
        if (!Ref.GetRefBonePose()[Index].GetScale3D().Equals(FVector::OneVector,1.e-4) ||
            (Ref.GetParentIndex(Index)==0 && Index!=Hips)) return false;
    return true;
}

bool JumpReferenceScaleForBody(const UAnimBlueprint* Blueprint,FName& Root,FVector& Scale)
{
    if (!Blueprint || !Blueprint->TargetSkeleton) return false;
    const bool bCompanion=Blueprint->GetFName()==TEXT("ABP_CompanionBody");
    const TCHAR* ResidentLoud=TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C");
    const TCHAR* ResidentShrewd=TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C");
    const TCHAR* Companion=TEXT("/Game/NPC/Companion/BP_Companion_Ergou.BP_Companion_Ergou_C");
    Root=NAME_None;
    for (const TCHAR* Path:{bCompanion?Companion:ResidentLoud,bCompanion?nullptr:ResidentShrewd})
    {
        if (!Path) continue;
        const auto* Class=LoadClass<ADeliveryNpcCharacter>(nullptr,Path);
        const auto* Character=Class?Class->GetDefaultObject<ADeliveryNpcCharacter>():nullptr;
        const auto* Mesh=Character && Character->GetMesh()?Character->GetMesh()->GetSkeletalMeshAsset():nullptr;
        FName MeshRoot;
        FVector MeshScale;
        if (!Mesh || Mesh->GetSkeleton()!=Blueprint->TargetSkeleton ||
            !JumpReferenceScale(Mesh->GetRefSkeleton(),MeshRoot,MeshScale)) return false;
        if (Root.IsNone()) { Root=MeshRoot; Scale=MeshScale; }
        else if (MeshRoot!=Root || !MeshScale.Equals(Scale,1.e-4)) return false;
    }
    return !Root.IsNone();
}

const UAnimGraphNode_SequenceEvaluator* CorrectedJumpClip(const UEdGraphNode* Output,FName Root,const FVector& Scale)
{
    const auto* Channels=Cast<UAnimGraphNode_BlendBoneByChannel>(Output);
    if (!Channels || Channels->BlendNode.TransformsSpace!=BCS_ComponentSpace ||
        !FMath::IsNearlyEqual(Channels->BlendNode.Alpha,1.f) || Channels->FindPin(TEXT("Alpha"),EGPD_Input) ||
        Channels->BlendNode.BoneDefinitions.Num()!=1) return nullptr;
    const auto& Pelvis=Channels->BlendNode.BoneDefinitions[0];
    if (Pelvis.SourceBone.BoneName!=JumpPelvisBone || Pelvis.TargetBone.BoneName!=JumpPelvisBone ||
        !Pelvis.bBlendTranslation || Pelvis.bBlendRotation || Pelvis.bBlendScale) return nullptr;
    const auto* ToLocal=Cast<UAnimGraphNode_ComponentToLocalSpace>(LinkedNpcBodyInput(Channels,TEXT("A")));
    const auto* RootScale=Cast<UAnimGraphNode_ModifyBone>(LinkedNpcBodyInput(ToLocal,TEXT("ComponentPose")));
    if (!RootScale || RootScale->Node.BoneToModify.BoneName!=Root ||
        RootScale->Node.ScaleMode!=BMM_Replace || RootScale->Node.ScaleSpace!=BCS_ParentBoneSpace ||
        RootScale->Node.TranslationMode!=BMM_Ignore || RootScale->Node.RotationMode!=BMM_Ignore ||
        !RootScale->Node.Scale.Equals(Scale,1.e-4) || !FMath::IsNearlyEqual(RootScale->Node.Alpha,1.f) ||
        RootScale->FindPin(TEXT("Scale"),EGPD_Input) || RootScale->FindPin(TEXT("Alpha"),EGPD_Input)) return nullptr;
    const auto* ToComponent=Cast<UAnimGraphNode_LocalToComponentSpace>(LinkedNpcBodyInput(RootScale,TEXT("ComponentPose")));
    const auto* ScaleInput=Cast<UAnimGraphNode_UseCachedPose>(LinkedNpcBodyInput(ToComponent,TEXT("LocalPose")));
    const auto* PelvisInput=Cast<UAnimGraphNode_UseCachedPose>(LinkedNpcBodyInput(Channels,TEXT("B")));
    const auto* Cache=ScaleInput?ScaleInput->SaveCachedPoseNode.Get():nullptr;
    if (!Cache || !PelvisInput || PelvisInput->SaveCachedPoseNode.Get()!=Cache) return nullptr;
    return Cast<UAnimGraphNode_SequenceEvaluator>(LinkedNpcBodyInput(Cache,TEXT("Pose")));
}

bool JumpPinsMatch(const UAnimGraphNode_BlendListByInt* Blend,FName Root,const FVector& Scale,bool bAllowLegacy=false)
{
    for (int32 Index=FirstJumpPose;Index<=LastJumpPose;++Index)
    {
        const auto* Output=LinkedNpcBodyInput(Blend,BlendPosePin(Index));
        const auto* Clip=CorrectedJumpClip(Output,Root,Scale);
        if (!Clip && bAllowLegacy) Clip=Cast<UAnimGraphNode_SequenceEvaluator>(Output);
        if (!Clip || NpcBodyInputProperty(Clip,TEXT("Sequence"))!=JumpAssetProperties[Index-FirstJumpPose] ||
            NpcBodyInputProperty(Clip,TEXT("ExplicitTime"))!=JumpTimeProperties[Index-FirstJumpPose]) return false;
    }
    return true;
}

bool HideJumpConstantPin(UAnimGraphNode_Base* Node,FName Name)
{
    for (int32 Index=0;Index<Node->ShowPinForProperties.Num();++Index)
        if (Node->ShowPinForProperties[Index].PropertyName==Name)
        {
            Node->SetPinVisibility(false,Index);
            break;
        }
    return !Node->FindPin(Name,EGPD_Input);
}

bool ConnectJumpPose(UEdGraph* Graph,UEdGraphNode* From,UEdGraphNode* To,FName InputName)
{
    auto* Input=To->FindPin(InputName,EGPD_Input);
    auto* Output=FindPosePin(From,EGPD_Output);
    return Input && Output && Graph->GetSchema()->TryCreateConnection(Output,Input);
}

bool CorrectBodyJumpPoses(UEdGraph* Graph,UAnimGraphNode_BlendListByInt* Blend,FName Root,const FVector& Scale)
{
    for (int32 Index=FirstJumpPose;Index<=LastJumpPose;++Index)
    {
        auto* Input=Blend->FindPin(BlendPosePin(Index),EGPD_Input);
        auto* Clip=Cast<UAnimGraphNode_SequenceEvaluator>(LinkedNpcBodyInput(Blend,BlendPosePin(Index)));
        if (!Clip) continue;
        const int32 X=Blend->NodePosX-1500;
        const int32 Y=Clip->NodePosY;
        Clip->Modify();
        Clip->NodePosX=X-260;
        for (const FName Pin:{FName(TEXT("Sequence")),FName(TEXT("ExplicitTime"))})
            if (auto* Parameter=LinkedNpcBodyInput(Clip,Pin))
            {
                Parameter->Modify();
                Parameter->NodePosX=X-485;
            }
        auto* Cache=AddAnimationNode<UAnimGraphNode_SaveCachedPose>(Graph,X,Y,TEXT("保存未修正的跳跃姿势"));
        Cache->CacheName=FString::Printf(TEXT("NpcJumpRaw%d"),Index);
        auto* ScaleInput=AddAnimationNode<UAnimGraphNode_UseCachedPose>(Graph,X+200,Y,TEXT("跳跃原姿势用于恢复根单位"));
        auto* PelvisInput=AddAnimationNode<UAnimGraphNode_UseCachedPose>(Graph,X+850,Y+115,TEXT("保留剪辑的骨盆组件位置"));
        ScaleInput->SaveCachedPoseNode=Cache;
        PelvisInput->SaveCachedPoseNode=Cache;
        auto* ToComponent=AddAnimationNode<UAnimGraphNode_LocalToComponentSpace>(Graph,X+440,Y,TEXT("转到组件空间"));
        auto* RootScale=AddAnimationNode<UAnimGraphNode_ModifyBone>(Graph,X+650,Y,TEXT("恢复正式网格的参考根缩放"));
        RootScale->Node.BoneToModify.BoneName=Root;
        RootScale->Node.Scale=Scale;
        RootScale->Node.ScaleMode=BMM_Replace;
        RootScale->Node.ScaleSpace=BCS_ParentBoneSpace;
        RootScale->Node.TranslationMode=BMM_Ignore;
        RootScale->Node.RotationMode=BMM_Ignore;
        RootScale->ReconstructNode();
        if (!HideJumpConstantPin(RootScale,TEXT("Scale")) || !HideJumpConstantPin(RootScale,TEXT("Alpha"))) return false;
        RootScale->Node.Scale=Scale;
        RootScale->Node.Alpha=1.f;
        auto* ToLocal=AddAnimationNode<UAnimGraphNode_ComponentToLocalSpace>(Graph,X+900,Y,TEXT("转回局部姿势"));
        auto* Channels=AddAnimationNode<UAnimGraphNode_BlendBoneByChannel>(Graph,X+1130,Y,TEXT("骨盆保留原位置，身体尺寸使用参考骨架"));
        Channels->BlendNode.TransformsSpace=BCS_ComponentSpace;
        FBlendBoneByChannelEntry Pelvis;
        Pelvis.SourceBone.BoneName=JumpPelvisBone;
        Pelvis.TargetBone.BoneName=JumpPelvisBone;
        Pelvis.bBlendTranslation=true;
        Pelvis.bBlendRotation=false;
        Pelvis.bBlendScale=false;
        Channels->BlendNode.BoneDefinitions={Pelvis};
        Channels->ReconstructNode();
        if (!HideJumpConstantPin(Channels,TEXT("Alpha"))) return false;
        Channels->BlendNode.Alpha=1.f;
        // 只替换跳跃输入，其他十个动作及家居混合继续走原图
        if (!Input || !ConnectPose(Graph,Clip,Cache) || !ConnectPose(Graph,ScaleInput,ToComponent) ||
            !ConnectPose(Graph,ToComponent,RootScale) || !ConnectPose(Graph,RootScale,ToLocal) ||
            !ConnectJumpPose(Graph,ToLocal,Channels,TEXT("A")) ||
            !ConnectJumpPose(Graph,PelvisInput,Channels,TEXT("B"))) return false;
        Input->BreakAllPinLinks();
        if (!ConnectNpcBodyPose(Graph,Channels,Blend,Index)) return false;
    }
    return JumpPinsMatch(Blend,Root,Scale);
}

bool AddBodyJumpPoses(UEdGraph* Graph,UAnimGraphNode_BlendListByInt* Blend)
{
    const TCHAR* Labels[]={TEXT("起跳"),TEXT("空中"),TEXT("落地")};
    const float TransitionTimes[]={.08f,.10f,.08f};
    for (int32 Index=FirstJumpPose;Index<=LastJumpPose;++Index) Blend->Node.AddPose();
    Blend->ReconstructNode();
    const auto* LastBase=LinkedNpcBodyInput(Blend,BlendPosePin(FirstJumpPose-1));
    if (!LastBase) return false;
    for (int32 Index=FirstJumpPose;Index<=LastJumpPose;++Index)
    {
        const int32 Phase=Index-FirstJumpPose;
        auto* BlendTime=Blend->FindPin(FName(*FString::Printf(TEXT("BlendTime_%d"),Index)),EGPD_Input);
        if (!BlendTime) return false;
        Graph->GetSchema()->TrySetDefaultValue(*BlendTime,FString::SanitizeFloat(TransitionTimes[Phase]));
        const int32 Y=LastBase->NodePosY+(Phase+1)*240;
        auto* Clip=AddAnimationNode<UAnimGraphNode_SequenceEvaluator>(Graph,LastBase->NodePosX,Y,Labels[Phase]);
        Clip->Node.SetGroupMethod(EAnimSyncMethod::DoNotSync);
        Clip->Node.SetShouldUseExplicitFrame(false);
        Clip->Node.SetShouldLoop(false);
        if (!SetNpcAnimationBool(FAnimNode_SequenceEvaluator::StaticStruct(),&Clip->Node,TEXT("bTeleportToExplicitTime"),true) ||
            !ConnectParameter(Graph,Clip,TEXT("Sequence"),JumpAssetProperties[Phase],Y) ||
            !ConnectParameter(Graph,Clip,TEXT("ExplicitTime"),JumpTimeProperties[Phase],Y+95) ||
            !ConnectNpcBodyPose(Graph,Clip,Blend,Index)) return false;
    }
    return true;
}
}

FString UDeliveryNpcAnimationAssetTools::CreateConversationAnimation(bool bReplaceGraph)
{
    const FString AnimationFolder=TEXT("/Game/NPC/Residents/Animation");
    const FString Name=TEXT("ABP_NpcConversation");
    auto* Man=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Residents/Appearance/SK_NpcManFace.SK_NpcManFace"));
    auto* Woman=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/NPC/Residents/Appearance/SK_NpcWomanFace.SK_NpcWomanFace"));
    auto* ManClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"));
    auto* WomanClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
    if (!Man || !Woman || !ManClass || !WomanClass || Man->GetSkeleton()!=Woman->GetSkeleton())
        return TEXT("Missing NPC face meshes or shared skeleton");
    FVector Front,Up,WomanFront,WomanUp;
    if (!GetHeadAxes(Man,ManClass->GetDefaultObject<ADeliveryNpcCharacter>()->GetMesh()->GetRelativeTransform(),Front,Up) ||
        !GetHeadAxes(Woman,WomanClass->GetDefaultObject<ADeliveryNpcCharacter>()->GetMesh()->GetRelativeTransform(),WomanFront,WomanUp) ||
        !Front.Equals(WomanFront,.01f) || !Up.Equals(WomanUp,.01f))
        return TEXT("NPC head axes differ; check the two meshes before sharing an animation graph");
    UE_LOG(LogTemp,Log,TEXT("NpcConversationAxes HeadForward=%s HeadUp=%s"),*Front.ToString(),*Up.ToString());
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*(AnimationFolder/Name+TEXT(".")+Name));
    if (Blueprint && Blueprint->ParentClass!=UDeliveryNpcConversationAnimInstance::StaticClass())
        return TEXT("Conversation animation blueprint uses another parent class");
    if (Blueprint && Blueprint->TargetSkeleton!=Man->GetSkeleton())
        return TEXT("Conversation animation blueprint uses another skeleton");
    if (!Blueprint)
    {
        auto* Factory=NewObject<UAnimBlueprintFactory>();
        Factory->ParentClass=UDeliveryNpcConversationAnimInstance::StaticClass();
        Factory->TargetSkeleton=Man->GetSkeleton();
        Factory->PreviewSkeletalMesh=Man;
        Blueprint=Cast<UAnimBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            Name,AnimationFolder,UAnimBlueprint::StaticClass(),Factory));
        bReplaceGraph=true;
    }
    if (!Blueprint) return TEXT("Could not create NPC conversation animation blueprint");
    if (bReplaceGraph)
    {
        UEdGraph* Graph=nullptr;
        for (UEdGraph* Candidate:Blueprint->FunctionGraphs)
            if (Candidate && Candidate->GetFName()==TEXT("AnimGraph")) Graph=Candidate;
        if (!Graph) return TEXT("Conversation animation graph is missing");
        Blueprint->Modify();
        Graph->Modify();
        const auto Nodes=Graph->Nodes;
        for (UEdGraphNode* Node:Nodes) FBlueprintEditorUtils::RemoveNode(Blueprint,Node,true);
        auto* Input=AddAnimationNode<UAnimGraphNode_LinkedInputPose>(Graph,0,0,TEXT("接收角色正在播放的动作"));
        auto* ToComponent=AddAnimationNode<UAnimGraphNode_LocalToComponentSpace>(Graph,280,0,TEXT("转为网格坐标"));
        auto* Chest=AddAnimationNode<UAnimGraphNode_ModifyBone>(Graph,600,0,TEXT("倾听时身体稍微前倾"));
        Chest->Node.BoneToModify.BoneName=TEXT("Spine2");
        Chest->Node.RotationMode=BMM_Additive;
        Chest->Node.RotationSpace=BCS_ComponentSpace;
        auto* Look=AddAnimationNode<UAnimGraphNode_LookAt>(Graph,1000,0,TEXT("头部平滑看向说话者"));
        Look->Node.BoneToModify.BoneName=TEXT("Head");
        Look->Node.LookAt_Axis.Axis=Front;
        Look->Node.LookAt_Axis.bInLocalSpace=true;
        Look->Node.LookUp_Axis.Axis=Up;
        Look->Node.LookUp_Axis.bInLocalSpace=true;
        Look->Node.bUseLookUpAxis=true;
        Look->Node.InterpolationTime=0;
        auto* Tilt=AddAnimationNode<UAnimGraphNode_ModifyBone>(Graph,1400,0,TEXT("倾听和思考时头部轻微侧倾"));
        Tilt->Node.BoneToModify.BoneName=TEXT("Head");
        Tilt->Node.RotationMode=BMM_Additive;
        Tilt->Node.RotationSpace=BCS_ComponentSpace;
        auto* ToLocal=AddAnimationNode<UAnimGraphNode_ComponentToLocalSpace>(Graph,1780,0,TEXT("转回骨骼局部坐标"));
        auto* Output=AddAnimationNode<UAnimGraphNode_Root>(Graph,2080,0,TEXT("保留原动作并叠加交谈姿态"));
        const bool bParameters=ConnectParameter(Graph,Chest,TEXT("Rotation"),TEXT("ChestRotation"),280) &&
            ConnectParameter(Graph,Chest,TEXT("Alpha"),TEXT("PostureWeight"),360) &&
            ConnectParameter(Graph,Look,TEXT("LookAtLocation"),TEXT("LookTargetWorld"),280) &&
            ConnectParameter(Graph,Look,TEXT("Alpha"),TEXT("LookWeight"),360) &&
            ConnectParameter(Graph,Look,TEXT("LookAtClamp"),TEXT("HeadTurnLimit"),440) &&
            ConnectParameter(Graph,Tilt,TEXT("Rotation"),TEXT("HeadRotation"),280) &&
            ConnectParameter(Graph,Tilt,TEXT("Alpha"),TEXT("PostureWeight"),360);
        const bool bPoses=ConnectPose(Graph,Input,ToComponent) && ConnectPose(Graph,ToComponent,Chest) &&
            ConnectPose(Graph,Chest,Look) && ConnectPose(Graph,Look,Tilt) && ConnectPose(Graph,Tilt,ToLocal) &&
            ConnectPose(Graph,ToLocal,Output);
        if (!bParameters || !bPoses) return TEXT("Conversation graph pins could not be connected");
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status==BS_Error || !Blueprint->GeneratedClass) return TEXT("Conversation animation compilation failed");
    if (!SaveNpcAnimationAsset(Blueprint)) return TEXT("Conversation animation save failed");
    for (auto* Mesh:{Man,Woman})
    {
        Mesh->Modify();
        Mesh->SetPostProcessAnimBlueprint(TSubclassOf<UAnimInstance>(Blueprint->GeneratedClass.Get()));
        if (!SaveNpcAnimationAsset(Mesh)) return TEXT("NPC face mesh save failed");
    }
    return FString::Printf(TEXT("NPC conversation animation saved; HeadForward=%s HeadUp=%s"),*Front.ToString(),*Up.ToString());
}

FString UDeliveryNpcAnimationAssetTools::CreateBodyAnimation(bool bReplaceGraph)
{
    const FString NpcAnimationFolder=TEXT("/Game/NPC/Residents/Animation");
    const FString Name=TEXT("ABP_NpcBody");
    const TCHAR* NpcPaths[]={TEXT("/Game/NPC/Residents/BP_ResidentLoud"),TEXT("/Game/NPC/Residents/BP_ResidentShrewd")};
    TArray<UBlueprint*> NpcBlueprints;
    USkeletalMesh* Preview=nullptr;
    USkeleton* Skeleton=nullptr;
    for (const TCHAR* Path:NpcPaths)
    {
        auto* NpcBP=LoadObject<UBlueprint>(nullptr,Path);
        auto* Npc=NpcBP && NpcBP->GeneratedClass?Cast<ADeliveryNpcCharacter>(NpcBP->GeneratedClass->GetDefaultObject()):nullptr;
        auto* Mesh=Npc?Npc->GetMesh()->GetSkeletalMeshAsset():nullptr;
        if (!Mesh || !Mesh->GetSkeleton()) return FString::Printf(TEXT("Missing NPC mesh or skeleton: %s"),Path);
        if (Skeleton && Skeleton!=Mesh->GetSkeleton()) return TEXT("NPC body animation requires the existing shared skeleton");
        if (!Mesh->GetPostProcessAnimBlueprint()) return TEXT("NPC conversation post-process animation is missing");
        if (!Preview) Preview=Mesh;
        Skeleton=Mesh->GetSkeleton();
        NpcBlueprints.Add(NpcBP);
    }
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*(NpcAnimationFolder/Name+TEXT(".")+Name));
    if (Blueprint && (Blueprint->ParentClass!=UDeliveryNpcBodyAnimInstance::StaticClass() || Blueprint->TargetSkeleton!=Skeleton))
        return TEXT("Existing body animation has another parent or skeleton; graph was not changed");
    if (!Blueprint)
    {
        auto* Factory=NewObject<UAnimBlueprintFactory>();
        Factory->ParentClass=UDeliveryNpcBodyAnimInstance::StaticClass();
        Factory->TargetSkeleton=Skeleton;
        Factory->PreviewSkeletalMesh=Preview;
        Blueprint=Cast<UAnimBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            Name,NpcAnimationFolder,UAnimBlueprint::StaticClass(),Factory));
        bReplaceGraph=true;
    }
    if (!Blueprint) return TEXT("Could not create NPC body animation blueprint");
    if (bReplaceGraph)
    {
        UEdGraph* Graph=nullptr;
        for (UEdGraph* Candidate:Blueprint->FunctionGraphs)
            if (Candidate && Candidate->GetFName()==TEXT("AnimGraph")) Graph=Candidate;
        if (!Graph) return TEXT("Body animation graph is missing");
        Blueprint->Modify(); Graph->Modify();
        const auto OldNodes=Graph->Nodes;
        for (UEdGraphNode* Node:OldNodes) FBlueprintEditorUtils::RemoveNode(Blueprint,Node,true);
        auto* Blend=AddAnimationNode<UAnimGraphNode_BlendListByInt>(Graph,800,640,TEXT("按动作编号混合，过渡时间可在此修改"));
        auto* TimesProperty=FindFProperty<FArrayProperty>(FAnimNode_BlendListBase::StaticStruct(),TEXT("BlendTime"));
        auto* TimeValue=TimesProperty?CastField<FFloatProperty>(TimesProperty->Inner):nullptr;
        if (!TimeValue) return TEXT("UE blend-time property is unavailable");
        FScriptArrayHelper Times(TimesProperty,TimesProperty->ContainerPtrToValuePtr<void>(&Blend->Node));
        // 先补齐输入，完整图连接后再编译，避免编辑器在缺少输出节点时反复编译
        while (Times.Num()<10) Blend->Node.AddPose();
        if (Times.Num()!=10) return TEXT("Body animation requires ten blend inputs");
        const float Durations[]={.16f,.16f,.16f,.16f,.16f,.16f,.06f,.08f,.16f,.06f};
        for (int32 Index=0;Index<10;++Index) TimeValue->SetPropertyValue(Times.GetRawPtr(Index),Durations[Index]);
        Blend->ReconstructNode();
        for (int32 Index=0;Index<10;++Index)
        {
            auto* Pin=Blend->FindPin(FName(*FString::Printf(TEXT("BlendTime_%d"),Index)),EGPD_Input);
            if (!Pin) return TEXT("Body animation blend-time pin is missing");
            Graph->GetSchema()->TrySetDefaultValue(*Pin,FString::SanitizeFloat(Durations[Index]));
        }
        if (!ConnectParameter(Graph,Blend,TEXT("ActiveChildIndex"),TEXT("PoseIndex"),540))
            return TEXT("Could not connect body pose index");
        const FName Assets[]={TEXT("IdleAnimation"),TEXT("TalkAnimation"),TEXT("WalkAnimation"),TEXT("PaceAnimation"),
            TEXT("HurryAnimation"),TEXT("HoldAnimation"),TEXT("AttackAnimation"),TEXT("DownAnimation"),TEXT("GetUpAnimation"),TEXT("HitAnimation")};
        const TCHAR* Labels[]={TEXT("0 站立"),TEXT("1 说话"),TEXT("2 行走"),TEXT("3 踱步"),TEXT("4 快步靠近"),
            TEXT("5 拽车"),TEXT("6 挥拳：按服务器时间显示"),TEXT("7 倒地：按游戏状态显示"),TEXT("8 起身：按游戏状态显示"),TEXT("9 受击：按服务器时间显示")};
        const FName ExplicitTimes[]={TEXT("StrikeTime"),TEXT("DownTime"),TEXT("GetUpTime"),TEXT("HitTime")};
        for (int32 Index=0;Index<10;++Index)
        {
            const int32 Y=Index*240;
            UAnimGraphNode_Base* Node=nullptr;
            if (Index<6)
            {
                auto* Player=AddAnimationNode<UAnimGraphNode_SequencePlayer>(Graph,200,Y,Labels[Index]);
                Player->Node.SetLoopAnimation(true);
                if (Index>=2 && Index<=4)
                {
                    Player->Node.SetGroupName(TEXT("NpcLocomotion"));
                    Player->Node.SetGroupRole(EAnimGroupRole::CanBeLeader);
                    Player->Node.SetGroupMethod(EAnimSyncMethod::SyncGroup);
                    if (!ConnectParameter(Graph,Player,TEXT("PlayRate"),TEXT("MovePlayRate"),Y+95))
                        return TEXT("Could not connect movement play rate");
                }
                Node=Player;
            }
            else
            {
                auto* Evaluator=AddAnimationNode<UAnimGraphNode_SequenceEvaluator>(Graph,200,Y,Labels[Index]);
                // Evaluator 加入同步组会强制推进时间，这些节点按服务器时间显示，不参与步态同步
                Evaluator->Node.SetGroupName(NAME_None);
                Evaluator->Node.SetGroupMethod(EAnimSyncMethod::DoNotSync);
                Evaluator->Node.SetShouldUseExplicitFrame(false);
                Evaluator->Node.SetShouldLoop(false);
                if (!SetNpcAnimationBool(FAnimNode_SequenceEvaluator::StaticStruct(),&Evaluator->Node,TEXT("bTeleportToExplicitTime"),true) ||
                    !ConnectParameter(Graph,Evaluator,TEXT("ExplicitTime"),ExplicitTimes[Index-6],Y+95))
                    return TEXT("Could not configure explicit animation time");
                Node=Evaluator;
            }
            if (!ConnectParameter(Graph,Node,TEXT("Sequence"),Assets[Index],Y) || !ConnectNpcBodyPose(Graph,Node,Blend,Index))
                return FString::Printf(TEXT("Could not connect NPC animation %s"),*Assets[Index].ToString());
        }
        auto* Output=AddAnimationNode<UAnimGraphNode_Root>(Graph,1200,640,TEXT("主动作完成后继续应用现有交谈后处理图"));
        FName JumpRoot;
        FVector JumpScale;
        if (!JumpReferenceScaleForBody(Blueprint,JumpRoot,JumpScale) || !ConnectPose(Graph,Blend,Output) ||
            !AddBodyJumpPoses(Graph,Blend) || !CorrectBodyJumpPoses(Graph,Blend,JumpRoot,JumpScale))
            return TEXT("Could not connect body animation output or scale-corrected jump poses");
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }
    const FString GraphCheck=CheckBodyAnimation();
    if (!GraphCheck.StartsWith(TEXT("NPC body graph checked;"))) return GraphCheck;
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave);
    if (Blueprint->Status==BS_Error || !Blueprint->GeneratedClass) return TEXT("Body animation blueprint compilation failed");
    auto* Defaults=Blueprint->GeneratedClass->GetDefaultObject<UDeliveryNpcBodyAnimInstance>();
    const auto* PreviewSettings=CastChecked<ADeliveryNpcCharacter>(NpcBlueprints[0]->GeneratedClass->GetDefaultObject())->Presentation.Get();
    // 编辑器预览使用正式居民的动作；游戏实例仍从各自角色的表现配置读取
    Defaults->IdleAnimation=PreviewSettings->IdleAnimation;
    Defaults->TalkAnimation=PreviewSettings->TalkAnimation;
    Defaults->WalkAnimation=PreviewSettings->WalkAnimation;
    Defaults->PaceAnimation=PreviewSettings->PaceAnimation;
    Defaults->HurryAnimation=PreviewSettings->HurryAnimation;
    Defaults->HoldAnimation=PreviewSettings->HoldAnimation;
    Defaults->AttackAnimation=PreviewSettings->AttackAnimation;
    Defaults->DownAnimation=PreviewSettings->DownAnimation;
    Defaults->GetUpAnimation=PreviewSettings->GetUpAnimation;
    Defaults->HitAnimation=PreviewSettings->HitAnimation;
    Defaults->SetRootMotionMode(ERootMotionMode::NoRootMotionExtraction);
    if (!SaveNpcAnimationAsset(Blueprint)) return TEXT("Body animation blueprint save failed");

    // 只写蓝图组件模板的两个动画字段，不初始化 CDO 的运行实例，也不改网格或后处理图
    auto* ModeProperty=FindFProperty<FByteProperty>(USkeletalMeshComponent::StaticClass(),TEXT("AnimationMode"));
    auto* ClassProperty=FindFProperty<FObjectPropertyBase>(USkeletalMeshComponent::StaticClass(),TEXT("AnimClass"));
    if (!ModeProperty || !ClassProperty) return TEXT("NPC mesh animation properties are unavailable");
    for (UBlueprint* NpcBP:NpcBlueprints)
    {
        auto* Npc=CastChecked<ADeliveryNpcCharacter>(NpcBP->GeneratedClass->GetDefaultObject());
        auto* Mesh=Npc->GetMesh();
        USkeletalMesh* OriginalMesh=Mesh->GetSkeletalMeshAsset();
        const auto OriginalPostProcess=OriginalMesh->GetPostProcessAnimBlueprint();
        const auto OriginalTransform=Mesh->GetRelativeTransform();
        const auto OriginalMaterials=Mesh->GetMaterials();
        NpcBP->Modify(); Npc->Modify(); Mesh->Modify();
        ModeProperty->SetPropertyValue_InContainer(Mesh,EAnimationMode::AnimationBlueprint);
        ClassProperty->SetObjectPropertyValue_InContainer(Mesh,Blueprint->GeneratedClass);
        FBlueprintEditorUtils::MarkBlueprintAsModified(NpcBP);
        FKismetEditorUtilities::CompileBlueprint(NpcBP,EBlueprintCompileOptions::SkipSave);
        if (NpcBP->Status==BS_Error) return TEXT("NPC blueprint compilation failed after assigning body animation");
        Mesh=CastChecked<ADeliveryNpcCharacter>(NpcBP->GeneratedClass->GetDefaultObject())->GetMesh();
        if (Mesh->GetAnimClass()!=Blueprint->GeneratedClass || Mesh->GetAnimationMode()!=EAnimationMode::AnimationBlueprint ||
            Mesh->GetSkeletalMeshAsset()!=OriginalMesh || OriginalMesh->GetPostProcessAnimBlueprint()!=OriginalPostProcess ||
            !Mesh->GetRelativeTransform().Equals(OriginalTransform) || Mesh->GetMaterials()!=OriginalMaterials)
            return TEXT("NPC body animation binding or preserved mesh settings did not match");
        if (!SaveNpcAnimationAsset(NpcBP)) return TEXT("NPC blueprint save failed after assigning body animation");
    }
    return FString::Printf(TEXT("NPC body animation saved; graph=%s; rebuilt=%d; poses=10; locomotion_group=NpcLocomotion"),
        *Blueprint->GetPathName(),bReplaceGraph);
}

FString UDeliveryNpcAnimationAssetTools::CheckBodyAnimation()
{
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,TEXT("/Game/NPC/Residents/Animation/ABP_NpcBody.ABP_NpcBody"));
    if (!Blueprint || Blueprint->ParentClass!=UDeliveryNpcBodyAnimInstance::StaticClass())
        return TEXT("Body animation blueprint or expected parent is missing");
    UAnimGraphNode_BlendListByInt* Blend=nullptr;
    for (UEdGraph* Graph:Blueprint->FunctionGraphs)
        if (Graph && Graph->GetFName()==TEXT("AnimGraph"))
            for (UEdGraphNode* Node:Graph->Nodes)
                if (auto* Candidate=Cast<UAnimGraphNode_BlendListByInt>(Node))
                {
                    if (Blend) return TEXT("Body animation has more than one pose selector");
                    Blend=Candidate;
                }
    auto* TimesProperty=FindFProperty<FArrayProperty>(FAnimNode_BlendListBase::StaticStruct(),TEXT("BlendTime"));
    if (!Blend || !TimesProperty) return TEXT("Body animation pose selector is missing");
    FScriptArrayHelper Times(TimesProperty,TimesProperty->ContainerPtrToValuePtr<void>(&Blend->Node));
    FName JumpRoot;
    FVector JumpScale;
    if (Times.Num()!=FirstJumpPose && (Times.Num()!=LastJumpPose+1 ||
        !JumpReferenceScaleForBody(Blueprint,JumpRoot,JumpScale) || !JumpPinsMatch(Blend,JumpRoot,JumpScale)))
        return TEXT("Body animation base or scale-corrected jump pose inputs are invalid");
    for (int32 Index=0;Index<10;++Index)
    {
        const auto* Pin=Blend->FindPin(FName(*FString::Printf(TEXT("BlendPose_%d"),Index)),EGPD_Input);
        if (!Pin || Pin->LinkedTo.Num()!=1) return TEXT("Body animation pose input is not connected");
    }
    const auto* HitPin=Blend->FindPin(TEXT("BlendPose_9"),EGPD_Input);
    const auto* Hit=Cast<UAnimGraphNode_SequenceEvaluator>(HitPin->LinkedTo[0]->GetOwningNode());
    if (!Hit) return TEXT("Body animation hit input requires a Sequence Evaluator");
    for (const auto& Pair:{TPair<FName,FName>(TEXT("Sequence"),TEXT("HitAnimation")),
        TPair<FName,FName>(TEXT("ExplicitTime"),TEXT("HitTime"))})
    {
        const auto* Pin=Hit->FindPin(Pair.Key,EGPD_Input);
        const auto* Get=Pin && Pin->LinkedTo.Num()==1?Cast<UK2Node_VariableGet>(Pin->LinkedTo[0]->GetOwningNode()):nullptr;
        if (!Get || Get->VariableReference.GetMemberName()!=Pair.Value)
            return TEXT("Body animation hit asset or time is not connected to the expected variable");
    }
    return FString::Printf(TEXT("NPC body graph checked; poses=10; hit_animation=HitAnimation; hit_time=HitTime; jump_poses=%d"),
        Times.Num()-FirstJumpPose);
}

FString UDeliveryNpcAnimationAssetTools::ConfigureBodyJumpAnimation(const FString& BodyPath,UAnimSequence* Start,UAnimSequence* Loop,
    UAnimSequence* Land,bool bApply)
{
    if (GEditor && GEditor->PlayWorld) return TEXT("Stop PIE before preparing a body animation graph");
    if (!IsNpcJumpBodyPath(BodyPath)) return TEXT("Unknown NPC body animation graph");
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*BodyPath);
    if (!Blueprint || !Blueprint->ParentClass || !Blueprint->ParentClass->IsChildOf(UDeliveryNpcBodyAnimInstance::StaticClass()) ||
        !Blueprint->TargetSkeleton || !Blueprint->GeneratedClass || Blueprint->GetPackage()->IsDirty())
        return TEXT("Body animation blueprint is missing, incompatible or has unsaved edits");
    for (const auto* Clip:{Start,Loop,Land})
        if (!IsValidNpcJumpClip(Clip,Blueprint->TargetSkeleton))
            return TEXT("Jump clips must be in-place animations on the body graph skeleton");
    FName JumpRoot;
    FVector JumpScale;
    if (!JumpReferenceScaleForBody(Blueprint,JumpRoot,JumpScale))
        return TEXT("NPC meshes must share the expected reference root and pelvis scale layout");

    auto* Blend=FindNativeBodyPoseSelector(Blueprint);
    const auto* Times=FindFProperty<FArrayProperty>(FAnimNode_BlendListBase::StaticStruct(),TEXT("BlendTime"));
    if (!Times || !Blend) return TEXT("Native body pose selector is missing");
    const int32 PoseCount=FScriptArrayHelper(Times,Times->ContainerPtrToValuePtr<void>(&Blend->Node)).Num();
    const bool bAddInputs=PoseCount==FirstJumpPose;
    if (!bAddInputs && (PoseCount!=LastJumpPose+1 || !JumpPinsMatch(Blend,JumpRoot,JumpScale,true)))
        return TEXT("Existing jump inputs have unknown connections");
    const bool bFixInputs=bAddInputs || !JumpPinsMatch(Blend,JumpRoot,JumpScale);
    for (int32 Index=0;Index<FirstJumpPose;++Index)
        if (!LinkedNpcBodyInput(Blend,BlendPosePin(Index))) return TEXT("Existing base pose is disconnected");

    const auto* Defaults=Blueprint->GeneratedClass->GetDefaultObject<UDeliveryNpcBodyAnimInstance>();
    if (!Defaults || (Defaults->JumpStartAnimation && Defaults->JumpStartAnimation!=Start) ||
        (Defaults->JumpLoopAnimation && Defaults->JumpLoopAnimation!=Loop) || (Defaults->JumpLandAnimation && Defaults->JumpLandAnimation!=Land))
        return TEXT("Existing jump clips differ; their settings were preserved");
    if (!bApply) return TEXT("NPC jump graph preflight passed");
    if (!bFixInputs && Defaults->JumpStartAnimation==Start && Defaults->JumpLoopAnimation==Loop && Defaults->JumpLandAnimation==Land)
        return TEXT("NPC jump graph ready; reference scale and pelvis verified; existing asset preserved");

    if (bFixInputs)
    {
        auto* Graph=Blend->GetGraph();
        Blueprint->Modify(); Graph->Modify(); Blend->Modify();
        if ((bAddInputs && !AddBodyJumpPoses(Graph,Blend)) || !CorrectBodyJumpPoses(Graph,Blend,JumpRoot,JumpScale))
            return TEXT("Scale-corrected jump pose connections failed before save");
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipSave);
        if (Blueprint->Status==BS_Error || !Blueprint->GeneratedClass || !JumpPinsMatch(Blend,JumpRoot,JumpScale))
            return TEXT("Jump animation blueprint compilation failed before save");
    }
    auto* Updated=Blueprint->GeneratedClass->GetDefaultObject<UDeliveryNpcBodyAnimInstance>();
    if (!Updated) return TEXT("Jump animation blueprint defaults are unavailable");
    Updated->Modify();
    Updated->JumpStartAnimation=Start;
    Updated->JumpLoopAnimation=Loop;
    Updated->JumpLandAnimation=Land;
    return SaveNpcAnimationAsset(Blueprint)?TEXT("NPC jump graph ready; reference scale and pelvis verified; asset saved"):
        TEXT("Could not save the NPC jump graph");
}

FString UDeliveryNpcAnimationAssetTools::CheckBodyJumpAnimation(const FString& BodyPath)
{
    if (!IsNpcJumpBodyPath(BodyPath)) return TEXT("Unknown NPC body animation graph");
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*BodyPath);
    FName Root;
    FVector Scale;
    if (!JumpReferenceScaleForBody(Blueprint,Root,Scale) || !Blueprint->GeneratedClass || Blueprint->Status==BS_Error)
        return TEXT("Body blueprint or official reference scale is unavailable");
    const auto* Blend=FindNativeBodyPoseSelector(Blueprint);
    const auto* Times=FindFProperty<FArrayProperty>(FAnimNode_BlendListBase::StaticStruct(),TEXT("BlendTime"));
    if (!Blend || !Times || FScriptArrayHelper(Times,Times->ContainerPtrToValuePtr<void>(&Blend->Node)).Num()!=LastJumpPose+1 ||
        !JumpPinsMatch(Blend,Root,Scale))
        return TEXT("Body blueprint is missing the three scale-corrected jump inputs");
    for (int32 Index=0;Index<FirstJumpPose;++Index)
        if (!LinkedNpcBodyInput(Blend,BlendPosePin(Index))) return TEXT("An original body pose is disconnected");
    const auto* Defaults=Cast<UDeliveryNpcBodyAnimInstance>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!Defaults || !IsValidNpcJumpClip(Defaults->JumpStartAnimation,Blueprint->TargetSkeleton) ||
        !IsValidNpcJumpClip(Defaults->JumpLoopAnimation,Blueprint->TargetSkeleton) ||
        !IsValidNpcJumpClip(Defaults->JumpLandAnimation,Blueprint->TargetSkeleton))
        return TEXT("Body blueprint has missing or incompatible jump clips");
    return FString::Printf(TEXT("NPC jump graph checked; poses=%d; root=%s; scale=%s; hips=component translation"),
        LastJumpPose+1,*Root.ToString(),*Scale.ToString());
}
