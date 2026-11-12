#include "DeliveryResidentToolset.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Builders/CubeBuilder.h"
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Components/CapsuleComponent.h"
#include "Misc/Paths.h"
#include "AI/Residents/DeliveryResidentCharacter.h"
#include "AI/Residents/DeliveryNpcPresentationComponent.h"
#include "AI/Residents/DeliveryResidentCombatComponent.h"
#include "AI/Residents/DeliveryConversationComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "TimerManager.h"
#include "AI/DeliveryNpcAgentComponent.h"
#include "AI/BTTask_DeliveryExecuteApprovedAction.h"
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

void RebuildGraph(UBehaviorTree* Tree)
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

FString UDeliveryResidentToolset::CreateResidentAssets()
{
    const FString Folder=TEXT("/Game/NPC/Residents/");
    auto* BB=ResidentAsset<UBlackboardData>(Folder+TEXT("AI/BB_Resident"));
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
    auto* TaskBP=ResidentBlueprint(UBTTask_DeliveryExecuteApprovedAction::StaticClass(),Folder+TEXT("AI/BTT_ResidentExecute"));
    FKismetEditorUtilities::CompileBlueprint(TaskBP); if (TaskBP->Status==BS_Error || !SaveResidentAsset(TaskBP)) return TEXT("Failed task blueprint");
    auto* Tree=ResidentAsset<UBehaviorTree>(Folder+TEXT("AI/BT_Resident"));
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
        RebuildGraph(Tree); if (!SaveResidentAsset(Tree)) return TEXT("Failed tree save");
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
        auto* BP=ResidentBlueprint(ADeliveryResidentCharacter::StaticClass(),Folder+(Index==0?TEXT("BP_ResidentLoud"):TEXT("BP_ResidentShrewd")));
        auto* CDO=CastChecked<ADeliveryResidentCharacter>(BP->GeneratedClass->GetDefaultObject());
        CDO->BehaviorTree=Tree;
        CDO->DisplayName=FText::FromString(Index==0?TEXT("大嗓门"):TEXT("小算盘"));
        CDO->Agent->Persona=Index==0?TEXT("你是大嗓门，爱面子、脾气急但能接受真诚道歉。与小算盘争论谁挡了路。连续鸣笛会让你恼火，可停下讲理、躺下抗议、靠近或拽住低速车辆。可以短暂互殴，受击后根据事实选择对象。台词短，有生活气息，不重复上一句，不编造经历。"):
            TEXT("你是小算盘，爱较真、嘴硬，喜欢找出对方说话的矛盾。与大嗓门互相抱怨。可回应玩家道歉，也可能被挑拨；不知道的事情要承认不确定。选择符合当前证据的短动作和短台词，避免无限重复。没有新事时安静等待。");
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
    auto* Action=ResidentAsset<UInputAction>(Folder+TEXT("Input/IA_Talk")); Action->ValueType=EInputActionValueType::Boolean;
    auto* Mapping=ResidentAsset<UInputMappingContext>(Folder+TEXT("Input/IMC_ResidentConversation"));
    if (Mapping->GetMappings().IsEmpty()) { Mapping->MapKey(Action,EKeys::T); Mapping->MapKey(Action,EKeys::Gamepad_DPad_Right); }
    if (!SaveResidentAsset(Action) || !SaveResidentAsset(Mapping)) return TEXT("Failed input assets");
    return TEXT("Saved BB_Resident, BT_Resident, BTT_ResidentExecute, resident pair and conversation input");
}
FString UDeliveryResidentToolset::PlaceResidentPair(FVector Location)
{
    UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr; if (!World || GEditor->PlayWorld) return TEXT("Editor world required");
    for (int Index=0;Index<2;++Index)
    {
        const FString Label=Index==0?TEXT("Resident_Loud"):TEXT("Resident_Shrewd");
        ADeliveryResidentCharacter* Existing=nullptr;
        for (TActorIterator<ADeliveryResidentCharacter> It(World);It;++It) if (It->GetActorLabel()==Label) Existing=*It;
        UClass* Class=LoadClass<ADeliveryResidentCharacter>(nullptr,Index==0?TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud_C"):TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd_C"));
        if (!Class) return TEXT("Missing resident blueprint");
        // 两人沿街相对站立，避免第二人向房屋方向偏移；已有实例只调整位置
        FVector Point=Location+FVector(Index*180.f,0,0); FHitResult Ground;
        FCollisionQueryParams Placement(SCENE_QUERY_STAT(ResidentPlacement),false,Existing);
        if (!World->LineTraceSingleByChannel(Ground,Point+FVector(0,0,300),Point-FVector(0,0,1000),ECC_WorldStatic,Placement)
            || Ground.ImpactNormal.Z<.7f) return TEXT("No walkable ground at requested position");
        const auto* Default=Class->GetDefaultObject<ADeliveryResidentCharacter>();
        Point=Ground.Location+FVector(0,0,Default->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+2.f);
        auto* Actor=Existing?Existing:World->SpawnActor<ADeliveryResidentCharacter>(Class,Point,FRotator::ZeroRotator);
        if (!Actor) return TEXT("Spawn rejected");
        Actor->SetActorLocationAndRotation(Point,FRotator(0,Index==0?0:180,0));
        Actor->SetActorLabel(Label); Actor->MarkPackageDirty();
    }
    // 仅给居民所在街区补导航，普通路人仍沿用现有 Boids 路线
    bool bHasBounds=false;
    for (TActorIterator<ANavMeshBoundsVolume> It(World);It;++It) if (It->GetActorLabel()==TEXT("Resident_Navigation")) bHasBounds=true;
    if (!bHasBounds)
    {
        auto* Volume=World->SpawnActor<ANavMeshBoundsVolume>(Location,FRotator::ZeroRotator);
        auto* Builder=NewObject<UCubeBuilder>(); Builder->X=3200; Builder->Y=3200; Builder->Z=1200;
        if (!Builder->Build(World,Volume)) return TEXT("Resident navigation bounds build failed");
        Volume->SetActorLocation(Location); Volume->SetActorLabel(TEXT("Resident_Navigation")); Volume->MarkPackageDirty();
        if (auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(World)) { Nav->OnNavigationBoundsUpdated(Volume); Nav->Build(); }
    }
    return TEXT("Placed resident pair and navigation; save current level to keep placement");
}
FString UDeliveryResidentToolset::InspectResidentWorlds()
{
    FString Result;
    for (const auto& Context:GEngine->GetWorldContexts())
        if (auto* World=Context.World())
            for (TActorIterator<ADeliveryResidentCharacter> It(World);It;++It)
            {
                auto State=It->Agent->GetPublicState();
                Result+=FString::Printf(TEXT("world=%s net=%d npc=%s pos=%s action=%s status=%d down=%d pending=%d speechActive=%d speech=%s\n"),
                    *World->GetName(),int32(World->GetNetMode()),*It->GetName(),*It->GetActorLocation().ToString(),*DeliveryNpc::ActionName(State.Action),int32(State.Status),It->IsKnockedDown(),It->Agent->IsDecisionPending(),It->Presentation->GetSpeech().bActive,*It->Presentation->GetSpeech().Text);
            }
    return Result;
}
bool UDeliveryResidentToolset::ConfigureResidentPIE(int32 Players)
{
    if (GEditor->PlayWorld || Players<1 || Players>3) return false;
    auto* Settings=GetMutableDefault<ULevelEditorPlaySettings>();
    Settings->SetPlayNumberOfClients(Players); Settings->SetPlayNetMode(PIE_ListenServer); Settings->SetRunUnderOneProcess(true); return true;
}

