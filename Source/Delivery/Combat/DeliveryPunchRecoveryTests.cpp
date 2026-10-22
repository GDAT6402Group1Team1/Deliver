#include "DeliveryBoxingPose.h"
#include "DeliveryRagdollCombatComponent.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsControlComponent.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Ragdoll/DeliveryRagdollPhysicsHelpers.h"
#include "Ragdoll/DeliveryActiveRagdollComponent.h"
#include "DeliveryCharacter.h"
#include "Components/BoxComponent.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryPunchRecoveryTest, "Delivery.Boxing.PlayerRecoveryTargets",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryPunchRecoveryTest::RunTest(const FString&)
{
 auto* Asset = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/A/renwu.renwu"));
 if (!TestNotNull(TEXT("actual player mesh"), Asset)) return false;
 const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
  .ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
 UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
 GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 AActor* Actor = World->SpawnActor<AActor>();
 auto* Mesh = NewObject<USkeletalMeshComponent>(Actor);
 Actor->SetRootComponent(Mesh);
 Mesh->SetSkeletalMeshAsset(Asset);
 auto* PlayerPhysics = LoadObject<UPhysicsAsset>(nullptr, TEXT("/Game/Characters/A/renwu_Physics1.renwu_Physics1"));
 if (PlayerPhysics) Mesh->SetPhysicsAsset(PlayerPhysics);
 Mesh->SetWorldRotation(FRotator(0,-90,0));
 Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
 Mesh->SetEnableGravity(false);
 Mesh->RegisterComponent();
 Mesh->RefreshBoneTransforms();
 UPhysicsAsset* SourcePhysics = Mesh->GetPhysicsAsset();
 const int32 SourceBodyCount = SourcePhysics->SkeletalBodySetups.Num();
 FDeliveryBoxingPose::CompletePhysicsAsset(Mesh);
 // Assert on live game-world joints, NOT just modified asset templates. PIE
 // applies DefaultProfile again while constructing these instances.
 const FDeliveryArmPoseSettings PoseSettings;
 for (int32 Rebuild=0; Rebuild<2; ++Rebuild)
 {
  Mesh->RecreatePhysicsState();
  FBodyInstance* Pelvis = Mesh->GetBodyInstance(TEXT("Hips"));
  if (TestNotNull(TEXT("pelvis live body"),Pelvis))
  {
   const float BeforeMass = Pelvis->GetBodyMass();
   const FVector BeforeInertia = Pelvis->GetBodyInertiaTensor();
   TestTrue(TEXT("actual player pathological pelvis repaired"),
    DeliveryRagdollPhysicsHelpers::StabilizePelvisMass(Mesh,TEXT("Hips")));
   TestTrue(TEXT("pelvis mass no longer near zero"),Pelvis->GetBodyMass()>1.f && Pelvis->GetBodyMass()>BeforeMass);
   TestTrue(TEXT("pelvis rotational inertia also restored"),Pelvis->GetBodyInertiaTensor().GetMin()>BeforeInertia.GetMax());
   const float RepairedMass=Pelvis->GetBodyMass();
   const FVector RepairedInertia=Pelvis->GetBodyInertiaTensor();
   double NeighborMinimum=TNumericLimits<double>::Max();
   for (const FName Neighbor : {FName(TEXT("Spine")),FName(TEXT("LeftUpLeg")),FName(TEXT("RightUpLeg"))})
    if (const FBodyInstance* Other=Mesh->GetBodyInstance(Neighbor))
     NeighborMinimum=FMath::Min(NeighborMinimum,Other->GetBodyInertiaTensor().GetMin());
   TestTrue(TEXT("inertia floor is bounded, not multiplied by shape-scale powers"),
    RepairedInertia.GetMin()>=NeighborMinimum*.249 && RepairedInertia.GetMin()<=NeighborMinimum*.26);
   TestFalse(TEXT("repair is idempotent"),DeliveryRagdollPhysicsHelpers::StabilizePelvisMass(Mesh,TEXT("Hips")));
   TestEqual(TEXT("mass does not accumulate"),Pelvis->GetBodyMass(),RepairedMass);
   TestTrue(TEXT("inertia does not accumulate"),Pelvis->GetBodyInertiaTensor().Equals(RepairedInertia));
   TestFalse(TEXT("ordinary limb mass is untouched"),DeliveryRagdollPhysicsHelpers::StabilizePelvisMass(Mesh,TEXT("LeftLeg")));
  }
  for (const FString Side : {FString(TEXT("Left")),FString(TEXT("Right"))})
  {
   for (bool bElbow : {false,true})
   {
    const FName Bone(*(Side + (bElbow ? TEXT("ForeArm") : TEXT("Arm"))));
    const FConstraintInstance* Joint = Mesh->FindConstraintInstance(Bone);
    if (!TestNotNull(*FString::Printf(TEXT("live joint %s"),*Bone.ToString()),Joint)) continue;
    const float Swing=bElbow ? PoseSettings.ElbowSwingLimit : PoseSettings.ShoulderSwingLimit;
    const float Twist=bElbow ? PoseSettings.ElbowTwistLimit : PoseSettings.ShoulderTwistLimit;
    TestEqual(TEXT("live swing1 matches configured range"),Joint->GetAngularSwing1Limit(),Swing);
    TestEqual(TEXT("live swing2 matches configured range"),Joint->GetAngularSwing2Limit(),Swing);
    TestEqual(TEXT("live twist matches configured range"),Joint->GetAngularTwistLimit(),Twist);
    TestTrue(TEXT("live angular axes remain limited"),Joint->GetAngularSwing1Motion()==ACM_Limited
     && Joint->GetAngularSwing2Motion()==ACM_Limited && Joint->GetAngularTwistMotion()==ACM_Limited);
   }
  }
  const FConstraintInstance* Wrist=Mesh->FindConstraintInstance(TEXT("RightHand"));
  if (TestNotNull(TEXT("runtime-added right wrist"),Wrist))
  {
   TestEqual(TEXT("new wrist preserves 15 degree swing"),Wrist->GetAngularSwing1Limit(),15.f);
   TestEqual(TEXT("new wrist preserves 15 degree twist"),Wrist->GetAngularTwistLimit(),15.f);
   TestTrue(TEXT("new wrist is limited, not Free"),Wrist->GetAngularSwing1Motion()==ACM_Limited
    && Wrist->GetAngularSwing2Motion()==ACM_Limited && Wrist->GetAngularTwistMotion()==ACM_Limited);
   TestTrue(TEXT("new wrist preserves disabled adjacent collision"),Wrist->IsCollisionDisabled());
   TestTrue(TEXT("new wrist preserves locked translation"),Wrist->GetLinearXMotion()==LCM_Locked
    && Wrist->GetLinearYMotion()==LCM_Locked && Wrist->GetLinearZMotion()==LCM_Locked);
  }
 }
 TestEqual(TEXT("source asset body count unchanged"),SourcePhysics->SkeletalBodySetups.Num(),SourceBodyCount);
 auto* Controls = NewObject<UPhysicsControlComponent>(Actor);
 Controls->RegisterComponent();
 // A deformed animation/physics buffer must never become the standing target.
 const TArray<FName> PostureBones = {TEXT("Spine1"),TEXT("Spine2"),TEXT("LeftUpLeg"),TEXT("RightUpLeg"),TEXT("LeftLeg"),TEXT("RightLeg")};
 FPhysicsControlData PostureData;
 PostureData.AngularStrength = 6.f;
 const auto VerifyPostureTargets = [&]()
 {
  for (int32 I=0; I<PostureBones.Num(); ++I)
  {
   const TArray<FName> Names = DeliveryRagdollPhysicsHelpers::CreateReferencePoseControls(
    Mesh, Controls, {PostureBones[I]}, PostureData, TEXT("PostureTest"));
   TestEqual(*FString::Printf(TEXT("posture control for %s"),*PostureBones[I].ToString()), Names.Num(), 1);
   if (Names.IsEmpty()) continue;
   FPhysicsControlData Data;
   Controls->GetControlData(Names[0], Data);
   TestFalse(TEXT("posture cannot follow physics-contaminated animation cache"), Data.bUseSkeletalAnimation);
   FPhysicsControlTarget Target;
   Controls->GetControlTarget(Names[0], Target);
   const int32 BoneIndex = Asset->GetRefSkeleton().FindBoneIndex(PostureBones[I]);
   const FQuat Expected = Asset->GetRefSkeleton().GetRefBonePose()[BoneIndex].GetRotation();
   TestTrue(TEXT("posture target equals immutable parent-relative reference"),
    Target.TargetOrientation.Quaternion().AngularDistance(Expected)<.001f);
  }
  Controls->DestroyControlsInSet(TEXT("All"));
 };
 VerifyPostureTargets();
 const TArray<FTransform> SavedPose = Mesh->GetEditableComponentSpaceTransforms();
 int32 PoseIndex = 0;
 for (FTransform& Bone : Mesh->GetEditableComponentSpaceTransforms())
  Bone.SetRotation(FQuat(FVector::RightVector, (++PoseIndex%2) ? 1.4f : -1.1f)*Bone.GetRotation());
 VerifyPostureTargets();
 Mesh->GetEditableComponentSpaceTransforms() = SavedPose;
 for (int32 Side : {0,1})
 for (float AimYaw : {-50.f,0.f,50.f})
 {
  FDeliveryBoxingPose Pose;
  Pose.Create(Mesh, Controls);
  Pose.SettleTime = 1;
  const FVector Aim = FRotator(0,AimYaw,0).Vector();
  for (int32 Frame=0; Frame<46; ++Frame)
  {
   Pose.Update(Mesh,Controls,0,1.f/60,Side,Frame>=19,Aim);
   // Twist during windup/strike must be anchored to the reference, not to the
   // path taken in previous frames. This catches independent axial-roll drift.
   const FName ActiveNames[] = {Pose.Arms[Side].UpperControl,Pose.Arms[Side].LowerControl};
   FQuat Expected[2];
   for (int32 Link=0; Link<2; ++Link)
   {
    FPhysicsControlTarget T; Controls->GetControlTarget(ActiveNames[Link],T);
    Expected[Link]=T.TargetOrientation.Quaternion();
   }
   FDeliveryBoxingPose DifferentHistory = Pose;
   DifferentHistory.Arms[Side].UpperTransport = FQuat(FVector::ForwardVector,1.2f);
   DifferentHistory.Arms[Side].LowerTransport = FQuat(FVector::RightVector,-1.2f);
   DifferentHistory.Update(Mesh,Controls,0,0,Side,Frame>=19,Aim);
   for (int32 Link=0; Link<2; ++Link)
   {
    FPhysicsControlTarget T; Controls->GetControlTarget(ActiveNames[Link],T);
    TestTrue(TEXT("punch twist cannot accumulate from rotation history"),
     Expected[Link].AngularDistance(T.TargetOrientation.Quaternion())<.001f);
   }
  }
  TestTrue(TEXT("cannot restart punch before retraction"),Pose.HasPendingPunchRecovery());
  FQuat Previous[2];
  const FName Names[] = {Pose.Arms[Side].UpperControl,Pose.Arms[Side].LowerControl};
  for (int32 Link=0; Link<2; ++Link) { FPhysicsControlTarget T; Controls->GetControlTarget(Names[Link],T); Previous[Link]=T.TargetOrientation.Quaternion(); }
  float Worst = 0;
  for (int32 Frame=0; Frame<60; ++Frame)
  {
   Pose.Update(Mesh,Controls,0,1.f/60,-1,false);
   for (int32 Link=0; Link<2; ++Link)
   {
    FPhysicsControlTarget T; Controls->GetControlTarget(Names[Link],T);
    const FQuat Q=T.TargetOrientation.Quaternion();
    Worst=FMath::Max(Worst,float(FMath::RadiansToDegrees(Previous[Link].AngularDistance(Q))));
    Previous[Link]=Q;
   }
  }
  AddInfo(FString::Printf(TEXT("side=%d aim=%.0f recovery max step=%.2f degrees"),Side,AimYaw,Worst));
  TestTrue(TEXT("recovery has no large single-frame rotation jump"),Worst<25.f);
  TestEqual(TEXT("extension fully recovered"),Pose.Arms[Side].Extension,0.f);
  TestFalse(TEXT("next punch allowed after retraction"),Pose.HasPendingPunchRecovery());
  Controls->DestroyControlsInSet(TEXT("All"));
 }
 World->DestroyWorld(false);
 GEngine->DestroyWorldContext(World);
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryLegIsolationTest, "Delivery.Ragdoll.LegIsolation",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FDeliveryLegIsolationTest::RunTest(const FString&)
{
 UClass* Class = LoadClass<ADeliveryCharacter>(nullptr,TEXT("/Game/Blueprint/Character/DeliverMan/BP_DeliveryMan.BP_DeliveryMan_C"));
 if (!TestNotNull(TEXT("production character"),Class)) return false;
 for (int32 Variant=0; Variant<10; ++Variant)
 {
  const auto Init = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
   .ShouldSimulatePhysics(true).CreateNavigation(false).CreateAISystem(false);
  UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
  GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
  AActor* Floor=World->SpawnActor<AActor>();
  auto* Box=NewObject<UBoxComponent>(Floor);
  Floor->SetRootComponent(Box);
  Box->SetBoxExtent(FVector(20000,20000,50));
  Box->SetWorldLocation(FVector(0,0,-50));
  if (Variant>=6 && Variant<=9)
  {
   const float Angle=Variant>=8 ? 30.f : 20.f;
   const FRotator Slope(Variant%2==0 ? Angle : -Angle,0,0);
   Box->SetWorldRotation(Slope);
   Box->SetWorldLocation(Slope.RotateVector(FVector(0,0,-50)));
  }
  Box->SetCollisionProfileName(TEXT("BlockAll"));
  Box->RegisterComponent();
  auto* Character=World->SpawnActor<ADeliveryCharacter>(Class,FVector(0,0,150),FRotator::ZeroRotator);
  auto* Rag=Character->GetActiveRagdoll();
  auto* Mesh=Character->GetMesh();
  World->InitializeActorsForPlay(FURL());
  World->GetWorldSettings()->NotifyBeginPlay();
  APlayerController* DownwardCamera = nullptr;
  if (Variant>=6 && Variant<=9)
  {
   DownwardCamera=World->SpawnActor<APlayerController>();
   DownwardCamera->Possess(Character);
   DownwardCamera->SetControlRotation(FRotator(-75.f,0.f,0.f));
  }
  Rag->StartRagdoll();
  if (Variant==2)
  {
   Rag->StopRagdoll();
   auto* Physics=DuplicateObject<UPhysicsAsset>(Mesh->GetPhysicsAsset(),Mesh);
   for (int32 A=0; A<Physics->SkeletalBodySetups.Num(); ++A)
    for (int32 B=A+1; B<Physics->SkeletalBodySetups.Num(); ++B) Physics->DisableCollision(A,B);
   Mesh->SetPhysicsAsset(Physics);
   Rag->StartRagdoll();
  }
  auto* Controls=Character->FindComponentByClass<UPhysicsControlComponent>();
  if (Variant==0)
  {
   Rag->SetMoveInput(FVector2D(0,1));
   for (int32 I=0; I<30; ++I) Rag->UpdateControlTargets(.03f);
   Rag->BlockedStepRecoveryRemaining=.2f;
   Rag->UpdateControlTargets(.03f);
   Rag->BlockedStepRecoveryRemaining=0.f;
   Rag->UpdateControlTargets(.03f);
   TestEqual(TEXT("held input across blocked-step release cannot restart startup lean"),Rag->AccelerationLeanRemaining,0.f);
   Rag->SetMoveInput(FVector2D::ZeroVector);
   Rag->UpdateControlTargets(.03f);
   Rag->SetMoveInput(FVector2D(0,1));
   Rag->UpdateControlTargets(.03f);
   TestTrue(TEXT("a genuine new key press still starts lean"),Rag->AccelerationLeanRemaining>0.f);
   Rag->SetMoveInput(FVector2D::ZeroVector);
  }
  Rag->RightFoot.Alpha = .99f;
  Rag->RightFoot.Elapsed = 1.f;
  Rag->RightFoot.Target = Mesh->GetCenterOfMass(Rag->RightFoot.Bone) + FVector(1000,0,0);
  Rag->UpdateFeet(.03f,FVector::ForwardVector);
  TestTrue(TEXT("failed step starts a release interval"),Rag->BlockedStepRecoveryRemaining>0.f);
  TestFalse(TEXT("failed foot is not re-enabled in same UpdateFeet"),Controls->GetControlEnabled(Rag->RightFoot.Control));
  Rag->UpdateFeet(.03f,FVector::ForwardVector);
  TestFalse(TEXT("next gait update also preserves release"),Controls->GetControlEnabled(Rag->RightFoot.Control));
  Rag->ReseedFromCurrentPose();
  if (Variant==1)
   for (FName Name : {FName(TEXT("LeftLeg")),FName(TEXT("RightLeg"))})
   {
    FPhysicsControlData Data;
    TestTrue(TEXT("isolation finds real foot motor"),Controls->GetControlData(Name,Data));
    Data.LinearStrength=0.f;
    Data.LinearExtraDamping=0.f;
    Controls->SetControlData(Name,Data);
   }
  const FQuat InitialHip=Mesh->GetBodyInstance(TEXT("Hips"))->GetUnrealWorldTransform().GetRotation();
  const FVector LocalUp=InitialHip.UnrotateVector(FVector::UpVector);
  const FVector InitialPosition=Mesh->GetBodyInstance(TEXT("Hips"))->GetUnrealWorldTransform().GetLocation();
  auto* Combat=Character->FindComponentByClass<UDeliveryRagdollCombatComponent>();
  ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, World, Rag, Mesh, Controls, Combat, DownwardCamera, LocalUp, InitialPosition, Variant, Frame=0, MaxTravel=0.0, WalkFrame=-1, LeanRestarts=0, PreviousLean=0.f, LowTime=0.f, MaxLowTime=0.f, StopPosition=FVector::ZeroVector]() mutable
  {
   const float Sign=((Frame/90)%2) ? -1.f : 1.f;
   if (Variant>=3)
   {
    if (Frame==90 && Variant>=4 && Variant<=5)
     TestTrue(TEXT("real punch starts"),Combat && Combat->StartPunch(Variant==4 ? EMeleeHand::Left : EMeleeHand::Right,FVector::ForwardVector));
    if (WalkFrame<0 && Frame>=91 && (Variant==3 || Variant>=6 ? Frame>=116 : Combat && !Combat->IsPunching())) WalkFrame=Frame;
    const bool bSlope=Variant>=6 && Variant<=9;
    Rag->SetMoveInput(WalkFrame>=0 && !(bSlope && Frame>=270)
     ? FVector2D(0,1) : FVector2D::ZeroVector);
    if (bSlope && Frame==120)
    {
     const FVector DownWish=Rag->GetWishDir();
     DownwardCamera->SetControlRotation(FRotator(0.f,0.f,0.f));
     const FVector LevelWish=Rag->GetWishDir();
     DownwardCamera->SetControlRotation(FRotator(-75.f,0.f,0.f));
     TestTrue(TEXT("looking down does not tilt movement input into ground"),
      DownWish.Equals(LevelWish,.001f) && FMath::IsNearlyZero(DownWish.Z,.001f));
    }
   }
   else Rag->SetMoveInput(Frame<90 || Frame>=270 ? FVector2D::ZeroVector : FVector2D(0,Sign));
   World->Tick(LEVELTICK_All,.03f);
   if (Variant>=6 && Variant<=9 && Frame==300) StopPosition=Mesh->GetCenterOfMass(TEXT("Hips"));
   if (Variant>=4 && WalkFrame>=0 && Rag->BoxingPose.HasPendingPunchRecovery())
    TestTrue(TEXT("torso remains braced while punch arms retract"),Rag->BraceAlpha>.9f);
   if (Variant>=3 && WalkFrame>=0)
   {
    if (Rag->AccelerationLeanRemaining>PreviousLean+.001f) ++LeanRestarts;
    PreviousLean=Rag->AccelerationLeanRemaining;
    LowTime=Rag->GetUprightDot()<.75f ? LowTime+.03f : 0.f;
    MaxLowTime=FMath::Max(MaxLowTime,LowTime);
   }
   MaxTravel=FMath::Max(MaxTravel,FVector::Dist(InitialPosition,
    Mesh->GetBodyInstance(TEXT("Hips"))->GetUnrealWorldTransform().GetLocation()));
   if (Frame%60==59)
   {
    const auto* Hip=Mesh->GetBodyInstance(TEXT("Hips"));
    const auto* Leg=Mesh->GetBodyInstance(TEXT("RightUpLeg"));
    FPhysicsControlTarget Target;
    Controls->GetControlTarget(TEXT("Hips_RightUpLeg"),Target);
    const FQuat Relative=Hip->GetUnrealWorldTransform().GetRotation().Inverse()*Leg->GetUnrealWorldTransform().GetRotation();
    AddInfo(FString::Printf(TEXT("LegIsolation variant=%d frame=%d upright=%.3f thighError=%.1f hip=%s"),Variant,Frame,
     FVector::DotProduct(Hip->GetUnrealWorldTransform().GetRotation().RotateVector(LocalUp),FVector::UpVector),
     FMath::RadiansToDegrees(Relative.AngularDistance(Target.TargetOrientation.Quaternion())),
     *Hip->GetUnrealWorldTransform().GetLocation().ToCompactString()));
   }
   if (++Frame<360) return false;
   if (Variant>=6 && Variant<=9)
   {
    const double Drift=FVector::Dist(StopPosition,Mesh->GetCenterOfMass(TEXT("Hips")));
    AddInfo(FString::Printf(TEXT("SlopeWalk variant=%d stoppedDrift=%.2f travel=%.2f"),Variant,Drift,MaxTravel));
    TestTrue(TEXT("slope gait travels, not stuck by traction"),MaxTravel>50.0);
    TestTrue(TEXT("slope stop does not keep sliding"),Drift<20.0);
   }
   if (Variant>=3)
   {
    AddInfo(FString::Printf(TEXT("PunchWalk variant=%d walkFrame=%d leanStarts=%d longestLow=%.2f finalUpright=%.3f"),Variant,WalkFrame,LeanRestarts,MaxLowTime,Rag->GetUprightDot()));
    TestTrue(TEXT("walk begins after punch"),WalkFrame>=0);
    TestTrue(TEXT("held input only starts lean once"),LeanRestarts<=1);
    TestTrue(TEXT("no sustained forward collapse"),MaxLowTime<1.f);
    TestTrue(TEXT("walking recovers upright"),Rag->GetUprightDot()>.85f);
   }
   TestTrue(TEXT("isolation actually advances physics, not a static pose"),MaxTravel>1.0);
   if (Variant==0) TestTrue(TEXT("production gait actually travels"),MaxTravel>50.0);
   World->DestroyWorld(false);
   GEngine->DestroyWorldContext(World);
   return true;
  }));
 }
 return true;
}
#endif
