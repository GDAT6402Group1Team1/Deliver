#include "DeliveryNpcAnimationAssetTools.h"
#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
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
#include "AnimGraphNode_LookAt.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

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
}

FString UDeliveryNpcAnimationAssetTools::CreateConversationAnimation(bool bReplaceGraph)
{
    const FString Folder=TEXT("/Game/NPC/Residents/Animation");
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
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*(Folder/Name+TEXT(".")+Name));
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
            Name,Folder,UAnimBlueprint::StaticClass(),Factory));
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
