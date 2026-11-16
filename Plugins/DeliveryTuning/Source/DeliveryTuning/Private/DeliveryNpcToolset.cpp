#include "DeliveryNpcToolset.h"
#include "DeliveryConversationAssetTools.h"
#include "AI/BehaviorTree/BTTask_DeliveryListen.h"
#include "AI/BehaviorTree/BTTask_DeliveryCompanionFollow.h"
#include "AI/BehaviorTree/BTTask_DeliveryCompanionErrand.h"
#include "AI/Presentation/Face/DeliveryNpcExpressionSet.h"
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
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/InputDeviceLibrary.h"
#include "InputKeyEventArgs.h"
#include "TimerManager.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "AI/Decision/DeliveryNpcBehaviorCatalog.h"
#include "AI/Network/DeliveryAiSettings.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
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

FString UDeliveryNpcToolset::CreateCompanionBehaviorAsset()
{
    if (!GEditor || GEditor->PlayWorld) return TEXT("Close PIE before editing companion assets");
    const FString Folder=TEXT("/Game/NPC/Companion/AI");
    auto* Tree=LoadObject<UBehaviorTree>(nullptr,*(Folder+TEXT("/BT_Companion")));
    if (!Tree)
    {
        auto* Original=LoadObject<UBehaviorTree>(nullptr,TEXT("/Game/NPC/Residents/AI/BT_Resident"));
        if (!Original || !Original->RootNode) return TEXT("Missing BT_Resident");
        Tree=Cast<UBehaviorTree>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().DuplicateAsset(TEXT("BT_Companion"),Folder,Original));
    }
    if (!Tree || !Tree->RootNode || !Tree->RootNode->IsA<UBTComposite_Selector>() || !Tree->BlackboardAsset || Tree->RootNode->Children.IsEmpty())
        return TEXT("Missing companion selector or blackboard; kept unchanged");
    // 只在原资产没有编辑图时补建；已有节点、服务、注释与布局保留原对象
    if (!Tree->BTGraph) RebuildResidentGraph(Tree);
    auto* Root=Tree->RootNode.Get();
    auto& Last=Root->Children.Last();
    if (!Last.Decorators.IsEmpty()) return TEXT("Default companion branch has decorators; kept unchanged");
    auto* Sequence=Cast<UBTComposite_Sequence>(Last.ChildComposite.Get());
    FBTCompositeChild* Leaf=Sequence && Sequence->Children.Num()==1?&Sequence->Children[0]:&Last;
    const bool bHasFollow=Leaf->ChildTask && Leaf->ChildTask->IsA<UBTTask_DeliveryCompanionFollow>();
    if (!bHasFollow && (!Leaf->ChildTask || !Leaf->ChildTask->IsA<UBTTask_Wait>() || !Leaf->Decorators.IsEmpty()))
        return TEXT("Default companion branch was customized; kept unchanged");

    auto BlackboardKey=[](const UBTDecorator* Decorator)->FName
    {
        if (!Decorator || !Decorator->IsA<UBTDecorator_Blackboard>()) return NAME_None;
        const auto* Property=FindFProperty<FStructProperty>(Decorator->GetClass(),TEXT("BlackboardKey"));
        return Property?Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator)->SelectedKeyName:NAME_None;
    };
    int32 ErrandIndex=INDEX_NONE;
    int32 UnavailableIndex=INDEX_NONE,ListeningIndex=INDEX_NONE,ApprovedIndex=INDEX_NONE;
    for (int32 Index=0;Index<Root->Children.Num();++Index)
    {
        const auto& Child=Root->Children[Index];
        if (Child.ChildTask && Child.ChildTask->IsA<UBTTask_DeliveryCompanionErrand>())
        {
            if (ErrandIndex!=INDEX_NONE) return TEXT("Duplicate companion errand branches; kept unchanged");
            ErrandIndex=Index;
        }
        for (const UBTDecorator* Decorator:Child.Decorators)
        {
            const FName Key=BlackboardKey(Decorator);
            if (Key==TEXT("Unavailable")) UnavailableIndex=Index;
            if (Key==TEXT("Listening")) ListeningIndex=Index;
            if (Key==TEXT("HasApprovedAction")) ApprovedIndex=Index;
        }
    }
    // 插入点必须位于既有高优先级分支之后，不能靠重排自定义节点改变游戏行为
    if (UnavailableIndex<0 || ListeningIndex<=UnavailableIndex || ApprovedIndex<=ListeningIndex || ApprovedIndex>=Root->Children.Num()-1)
        return TEXT("Companion priority branches were customized; kept unchanged");
    if (ErrandIndex!=INDEX_NONE)
    {
        const auto& Child=Root->Children[ErrandIndex];
        if (ErrandIndex!=Root->Children.Num()-2 || ErrandIndex<=ApprovedIndex || Child.Decorators.Num()!=1 ||
            BlackboardKey(Child.Decorators[0])!=TEXT("HasCompanionErrand"))
            return TEXT("Existing companion errand branch was customized; kept unchanged");
        const auto* Decorator=CastChecked<UBTDecorator_Blackboard>(Child.Decorators[0]);
        const auto* AbortProperty=FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("FlowAbortMode"));
        const auto* OperationProperty=FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("BasicOperation"));
        if (!AbortProperty || !OperationProperty || AbortProperty->GetPropertyValue_InContainer(Decorator)!=EBTFlowAbortMode::Both ||
            OperationProperty->GetPropertyValue_InContainer(Decorator)!=EBasicKeyOperation::Set)
            return TEXT("Existing errand abort condition was customized; kept unchanged");
    }

    auto* Graph=Cast<UBehaviorTreeGraph>(Tree->BTGraph);
    if (!Graph) return TEXT("Unsupported companion graph; kept unchanged");
    auto FindGraphNode=[&](const UObject* Instance)->UBehaviorTreeGraphNode*
    {
        for (UEdGraphNode* Raw:Graph->Nodes)
            if (auto* Node=Cast<UBehaviorTreeGraphNode>(Raw); Node && Node->NodeInstance==Instance) return Node;
        return nullptr;
    };
    auto* RootGraph=FindGraphNode(Root);
    auto* LeafGraph=FindGraphNode(Leaf->ChildTask);
    auto* LastGraph=FindGraphNode(Root->GetChildNode(Root->Children.Num()-1));
    auto* PreviousGraph=FindGraphNode(Root->GetChildNode(Root->Children.Num()-2));
    UBehaviorTreeGraphNode_Root* GraphRoot=nullptr;
    int32 GraphErrandCount=0;
    for (UEdGraphNode* Raw:Graph->Nodes)
    {
        if (auto* Node=Cast<UBehaviorTreeGraphNode_Root>(Raw)) GraphRoot=Node;
        const auto* Node=Cast<UBehaviorTreeGraphNode>(Raw);
        if (Node && Node->NodeInstance && Node->NodeInstance->IsA<UBTTask_DeliveryCompanionErrand>()) ++GraphErrandCount;
    }
    if (GraphErrandCount!=(ErrandIndex==INDEX_NONE?0:1))
        return TEXT("Companion errand was placed in a custom branch; kept unchanged");
    if (!RootGraph || !LeafGraph || !LastGraph || !PreviousGraph || !GraphRoot || !RootGraph->GetOutputPin())
        return TEXT("Companion graph does not match runtime nodes; kept unchanged");
    auto* Output=RootGraph->GetOutputPin();
    if (Output->LinkedTo.Num()!=Root->Children.Num()) return TEXT("Companion root links were customized; kept unchanged");
    int32 PreviousX=MIN_int32;
    for (int32 Index=0;Index<Root->Children.Num();++Index)
    {
        const auto* Node=FindGraphNode(Root->GetChildNode(Index));
        if (!Node || !Node->GetInputPin() || !Output->LinkedTo.Contains(Node->GetInputPin()) || Node->NodePosX<=PreviousX)
            return TEXT("Companion graph order differs from runtime priority; kept unchanged");
        PreviousX=Node->NodePosX;
    }
    if (ErrandIndex==INDEX_NONE && int64(LastGraph->NodePosX)-PreviousGraph->NodePosX<2)
        return TEXT("No graph space before companion follow branch; kept unchanged");

    auto* SourceBB=Tree->BlackboardAsset.Get();
    auto* BB=LoadObject<UBlackboardData>(nullptr,*(Folder+TEXT("/BB_Companion")));
    if (!BB)
        BB=Cast<UBlackboardData>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().DuplicateAsset(TEXT("BB_Companion"),Folder,SourceBB));
    if (!BB) return TEXT("Failed to duplicate companion blackboard");
    // 失败后重跑时仍保留专属黑板的配置，遇到不兼容的源键则交回编辑器处理
    for (int32 Index=0;Index<SourceBB->GetNumKeys();++Index)
    {
        const auto Key=static_cast<FBlackboard::FKey>(Index);
        const auto OtherKey=BB->GetKeyID(SourceBB->GetKeyName(Key));
        if (OtherKey==FBlackboard::InvalidKey || BB->GetKeyType(OtherKey)!=SourceBB->GetKeyType(Key) ||
            BB->IsKeyInstanceSynced(OtherKey)!=SourceBB->IsKeyInstanceSynced(Key))
            return TEXT("Existing companion blackboard differs from tree keys; kept unchanged");
    }
    const FName ErrandKey=TEXT("HasCompanionErrand");
    const auto ExistingKey=BB->GetKeyID(ErrandKey);
    if (ExistingKey!=FBlackboard::InvalidKey && (BB->GetKeyType(ExistingKey)!=UBlackboardKeyType_Bool::StaticClass() || BB->IsKeyInstanceSynced(ExistingKey)))
        return TEXT("HasCompanionErrand must be an unsynchronized bool; kept unchanged");
    if (ExistingKey==FBlackboard::InvalidKey)
    {
        BB->Modify();
        FBlackboardEntry Entry; Entry.EntryName=ErrandKey; Entry.KeyType=NewObject<UBlackboardKeyType_Bool>(BB);
        Entry.EntryDescription=TEXT("本同伴是否持有尚未结束的主人委托，暂停导航时仍保留");
        BB->Keys.Add(Entry); BB->UpdateKeyIDs();
    }
    if (bHasFollow && ErrandIndex!=INDEX_NONE && Tree->BlackboardAsset==BB && GraphRoot->BlackboardAsset==BB && ExistingKey!=FBlackboard::InvalidKey)
        return TEXT("BT_Companion and BB_Companion already configured");
    UBlueprint* FollowBP=nullptr;
    if (!bHasFollow)
    {
        FollowBP=ResidentBlueprint(UBTTask_DeliveryCompanionFollow::StaticClass(),Folder+TEXT("/BTT_CompanionFollow"));
        if (!FollowBP || !FollowBP->ParentClass || !FollowBP->ParentClass->IsChildOf(UBTTask_DeliveryCompanionFollow::StaticClass()))
            return TEXT("Companion follow task has an incompatible parent; kept unchanged");
        FKismetEditorUtilities::CompileBlueprint(FollowBP);
        if (FollowBP->Status==BS_Error || !SaveResidentAsset(FollowBP)) return TEXT("Failed companion follow task save");
    }
    UBlueprint* ErrandBP=nullptr;
    if (ErrandIndex==INDEX_NONE)
    {
        ErrandBP=ResidentBlueprint(UBTTask_DeliveryCompanionErrand::StaticClass(),Folder+TEXT("/BTT_CompanionErrand"));
        if (!ErrandBP || !ErrandBP->ParentClass || !ErrandBP->ParentClass->IsChildOf(UBTTask_DeliveryCompanionErrand::StaticClass()))
            return TEXT("Companion errand task has an incompatible parent; kept unchanged");
        FKismetEditorUtilities::CompileBlueprint(ErrandBP);
        if (ErrandBP->Status==BS_Error || !SaveResidentAsset(ErrandBP)) return TEXT("Failed companion errand task save");
    }
    if (!SaveResidentAsset(BB)) return TEXT("Failed companion blackboard save");
    Tree->Modify(); Graph->Modify(); Graph->LockUpdates();
    Tree->BlackboardAsset=BB;
    GraphRoot->Modify(); GraphRoot->BlackboardAsset=BB;
    if (FollowBP)
    {
        LeafGraph->Modify();
        LeafGraph->NodeInstance=NewObject<UBTTaskNode>(Tree,FollowBP->GeneratedClass);
        LeafGraph->ClassData=FGraphNodeClassData(FollowBP->GeneratedClass,FString());
    }
    if (ErrandBP)
    {
        FGraphNodeCreator<UBehaviorTreeGraphNode_Task> Builder(*Graph);
        auto* Node=Builder.CreateNode();
        Node->NodeInstance=NewObject<UBTTaskNode>(Tree,ErrandBP->GeneratedClass);
        Node->ClassData=FGraphNodeClassData(ErrandBP->GeneratedClass,FString());
        Node->NodePosX=static_cast<int32>((int64(LastGraph->NodePosX)+PreviousGraph->NodePosX)/2);
        Node->NodePosY=LastGraph->NodePosY;
        Builder.Finalize();
        auto* Decorator=NewObject<UBTDecorator_Blackboard>(Tree);
        auto* Property=FindFProperty<FStructProperty>(Decorator->GetClass(),TEXT("BlackboardKey"));
        auto* Selector=Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator);
        Selector->SelectedKeyName=ErrandKey; Selector->ResolveSelectedKey(*BB);
        FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("OperationType"))->SetPropertyValue_InContainer(Decorator,EBasicKeyOperation::Set);
        FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("BasicOperation"))->SetPropertyValue_InContainer(Decorator,EBasicKeyOperation::Set);
        FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("FlowAbortMode"))->SetPropertyValue_InContainer(Decorator,EBTFlowAbortMode::Both);
        static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*Tree); Decorator->BuildDescription();
        auto* DecoratorNode=NewObject<UBehaviorTreeGraphNode_Decorator>(Graph);
        DecoratorNode->NodeInstance=Decorator;
        DecoratorNode->ClassData=FGraphNodeClassData(Decorator->GetClass(),FString());
        Node->AddSubNode(DecoratorNode,Graph);
        Output->Modify(); Output->MakeLinkTo(Node->GetInputPin());
    }
    Graph->UnlockUpdates();
    Graph->UpdateBlackboardChange(); Graph->UpdateAsset();
    Graph->NotifyGraphChanged();
    const int32 Count=Root->Children.Num();
    if (Count<2 || !Root->Children[Count-2].ChildTask || !Root->Children[Count-2].ChildTask->IsA<UBTTask_DeliveryCompanionErrand>())
        return TEXT("Companion errand graph failed priority check; tree was not saved");
    return SaveResidentAsset(Tree)?TEXT("Saved BT_Companion, BB_Companion and persistent companion tasks"):TEXT("Failed companion tree save");
}

