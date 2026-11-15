#include "World/DeliveryHome.h"
#include "NavLinkCustomComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Sound/SoundBase.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Delivery.h"

namespace
{
const FLinearColor ClaimGold(1.f,.80f,.22f,1.f);

// 把门框上的星光和门口标记按世界位置画到本机屏幕上，不参与点击
class SDeliveryHomeClaimMarks : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SDeliveryHomeClaimMarks) {}
        SLATE_ARGUMENT(TWeakObjectPtr<ADeliveryHome>,Home)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args) { Home=Args._Home; SetCanTick(false); }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry& Geometry,const FSlateRect&,
        FSlateWindowElementList& Elements,int32 Layer,const FWidgetStyle&,bool) const override
    {
        const auto* Owner=Home.Get();
        const auto* World=Owner?Owner->GetWorld():nullptr;
        auto* Player=World?World->GetFirstPlayerController():nullptr;
        FDeliveryHomeClaimFeedback View;
        if (!Player || !Player->IsLocalController() || !Owner->GetClaimFeedback(View) || Geometry.Scale<=KINDA_SMALL_NUMBER) return Layer;
        FVector Eye; FRotator Facing;
        Player->GetPlayerViewPoint(Eye,Facing);
        const auto Draw=[&](UTexture2D* Texture,FSlateBrush& Brush,const FVector& Point,float Pixels,const FLinearColor& Tint)
        {
            FVector2D Screen;
            if (!Texture || Pixels<1.f || Tint.A<=.01f || !Player->ProjectWorldLocationToScreen(Point,Screen,true)) return;
            if (Brush.GetResourceObject()!=Texture) { Brush.SetResourceObject(Texture); Brush.DrawAs=ESlateBrushDrawType::Image; }
            // 屏幕像素换成控件坐标，贴图大小随距离缩放
            const float Reach=FMath::Max(FVector::Dist(Eye,Point),1.);
            const float Size=Pixels*FMath::Clamp(900.f/Reach,.3f,1.7f);
            const FVector2f Corner=FVector2f(Screen)/Geometry.Scale-FVector2f(Size*.5f);
            FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(FVector2f(Size),FSlateLayoutTransform(Corner)),
                &Brush,ESlateDrawEffect::None,Tint);
        };
        const FVector Up=FVector::UpVector;
        const float HalfWidth=View.FrameWidth*.5f;
        if (View.Elapsed>=0 && View.Elapsed<View.StarSeconds)
        {
            const float Progress=View.Elapsed/View.StarSeconds;
            // 四角大星先亮，边上的小星错开一点跟着亮，整圈亮一下后向外散开
            const FVector Corners[4]={View.FrameBase-View.FrameRight*HalfWidth,View.FrameBase+View.FrameRight*HalfWidth,
                View.FrameBase-View.FrameRight*HalfWidth+Up*View.FrameHeight,View.FrameBase+View.FrameRight*HalfWidth+Up*View.FrameHeight};
            const FVector Middle=View.FrameBase+Up*(View.FrameHeight*.5f);
            const auto Glow=[&](float Delay)
            {
                const float Local=FMath::Clamp((Progress-Delay)/FMath::Max(1.f-Delay,.05f),0.f,1.f);
                return FMath::Sin(Local*PI);
            };
            for (int32 Index=0;Index<12;++Index)
            {
                // 沿左边、门楣、右边各放四颗
                const float Step=(Index%4+.5f)/4.f;
                const FVector OnFrame=Index<4?Corners[0]+Up*(View.FrameHeight*Step):
                    Index<8?Corners[2]+View.FrameRight*(View.FrameWidth*Step):Corners[1]+Up*(View.FrameHeight*Step);
                const FVector Outward=(OnFrame-Middle).GetSafeNormal();
                const float Shine=Glow(.08f+.04f*(Index%5));
                FLinearColor Tint=ClaimGold; Tint.A=Shine;
                Draw(View.Spark,SparkBrush,OnFrame+Outward*(22.f*Progress),34.f+26.f*Shine,Tint);
            }
            for (int32 Index=0;Index<4;++Index)
            {
                const FVector Outward=(Corners[Index]-Middle).GetSafeNormal();
                const float Shine=Glow(0.f);
                FLinearColor Tint=ClaimGold; Tint.A=Shine;
                Draw(View.Star,StarBrush,Corners[Index]+Outward*(30.f*Progress),58.f+52.f*Shine,Tint);
            }
        }
        if (View.bMarker)
        {
            const FVector Point=View.FrameBase+Up*(View.FrameHeight+50.f);
            const float Reach=FVector::Dist(Eye,Point);
            // 星光快结束时标记才出现，之后一直留在门口
            float Alpha=View.Elapsed>=0?FMath::Clamp(float(View.Elapsed/View.StarSeconds-.6f)/.4f,0.f,1.f):1.f;
            Alpha*=FMath::Clamp((View.MarkerDistance-Reach)/(View.MarkerDistance*.2f),0.f,1.f);
            if (Alpha>.01f)
            {
                // 隔着墙不显示；标记贴着门楣，留一点余量免得被门框自己挡掉
                FHitResult Hit;
                FCollisionQueryParams Query(SCENE_QUERY_STAT(HomeClaimMarker),false,Player->GetPawn());
                Query.AddIgnoredActor(Owner);
                if (World->LineTraceSingleByChannel(Hit,Eye,Point,ECC_Visibility,Query) && Hit.Distance<Reach-120.f) Alpha=0;
            }
            if (Alpha>.01f)
            {
                Draw(View.Marker,ShadowBrush,Point-Up*3.f,54.f,FLinearColor(0,0,0,.55f*Alpha));
                Draw(View.Marker,MarkerBrush,Point,50.f,FLinearColor(1.f,.93f,.62f,Alpha));
            }
        }
        return Layer;
    }
