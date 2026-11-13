#include "DeliveryNpcToolset.h"
#include "DeliveryConversationAssetTools.h"
#include "AI/BehaviorTree/BTTask_DeliveryListen.h"
#include "AI/Presentation/DeliveryNpcExpressionSet.h"
#include "Curves/CurveFloat.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Builders/CubeBuilder.h"
#include "ActorFactories/ActorFactory.h"
#include "Components/BrushComponent.h"
#include "Model.h"
#include "Engine/Polys.h"
#include "NavigationData.h"
#include "AssetCompilingManager.h"
#include "UObject/UObjectGlobals.h"
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Components/CapsuleComponent.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Characters/DeliveryNpcProfile.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Conversation/DeliveryConversationComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "TimerManager.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshModel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Settings/LevelEditorPlaySettings.h"

namespace
{
struct FNpcEditorProfile
{
    FDataTableRowHandle Row;
};

// JSON 只用于首次导入，创建蓝图和游戏运行都读取正式人物表
bool ReadNpcProfiles(TMap<FString,FNpcEditorProfile>& Profiles)
{
    auto* Table=LoadObject<UDataTable>(nullptr,TEXT("/Game/NPC/Residents/Config/DT_NpcProfiles"));
    if (!Table || Table->GetRowStruct()!=FDeliveryNpcProfileRow::StaticStruct()) return false;
    for (int32 Index=0;Index<2;++Index)
    {
        const FName RowName=Index==0?TEXT("chunlai"):TEXT("xiumei");
        const auto* Row=Table->FindRow<FDeliveryNpcProfileRow>(RowName,TEXT("CreateNpcAssets"),false);
        if (!Row || Row->DisplayName.IsEmpty() || Row->Persona.IsEmpty() || Row->Persona.Len()>1024) return false;
        FNpcEditorProfile Profile;Profile.Row.DataTable=Table;Profile.Row.RowName=RowName;
        Profiles.Add(Index==0?TEXT("/Game/NPC/Residents/BP_ResidentLoud"):TEXT("/Game/NPC/Residents/BP_ResidentShrewd"),MoveTemp(Profile));
    }
    return Profiles.Contains(TEXT("/Game/NPC/Residents/BP_ResidentLoud")) &&
        Profiles.Contains(TEXT("/Game/NPC/Residents/BP_ResidentShrewd"));
}

bool SaveResidentAsset(UObject* Asset)
{
    if (!Asset) return false;
    Asset->MarkPackageDirty(); FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(Asset->GetPackage(),Asset,*FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}
template<typename T> T* ResidentAsset(const FString& Path)
{
    if (auto* Existing=LoadObject<T>(nullptr,*Path)) return Existing;
    auto* Asset=NewObject<T>(CreatePackage(*Path),*FPackageName::GetLongPackageAssetName(Path),RF_Public|RF_Standalone|RF_Transactional);
    FAssetRegistryModule::AssetCreated(Asset); return Asset;
}
UBlueprint* ResidentBlueprint(UClass* Parent,const FString& Path)
{
    if (auto* Existing=LoadObject<UBlueprint>(nullptr,*Path)) return Existing;
    auto* BP=FKismetEditorUtilities::CreateBlueprint(Parent,CreatePackage(*Path),*FPackageName::GetLongPackageAssetName(Path),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
    FAssetRegistryModule::AssetCreated(BP); return BP;
}
UAnimSequence* ImportResidentAnimation(const FString& Name,USkeletalMesh* Mesh)
{
    const FString Path=TEXT("/Game/NPC/Residents/Animation/")+Name;
    if (auto* Existing=LoadObject<UAnimSequence>(nullptr,*Path)) return Existing;
    const FString File=FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()/TEXT("Design/Art/NPC/Residents")/(Name+TEXT(".fbx")));
    if (!FPaths::FileExists(File)) return nullptr;
    auto* Task=NewObject<UAssetImportTask>(); Task->Filename=File; Task->DestinationPath=TEXT("/Game/NPC/Residents/Animation"); Task->DestinationName=Name;
    Task->bAutomated=true; Task->bSave=true; Task->bReplaceExisting=false; Task->Factory=NewObject<UFbxFactory>();
    auto* Options=NewObject<UFbxImportUI>(); Options->bAutomatedImportShouldDetectType=false;
    Options->MeshTypeToImport=FBXIT_Animation; Options->bImportMesh=false; Options->bImportAsSkeletal=true; Options->bImportAnimations=true;
    Options->bImportMaterials=false; Options->bImportTextures=false; Options->Skeleton=Mesh->GetSkeleton();
    Options->AnimSequenceImportData->bUseDefaultSampleRate=true;
    if (auto* Data=Cast<UFbxSkeletalMeshImportData>(Mesh->GetAssetImportData())) Options->AnimSequenceImportData->ImportUniformScale=Data->ImportUniformScale;
    Task->Options=Options;
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task});
    auto* Animation=LoadObject<UAnimSequence>(nullptr,*Path);
    if (Animation) { Animation->bEnableRootMotion=false; SaveResidentAsset(Animation); }
    return Animation;
}

