#include "DeliveryGrandpaAssetTools.h"
#include "NPC/DeliveryGrandpa.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_Event.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
const FString Folder = TEXT("/Game/NPC/Grandpa/AI/");

bool Save(UObject* Asset)
{
    Asset->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    return UPackage::SavePackage(Asset->GetPackage(), Asset,
        *FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
}

template<typename T> T* NewAsset(const FString& Name)
{
    UPackage* Package = CreatePackage(*(Folder + Name));
    T* Asset = NewObject<T>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    FAssetRegistryModule::AssetCreated(Asset);
    return Asset;
}

void RebuildGraph(UBehaviorTree* Tree)
{
    auto* Root = Tree->RootNode.Get();
    if (Tree->BTGraph)
    {
        Tree->BTGraph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        Tree->BTGraph = nullptr;
    }
    Tree->BTGraph = FBlueprintEditorUtils::CreateNewGraph(Tree, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());
    auto* Graph = CastChecked<UBehaviorTreeGraph>(Tree->BTGraph);
    Graph->LockUpdates();
    Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
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

UClass* CreateTask(const TCHAR* Name, EDeliveryGrandpaAction Action)
{
    const FString Path = Folder + Name;
    if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *Path)) return Existing->GeneratedClass;
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(UBTTask_BlueprintBase::StaticClass(),
        CreatePackage(*Path), Name, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!Graph) return nullptr;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    auto* Event = NewObject<UK2Node_Event>(Graph);
    Event->EventReference.SetExternalMember(TEXT("ReceiveExecuteAI"), UBTTask_BlueprintBase::StaticClass());
    Event->bOverrideFunction = true;
    Graph->AddNode(Event, false, false); Event->CreateNewGuid(); Event->PostPlacedNewNode(); Event->AllocateDefaultPins();
    auto* Cast = NewObject<UK2Node_DynamicCast>(Graph);
    Cast->TargetType = ADeliveryGrandpa::StaticClass();
    Graph->AddNode(Cast, false, false); Cast->CreateNewGuid(); Cast->PostPlacedNewNode(); Cast->AllocateDefaultPins();
    Cast->NodePosX = 300;
    auto AddCall = [&](UFunction* Fn, int X)
    {
        auto* Node = NewObject<UK2Node_CallFunction>(Graph);
        Node->SetFromFunction(Fn);
        Graph->AddNode(Node, false, false); Node->CreateNewGuid(); Node->PostPlacedNewNode(); Node->AllocateDefaultPins();
        Node->NodePosX = X;
        return Node;
    };
    auto* Apply = AddCall(ADeliveryGrandpa::StaticClass()->FindFunctionByName(TEXT("ApplyBehaviorAction")), 600);
    bool Ok = true;
    auto Connect = [&](UEdGraphPin* A, UEdGraphPin* B) { Ok &= A && B && Schema->TryCreateConnection(A, B); };
    Connect(Event->FindPin(TEXT("then")), Cast->GetExecPin());
    Connect(Event->FindPin(TEXT("ControlledPawn")), Cast->GetCastSourcePin());
    Connect(Cast->GetValidCastPin(), Apply->GetExecPin());
    Connect(Cast->GetCastResultPin(), Apply->FindPin(TEXT("self")));
    Schema->TrySetDefaultValue(*Apply->FindPin(TEXT("Action")), StaticEnum<EDeliveryGrandpaAction>()->GetNameStringByValue(int64(Action)));
    // 稳态任务保持运行，由黑板装饰器中断，不使用巨大等待时长或循环重播动画
    if (Action != EDeliveryGrandpaAction::Idle && Action != EDeliveryGrandpaAction::Receive && Action != EDeliveryGrandpaAction::Finished)
    {
        auto* Finish = AddCall(UBTTask_BlueprintBase::StaticClass()->FindFunctionByName(TEXT("FinishExecute")), 900);
        Connect(Apply->GetThenPin(), Finish->GetExecPin());
        Schema->TrySetDefaultValue(*Finish->FindPin(TEXT("bSuccess")), TEXT("true"));
    }
    auto* Fail = AddCall(UBTTask_BlueprintBase::StaticClass()->FindFunctionByName(TEXT("FinishExecute")), 600);
    Fail->NodePosY = 250;
    Connect(Cast->GetInvalidCastPin(), Fail->GetExecPin());
    Schema->TrySetDefaultValue(*Fail->FindPin(TEXT("bSuccess")), TEXT("false"));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    FKismetEditorUtilities::CompileBlueprint(BP);
    if (!Ok || BP->Status == BS_Error) return nullptr;
    FAssetRegistryModule::AssetCreated(BP);
    return Save(BP) ? BP->GeneratedClass : nullptr;
}
}