FString UDeliveryNpcToolset::InstallNearbyItemSearchSubtree()
{
    if (!GEditor || GEditor->PlayWorld) return TEXT("退出 PIE 后再接入寻找子树");
    auto* Tree=LoadObject<UBehaviorTree>(nullptr,TEXT("/Game/NPC/Companion/AI/BT_Companion"));
    auto* Root=Tree?Cast<UBTComposite_Selector>(Tree->RootNode.Get()):nullptr;
    auto* Graph=Tree?Cast<UBehaviorTreeGraph>(Tree->BTGraph):nullptr;
    auto* BB=Tree?Tree->BlackboardAsset.Get():nullptr;
    if (!Root || !Graph || !BB || Root->Children.Num()<4) return TEXT("找不到同伴树、图或黑板");
    const FString SubtreePath=TEXT("/Game/NPC/Shared/AI/BT_SearchNearbyItem");
    const FName SearchKey=TEXT("HasNearbyItemSearch");
    const int32 ErrandIndex=Root->Children.Num()-2;
    const auto* ErrandTask=Cast<UBTTask_DeliveryCompanionErrand>(Root->GetChildNode(ErrandIndex));
    if (!ErrandTask || ErrandTask->bSearchStageOnly) return TEXT("未找到正式委托分支");
    const auto KeyId=BB->GetKeyID(SearchKey);
    if (KeyId!=FBlackboard::InvalidKey && (BB->GetKeyType(KeyId)!=UBlackboardKeyType_Bool::StaticClass() || BB->IsKeyInstanceSynced(KeyId)))
        return TEXT("寻找键必须使用非同步 Bool");
    if (const auto* Run=Cast<UBTTask_RunBehavior>(Root->GetChildNode(ErrandIndex-1));
        Run && Run->GetSubtreeAsset() && Run->GetSubtreeAsset()->GetPackage()->GetName()==SubtreePath)
    {
        const auto& Branch=Root->Children[ErrandIndex-1];
        const auto* Gate=Branch.Decorators.Num()==1?Cast<UBTDecorator_Blackboard>(Branch.Decorators[0]):nullptr;
        const auto* SubRoot=Run->GetSubtreeAsset()->RootNode.Get();
        const auto* Search=SubRoot && SubRoot->Children.Num()==1?Cast<UBTTask_DeliveryCompanionErrand>(SubRoot->GetChildNode(0)):nullptr;
        const bool bReady=Gate && Gate->GetSelectedBlackboardKey()==SearchKey && Gate->GetFlowAbortMode()==EBTFlowAbortMode::Both &&
            KeyId!=FBlackboard::InvalidKey && Search && Search->bSearchStageOnly && Run->GetSubtreeAsset()->BlackboardAsset==BB;
        return bReady?TEXT("同伴寻找子树已接入"):TEXT("已有寻找分支与预期配置不同");
    }
    UBehaviorTreeGraphNode* RootGraph=nullptr;
    UBehaviorTreeGraphNode* PreviousGraph=nullptr;
    UBehaviorTreeGraphNode* ErrandGraph=nullptr;
    for (UEdGraphNode* Raw:Graph->Nodes)
    {
        auto* Node=Cast<UBehaviorTreeGraphNode>(Raw);
        if (!Node) continue;
        if (Node->NodeInstance==Root) RootGraph=Node;
        if (Node->NodeInstance==Root->GetChildNode(ErrandIndex-1)) PreviousGraph=Node;
        if (Node->NodeInstance==ErrandTask) ErrandGraph=Node;
    }
    auto* Output=RootGraph?RootGraph->GetOutputPin():nullptr;
    if (!Output || !PreviousGraph || !ErrandGraph || !Output->LinkedTo.Contains(PreviousGraph->GetInputPin()) ||
        !Output->LinkedTo.Contains(ErrandGraph->GetInputPin()) || ErrandGraph->NodePosX-PreviousGraph->NodePosX<2)
        return TEXT("寻找分支插入点与正式委托图不匹配");
    auto* Subtree=ResidentAsset<UBehaviorTree>(SubtreePath);
    if (!Subtree) return TEXT("创建寻找子树失败");
    if (Subtree->RootNode || Subtree->BTGraph)
    {
        const auto* Existing=Subtree->RootNode && Subtree->RootNode->Children.Num()==1?
            Cast<UBTTask_DeliveryCompanionErrand>(Subtree->RootNode->GetChildNode(0)):nullptr;
        if (!Existing || !Existing->bSearchStageOnly || Subtree->BlackboardAsset!=BB)
            return TEXT("已有寻找子树与同伴配置不兼容");
    }
    else
    {
        Subtree->BlackboardAsset=BB;
        Subtree->RootNode=NewObject<UBTComposite_Sequence>(Subtree);
        auto* Search=NewObject<UBTTask_DeliveryCompanionErrand>(Subtree);
        Search->bSearchStageOnly=true;
        Search->NodeName=TEXT("前往线索并搜索附近");
        Subtree->RootNode->Children.AddDefaulted_GetRef().ChildTask=Search;
        RebuildResidentGraph(Subtree);
        if (!Subtree->BTGraph || !SaveResidentAsset(Subtree)) return TEXT("保存寻找子树失败");
    }
    if (KeyId==FBlackboard::InvalidKey)
    {
        BB->Modify();
        FBlackboardEntry Entry; Entry.EntryName=SearchKey; Entry.KeyType=NewObject<UBlackboardKeyType_Bool>(BB);
        Entry.EntryDescription=TEXT("当前委托需要走到主人指出的位置附近，观察后才转入拿取");
        BB->Keys.Add(Entry); BB->UpdateKeyIDs();
        if (!SaveResidentAsset(BB)) return TEXT("保存寻找黑板键失败");
    }
    Tree->Modify(); Graph->Modify(); Graph->LockUpdates();
    FGraphNodeCreator<UBehaviorTreeGraphNode_Task> Builder(*Graph);
    auto* Node=Builder.CreateNode();
    auto* Run=NewObject<UBTTask_RunBehavior>(Tree);
    FindFProperty<FObjectProperty>(UBTTask_RunBehavior::StaticClass(),TEXT("BehaviorAsset"))->SetObjectPropertyValue_InContainer(Run,Subtree);
    Node->NodeInstance=Run;
    Node->ClassData=FGraphNodeClassData(Run->GetClass(),FString());
    Node->NodePosX=PreviousGraph->NodePosX+(ErrandGraph->NodePosX-PreviousGraph->NodePosX)/2;
    Node->NodePosY=ErrandGraph->NodePosY;
    Builder.Finalize();
    auto* Gate=NewObject<UBTDecorator_Blackboard>(Tree);
    const auto* KeyProperty=FindFProperty<FStructProperty>(Gate->GetClass(),TEXT("BlackboardKey"));
    auto* Selector=KeyProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(Gate);
    Selector->SelectedKeyName=SearchKey; Selector->ResolveSelectedKey(*BB);
    FindFProperty<FByteProperty>(Gate->GetClass(),TEXT("OperationType"))->SetPropertyValue_InContainer(Gate,EBasicKeyOperation::Set);
    FindFProperty<FByteProperty>(Gate->GetClass(),TEXT("BasicOperation"))->SetPropertyValue_InContainer(Gate,EBasicKeyOperation::Set);
    FindFProperty<FByteProperty>(Gate->GetClass(),TEXT("FlowAbortMode"))->SetPropertyValue_InContainer(Gate,EBTFlowAbortMode::Both);
    static_cast<UBTNode*>(Gate)->InitializeFromAsset(*Tree); Gate->BuildDescription();
    auto* GateNode=NewObject<UBehaviorTreeGraphNode_Decorator>(Graph);
    GateNode->NodeInstance=Gate;
    GateNode->ClassData=FGraphNodeClassData(Gate->GetClass(),FString());
    Node->AddSubNode(GateNode,Graph);
    Output->Modify(); Output->MakeLinkTo(Node->GetInputPin());
    Graph->UnlockUpdates();
    Graph->UpdateBlackboardChange(); Graph->UpdateAsset(); Graph->NotifyGraphChanged();
    if (Root->Children.Num()<4 || Root->GetChildNode(ErrandIndex)!=Run || Root->GetChildNode(ErrandIndex+1)!=ErrandTask)
        return TEXT("寻找分支优先级检查失败，未保存同伴树");
    return SaveResidentAsset(Tree)?TEXT("保存了同伴寻找子树与黑板接线"):TEXT("保存同伴树失败");
}