void RebuildResidentGraph(UBehaviorTree* Tree)
{
    auto* Root = Tree->RootNode.Get();
    auto* Blackboard = Tree->BlackboardAsset.Get();
    if (Tree->BTGraph)
    {
        Tree->BTGraph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        Tree->BTGraph = nullptr;
    }
    Tree->BTGraph = FBlueprintEditorUtils::CreateNewGraph(Tree, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());
    auto* Graph = CastChecked<UBehaviorTreeGraph>(Tree->BTGraph);
    Graph->LockUpdates();
    Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
    // 新建图的 Root 会自动挑选第一个已加载黑板，必须恢复本树显式指定的黑板
    for (UEdGraphNode* Node : Graph->Nodes)
        if (auto* GraphRoot=Cast<UBehaviorTreeGraphNode_Root>(Node))
        { GraphRoot->BlackboardAsset=Blackboard; GraphRoot->UpdateBlackboard(); }
    // 无界面命令行没有 Slate 节点控件，不能调用依赖控件尺寸的 AutoArrange
    Graph->OnCreated(); Graph->Initialize();
    for (UEdGraphNode* Raw : Graph->Nodes)
    {
        auto* Node = Cast<UBehaviorTreeGraphNode>(Raw);
        if (!Node) continue;
        if (Node->NodeInstance) Node->ClassData = FGraphNodeClassData(Node->NodeInstance->GetClass(), FString());
        for (UAIGraphNode* Sub : Node->SubNodes)
            if (Sub && Sub->NodeInstance) Sub->ClassData = FGraphNodeClassData(Sub->NodeInstance->GetClass(), FString());
        if (!Node->NodeInstance) { Node->NodePosX = 1900; Node->NodePosY = 0; }
        if (Node->NodeInstance == Root) { Node->NodePosX = 1900; Node->NodePosY = 150; }
        for (int32 BranchIndex = 0; BranchIndex < Root->Children.Num(); ++BranchIndex)
        {
            auto* Seq = Root->Children[BranchIndex].ChildComposite.Get();
            if (!Seq)
            {
                if (Node->NodeInstance==Root->Children[BranchIndex].ChildTask)
                { Node->NodePosX=BranchIndex*1400+400; Node->NodePosY=350; }
                continue;
            }
            if (Node->NodeInstance == Seq) { Node->NodePosX = BranchIndex*1400+400; Node->NodePosY = 350; }
            for (int32 ChildIndex = 0; ChildIndex < Seq->Children.Num(); ++ChildIndex)
                if (Node->NodeInstance == Seq->GetChildNode(ChildIndex))
                { Node->NodePosX = BranchIndex*1400+ChildIndex*330; Node->NodePosY = 650; }
        }
    }
    // 生成装饰器期间 AddSubNode 会请求 UpdateAsset，必须等实例全部绑定后才能重建运行树
    Graph->UnlockUpdates();
    Graph->UpdateAsset();
}

}

FString UDeliveryNpcToolset::RebuildConversationWidget()
{
    if (!GEditor || GEditor->PlayWorld) return TEXT("退出 PIE 后再更新交谈界面");
    return UDeliveryConversationAssetTools::CreateConversationWidget(true);
}