private:
    TWeakObjectPtr<ADeliveryHome> Home;
    mutable FSlateBrush StarBrush,SparkBrush,MarkerBrush,ShadowBrush;
};

// 解锁提示条：催爷爷搬走的玩家看到「新家到手！」，其他人看到「街坊小屋已解锁」
class SDeliveryHomeClaimOverlay : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SDeliveryHomeClaimOverlay) {}
        SLATE_ARGUMENT(TWeakObjectPtr<ADeliveryHome>,Home)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Home=Args._Home;
        SetVisibility(EVisibility::HitTestInvisible);
        ChildSlot
        [
            SNew(SOverlay)
            +SOverlay::Slot()[SNew(SDeliveryHomeClaimMarks).Home(Home)]
            +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(0,150,0,0))
            [
                SNew(SBorder)
                .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                .BorderBackgroundColor(FLinearColor(.05f,.04f,.02f,.82f))
                .Padding(FMargin(34,14))
                .Visibility(this,&SDeliveryHomeClaimOverlay::GetBannerVisibility)
                .ColorAndOpacity(this,&SDeliveryHomeClaimOverlay::GetBannerTint)
                [
                    SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
                    [
                        SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold",30))
                        .ColorAndOpacity(FSlateColor(ClaimGold))
                        .Text(this,&SDeliveryHomeClaimOverlay::GetTitle)
                    ]
                    +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0,6,0,0))
                    [
                        SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",16))
                        .ColorAndOpacity(FSlateColor(FLinearColor::White))
                        .Text(NSLOCTEXT("DeliveryHome","ClaimedDetail","你可以回这里休息了"))
                        .Visibility(this,&SDeliveryHomeClaimOverlay::GetDetailVisibility)
                    ]
                ]
            ]
        ];
    }