bool UDeliveryGrandpaAssetTools::CreateBehaviorAssets()
{
    // 重跑只使用已保存资产，避免覆盖设计师手工修改的图
    UBehaviorTree* ExistingTree = LoadObject<UBehaviorTree>(nullptr, *(Folder + TEXT("BT_Grandpa")));
    if (ExistingTree) return true;
    TMap<EDeliveryGrandpaAction, UClass*> Tasks;
    for (auto Action : {EDeliveryGrandpaAction::Idle, EDeliveryGrandpaAction::Reply, EDeliveryGrandpaAction::Startled,
        EDeliveryGrandpaAction::Receive, EDeliveryGrandpaAction::Thanks, EDeliveryGrandpaAction::FinishReply, EDeliveryGrandpaAction::Finished})
    {
        UClass* Task = CreateTask(*(TEXT("BTT_Grandpa") + StaticEnum<EDeliveryGrandpaAction>()->GetNameStringByValue(int64(Action))), Action);
        if (!Task) return false;
        Tasks.Add(Action, Task);
    }
    UBlackboardData* BB = NewAsset<UBlackboardData>(TEXT("BB_Grandpa"));
    for (const TCHAR* Key : {TEXT("Delivered"), TEXT("DoorBroken"), TEXT("ReplyPending")})
    {
        FBlackboardEntry Entry;
        Entry.EntryName = Key;
        Entry.KeyType = NewObject<UBlackboardKeyType_Bool>(BB);
        BB->Keys.Add(Entry);
    }
    if (!Save(BB)) return false;
    UBehaviorTree* Tree = NewAsset<UBehaviorTree>(TEXT("BT_Grandpa"));
    Tree->BlackboardAsset = BB;
    auto* Root = NewObject<UBTComposite_Selector>(Tree);
    Tree->RootNode = Root;
    auto Branch = [&](const TCHAR* Key)
    {
        auto* Seq = NewObject<UBTComposite_Sequence>(Tree);
        FBTCompositeChild Child;
        Child.ChildComposite = Seq;
        if (Key)
        {
            auto* Decorator = NewObject<UBTDecorator_Blackboard>(Tree);
            auto* Property = FindFProperty<FStructProperty>(Decorator->GetClass(), TEXT("BlackboardKey"));
            auto* Selector = Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Decorator);
            Selector->SelectedKeyName = Key;
            Selector->ResolveSelectedKey(*BB);
            FindFProperty<FByteProperty>(Decorator->GetClass(), TEXT("OperationType"))->SetPropertyValue_InContainer(Decorator, EBasicKeyOperation::Set);
            FindFProperty<FByteProperty>(Decorator->GetClass(), TEXT("BasicOperation"))->SetPropertyValue_InContainer(Decorator, EBasicKeyOperation::Set);
            FindFProperty<FByteProperty>(Decorator->GetClass(), TEXT("FlowAbortMode"))->SetPropertyValue_InContainer(Decorator, EBTFlowAbortMode::Both);
            static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*Tree);
            Decorator->BuildDescription();
            Child.Decorators.Add(Decorator);
        }
        Root->Children.Add(Child);
        return Seq;
    };
    auto Action = [&](UBTComposite_Sequence* Seq, EDeliveryGrandpaAction Value)
    {
        FBTCompositeChild Child;
        Child.ChildTask = NewObject<UBTTaskNode>(Tree, Tasks[Value]);
        Seq->Children.Add(Child);
    };
    auto Wait = [&](UBTComposite_Sequence* Seq, float Seconds)
    {
        FBTCompositeChild Child;
        auto* Task = NewObject<UBTTask_Wait>(Tree);
        Task->WaitTime = Seconds; Task->RandomDeviation = 0.f;
        Child.ChildTask = Task; Seq->Children.Add(Child);
    };
    auto* Done = Branch(TEXT("Delivered"));
    Action(Done, EDeliveryGrandpaAction::Thanks); Wait(Done, 2.f);
    Action(Done, EDeliveryGrandpaAction::Finished);
    auto* Broken = Branch(TEXT("DoorBroken"));
    Action(Broken, EDeliveryGrandpaAction::Startled); Wait(Broken, 1.f);
    Action(Broken, EDeliveryGrandpaAction::Receive);
    auto* Reply = Branch(TEXT("ReplyPending"));
    Action(Reply, EDeliveryGrandpaAction::Reply); Wait(Reply, 2.f); Action(Reply, EDeliveryGrandpaAction::FinishReply);
    auto* Idle = Branch(nullptr);
    Action(Idle, EDeliveryGrandpaAction::Idle);
    RebuildGraph(Tree);
    return Save(Tree);
}


bool UDeliveryGrandpaAssetTools::UpgradeExpressiveBehavior()
{
    auto* Tree = LoadObject<UBehaviorTree>(nullptr, *(Folder + TEXT("BT_Grandpa")));
    if (!Tree || !Tree->RootNode || Tree->RootNode->Children.Num() != 4) return false;
    auto* Idle = Tree->RootNode->Children[3].ChildComposite.Get();
    auto* Broken = Tree->RootNode->Children[1].ChildComposite.Get();
    if (!Idle || !Broken || Broken->Children.Num() < 2) return false;
    if (auto* Wait = Cast<UBTTask_Wait>(Broken->Children[1].ChildTask)) Wait->WaitTime = 2.5f;
    Idle->Children.Reset();
    for (auto Value : {EDeliveryGrandpaAction::LookAround, EDeliveryGrandpaAction::StrollOut,
        EDeliveryGrandpaAction::LookAround, EDeliveryGrandpaAction::StrollHome})
    {
        const FString Name = TEXT("BTT_Grandpa") + StaticEnum<EDeliveryGrandpaAction>()->GetNameStringByValue(int64(Value));
        UClass* Class = CreateTask(*Name, Value);
        if (!Class) return false;
        FBTCompositeChild Action; Action.ChildTask = NewObject<UBTTaskNode>(Tree, Class); Idle->Children.Add(Action);
        FBTCompositeChild Pause; auto* Wait = NewObject<UBTTask_Wait>(Tree);
        Wait->WaitTime = Value == EDeliveryGrandpaAction::LookAround ? 3.5f : 2.4f;
        Wait->RandomDeviation = 0.f; Pause.ChildTask = Wait; Idle->Children.Add(Pause);
    }
    RebuildGraph(Tree);
    return Save(Tree);
}
