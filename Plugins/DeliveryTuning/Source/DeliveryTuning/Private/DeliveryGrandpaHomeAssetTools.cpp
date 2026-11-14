#include "DeliveryGrandpaHomeAssetTools.h"
#include "NPC/BTTask_DeliveryGrandpaMoveOut.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "World/DeliveryHome.h"
#include "NavLinkCustomComponent.h"
#include "NavigationSystem.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/ScopeExit.h"
#include "AI/Companion/DeliveryCompanionCharacter.h"
#include "NPC/DeliveryGrandpa.h"
#include "Components/SceneComponent.h"
#include "AssetCompilingManager.h"

namespace
{
const TCHAR* TreePath=TEXT("/Game/NPC/Grandpa/AI/BT_Grandpa");
const TCHAR* TaskPath=TEXT("/Game/NPC/Grandpa/AI/BTT_GrandpaMoveOut");

bool SaveHomeBehavior(UObject* Asset)
{
    Asset->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(Asset->GetPackage(),Asset,
        *FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}

UBTCompositeNode* GetDeliveredBranch(UBehaviorTree* Tree)
{
    if (!Tree || !Tree->RootNode || Tree->RootNode->Children.Num()!=4) return nullptr;
    auto* Done=Tree->RootNode->Children[0].ChildComposite.Get();
    if (!Done || Done->Children.Num()<3 || Done->Children.Num()>4) return nullptr;
    const auto* First=Done->Children[0].ChildTask.Get();
    const auto* Last=Done->Children.Last().ChildTask.Get();
    if (!First || First->GetClass()->GetName()!=TEXT("BTT_GrandpaThanks_C") ||
        !Cast<UBTTask_Wait>(Done->Children[1].ChildTask) ||
        !Last || Last->GetClass()->GetName()!=TEXT("BTT_GrandpaFinished_C")) return nullptr;
    return Done;
}

void RebuildHomeBehaviorGraph(UBehaviorTree* Tree)
{
    if (Tree->BTGraph)
    {
        Tree->BTGraph->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
        Tree->BTGraph=nullptr;
    }
    Tree->BTGraph=FBlueprintEditorUtils::CreateNewGraph(Tree,TEXT("Behavior Tree"),UBehaviorTreeGraph::StaticClass(),UEdGraphSchema_BehaviorTree::StaticClass());
    auto* Graph=CastChecked<UBehaviorTreeGraph>(Tree->BTGraph);
    Graph->LockUpdates();
    Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
    Graph->OnCreated(); Graph->Initialize();
    for (UEdGraphNode* Raw:Graph->Nodes)
    {
        auto* Node=Cast<UBehaviorTreeGraphNode>(Raw);
        if (!Node) continue;
        if (Node->NodeInstance) Node->ClassData=FGraphNodeClassData(Node->NodeInstance->GetClass(),FString());
        for (UAIGraphNode* Sub:Node->SubNodes)
            if (Sub && Sub->NodeInstance) Sub->ClassData=FGraphNodeClassData(Sub->NodeInstance->GetClass(),FString());
        if (!Node->NodeInstance) { Node->NodePosX=1900; Node->NodePosY=0; }
        if (Node->NodeInstance==Tree->RootNode.Get()) { Node->NodePosX=1900; Node->NodePosY=150; }
        for (int32 Branch=0;Branch<Tree->RootNode->Children.Num();++Branch)
        {
            auto* Sequence=Tree->RootNode->Children[Branch].ChildComposite.Get();
            if (!Sequence) continue;
            if (Node->NodeInstance==Sequence) { Node->NodePosX=Branch*1400+400; Node->NodePosY=350; }
            for (int32 Child=0;Child<Sequence->Children.Num();++Child)
                if (Node->NodeInstance==Sequence->GetChildNode(Child))
                { Node->NodePosX=Branch*1400+Child*330; Node->NodePosY=650; }
        }
    }
    Graph->UnlockUpdates();
    Graph->UpdateAsset();
}
}

bool UDeliveryGrandpaHomeAssetTools::InspectMoveOutBehavior()
{
    const auto* Branch=GetDeliveredBranch(LoadObject<UBehaviorTree>(nullptr,TreePath));
    const auto* BP=LoadObject<UBlueprint>(nullptr,TaskPath);
    return Branch && Branch->Children.Num()==4 && BP && BP->Status!=BS_Error && BP->GeneratedClass &&
        BP->GeneratedClass->IsChildOf(UBTTask_DeliveryGrandpaMoveOut::StaticClass()) &&
        Branch->Children[2].ChildTask && Branch->Children[2].ChildTask->GetClass()==BP->GeneratedClass;
}

bool UDeliveryGrandpaHomeAssetTools::ConfigureMoveOutBehavior()
{
    auto* Tree=LoadObject<UBehaviorTree>(nullptr,TreePath);
    auto* Branch=GetDeliveredBranch(Tree);
    if (!Branch) return false;
    if (Branch->Children.Num()==4) return InspectMoveOutBehavior();
    auto* BP=LoadObject<UBlueprint>(nullptr,TaskPath);
    if (!BP)
    {
        BP=FKismetEditorUtilities::CreateBlueprint(UBTTask_DeliveryGrandpaMoveOut::StaticClass(),CreatePackage(TaskPath),
            TEXT("BTT_GrandpaMoveOut"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
        if (!BP) return false;
        FAssetRegistryModule::AssetCreated(BP);
        FKismetEditorUtilities::CompileBlueprint(BP);
        if (BP->Status==BS_Error || !BP->GeneratedClass || !SaveHomeBehavior(BP)) return false;
    }
    if (BP->Status==BS_Error || !BP->GeneratedClass || !BP->GeneratedClass->IsChildOf(UBTTask_DeliveryGrandpaMoveOut::StaticClass())) return false;
    FBTCompositeChild MoveOut;
    MoveOut.ChildTask=NewObject<UBTTaskNode>(Tree,BP->GeneratedClass);
    Branch->Children.Insert(MoveOut,2);
    RebuildHomeBehaviorGraph(Tree);
    return SaveHomeBehavior(Tree) && InspectMoveOutBehavior();
}

bool UDeliveryGrandpaHomeAssetTools::BakeDoorwayNavigation(ADeliveryHome* Home)
{
    if (!GEditor || GEditor->PlayWorld || !IsValid(Home) || !Home->DoorwayLink ||
        Home->GetWorld()!=GEditor->GetEditorWorldContext().World() || Home->GetWorld()->HasBegunPlay()) return false;
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(Home->GetWorld());
    if (!Nav) return false;
    FlushAsyncLoading();
    FAssetCompilingManager::Get().FinishAllCompilation();
    // Commandlet 不推进编辑器延迟解锁的帧计数，确认资源准备好后只解除异步加载锁
    if (IsAsyncLoading() || FAssetCompilingManager::Get().GetNumRemainingAssets()!=0) return false;
    Nav->RemoveNavigationBuildLock(ENavigationBuildLock::AsyncLoadLock,UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
    if (Nav->IsNavigationBuildingLocked(static_cast<uint8>(~ENavigationBuildLock::NoUpdateInEditor))) return false;
    Home->DoorwayLink->SetEnabled(false);
    Nav->FlushPendingOperations();
    Nav->Build();
    for (TActorIterator<ANavigationData> It(Home->GetWorld());It;++It) It->EnsureBuildCompletion();
    return !Nav->IsNavigationBuildInProgress();
}

FString UDeliveryGrandpaHomeAssetTools::InspectDoorwayNavigation(ADeliveryHome* Home)
{
    if (!GEditor || GEditor->PlayWorld || !IsValid(Home) || !Home->DoorwayLink ||
        Home->GetWorld()!=GEditor->GetEditorWorldContext().World() || Home->GetWorld()->HasBegunPlay())
        return TEXT("{\"error\":\"EditorWorldRequired\"}");
    auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(Home->GetWorld());
    if (!Nav) return TEXT("{\"error\":\"NavigationMissing\"}");
    auto* Link=Home->DoorwayLink.Get();
    ADeliveryCompanionCharacter* Npc=nullptr;
    for (TActorIterator<ADeliveryCompanionCharacter> It(Home->GetWorld());It;++It)
    {
        if (Npc) return TEXT("{\"error\":\"CompanionNotUnique\"}");
        Npc=*It;
    }
    if (!Npc || !IsValid(Home->FormerResident)) return TEXT("{\"error\":\"SceneActorsMissing\"}");
    FNavLocation Outside,Inside;
    const FVector GrandpaFeet=Home->FormerResident->GetActorLocation()-FVector(0,0,Home->FormerResident->GetSimpleCollisionHalfHeight());
    if (!Nav->ProjectPointToNavigation(Home->ReturnAnchor->GetComponentLocation(),Outside,FVector(100,100,150),&Npc->GetNavAgentPropertiesRef()) ||
        !Nav->ProjectPointToNavigation(GrandpaFeet,Inside,FVector(100,100,150),&Npc->GetNavAgentPropertiesRef()))
        return TEXT("{\"error\":\"SceneEndpointsMissing\"}");
    const bool bOriginal=Link->IsEnabled();
    ON_SCOPE_EXIT { Link->SetEnabled(bOriginal); };
    auto Root=MakeShared<FJsonObject>();
    Root->SetBoolField(TEXT("original_enabled"),bOriginal);
    Root->SetStringField(TEXT("navigation_agent"),Npc->GetPathName());
    Root->SetStringField(TEXT("outside"),Outside.Location.ToString());
    Root->SetStringField(TEXT("inside"),Inside.Location.ToString());
    for (const bool bEnabled:{false,true})
    {
        Link->SetEnabled(bEnabled);
        auto Entry=MakeShared<FJsonObject>();
        bool bComplete=true,bUsesLink=true;
        for (const bool bReturn:{false,true})
        {
            auto* Path=Nav->FindPathToLocationSynchronously(Home->GetWorld(),
                bReturn?Inside.Location:Outside.Location,bReturn?Outside.Location:Inside.Location,Npc);
            const bool bRouteComplete=Path && Path->IsValid() && !Path->IsPartial();
            const bool bRouteUsesLink=Path && Path->GetPath().IsValid() && Path->GetPath()->ContainsCustomLink(Link->GetId());
            auto Route=MakeShared<FJsonObject>();
            Route->SetBoolField(TEXT("complete"),bRouteComplete);
            Route->SetBoolField(TEXT("uses_home_doorway"),bRouteUsesLink);
            Route->SetNumberField(TEXT("length_cm"),Path && Path->IsValid()?Path->GetPathLength():0);
            Entry->SetObjectField(bReturn?TEXT("grandpa_to_return_anchor"):TEXT("return_anchor_to_grandpa"),Route);
            bComplete&=bRouteComplete; bUsesLink&=bRouteUsesLink;
        }
        Entry->SetBoolField(TEXT("complete"),bComplete);
        Entry->SetBoolField(TEXT("uses_home_doorway"),bUsesLink);
        Root->SetObjectField(bEnabled?TEXT("link_enabled"):TEXT("link_disabled"),Entry);
    }
    Link->SetEnabled(bOriginal);
    Root->SetBoolField(TEXT("restored"),Link->IsEnabled()==bOriginal);
    FString Result;
    FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Result));
    return Result;
}