private:
    bool Read(FDeliveryHomeClaimFeedback& View) const
    {
        const auto* Owner=Home.Get();
        return Owner && Owner->GetClaimFeedback(View) && View.Elapsed>=0 && View.Elapsed<View.BannerSeconds;
    }
    EVisibility GetBannerVisibility() const
    {
        FDeliveryHomeClaimFeedback View;
        return Read(View)?EVisibility::HitTestInvisible:EVisibility::Collapsed;
    }
    EVisibility GetDetailVisibility() const
    {
        FDeliveryHomeClaimFeedback View;
        return Read(View) && View.bMine?EVisibility::HitTestInvisible:EVisibility::Collapsed;
    }
    FLinearColor GetBannerTint() const
    {
        FDeliveryHomeClaimFeedback View;
        if (!Read(View)) return FLinearColor::Transparent;
        // 两头各用四分之一秒淡入淡出
        const float Fade=FMath::Min(float(View.Elapsed),View.BannerSeconds-float(View.Elapsed))/.25f;
        return FLinearColor(1,1,1,FMath::Clamp(Fade,0.f,1.f));
    }
    FText GetTitle() const
    {
        FDeliveryHomeClaimFeedback View;
        return Read(View) && !View.bMine?NSLOCTEXT("DeliveryHome","ClaimedShared","街坊小屋已解锁"):
            NSLOCTEXT("DeliveryHome","Claimed","新家到手！");
    }
    TWeakObjectPtr<ADeliveryHome> Home;
};
}

void ADeliveryHome::OnRep_ClaimEventId() { ShowClaimHint(); }

bool ADeliveryHome::IsClaimedByLocalPlayer() const
{
    // 没有记录催促者时（例如关闭了催促开关）所有人都按完成者处理
    if (!IsValid(ClaimedBy)) return true;
    const auto* World=GetWorld();
    const auto* Player=World?World->GetFirstPlayerController():nullptr;
    return Player && Player->IsLocalController() && Player->PlayerState==ClaimedBy;
}

void ADeliveryHome::ShowClaimHint()
{
    const auto* World=GetWorld();
    if (!World || GetNetMode()==NM_DedicatedServer || !bClaimed || !ClaimEventId.IsValid() || ShownClaimEvent==ClaimEventId) return;
    ShownClaimEvent=ClaimEventId;
    const auto* State=World->GetGameState();
    const double ServerNow=State?State->GetServerWorldTimeSeconds():World->GetTimeSeconds();
    // 解锁已经过去一阵才连进来的玩家只看门口标记，不补播星光、提示和确认音
    if (ClaimedServerTime>=0 && ServerNow-ClaimedServerTime>5.) return;
    EnsureClaimOverlay();
    bClaimFeedbackMine=IsClaimedByLocalPlayer();
    ClaimFeedbackStartedAt=World->GetRealTimeSeconds();
    if (bClaimFeedbackMine)
    {
        auto* Sound=ClaimSound.LoadSynchronous();
        if (!Sound) Sound=ClaimFallbackSound.LoadSynchronous();
        if (Sound) UGameplayStatics::PlaySound2D(this,Sound);
        else UE_LOG(LogDelivery,Warning,TEXT("SharedHomeClaimFeedback 没有可用的确认音 home=%s"),*GetName());
    }
    UE_LOG(LogDelivery,Log,TEXT("SharedHomeClaimFeedback home=%s delivery=%s mine=%d overlay=%d star=%d marker=%d"),
        *GetName(),*ClaimEventId.ToString(),bClaimFeedbackMine,ClaimOverlay.IsValid(),IsValid(LoadedClaimStar),IsValid(LoadedClaimMarker));
}

