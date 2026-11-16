#if WITH_DEV_AUTOMATION_TESTS

#include "AI/Decision/DeliveryNpcBehaviorCatalog.h"
#include "Misc/AutomationTest.h"
#include "NativeGameplayTags.h"

UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_TestNpcBehaviorLook,TEXT("Npc.Behavior.Test.Look"));
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_TestNpcBehaviorWait,TEXT("Npc.Behavior.Test.Wait"));

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryNpcBehaviorCatalogTest,"Delivery.AI.BehaviorCatalog",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryNpcBehaviorCatalogTest::RunTest(const FString&)
{
    auto* Table=NewObject<UDataTable>();
    Table->RowStruct=FDeliveryNpcBehaviorCatalogRow::StaticStruct();
    FDeliveryNpcBehaviorCatalogRow Look;
    Look.Action=EDeliveryNpcAction::LookAt;
    Look.DisplayName=FText::FromString(TEXT("转头看"));
    Look.Category=TEXT("Attention");
    Look.bPlayerRequestOnly=true;
    Look.ExcludedProfiles.Add(TEXT("xiumei"));
    Table->AddRow(TAG_TestNpcBehaviorLook.GetTag().GetTagName(),Look);
    FDeliveryNpcBehaviorCatalogRow Wait=Look;
    Wait.Action=EDeliveryNpcAction::Wait;
    Wait.bOfferToModel=false;
    Table->AddRow(TAG_TestNpcBehaviorWait.GetTag().GetTagName(),Wait);

    FDeliveryNpcBehaviorCatalog Catalog;
    if (!TestTrue(TEXT("载入有效行为表"),Catalog.Load(*Table))) return false;
    const auto* Found=Catalog.Find(TAG_TestNpcBehaviorLook);
    TestTrue(TEXT("用注册标签查到正确动作"),Found && Found->Action==EDeliveryNpcAction::LookAt);

    FDeliveryNpcDecisionRequest Request;
    Request.AllowedActions={EDeliveryNpcAction::Wait,EDeliveryNpcAction::LookAt,EDeliveryNpcAction::ContinueCurrent};
    for (const auto Action:Request.AllowedActions)
        Request.Candidates.AddDefaulted_GetRef().Action=Action;
    const FGuid Subject=FGuid::NewGuid();
    Request.Candidates[1].TargetId=Subject;
    auto Ambient=Request;
    Catalog.FilterModelOptions(Ambient,TEXT("chunlai"));
    TestTrue(TEXT("环境请求只移除被限定的动作，保留原有结束和继续入口"),Ambient.Candidates.Num()==2 &&
        Ambient.Candidates[0].Action==EDeliveryNpcAction::Wait && Ambient.Candidates[1].Action==EDeliveryNpcAction::ContinueCurrent &&
        !Ambient.AllowedActions.Contains(EDeliveryNpcAction::LookAt));
    Request.bPlayerRequest=true;
    auto Excluded=Request;
    Catalog.FilterModelOptions(Excluded,TEXT("xiumei"));
    TestEqual(TEXT("角色配置同时约束允许动作与候选"),Excluded.Candidates.Num(),2);
    Catalog.FilterModelOptions(Request,TEXT("chunlai"));
    TestTrue(TEXT("保留允许候选的顺序和原目标"),Request.Candidates.Num()==3 && Request.Candidates[1].TargetId==Subject);

    Table->AddRow(TAG_TestNpcBehaviorWait.GetTag().GetTagName(),Look);
    TestFalse(TEXT("拒绝一个执行动作的重复定义"),Catalog.Load(*Table));
    TestTrue(TEXT("错误表不覆盖原来有效索引"),Catalog.Find(TAG_TestNpcBehaviorLook)!=nullptr);
    return true;
}

#endif
