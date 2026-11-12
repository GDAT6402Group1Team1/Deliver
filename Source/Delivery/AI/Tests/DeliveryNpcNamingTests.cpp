#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AI/Characters/DeliveryNpcCharacter.h"
#include "AI/Characters/DeliveryNpcAIController.h"
#include "AI/Actions/DeliveryNpcCombatComponent.h"
#include "AI/Presentation/DeliveryNpcPresentationComponent.h"
#include "AI/BehaviorTree/BTTask_DeliveryExecuteNpcAction.h"
#include "AI/Decision/DeliveryNpcAgentComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/CoreRedirects.h"

// 加载保留旧父类名称的正式蓝图，检查重命名后仍能编译且保留配置
// 测试只在内存中编译，不保存资产，也不进入 PIE 或请求模型
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcNamingTest, "Delivery.AI.NpcNaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcNamingTest::RunTest(const FString& Parameters)
{
    struct FClassName
    {
        const TCHAR* OldPath;
        UClass* NewClass;
    };
    const FClassName Classes[] = {
        {TEXT("/Script/Delivery.DeliveryResidentCharacter"), ADeliveryNpcCharacter::StaticClass()},
        {TEXT("/Script/Delivery.DeliveryResidentAIController"), ADeliveryNpcAIController::StaticClass()},
        {TEXT("/Script/Delivery.DeliveryResidentCombatComponent"), UDeliveryNpcCombatComponent::StaticClass()},
        {TEXT("/Script/Delivery.BTTask_DeliveryExecuteApprovedAction"), UBTTask_DeliveryExecuteNpcAction::StaticClass()},
    };
    for (const FClassName& Entry : Classes)
    {
        // 原生类字符串加载不会自动套用 ClassRedirects，先检查映射，再加载新路径
        // 下方另外加载未改名的正式蓝图，验证资产反序列化确实使用了这些映射
        const auto Redirect = FCoreRedirects::GetRedirectedName(
            ECoreRedirectFlags::Type_Class, FCoreRedirectObjectName(FString(Entry.OldPath)));
        TestEqual(TEXT("旧类名映射到新类路径"), Redirect.ToString(), Entry.NewClass->GetPathName());
        TestEqual(FString::Printf(TEXT("映射后的类可以加载：%s"), Entry.OldPath),
            LoadObject<UClass>(nullptr, *Redirect.ToString()), Entry.NewClass);
    }

    const auto CheckFunction = [&](const TCHAR* OldPath, UClass* Owner, const TCHAR* NewName)
    {
        const auto Redirect = FCoreRedirects::GetRedirectedName(
            ECoreRedirectFlags::Type_Function, FCoreRedirectObjectName(FString(OldPath)));
        TestEqual(TEXT("反射函数映射到新名称"), Redirect.ObjectName, FName(NewName));
        TestNotNull(TEXT("新函数已注册到 UE 反射系统"), Owner->FindFunctionByName(FName(NewName)));
    };
    CheckFunction(TEXT("/Script/Delivery.DeliveryNpcAgentComponent.StartApprovedAction"),
        UDeliveryNpcAgentComponent::StaticClass(), TEXT("StartAction"));
    CheckFunction(TEXT("/Script/Delivery.DeliveryResidentAIController.OnTargetPerceived"),
        ADeliveryNpcAIController::StaticClass(), TEXT("OnTargetSeenOrHeard"));

    for (const TCHAR* Path : {
        TEXT("/Game/NPC/Residents/BP_ResidentLoud.BP_ResidentLoud"),
        TEXT("/Game/NPC/Residents/BP_ResidentShrewd.BP_ResidentShrewd"),
        TEXT("/Game/NPC/Residents/AI/BTT_ResidentExecute.BTT_ResidentExecute")})
    {
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, Path);
        if (!TestNotNull(Path, Blueprint)) continue;
        const bool bTask = FString(Path).Contains(TEXT("BTT_"));
        UClass* ExpectedParent = bTask ? UBTTask_DeliveryExecuteNpcAction::StaticClass() : ADeliveryNpcCharacter::StaticClass();
        TestEqual(TEXT("旧蓝图加载后使用新的父类"), Blueprint->ParentClass.Get(), ExpectedParent);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        TestTrue(TEXT("正式蓝图能够重新编译"), Blueprint->Status != BS_Error);
        if (!TestNotNull(TEXT("蓝图生成类存在"), Blueprint->GeneratedClass.Get())) continue;
        TestTrue(TEXT("生成类继承新的 C++ 类"), Blueprint->GeneratedClass->IsChildOf(ExpectedParent));
        if (bTask) continue;

        const auto* Npc = Cast<ADeliveryNpcCharacter>(Blueprint->GeneratedClass->GetDefaultObject());
        if (!TestNotNull(TEXT("NPC 蓝图默认对象存在"), Npc)) continue;
        if (!TestTrue(TEXT("原有组件仍存在"), Npc->Agent && Npc->Combat && Npc->Presentation && Npc->ConversationInteractable)) continue;
        TestEqual(TEXT("战斗组件实例名保持兼容"), Npc->Combat->GetFName(), FName(TEXT("Combat")));
        TestTrue(TEXT("控制器类引用已更新"), Npc->AIControllerClass && Npc->AIControllerClass->IsChildOf(ADeliveryNpcAIController::StaticClass()));
        TestNotNull(TEXT("角色模型仍有配置"), Npc->GetMesh()->GetSkeletalMeshAsset());
        TestNotNull(TEXT("行为树仍有配置"), Npc->BehaviorTree.Get());
        TestTrue(TEXT("角色名称仍有配置"), !Npc->DisplayName.IsEmpty());
        TestNotNull(TEXT("说话动画仍有配置"), Npc->Presentation->TalkAnimation.Get());
    }
    return !HasAnyErrors();
}
#endif
