#include "Input/DeliveryRightMouseGesture.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryRightMouseGestureTest,"Delivery.Systems.RightMouseGesture",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)

bool FDeliveryRightMouseGestureTest::RunTest(const FString&)
{
    FDeliveryRightMouseGesture Gesture;
    TestEqual(TEXT("手柄等其他输入不使用鼠标点击判断"),Gesture.Finish(1),EDeliveryRightMouseRelease::OtherInput);
    Gesture.Begin(10);
    TestEqual(TEXT("明确短按松开后执行右拳"),Gesture.Finish(10.1),EDeliveryRightMouseRelease::Click);
    TestEqual(TEXT("重复松开不会补第二拳"),Gesture.Finish(10.15),EDeliveryRightMouseRelease::OtherInput);

    Gesture.Begin(20);
    TestEqual(TEXT("右键长按即使没有移动也不出拳"),Gesture.Finish(20.3),EDeliveryRightMouseRelease::ViewOrGrab);
    Gesture.Begin(30);
    Gesture.AddMovement(FVector2D(8,0));
    TestEqual(TEXT("快速转动视角也不会被短按误判"),Gesture.Finish(30.05),EDeliveryRightMouseRelease::ViewOrGrab);
    Gesture.Begin(40);
    Gesture.AddMovement(FVector2D(1,0));
    Gesture.AddMovement(FVector2D(0,1));
    TestEqual(TEXT("轻微鼠标抖动仍可点击"),Gesture.Finish(40.1),EDeliveryRightMouseRelease::Click);
    Gesture.Begin(45);
    Gesture.AddMovement(FVector2D(2,0),100);
    Gesture.AddMovement(FVector2D(2,0),100);
    TestEqual(TEXT("松开时重复读取同帧位移不会把轻微抖动误判为拖动"),Gesture.Finish(45.1),EDeliveryRightMouseRelease::Click);
    Gesture.Begin(50);
    Gesture.AddMovement(FVector2D(2,0));
    Gesture.AddMovement(FVector2D(-2,0));
    TestEqual(TEXT("拖动后回到原处仍属于转视角"),Gesture.Finish(50.1),EDeliveryRightMouseRelease::ViewOrGrab);

    Gesture.Begin(60);
    Gesture.BlockClick();
    TestEqual(TEXT("参加双键抓取后松开不补右拳"),Gesture.Finish(60.1),EDeliveryRightMouseRelease::ViewOrGrab);
    Gesture.Begin(70);
    Gesture.BlockClick();
    Gesture.AddMovement(FVector2D::ZeroVector);
    TestEqual(TEXT("输入取消后零位移也不能重新变成点击"),Gesture.Finish(70.1),EDeliveryRightMouseRelease::ViewOrGrab);
    Gesture.Begin(80);
    Gesture.Reset();
    TestFalse(TEXT("换角色后清除尚未松开的右键"),Gesture.IsActive());
    TestEqual(TEXT("旧角色迟到的松开不再属于一次点击"),Gesture.Finish(80.1),EDeliveryRightMouseRelease::OtherInput);
    Gesture.Begin(90);
    TestEqual(TEXT("取消后新的短按可以正常出拳"),Gesture.Finish(90.1),EDeliveryRightMouseRelease::Click);
    return true;
}
#endif
