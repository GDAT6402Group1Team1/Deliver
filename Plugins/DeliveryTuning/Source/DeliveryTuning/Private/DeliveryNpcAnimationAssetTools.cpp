#include "DeliveryNpcAnimationAssetTools.h"
#include "AI/Presentation/DeliveryNpcConversationAnimInstance.h"
#include "AI/Presentation/DeliveryNpcBodyAnimInstance.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
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
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
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
    const FString Folder=TEXT("/Game/NPC/Residents/Animation");
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
    auto* Blueprint=LoadObject<UAnimBlueprint>(nullptr,*(Folder/Name+TEXT(".")+Name));
    if (Blueprint && (Blueprint->ParentClass!=UDeliveryNpcBodyAnimInstance::StaticClass() || Blueprint->TargetSkeleton!=Skeleton))
        return TEXT("Existing body animation has another parent or skeleton; graph was not changed");
    if (!Blueprint)
    {
        auto* Factory=NewObject<UAnimBlueprintFactory>();
        Factory->ParentClass=UDeliveryNpcBodyAnimInstance::StaticClass();
        Factory->TargetSkeleton=Skeleton;
        Factory->PreviewSkeletalMesh=Preview;
        Blueprint=Cast<UAnimBlueprint>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAsset(
            Name,Folder,UAnimBlueprint::StaticClass(),Factory));
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
        if (!ConnectPose(Graph,Blend,Output)) return TEXT("Could not connect body animation output");
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
    if (Times.Num()!=10) return TEXT("Body animation requires ten inputs; rebuild with -ReplaceNpcBodyAnimation");
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
    return TEXT("NPC body graph checked; poses=10; hit_animation=HitAnimation; hit_time=HitTime");
}