FString UDeliveryNpcToolset::DescribeCompanionBehaviorAsset()
{
    auto Report=MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema_version"),1);
    TArray<TSharedPtr<FJsonValue>> Errors;
    auto Error=[&](const FString& Reason){ Errors.Add(MakeShared<FJsonValueString>(Reason)); };
    auto* Tree=LoadObject<UBehaviorTree>(nullptr,TEXT("/Game/NPC/Companion/AI/BT_Companion"));
    Report->SetStringField(TEXT("tree_path"),GetPathNameSafe(Tree));
    auto* BB=Tree?Tree->BlackboardAsset.Get():nullptr;
    Report->SetStringField(TEXT("blackboard_path"),GetPathNameSafe(BB));
    if (!Tree || !Tree->RootNode) Error(TEXT("Missing companion behavior tree root"));
    if (!BB) Error(TEXT("Missing companion blackboard"));
    if (BB)
    {
        auto Data=MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("path"),BB->GetPathName());
        Data->SetStringField(TEXT("parent_path"),GetPathNameSafe(BB->Parent));
        TArray<TSharedPtr<FJsonValue>> Keys;
        for (int32 Index=0;Index<BB->GetNumKeys();++Index)
        {
            const auto KeyId=static_cast<FBlackboard::FKey>(Index);
            auto Key=MakeShared<FJsonObject>();
            Key->SetNumberField(TEXT("id"),Index); Key->SetStringField(TEXT("name"),BB->GetKeyName(KeyId).ToString());
            Key->SetStringField(TEXT("type"),GetPathNameSafe(BB->GetKeyType(KeyId).Get()));
            Key->SetBoolField(TEXT("instance_synced"),BB->IsKeyInstanceSynced(KeyId));
            Key->SetBoolField(TEXT("inherited"),Index<static_cast<int32>(BB->GetFirstKeyID()));
            Keys.Add(MakeShared<FJsonValueObject>(Key));
        }
        Data->SetArrayField(TEXT("keys"),Keys); Report->SetObjectField(TEXT("blackboard"),Data);
        if (BB->GetPathName()!=TEXT("/Game/NPC/Companion/AI/BB_Companion.BB_Companion")) Error(TEXT("Tree must use its independent BB_Companion"));
        const auto Key=BB->GetKeyID(TEXT("HasCompanionErrand"));
        if (Key==FBlackboard::InvalidKey || BB->GetKeyType(Key)!=UBlackboardKeyType_Bool::StaticClass() || BB->IsKeyInstanceSynced(Key))
            Error(TEXT("HasCompanionErrand must be an unsynchronized bool"));
    }
    auto DescribeDecorator=[](const UBTDecorator* Decorator)
    {
        auto Item=MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("path"),GetPathNameSafe(Decorator));
        if (!Decorator) return Item;
        Item->SetStringField(TEXT("class"),Decorator->GetClass()->GetPathName());
        Item->SetNumberField(TEXT("flow_abort_mode"),Decorator->GetFlowAbortMode());
        const auto Abort=Decorator->GetFlowAbortMode();
        Item->SetStringField(TEXT("flow_abort_mode_name"),Abort==EBTFlowAbortMode::Both?TEXT("Both"):
            Abort==EBTFlowAbortMode::Self?TEXT("Self"):Abort==EBTFlowAbortMode::LowerPriority?TEXT("LowerPriority"):TEXT("None"));
        if (const auto* Blackboard=Cast<UBTDecorator_Blackboard>(Decorator))
        {
            Item->SetStringField(TEXT("key"),Blackboard->GetSelectedBlackboardKey().ToString());
            for (const auto& Pair:{TPair<const TCHAR*,const TCHAR*>(TEXT("OperationType"),TEXT("operation")),
                TPair<const TCHAR*,const TCHAR*>(TEXT("BasicOperation"),TEXT("basic_operation"))})
                if (const auto* Property=FindFProperty<FByteProperty>(Decorator->GetClass(),Pair.Key))
                    Item->SetNumberField(Pair.Value,Property->GetPropertyValue_InContainer(Decorator));
            if (const auto* Property=FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("BasicOperation")))
                Item->SetStringField(TEXT("basic_operation_name"),Property->GetPropertyValue_InContainer(Decorator)==EBasicKeyOperation::Set?TEXT("Set"):TEXT("NotSet"));
        }
        return Item;
    };
    TSet<const UBTNode*> RuntimeVisited;
    TFunction<TSharedRef<FJsonObject>(const UBTNode*)> DescribeRuntime;
    DescribeRuntime=[&](const UBTNode* Node)
    {
        auto Data=MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("path"),GetPathNameSafe(Node));
        if (!Node) return Data;
        Data->SetStringField(TEXT("class"),Node->GetClass()->GetPathName());
        Data->SetStringField(TEXT("name"),Node->GetNodeName());
        if (RuntimeVisited.Contains(Node)) { Data->SetBoolField(TEXT("already_described"),true); return Data; }
        RuntimeVisited.Add(Node);
        if (const auto* Errand=Cast<UBTTask_DeliveryCompanionErrand>(Node))
            Data->SetBoolField(TEXT("search_stage_only"),Errand->bSearchStageOnly);
        if (const auto* Run=Cast<UBTTask_RunBehavior>(Node))
            if (const auto* Subtree=Run->GetSubtreeAsset())
            {
                auto Asset=MakeShared<FJsonObject>();
                Asset->SetStringField(TEXT("path"),Subtree->GetPathName());
                Asset->SetStringField(TEXT("blackboard_path"),GetPathNameSafe(Subtree->BlackboardAsset.Get()));
                Asset->SetObjectField(TEXT("root"),DescribeRuntime(Subtree->RootNode));
                Data->SetObjectField(TEXT("subtree"),Asset);
            }
        TArray<TSharedPtr<FJsonValue>> Services,Children;
        const auto* Composite=Cast<UBTCompositeNode>(Node);
        Data->SetStringField(TEXT("kind"),Composite?TEXT("composite"):Node->IsA<UBTTaskNode>()?TEXT("task"):TEXT("auxiliary"));
        if (Composite)
        {
            for (const auto& Service:Composite->Services) Services.Add(MakeShared<FJsonValueObject>(DescribeRuntime(Service)));
            for (int32 Index=0;Index<Composite->Children.Num();++Index)
            {
                const auto& Child=Composite->Children[Index];
                auto Entry=MakeShared<FJsonObject>(); Entry->SetNumberField(TEXT("index"),Index);
                Entry->SetObjectField(TEXT("node"),DescribeRuntime(Composite->GetChildNode(Index)));
                TArray<TSharedPtr<FJsonValue>> Decorators,Operations;
                for (const auto& Decorator:Child.Decorators) Decorators.Add(MakeShared<FJsonValueObject>(DescribeDecorator(Decorator)));
                for (const auto& Operation:Child.DecoratorOps)
                {
                    auto Op=MakeShared<FJsonObject>(); Op->SetNumberField(TEXT("operation"),static_cast<int32>(Operation.Operation));
                    Op->SetNumberField(TEXT("number"),Operation.Number); Operations.Add(MakeShared<FJsonValueObject>(Op));
                }
                Entry->SetArrayField(TEXT("decorators"),Decorators); Entry->SetArrayField(TEXT("decorator_ops"),Operations);
                Children.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        else if (const auto* Task=Cast<UBTTaskNode>(Node))
            for (const auto& Service:Task->Services) Services.Add(MakeShared<FJsonValueObject>(DescribeRuntime(Service)));
        Data->SetArrayField(TEXT("services"),Services); Data->SetArrayField(TEXT("children"),Children);
        return Data;
    };
    if (Tree)
    {
        Report->SetObjectField(TEXT("root"),DescribeRuntime(Tree->RootNode));
        TArray<TSharedPtr<FJsonValue>> Decorators;
        for (const auto& Decorator:Tree->RootDecorators) Decorators.Add(MakeShared<FJsonValueObject>(DescribeDecorator(Decorator)));
        Report->SetArrayField(TEXT("root_decorators"),Decorators);
    }
    auto* Graph=Tree?Cast<UBehaviorTreeGraph>(Tree->BTGraph):nullptr;
    if (!Graph) Error(TEXT("Missing companion editor graph"));
    if (Graph)
    {
        auto Data=MakeShared<FJsonObject>(); Data->SetStringField(TEXT("path"),Graph->GetPathName());
        int32 GraphRootCount=0;
        TSet<const UEdGraphNode*> GraphVisited;
        TFunction<TSharedRef<FJsonObject>(const UEdGraphNode*)> DescribeGraphNode;
        DescribeGraphNode=[&](const UEdGraphNode* Node)
        {
            auto Entry=MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("path"),GetPathNameSafe(Node));
            if (!Node) return Entry;
            Entry->SetStringField(TEXT("guid"),Node->NodeGuid.ToString()); Entry->SetStringField(TEXT("class"),Node->GetClass()->GetPathName());
            Entry->SetNumberField(TEXT("x"),Node->NodePosX); Entry->SetNumberField(TEXT("y"),Node->NodePosY);
            Entry->SetStringField(TEXT("comment"),Node->NodeComment);
            if (GraphVisited.Contains(Node)) { Entry->SetBoolField(TEXT("already_described"),true); return Entry; }
            GraphVisited.Add(Node);
            TArray<TSharedPtr<FJsonValue>> Pins,SubNodes,Decorators,Services;
            for (const UEdGraphPin* Pin:Node->Pins)
            {
                if (!Pin) continue;
                auto P=MakeShared<FJsonObject>(); P->SetStringField(TEXT("name"),Pin->PinName.ToString());
                P->SetStringField(TEXT("direction"),Pin->Direction==EGPD_Input?TEXT("Input"):TEXT("Output"));
                TArray<TSharedPtr<FJsonValue>> Links;
                for (const UEdGraphPin* Link:Pin->LinkedTo)
                {
                    if (!Link) continue;
                    auto L=MakeShared<FJsonObject>(); L->SetStringField(TEXT("node_guid"),Link->GetOwningNode()->NodeGuid.ToString());
                    L->SetStringField(TEXT("pin_name"),Link->PinName.ToString());
                    L->SetStringField(TEXT("direction"),Link->Direction==EGPD_Input?TEXT("Input"):TEXT("Output"));
                    Links.Add(MakeShared<FJsonValueObject>(L));
                }
                P->SetArrayField(TEXT("links"),Links); Pins.Add(MakeShared<FJsonValueObject>(P));
            }
            if (const auto* BT=Cast<UBehaviorTreeGraphNode>(Node))
            {
                Entry->SetStringField(TEXT("instance_path"),GetPathNameSafe(BT->NodeInstance));
                Entry->SetStringField(TEXT("instance_class"),BT->NodeInstance?BT->NodeInstance->GetClass()->GetPathName():FString());
                for (const auto& Sub:BT->SubNodes) SubNodes.Add(MakeShared<FJsonValueObject>(DescribeGraphNode(Sub)));
                for (const auto& Sub:BT->Decorators) Decorators.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Sub)));
                for (const auto& Sub:BT->Services) Services.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Sub)));
            }
            if (const auto* Root=Cast<UBehaviorTreeGraphNode_Root>(Node))
            {
                ++GraphRootCount;
                Entry->SetStringField(TEXT("blackboard_path"),GetPathNameSafe(Root->BlackboardAsset));
                if (Root->BlackboardAsset!=BB) Error(TEXT("Editor graph root blackboard differs from runtime tree"));
            }
            Entry->SetArrayField(TEXT("pins"),Pins); Entry->SetArrayField(TEXT("sub_nodes"),SubNodes);
            Entry->SetArrayField(TEXT("decorator_paths"),Decorators); Entry->SetArrayField(TEXT("service_paths"),Services);
            return Entry;
        };
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const auto& Node:Graph->Nodes) Nodes.Add(MakeShared<FJsonValueObject>(DescribeGraphNode(Node)));
        if (GraphRootCount!=1) Error(TEXT("Editor graph must contain exactly one root"));
        Data->SetArrayField(TEXT("nodes"),Nodes); Report->SetObjectField(TEXT("graph"),Data);
    }
    if (Tree && Tree->RootNode)
    {
        const auto* Root=Tree->RootNode.Get();
        if (!Root->IsA<UBTComposite_Selector>()) Error(TEXT("Companion root must be a selector"));
        auto Leaf=[](const UBTNode* Node)
        {
            TSet<const UBTNode*> Seen;
            while (const auto* Sequence=Cast<UBTComposite_Sequence>(Node))
            {
                if (Sequence->Children.Num()!=1 || Seen.Contains(Node)) break;
                Seen.Add(Node); Node=Sequence->GetChildNode(0);
            }
            return Node;
        };
        const TArray<FName> Keys={TEXT("Unavailable"),TEXT("Listening"),TEXT("HasApprovedAction"),TEXT("HasCompanionErrand")};
        const TArray<UClass*> Classes={UBTTask_Wait::StaticClass(),UBTTask_DeliveryListen::StaticClass(),UBTTask_DeliveryExecuteNpcAction::StaticClass(),UBTTask_DeliveryCompanionErrand::StaticClass()};
        int32 PreviousIndex=INDEX_NONE;
        for (int32 Kind=0;Kind<Keys.Num();++Kind)
        {
            int32 FoundIndex=INDEX_NONE;
            for (int32 Index=0;Index<Root->Children.Num();++Index)
                for (const auto& Raw:Root->Children[Index].Decorators)
                {
                    const auto* Decorator=Cast<UBTDecorator_Blackboard>(Raw);
                    if (!Decorator || Decorator->GetSelectedBlackboardKey()!=Keys[Kind]) continue;
                    if (FoundIndex!=INDEX_NONE) Error(TEXT("Duplicate branch condition: ")+Keys[Kind].ToString());
                    FoundIndex=Index;
                    const auto* Task=Leaf(Root->GetChildNode(Index));
                    const auto* Operation=FindFProperty<FByteProperty>(Decorator->GetClass(),TEXT("BasicOperation"));
                    if (!Task || !Task->IsA(Classes[Kind]) || !Operation || Operation->GetPropertyValue_InContainer(Decorator)!=EBasicKeyOperation::Set || Decorator->GetFlowAbortMode()!=EBTFlowAbortMode::Both)
                        Error(TEXT("Invalid task or Set/Both condition for ")+Keys[Kind].ToString());
                }
            if (FoundIndex<=PreviousIndex) Error(TEXT("Missing or misplaced branch: ")+Keys[Kind].ToString());
            PreviousIndex=FoundIndex;
        }
        if (Root->Children.Num()<2 || PreviousIndex!=Root->Children.Num()-2) Error(TEXT("Errand must immediately precede default follow"));
        const auto* Default=Root->Children.IsEmpty()?nullptr:Leaf(Root->GetChildNode(Root->Children.Num()-1));
        if (!Default || !Default->IsA<UBTTask_DeliveryCompanionFollow>() || !Root->Children.Last().Decorators.IsEmpty())
            Error(TEXT("Default branch must be undecorated companion follow"));
    }
    Report->SetBoolField(TEXT("ready_for_errands"),Errors.IsEmpty()); Report->SetArrayField(TEXT("validation_errors"),Errors);
    FString Json; FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json)); return Json;
}