bool ADeliveryHome::GetClaimFeedback(FDeliveryHomeClaimFeedback& Out) const
{
    const auto* World=GetWorld();
    if (!World || !IsValid(DoorwayLink)) return false;
    Out.bMarker=bClaimed;
    Out.bMine=bClaimFeedbackMine;
    Out.Elapsed=ClaimFeedbackStartedAt>=0?World->GetRealTimeSeconds()-ClaimFeedbackStartedAt:-1.;
    Out.StarSeconds=FMath::Clamp(ClaimStarSeconds,.3f,4.f);
    Out.BannerSeconds=FMath::Clamp(ClaimHintSeconds,.5f,8.f);
    Out.MarkerDistance=FMath::Max(ClaimMarkerDistance,500.f);
    if (!Out.bMarker && (Out.Elapsed<0 || Out.Elapsed>=FMath::Max(Out.StarSeconds,Out.BannerSeconds))) return false;
    // 门洞导航边的屋外一端就在门前地面，朝屋内的方向定出门框朝向
    FVector Outside,Inside; ENavLinkDirection::Type Direction;
    DoorwayLink->GetLinkData(Outside,Inside,Direction);
    // 导航边端点相对房屋标记保存
    const FTransform Space=GetActorTransform();
    const FVector Ground=Space.TransformPosition(Outside);
    FVector Inward=(Space.TransformPosition(Inside)-Ground).GetSafeNormal2D();
    if (Inward.IsNearlyZero()) Inward=GetActorForwardVector().GetSafeNormal2D();
    if (Inward.IsNearlyZero()) return false;
    Out.FrameRight=FVector::CrossProduct(FVector::UpVector,Inward).GetSafeNormal();
    Out.FrameBase=Ground+Out.FrameRight*ClaimFrameOffset.X-Inward*ClaimFrameOffset.Y+FVector(0,0,ClaimFrameOffset.Z);
    Out.FrameWidth=ClaimFrameWidth;
    Out.FrameHeight=ClaimFrameHeight;
    Out.Star=LoadedClaimStar;
    Out.Spark=LoadedClaimSpark?LoadedClaimSpark.Get():LoadedClaimStar.Get();
    Out.Marker=LoadedClaimMarker;
    return true;
}

void ADeliveryHome::EnsureClaimOverlay()
{
    auto* World=GetWorld();
    if (!World || GetNetMode()==NM_DedicatedServer) { GetWorldTimerManager().ClearTimer(ClaimOverlayTimer); return; }
    if (ClaimOverlay.IsValid() && ClaimOverlayViewport.IsValid()) { GetWorldTimerManager().ClearTimer(ClaimOverlayTimer); return; }
    auto* Viewport=World->GetGameViewport();
    if (!Viewport) return;
    // 贴图由房屋持有，控件只借用
    if (!LoadedClaimStar) LoadedClaimStar=ClaimStarTexture.LoadSynchronous();
    if (!LoadedClaimSpark) LoadedClaimSpark=ClaimSparkTexture.LoadSynchronous();
    if (!LoadedClaimMarker) LoadedClaimMarker=ClaimMarkerTexture.LoadSynchronous();
    if (!LoadedClaimStar || !LoadedClaimMarker)
        UE_LOG(LogDelivery,Warning,TEXT("SharedHomeClaimFeedback 缺少贴图 star=%d marker=%d，先运行 Content/Python/import_home_claim_assets.py"),
            IsValid(LoadedClaimStar),IsValid(LoadedClaimMarker));
    ClaimOverlay=SNew(SDeliveryHomeClaimOverlay).Home(this);
    ClaimOverlayViewport=Viewport;
    // 层级低于提示和对话界面
    Viewport->AddViewportWidgetContent(ClaimOverlay.ToSharedRef(),3);
    GetWorldTimerManager().ClearTimer(ClaimOverlayTimer);
}

void ADeliveryHome::RemoveClaimOverlay()
{
    if (auto* Viewport=ClaimOverlayViewport.Get(); Viewport && ClaimOverlay.IsValid())
        Viewport->RemoveViewportWidgetContent(ClaimOverlay.ToSharedRef());
    ClaimOverlay.Reset();
    ClaimOverlayViewport.Reset();
}