FString UDeliveryResidentToolset::SpeakInResidentPIE(FString Text)
{
    if (Text.IsEmpty() || Text.Len()>120) return TEXT("Speech must contain 1 to 120 characters");
    for (const auto& Context:GEngine->GetWorldContexts())
        if (Context.WorldType==EWorldType::PIE && Context.World() && Context.World()->GetNetMode()!=NM_Client)
            for (TActorIterator<ADeliveryResidentCharacter> It(Context.World());It;++It)
                if (It->DisplayName.ToString()==TEXT("小算盘"))
                    return It->Presentation->PublishSpeech(Text,TEXT("Angry"),0)?TEXT("Published through real speech perception path"):TEXT("Speaker busy");
    return TEXT("No authoritative resident PIE");
}

FString UDeliveryResidentToolset::RunResidentValidation(bool bFull)
{
    if (!GEditor || GEditor->PlayWorld) return TEXT("Stop PIE before validation");
    const TCHAR* Tests=bFull?TEXT("Automation RunTests Delivery.Startup+Delivery.AI.Protocol+Delivery.AI.AuthorityLifecycle+Delivery.AI.ContextBoundary+Delivery.AI.BehaviorTreeExecution+Delivery.AI.ResidentIntegration+Delivery.AI.ResidentMultiplayer+Delivery.AI.ResidentMap+Delivery.Traffic.Horn+Delivery.Vehicle.PedestrianImpact+Delivery.NPC.Boids+Delivery.Systems.InputLifecycle+Delivery.Interaction.WorldRegistry+Delivery.UI.PhoneController+Delivery.UI.PersistentHint+Delivery.Vehicle.Summon.EmptyMap"):
        TEXT("Automation RunTests Delivery.AI.ResidentIntegration+Delivery.AI.ResidentMultiplayer+Delivery.AI.ResidentMap");
    return GEngine->Exec(GEditor->GetEditorWorldContext().World(),Tests)?TEXT("Validation queued; inspect Automation results and Delivery.log"):TEXT("Automation command rejected");
}

FString UDeliveryResidentToolset::TalkInResidentPIE(bool bClient, FString Text)
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
        ADeliveryResidentCharacter* Resident=nullptr; double Distance=MAX_dbl;
        for (TActorIterator<ADeliveryResidentCharacter> It(World);It;++It)
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