FString UDeliveryNpcToolset::UpdateNpcConversationAssets()
{
    const FString Appearance=TEXT("/Game/NPC/Residents/Appearance/");
    auto* Expressions=LoadObject<UDeliveryNpcExpressionSet>(nullptr,*(Appearance+TEXT("DA_NpcExpressions")));
    if (!Expressions)
    {
        Expressions=ResidentAsset<UDeliveryNpcExpressionSet>(Appearance+TEXT("DA_NpcExpressions"));
        auto* Blink=ResidentAsset<UCurveFloat>(Appearance+TEXT("C_NpcBlink"));
        for (const FVector2D Key:{FVector2D(0,0),FVector2D(.28,1),FVector2D(.4,.86),FVector2D(.7,.2),FVector2D(1,0)})
            Blink->FloatCurve.AddKey(Key.X,Key.Y);
        for (auto It=Blink->FloatCurve.GetKeyHandleIterator();It;++It) Blink->FloatCurve.SetKeyInterpMode(*It,RCIM_Cubic);
        Blink->FloatCurve.AutoSetTangents(); Expressions->BlinkCurve=Blink;
        for (const auto& Pair:TMap<FName,FName>{{TEXT("Angry"),TEXT("FaceAngry")},{TEXT("Surprised"),TEXT("FaceSurprised")},
            {TEXT("Listening"),TEXT("FaceListening")},{TEXT("Thinking"),TEXT("FaceThinking")},{TEXT("Down"),TEXT("EyesBlink")}})
            Expressions->Poses.FindOrAdd(Pair.Key).MorphWeights.Add(Pair.Value,1.f);
        Expressions->Poses.FindOrAdd(TEXT("Calm"));
        if (!SaveResidentAsset(Blink) || !SaveResidentAsset(Expressions)) return TEXT("Failed expression data save");
    }
    // 将生气时的眼睛和眉毛分别配置，眨眼时保持眉毛的位置
    auto& Angry=Expressions->Poses.FindOrAdd(TEXT("Angry")).MorphWeights;
    float AngryWeight=0;
    if (Angry.RemoveAndCopyValue(TEXT("FaceAngry"),AngryWeight))
    {
        Angry.FindOrAdd(TEXT("EyesAngry"))=AngryWeight;
        Angry.FindOrAdd(TEXT("BrowsAngry"))=AngryWeight;
        if (!SaveResidentAsset(Expressions)) return TEXT("Failed separate eyebrow configuration save");
    }
    for (int32 Index=0;Index<2;++Index)
    {
        auto* Blueprint=LoadObject<UBlueprint>(nullptr,Index==0?TEXT("/Game/NPC/Residents/BP_ResidentLoud"):TEXT("/Game/NPC/Residents/BP_ResidentShrewd"));
        auto* Mesh=LoadObject<USkeletalMesh>(nullptr,*(Appearance+(Index==0?TEXT("SK_NpcManFace"):TEXT("SK_NpcWomanFace"))));
        if (!Blueprint || !Mesh) continue;
        auto* Npc=CastChecked<ADeliveryNpcCharacter>(Blueprint->GeneratedClass->GetDefaultObject());
        Npc->GetMesh()->SetSkeletalMeshAsset(Mesh);
        Npc->Presentation->Expressions=Expressions;
        if (!SaveResidentAsset(Blueprint)) return TEXT("Failed NPC appearance save");
    }
    auto* BB=LoadObject<UBlackboardData>(nullptr,TEXT("/Game/NPC/Residents/AI/BB_Resident"));
    auto* Tree=LoadObject<UBehaviorTree>(nullptr,TEXT("/Game/NPC/Residents/AI/BT_Resident"));
    if (!BB || !Tree || !Tree->RootNode) return TEXT("Missing NPC behavior tree");
    if (!BB->Keys.ContainsByPredicate([](const auto& Entry){return Entry.EntryName==TEXT("Listening");}))
    { FBlackboardEntry Entry; Entry.EntryName=TEXT("Listening"); Entry.KeyType=NewObject<UBlackboardKeyType_Bool>(BB); BB->Keys.Add(Entry); }
    auto* TaskBP=ResidentBlueprint(UBTTask_DeliveryListen::StaticClass(),TEXT("/Game/NPC/Residents/AI/BTT_NpcListen"));
    FKismetEditorUtilities::CompileBlueprint(TaskBP);
    if (TaskBP->Status==BS_Error || !SaveResidentAsset(TaskBP) || !SaveResidentAsset(BB)) return TEXT("Failed listening task save");
    bool bFound=false;
    for (const auto& Child:Tree->RootNode->Children)
        if (Child.ChildTask && Child.ChildTask->IsA<UBTTask_DeliveryListen>()) bFound=true;
    if (!bFound)
    {
        FBTCompositeChild Child; Child.ChildTask=NewObject<UBTTaskNode>(Tree,TaskBP->GeneratedClass);
        auto* Decorator=NewObject<UBTDecorator_Blackboard>(Tree);
        auto* Property=FindFProperty<FStructProperty>(Decorator->GetClass(),TEXT("BlackboardKey"));
        auto* Selector=Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator);
        Selector->SelectedKeyName=TEXT("Listening"); Selector->ResolveSelectedKey(*BB);
        FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("OperationType"))->SetPropertyValue_InContainer(Decorator,EBasicKeyOperation::Set);
        FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("BasicOperation"))->SetPropertyValue_InContainer(Decorator,EBasicKeyOperation::Set);
        FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("FlowAbortMode"))->SetPropertyValue_InContainer(Decorator,EBTFlowAbortMode::Both);
        static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*Tree); Decorator->BuildDescription(); Child.Decorators.Add(Decorator);
        Tree->RootNode->Children.Insert(Child,FMath::Min(1,Tree->RootNode->Children.Num()));
        RebuildResidentGraph(Tree);
    }
    return SaveResidentAsset(Tree)?TEXT("NPC listening branch saved"):TEXT("Failed tree save");
}