FString UDeliveryNpcToolset::InspectNpcBehaviorCatalog()
{
    const auto& Reference=GetDefault<UDeliveryAiSettings>()->NpcBehaviorCatalog;
    const auto* Table=Reference.LoadSynchronous();
    FDeliveryNpcBehaviorCatalog Catalog;
    const bool bLoaded=Table && Catalog.Load(*Table);
    const auto* Fetch=Catalog.Find(FGameplayTag::RequestGameplayTag(TEXT("Npc.Behavior.Inventory.FetchItem"),false));
    return FString::Printf(TEXT("valid=%d rows=%d fetch_item=%d asset=%s"),bLoaded,Catalog.Num(),
        Fetch && Fetch->Action==EDeliveryNpcAction::FetchItem,*Reference.ToSoftObjectPath().ToString());
}

FString UDeliveryNpcToolset::CreateResidentAssets()
{
    TArray<FString> BlackboardMigrationWarnings;
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
        // 只迁移旧 SelfActor，合法条件保持原样；倾听分支插入后节点下标已经变化
        for (int32 Index=0;Index<Tree->RootNode->Children.Num();++Index)
        {
            const auto& Branch=Tree->RootNode->Children[Index];
            for (UBTDecorator* Decorator:Branch.Decorators)
            {
                if (!Decorator || !Decorator->IsA<UBTDecorator_Blackboard>()) continue;
                auto* Property=FindFProperty<FStructProperty>(Decorator->GetClass(),TEXT("BlackboardKey"));
                if (!Property) continue;
                auto* Selector=Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator);
                if (Selector->SelectedKeyName!=TEXT("SelfActor")) continue;
                const UBTNode* LeafNode=Tree->RootNode->GetChildNode(Index);
                TSet<const UBTNode*> Visited;
                while (const auto* Sequence=Cast<UBTComposite_Sequence>(LeafNode))
                {
                    if (Sequence->Children.Num()!=1 || Visited.Contains(Sequence)) break;
                    Visited.Add(Sequence); LeafNode=Sequence->GetChildNode(0);
                }
                FName Key=NAME_None;
                if (LeafNode && LeafNode->IsA<UBTTask_DeliveryListen>()) Key=TEXT("Listening");
                else if (LeafNode && LeafNode->IsA<UBTTask_DeliveryExecuteNpcAction>()) Key=TEXT("HasApprovedAction");
                else if (LeafNode && LeafNode->IsA<UBTTask_Wait>()) Key=TEXT("Unavailable");
                const auto KeyId=BB->GetKeyID(Key);
                if (Key.IsNone() || KeyId==FBlackboard::InvalidKey || BB->GetKeyType(KeyId)!=UBlackboardKeyType_Bool::StaticClass())
                {
                    BlackboardMigrationWarnings.Add(FString::Printf(TEXT("branch %d SelfActor kept: unsupported leaf %s or missing bool key %s"),
                        Index,*GetNameSafe(LeafNode?LeafNode->GetClass():nullptr),*Key.ToString()));
                    continue;
                }
                Decorator->Modify(); Selector->SelectedKeyName=Key;
                Selector->ResolveSelectedKey(*BB);
                static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*Tree);
                CastChecked<UBTDecorator_Blackboard>(Decorator)->BuildDescription();
            }
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
    FString Result=TEXT("Saved BB_Resident, BT_Resident, BTT_ResidentExecute, resident pair and conversation input");
    if (!BlackboardMigrationWarnings.IsEmpty()) Result+=TEXT("; migration warnings: ")+FString::Join(BlackboardMigrationWarnings,TEXT("; "));
    return Result;
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
    // 显式重建时先等模型编译，启动编辑器运行的资产脚本也可能早于异步编译完成
    FlushAsyncLoading();
    FAssetCompilingManager::Get().FinishAllCompilation();
    bool bRemovedAsyncLoadLock=false;
    if (IsRunningCommandlet())
    {
        // 命令行脚本没有编辑器的延后 Tick，只在这里解除已结束加载的导航锁
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

bool UDeliveryNpcToolset::HoldCompanionPlaytestKey(FName KeyName,float Seconds)
{
    static const TSet<FName> Keys={TEXT("E"),TEXT("W"),TEXT("A"),TEXT("S"),TEXT("D")};
    if (!GEditor || !GEditor->PlayWorld || !Keys.Contains(KeyName) || !FMath::IsFinite(Seconds) || Seconds<.15f || Seconds>15.f)
        return false;
    auto* World=GEditor->PlayWorld.Get();
    auto* PC=World->GetFirstPlayerController();
    auto* ViewportClient=World->GetGameViewport();
    const auto* Player=PC?PC->GetLocalPlayer():nullptr;
    if (!Player || !PC->GetPawn() || !ViewportClient || !ViewportClient->Viewport) return false;
    const FInputDeviceId Device=UInputDeviceLibrary::GetPrimaryInputDeviceForUser(Player->GetPlatformUserId());
    const FKey Key(KeyName);
    if (!Device.IsValid() || PC->IsInputKeyDown(Key)) return false;
    // 走实际视口与 Enhanced Input，计时使用当前游戏世界，退出 PIE 时一起清理
    ViewportClient->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f,-1,Device,false,ViewportClient->Viewport));
    FTimerHandle Release;
    World->GetTimerManager().SetTimer(Release,FTimerDelegate::CreateWeakLambda(ViewportClient,[ViewportClient,Key,Device]()
    {
        if (ViewportClient->Viewport)
            ViewportClient->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Released,0.f,-1,Device,false,ViewportClient->Viewport));
    }),Seconds,false);
    return true;
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