FString UDeliveryNpcToolset::CreateResidentAssets()
{
    TMap<FString,FNpcEditorProfile> Profiles;
    if (!ReadNpcProfiles(Profiles)) return TEXT("Run update_npc_profiles.py to create DT_NpcProfiles before creating NPC assets");
    const FString ResidentFolder=TEXT("/Game/NPC/Residents/");
    auto* BB=ResidentAsset<UBlackboardData>(ResidentFolder+TEXT("AI/BB_Resident"));
    // UE 自动添加 SelfActor，所以不能用 Keys.IsEmpty 判断业务黑板是否初始化
    {
        for (const TCHAR* Name : {TEXT("Unavailable"),TEXT("HasApprovedAction")})
            if (!BB->Keys.ContainsByPredicate([&](const auto& Entry){return Entry.EntryName==Name;}))
            { FBlackboardEntry Entry; Entry.EntryName=Name; Entry.KeyType=NewObject<UBlackboardKeyType_Bool>(BB); BB->Keys.Add(Entry); }
        if (!BB->Keys.ContainsByPredicate([](const auto& Entry){return Entry.EntryName==TEXT("ActionTarget");}))
        { FBlackboardEntry Target; Target.EntryName=TEXT("ActionTarget"); Target.KeyType=NewObject<UBlackboardKeyType_Object>(BB); BB->Keys.Add(Target); }
        if (!BB->Keys.ContainsByPredicate([](const auto& Entry){return Entry.EntryName==TEXT("PlanRevision");}))
        { FBlackboardEntry Revision; Revision.EntryName=TEXT("PlanRevision"); Revision.KeyType=NewObject<UBlackboardKeyType_Int>(BB); BB->Keys.Add(Revision); }
        if (!SaveResidentAsset(BB)) return TEXT("Failed blackboard save");
    }
    auto* TaskBP=ResidentBlueprint(UBTTask_DeliveryExecuteNpcAction::StaticClass(),ResidentFolder+TEXT("AI/BTT_ResidentExecute"));
    FKismetEditorUtilities::CompileBlueprint(TaskBP); if (TaskBP->Status==BS_Error || !SaveResidentAsset(TaskBP)) return TEXT("Failed task blueprint");
    auto* Tree=ResidentAsset<UBehaviorTree>(ResidentFolder+TEXT("AI/BT_Resident"));
    Tree->BlackboardAsset=BB;
    if (!Tree->RootNode)
    {
        Tree->BlackboardAsset=BB;
        auto* Root=NewObject<UBTComposite_Selector>(Tree); Tree->RootNode=Root;
        auto Branch=[&](const TCHAR* Key,bool bExecute)
        {
            auto* Seq=NewObject<UBTComposite_Sequence>(Tree); FBTCompositeChild Child; Child.ChildComposite=Seq;
            if (Key)
            {
                auto* Decorator=NewObject<UBTDecorator_Blackboard>(Tree);
                auto* Property=FindFProperty<FStructProperty>(Decorator->GetClass(),TEXT("BlackboardKey"));
                auto* Selector=Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator);
                Selector->SelectedKeyName=Key; Selector->ResolveSelectedKey(*BB);
                FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("OperationType"))->SetPropertyValue_InContainer(Decorator,EBasicKeyOperation::Set);
                FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("BasicOperation"))->SetPropertyValue_InContainer(Decorator,EBasicKeyOperation::Set);
                FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("FlowAbortMode"))->SetPropertyValue_InContainer(Decorator,EBTFlowAbortMode::Both);
                static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*Tree); Decorator->BuildDescription(); Child.Decorators.Add(Decorator);
            }
            Root->Children.Add(Child); FBTCompositeChild Task;
            if (bExecute) Task.ChildTask=NewObject<UBTTaskNode>(Tree,TaskBP->GeneratedClass);
            else { auto* Wait=NewObject<UBTTask_Wait>(Tree); Wait->WaitTime=.25f; Wait->RandomDeviation=0; Task.ChildTask=Wait; }
            Seq->Children.Add(Task);
        };
        Branch(TEXT("Unavailable"),false); Branch(TEXT("HasApprovedAction"),true); Branch(nullptr,false);
        RebuildResidentGraph(Tree); if (!SaveResidentAsset(Tree)) return TEXT("Failed tree save");
    }
    if (auto* Graph=Cast<UBehaviorTreeGraph>(Tree->BTGraph))
    {
        for (UEdGraphNode* Node : Graph->Nodes)
            if (auto* GraphRoot=Cast<UBehaviorTreeGraphNode_Root>(Node))
            { GraphRoot->BlackboardAsset=BB; GraphRoot->UpdateBlackboard(); }
        // 修复旧生成资产中被错误黑板重置成 SelfActor 的两个条件
        for (int32 Index=0; Index<2 && Index<Tree->RootNode->Children.Num(); ++Index)
            for (UBTDecorator* Decorator : Tree->RootNode->Children[Index].Decorators)
            {
                auto* Property=FindFProperty<FStructProperty>(Decorator->GetClass(),TEXT("BlackboardKey"));
                if (!Property) continue;
                auto* Selector=Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator);
                Selector->SelectedKeyName=Index==0?TEXT("Unavailable"):TEXT("HasApprovedAction");
                Selector->ResolveSelectedKey(*BB);
                static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*Tree);
            }
        Graph->UpdateBlackboardChange(); Graph->UpdateAsset();
        if (!SaveResidentAsset(Tree)) return TEXT("Failed resident blackboard repair");
    }
    for (int Index=0;Index<2;++Index)
    {
        auto* BP=ResidentBlueprint(ADeliveryNpcCharacter::StaticClass(),ResidentFolder+(Index==0?TEXT("BP_ResidentLoud"):TEXT("BP_ResidentShrewd")));
        auto* CDO=CastChecked<ADeliveryNpcCharacter>(BP->GeneratedClass->GetDefaultObject());
        CDO->BehaviorTree=Tree;
        const auto& Profile=Profiles.FindChecked(ResidentFolder+(Index==0?TEXT("BP_ResidentLoud"):TEXT("BP_ResidentShrewd")));
        CDO->Profile=Profile.Row;
        if (!CDO->ApplyProfile()) return TEXT("Invalid NPC profile row");
        const TCHAR* Mesh=Index==0?TEXT("/Game/Characters/NPC/SK_NpcMan"):TEXT("/Game/Characters/NPC/SK_NpcWoman");
        CDO->GetMesh()->SetSkeletalMeshAsset(LoadObject<USkeletalMesh>(nullptr,Mesh));
        CDO->GetCapsuleComponent()->SetCollisionObjectType(ECC_Pawn); CDO->GetCapsuleComponent()->SetCapsuleSize(30,70); CDO->GetMesh()->SetRelativeLocationAndRotation(FVector(0,0,-70),FRotator(0,0,0));
        auto* P=CDO->Presentation.Get(); P->VoiceFrequency=Index==0?150.f:300.f;
        auto* Rig=LoadObject<USkeletalMesh>(nullptr,TEXT("/Game/Characters/NPC/SK_NpcMan"));
        P->IdleAnimation=ImportResidentAnimation(TEXT("A_ResidentIdle"),Rig);
        P->WalkAnimation=ImportResidentAnimation(TEXT("A_ResidentWalk"),Rig);
        P->AttackAnimation=ImportResidentAnimation(TEXT("A_ResidentPunch"),Rig);
        P->DownAnimation=ImportResidentAnimation(TEXT("A_ResidentDown"),Rig);
        P->HoldAnimation=ImportResidentAnimation(TEXT("A_ResidentHold"),Rig);
        P->TalkAnimation=ImportResidentAnimation(TEXT("A_ResidentTalk"),Rig);
        P->GetUpAnimation=ImportResidentAnimation(TEXT("A_ResidentGetUp"),Rig);
        if (!P->IdleAnimation || !P->TalkAnimation || !P->AttackAnimation || !P->DownAnimation || !P->HoldAnimation || !P->WalkAnimation || !P->GetUpAnimation) return TEXT("Missing resident animation imports");
        FBlueprintEditorUtils::MarkBlueprintAsModified(BP); FKismetEditorUtilities::CompileBlueprint(BP);
        if (BP->Status==BS_Error || !SaveResidentAsset(BP)) return TEXT("Failed resident blueprint");
    }
    auto* Action=ResidentAsset<UInputAction>(ResidentFolder+TEXT("Input/IA_Talk")); Action->ValueType=EInputActionValueType::Boolean;
    auto* Mapping=ResidentAsset<UInputMappingContext>(ResidentFolder+TEXT("Input/IMC_ResidentConversation"));
    if (Mapping->GetMappings().IsEmpty()) { Mapping->MapKey(Action,EKeys::T); Mapping->MapKey(Action,EKeys::Gamepad_DPad_Right); }
    if (!SaveResidentAsset(Action) || !SaveResidentAsset(Mapping)) return TEXT("Failed input assets");
    return TEXT("Saved BB_Resident, BT_Resident, BTT_ResidentExecute, resident pair and conversation input");
}
FString UDeliveryNpcToolset::PlaceResidentPair(FVector Location)
{
    UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr; if (!World || GEditor->PlayWorld) return TEXT("Editor world required");
    for (int Index=0;Index<2;++Index)
    {
        const FString Label=Index==0?TEXT("Resident_Loud"):TEXT("Resident_Shrewd");
        ADeliveryNpcCharacter* Existing=nullptr;
        for (TActorIterator<ADeliveryNpcCharacter> It(World);It;++It) if (It->GetActorLabel()==Label) Existing=*It;
        UClass* Class=LoadClass<ADeliveryNpcCharacter>(nullptr,Index==0?TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"):TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
        if (!Class) return TEXT("Missing resident blueprint");
        // 两人沿街相对站立，避免第二人向房屋方向偏移；已有实例只调整位置
        FVector Point=Location+FVector(Index*180.f,0,0); FHitResult Ground;
        FCollisionQueryParams Placement(SCENE_QUERY_STAT(ResidentPlacement),false,Existing);
        if (!World->LineTraceSingleByChannel(Ground,Point+FVector(0,0,300),Point-FVector(0,0,1000),ECC_WorldStatic,Placement)
            || Ground.ImpactNormal.Z<.7f) return TEXT("No walkable ground at requested position");
        const auto* Default=Class->GetDefaultObject<ADeliveryNpcCharacter>();
        Point=Ground.Location+FVector(0,0,Default->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+2.f);
        auto* Actor=Existing?Existing:World->SpawnActor<ADeliveryNpcCharacter>(Class,Point,FRotator::ZeroRotator);
        if (!Actor) return TEXT("Spawn rejected");
        Actor->SetActorLocationAndRotation(Point,FRotator(0,Index==0?0:180,0));
        Actor->SetActorLabel(Label); Actor->MarkPackageDirty();
    }
    // 新建体积交给统一检查补全，已有有效范围不会随人物位置被改写
    bool bHasBounds=false;
    for (TActorIterator<ANavMeshBoundsVolume> It(World);It;++It) if (It->GetActorLabel()==TEXT("Resident_Navigation")) bHasBounds=true;
    if (!bHasBounds)
    {
        auto* Volume=World->SpawnActor<ANavMeshBoundsVolume>(Location,FRotator::ZeroRotator);
        if (!Volume) return TEXT("Resident navigation volume spawn failed");
        Volume->SetActorLabel(TEXT("Resident_Navigation"));
    }
    return CheckNpcNavigation(true);
}
FString UDeliveryNpcToolset::GetNpcMorphStats(USkeletalMesh* Mesh, FName MorphName, int32 LODIndex)
{
    auto Report = MakeShared<FJsonObject>();
    auto Finish = [&Report](const TCHAR* Error = nullptr)
    {
        Report->SetBoolField(TEXT("valid_data"), Error == nullptr);
        if (Error) Report->SetStringField(TEXT("error"), Error);
        FString Json;
        FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
        return Json;
    };
    if (!IsInGameThread() || !IsValid(Mesh)) return Finish(TEXT("需要在游戏线程读取有效模型"));
    if (Mesh->IsCompiling()) return Finish(TEXT("模型仍在编译，请完成编译后再检查"));
    const UMorphTarget* Morph = Mesh->FindMorphTarget(MorphName);
    if (!Morph || !Morph->GetMorphLODModels().IsValidIndex(LODIndex))
        return Finish(TEXT("模型中没有指定表情或 LOD"));
    const FSkeletalMeshModel* Imported = Mesh->GetImportedModel();
    if (!Imported || !Imported->LODModels.IsValidIndex(LODIndex))
        return Finish(TEXT("模型没有对应的 LOD 顶点数据"));
    // Morph 的 NumBaseMeshVerts 保存的是生成表情时传入的位移条目数，索引上限应取实际 LOD 顶点数
    const uint32 VertexCount = Imported->LODModels[LODIndex].NumVertices;
    const auto Deltas = Morph->GetMorphTargetDeltas(LODIndex);
    Report->SetStringField(TEXT("mesh"), Mesh->GetPathName());
    Report->SetStringField(TEXT("morph"), MorphName.ToString());
    Report->SetNumberField(TEXT("lod"), LODIndex);
    Report->SetNumberField(TEXT("base_vertex_count"), VertexCount);
    Report->SetNumberField(TEXT("morph_input_delta_count"), Morph->GetMorphLODModels()[LODIndex].NumBaseMeshVerts);
    Report->SetNumberField(TEXT("delta_count"), Deltas.Num());
    int32 MovingVertices = 0;
    double MaxDistance = 0.0;
    // 直接读取编辑器保存的位移，不创建渲染组件，也不导出或烘焙材质
    for (const FMorphTargetDelta& Delta : Deltas)
    {
        if (Delta.SourceIdx >= VertexCount)
        {
            Report->SetNumberField(TEXT("invalid_vertex_index"), Delta.SourceIdx);
            return Finish(TEXT("表情顶点编号超出 LOD 范围"));
        }
        if (Delta.PositionDelta.ContainsNaN() || Delta.TangentZDelta.ContainsNaN())
            return Finish(TEXT("表情包含无效数值"));
        const double Distance = Delta.PositionDelta.Size();
        if (Distance > 1.e-5) ++MovingVertices;
        MaxDistance = FMath::Max(MaxDistance, Distance);
    }
    Report->SetNumberField(TEXT("nonzero_position_deltas"), MovingVertices);
    Report->SetNumberField(TEXT("max_position_delta_cm"), MaxDistance);
    return Finish();
}

FString UDeliveryNpcToolset::CheckNpcNavigation(bool bRepairEmpty)
{
    auto Report=MakeShared<FJsonObject>();
    const auto Finish=[&Report](const FString& Status)
    {
        Report->SetStringField(TEXT("status"),Status);
        FString Json;
        FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json));
        return Json;
    };
    UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
    if (!World || GEditor->PlayWorld) return Finish(TEXT("editor_world_required"));
    Report->SetStringField(TEXT("world"),World->GetOutermost()->GetName());
    TArray<ADeliveryNpcCharacter*> Npcs;
    TArray<ANavMeshBoundsVolume*> Volumes;
    for (TActorIterator<ADeliveryNpcCharacter> It(World);It;++It)
        if (It->GetActorLabel()==TEXT("Resident_Loud") || It->GetActorLabel()==TEXT("Resident_Shrewd")) Npcs.Add(*It);
    for (TActorIterator<ANavMeshBoundsVolume> It(World);It;++It)
        if (It->GetActorLabel()==TEXT("Resident_Navigation")) Volumes.Add(*It);
    Report->SetNumberField(TEXT("npc_count"),Npcs.Num());
    Report->SetNumberField(TEXT("volume_count"),Volumes.Num());
    if (Npcs.Num()!=2 || Volumes.Num()!=1 || Npcs[0]->GetActorLabel()==Npcs[1]->GetActorLabel())
        return Finish(TEXT("unexpected_actors"));
    auto* Volume=Volumes[0];
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
    const auto HasBrush=[Volume]()
    {
        return Volume->Brush && Volume->Brush->Polys && Volume->Brush->Polys->Element.Num()>=4
            && Volume->GetBrushComponent() && Volume->GetBrushComponent()->Bounds.BoxExtent.GetMin()>1.f;
    };
    const auto Describe=[&]()
    {
        auto State=MakeShared<FJsonObject>();
        State->SetBoolField(TEXT("valid_brush"),HasBrush());
        State->SetNumberField(TEXT("polygons"),Volume->Brush && Volume->Brush->Polys?Volume->Brush->Polys->Element.Num():0);
        State->SetStringField(TEXT("volume_transform"),Volume->GetActorTransform().ToHumanReadableString());
        const FBox Bounds=Volume->GetComponentsBoundingBox(true);
        State->SetStringField(TEXT("bounds"),Bounds.ToString());
        TArray<TSharedPtr<FJsonValue>> Actors;
        for (auto* Npc:Npcs)
        {
            const FVector Feet=Npc->GetNavAgentLocation();
            auto Actor=MakeShared<FJsonObject>();
            Actor->SetStringField(TEXT("label"),Npc->GetActorLabel());
            Actor->SetStringField(TEXT("feet"),Feet.ToString());
            Actor->SetBoolField(TEXT("inside_bounds"),HasBrush() && Bounds.IsInsideOrOn(Feet));
            const auto* Data=Nav?Nav->GetNavDataForProps(Npc->GetNavAgentPropertiesRef(),Feet):nullptr;
            FNavLocation Projected;
            const bool bProjected=Data && Nav->ProjectPointToNavigation(Feet,Projected,FVector(60,60,150),Data);
            Actor->SetBoolField(TEXT("on_navigation"),bProjected);
            Actor->SetStringField(TEXT("projected_feet"),bProjected?Projected.Location.ToString():TEXT(""));
            bool bCompletePath=false;
            if (Data)
            {
                auto* Other=Npcs[0]==Npc?Npcs[1]:Npcs[0];
                FPathFindingQuery Query(Npc,*Data,Feet,Other->GetNavAgentLocation());
                Query.SetAllowPartialPaths(false);
                const FPathFindingResult Path=Nav->FindPathSync(Npc->GetNavAgentPropertiesRef(),Query);
                bCompletePath=Path.IsSuccessful() && Path.Path.IsValid() && Path.Path->IsValid() && !Path.Path->IsPartial();
            }
            Actor->SetBoolField(TEXT("complete_path_to_other"),bCompletePath);
            Actors.Add(MakeShared<FJsonValueObject>(Actor));
        }
        State->SetArrayField(TEXT("npcs"),Actors);
        return State;
    };
    Report->SetObjectField(TEXT("before"),Describe());
    if (!bRepairEmpty) return Finish(TEXT("inspected"));
    if (!Nav) return Finish(TEXT("navigation_system_missing"));
    const auto ReadBuildLocks=[Nav]()
    {
        uint8 Flags=0;
        for (uint32 Bit=1;Bit<=128;Bit<<=1)
            if (Nav->IsNavigationBuildingLocked(static_cast<uint8>(Bit))) Flags|=static_cast<uint8>(Bit);
        return Flags;
    };
    Report->SetNumberField(TEXT("build_locks_before"),ReadBuildLocks());
    Report->SetBoolField(TEXT("async_loading_before"),IsAsyncLoading());
    Report->SetNumberField(TEXT("assets_compiling_before"),FAssetCompilingManager::Get().GetNumRemainingAssets());
    bool bRemovedAsyncLoadLock=false;
    if (IsRunningCommandlet())
    {
        // 命令行脚本没有编辑器的延后 Tick，先等模型加载与编译完成，再解除加载期间的导航锁
        FlushAsyncLoading();
        FAssetCompilingManager::Get().FinishAllCompilation();
        if (!IsAsyncLoading() && FAssetCompilingManager::Get().GetNumRemainingAssets()==0
            && Nav->IsNavigationBuildingLocked(ENavigationBuildLock::AsyncLoadLock))
        {
            Nav->RemoveNavigationBuildLock(ENavigationBuildLock::AsyncLoadLock,
                UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
            bRemovedAsyncLoadLock=true;
        }
    }
    Report->SetBoolField(TEXT("async_loading_after"),IsAsyncLoading());
    Report->SetNumberField(TEXT("assets_compiling_after"),FAssetCompilingManager::Get().GetNumRemainingAssets());
    Report->SetBoolField(TEXT("removed_async_load_lock"),bRemovedAsyncLoadLock);
    Report->SetNumberField(TEXT("build_locks_after"),ReadBuildLocks());
    if (IsAsyncLoading() || FAssetCompilingManager::Get().GetNumRemainingAssets()!=0)
        return Finish(TEXT("assets_not_ready"));
    // 用户设置和其他系统留下的锁保持原样，显式 Build 沿用引擎对编辑器自动更新开关的处理
    if (Nav->IsNavigationBuildingLocked(static_cast<uint8>(~ENavigationBuildLock::NoUpdateInEditor)))
        return Finish(TEXT("navigation_build_locked"));
    FString Status=TEXT("rebuilt_existing_bounds");
    if (HasBrush())
    {
        // 有效范围可能是用户手工配置的，人物移到范围外时报告问题，不自动扩张
        const FBox Bounds=Volume->GetComponentsBoundingBox(true);
        for (auto* Npc:Npcs) if (!Bounds.IsInsideOrOn(Npc->GetNavAgentLocation())) return Finish(TEXT("existing_bounds_exclude_npc"));
    }
    else
    {
        const FVector A=Npcs[0]->GetNavAgentLocation(),B=Npcs[1]->GetNavAgentLocation();
        Volume->Modify();
        Volume->SetActorTransform(FTransform(FQuat::Identity,(A+B)*.5f,FVector::OneVector));
        auto* Builder=NewObject<UCubeBuilder>();
        Builder->X=FMath::Max(3200.f,float(FMath::Abs(A.X-B.X)+1200.f));
        Builder->Y=FMath::Max(3200.f,float(FMath::Abs(A.Y-B.Y)+1200.f));
        Builder->Z=FMath::Max(1200.f,float(FMath::Abs(A.Z-B.Z)+400.f));
        // SpawnActor 的体积没有 UModel，直接调用 CubeBuilder 会返回成功却留下一只空盒
        UActorFactory::CreateBrushForVolumeActor(Volume,Builder);
        Volume->MarkPackageDirty();
        if (!HasBrush()) return Finish(TEXT("brush_creation_failed"));
        Status=TEXT("repaired_empty_bounds");
    }
    Nav->FlushPendingOperations();
    Nav->OnNavigationBoundsUpdated(Volume);
    Nav->Build();
    Report->SetObjectField(TEXT("after"),Describe());
    return Finish(Status);
}
FString UDeliveryNpcToolset::InspectResidentWorlds()
{
    FString Result;
    for (const auto& Context:GEngine->GetWorldContexts())
        if (auto* World=Context.World())
            for (TActorIterator<ADeliveryNpcCharacter> It(World);It;++It)
            {
                auto State=It->Agent->GetPublicState();
                Result+=FString::Printf(TEXT("world=%s net=%d npc=%s pos=%s action=%s status=%d down=%d pending=%d speechActive=%d speech=%s\n"),
                    *World->GetName(),int32(World->GetNetMode()),*It->GetName(),*It->GetActorLocation().ToString(),*DeliveryNpc::ActionName(State.Action),int32(State.Status),It->IsKnockedDown(),It->Agent->IsDecisionPending(),It->Presentation->GetSpeech().bActive,*It->Presentation->GetSpeech().Text);
            }
    return Result;
}
bool UDeliveryNpcToolset::ConfigureResidentPIE(int32 Players)
{
    if (GEditor->PlayWorld || Players<1 || Players>3) return false;
    auto* Settings=GetMutableDefault<ULevelEditorPlaySettings>();
    Settings->SetPlayNumberOfClients(Players); Settings->SetPlayNetMode(PIE_ListenServer); Settings->SetRunUnderOneProcess(true); return true;
}

FString UDeliveryNpcToolset::SpeakInResidentPIE(FString Text)
{
    if (Text.IsEmpty() || Text.Len()>120) return TEXT("Speech must contain 1 to 120 characters");
    UClass* SpeakerClass=LoadClass<ADeliveryNpcCharacter>(nullptr,TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
    if (!SpeakerClass) return TEXT("Missing conversation speaker blueprint");
    for (const auto& Context:GEngine->GetWorldContexts())
        if (Context.WorldType==EWorldType::PIE && Context.World() && Context.World()->GetNetMode()!=NM_Client)
            for (TActorIterator<ADeliveryNpcCharacter> It(Context.World());It;++It)
                if (It->IsA(SpeakerClass))
                    return It->Presentation->PublishSpeech(Text,TEXT("Angry"),0)?TEXT("Published through real speech perception path"):TEXT("Speaker busy");
    return TEXT("No authoritative resident PIE");
}

FString UDeliveryNpcToolset::RunResidentValidation(bool bFull)
{
    if (!GEditor || GEditor->PlayWorld) return TEXT("Stop PIE before validation");
    const TCHAR* Tests=bFull?TEXT("Automation RunTests Delivery.Startup+Delivery.AI.Protocol+Delivery.AI.AuthorityLifecycle+Delivery.AI.ContextBoundary+Delivery.AI.BehaviorTreeExecution+Delivery.AI.ResidentIntegration+Delivery.AI.ResidentMultiplayer+Delivery.AI.ResidentMap+Delivery.Traffic.Horn+Delivery.Vehicle.PedestrianImpact+Delivery.NPC.Boids+Delivery.Systems.InputLifecycle+Delivery.Interaction.WorldRegistry+Delivery.UI.PhoneController+Delivery.UI.PersistentHint+Delivery.Vehicle.Summon.EmptyMap"):
        TEXT("Automation RunTests Delivery.AI.ResidentIntegration+Delivery.AI.ResidentMultiplayer+Delivery.AI.ResidentMap");
    return GEngine->Exec(GEditor->GetEditorWorldContext().World(),Tests)?TEXT("Validation queued; inspect Automation results and Delivery.log"):TEXT("Automation command rejected");
}

FString UDeliveryNpcToolset::TalkInResidentPIE(bool bClient, FString Text)
{
    if (Text.TrimStartAndEnd().IsEmpty() || Text.Len()>256) return TEXT("Provide 1 to 256 characters");
    for (const auto& Context:GEngine->GetWorldContexts())
    {
        auto* World=Context.World();
        if (Context.WorldType!=EWorldType::PIE || !World || (World->GetNetMode()==NM_Client)!=bClient) continue;
        auto* PC=World->GetFirstPlayerController();
        if (!PC || !PC->IsLocalController() || !PC->GetPawn()) continue;
        auto* Conversation=PC->FindComponentByClass<UDeliveryConversationComponent>();
        if (!Conversation) continue;
        ADeliveryNpcCharacter* Resident=nullptr; double Distance=MAX_dbl;
        for (TActorIterator<ADeliveryNpcCharacter> It(World);It;++It)
        {
            const double Current=FVector::DistSquared(PC->GetPawn()->GetActorLocation(),It->GetActorLocation());
            if (Current<Distance && Conversation->CanTalkTo(*It)) { Resident=*It; Distance=Current; }
        }
        if (!Resident) return TEXT("No nearby visible resident; move the player closer");
        PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::T,IE_Pressed,1));
        FTimerHandle Release;
        World->GetTimerManager().SetTimer(Release,FTimerDelegate::CreateWeakLambda(PC,[PC]
        { PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::T,IE_Released,0)); }),.1f,false);
        Conversation->Submit(Resident,Text);
        return FString::Printf(TEXT("Submitted from %s to %s via player RPC; awaiting server/model result"),bClient?TEXT("client"):TEXT("host"),*Resident->DisplayName.ToString());
    }
    return TEXT("No matching local PIE player");
}
