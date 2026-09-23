// 有些角色的物理资产缺少前臂/手的刚体或关节，电机就无法推动拳头。
// 本文件在运行时检查这些缺件，必要时复制物理资产并补齐；不会修改项目里的源资产。
#include "DeliveryBoxingPose.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SphylElem.h"
#include "Delivery.h"

namespace
{
	/** 关节限位是否已经放到出拳需要的范围。够宽就不用再复制一份物理资产。 */
	bool JointIsOpenedUp(const FConstraintInstance& Joint, float SwingLimit, float TwistLimit)
	{
		const bool bSwingOpen = Joint.GetAngularSwing1Motion() == ACM_Free
			|| (Joint.GetAngularSwing1Motion() == ACM_Limited && Joint.GetAngularSwing1Limit() >= SwingLimit - 0.5f);
		const bool bTwistOpen = Joint.GetAngularTwistMotion() == ACM_Free
			|| (Joint.GetAngularTwistMotion() == ACM_Limited && Joint.GetAngularTwistLimit() >= TwistLimit - 0.5f);
		return bSwingOpen && bTwistOpen;
	}

	void OpenUpJoint(FConstraintInstance& Joint, float SwingLimit, float TwistLimit)
	{
		Joint.SetAngularSwing1Limit(ACM_Limited, SwingLimit);
		Joint.SetAngularSwing2Limit(ACM_Limited, SwingLimit);
		Joint.SetAngularTwistLimit(ACM_Limited, TwistLimit);
	}

	USkeletalBodySetup* AddBoneCapsule(UPhysicsAsset* Asset, const FName Bone,
		const FTransform& BoneTM, const FVector& WorldEnd, const float RadiusScale, const float MinRadius, const float MaxRadius)
	{
		// 把“骨骼起点到下一关节”的线段变成胶囊刚体。先换到骨骼局部坐标，
		// 这样身体转向时碰撞形状仍跟着对应骨骼运动。
		FVector LocalEnd = BoneTM.InverseTransformPosition(WorldEnd);
		if (LocalEnd.SizeSquared() < 1.0f)
		{
			LocalEnd = FVector(0.0f, 0.0f, 8.0f);
		}

		USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(Asset, NAME_None, RF_Transient);
		Body->BoneName = Bone;
		Body->PhysicsType = PhysType_Default;
		FKSphylElem Capsule;
		Capsule.Center = LocalEnd * 0.5f;
		Capsule.Radius = FMath::Clamp(LocalEnd.Size() * RadiusScale, MinRadius, MaxRadius);
		Capsule.Length = FMath::Max(1.0f, LocalEnd.Size() - 2.0f * Capsule.Radius);
		Capsule.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, LocalEnd.GetSafeNormal()).Rotator();
		Body->AggGeom.SphylElems.Add(Capsule);
		Body->CreatePhysicsMeshes();
		Asset->SkeletalBodySetups.Add(Body);
		Asset->UpdateBodySetupIndexMap();
		return Body;
	}

	void AddBallSocket(UPhysicsAsset* Asset, const FName Child, const FName Parent,
		const FTransform& ChildTM, const FTransform& ParentTM, const float SwingLimit, const float TwistLimit)
	{
		// 将新刚体接到父骨骼；位置锁住但旋转允许在拳击所需角度内摆动。
		UPhysicsConstraintTemplate* Constraint = NewObject<UPhysicsConstraintTemplate>(Asset, NAME_None, RF_Transient);
		FConstraintInstance& Joint = Constraint->DefaultInstance;
		Joint.JointName = Child;
		Joint.ConstraintBone1 = Child;
		Joint.ConstraintBone2 = Parent;
		const FTransform WorldFrame(ChildTM.GetRotation(), ChildTM.GetLocation());
		Joint.SetRefFrame(EConstraintFrame::Frame1, WorldFrame.GetRelativeTransform(ChildTM));
		Joint.SetRefFrame(EConstraintFrame::Frame2, WorldFrame.GetRelativeTransform(ParentTM));
		Joint.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0);
		OpenUpJoint(Joint, SwingLimit, TwistLimit);
		Joint.SetDisableCollision(true);
		Asset->ConstraintSetup.Add(Constraint);
	}

	bool HasConstraintBetween(const UPhysicsAsset* Asset, const FName A, const FName B)
	{
		for (const UPhysicsConstraintTemplate* Template : Asset->ConstraintSetup)
		{
			if (!Template)
			{
				continue;
			}
			const FConstraintInstance& Joint = Template->DefaultInstance;
			const bool bAB = Joint.ConstraintBone1 == A && Joint.ConstraintBone2 == B;
			const bool bBA = Joint.ConstraintBone1 == B && Joint.ConstraintBone2 == A;
			if (bAB || bBA)
			{
				return true;
			}
		}
		return false;
	}

	FVector FindHandTip(const USkeletalMeshComponent* Mesh, const FString& Side, const FTransform& HandTM)
	{
		const FName Candidates[] = {
			FName(*(Side + TEXT("Hand_end"))),
			FName(*(Side + TEXT("Hand_end_end"))),
			FName(*(Side + TEXT("HandThumb1")))
		};
		for (const FName Tip : Candidates)
		{
			if (Mesh->GetBoneIndex(Tip) != INDEX_NONE)
			{
				return Mesh->GetSocketTransform(Tip).GetLocation();
			}
		}
		return HandTM.GetLocation() + HandTM.GetUnitAxis(EAxis::X) * 8.0f;
	}
}

void FDeliveryBoxingPose::CompletePhysicsAsset(USkeletalMeshComponent* Mesh, const FDeliveryArmPoseSettings& Settings)
{
	// 按左右手逐段检查。源资产已经够用时直接返回；只有确实缺件或限位
	// 太窄时才建立运行时副本，避免把补全结果写回编辑器资源。
	UPhysicsAsset* Source = Mesh->GetPhysicsAsset();
	if (!Source) return;
	UPhysicsAsset* Asset = nullptr;
	for (const FString Side : { FString(TEXT("Left")), FString(TEXT("Right")) })
	{
		const FName Upper(*(Side + TEXT("Arm"))), Lower(*(Side + TEXT("ForeArm"))), Hand(*(Side + TEXT("Hand")));
		if (Mesh->GetBoneIndex(Upper) == INDEX_NONE || Mesh->GetBoneIndex(Lower) == INDEX_NONE
			|| Mesh->GetBoneIndex(Hand) == INDEX_NONE)
		{
			continue;
		}

		UPhysicsAsset* Current = Asset ? Asset : Source;
		if (Current->FindBodyIndex(Upper) == INDEX_NONE)
		{
			continue;
		}

		const bool bNeedsHand = Current->FindBodyIndex(Hand) == INDEX_NONE;
		const bool bNeedsForearm = Current->FindBodyIndex(Lower) == INDEX_NONE;
		bool bNeedsJointRange = false;
		for (const UPhysicsConstraintTemplate* Template : Current->ConstraintSetup)
		{
			const FConstraintInstance& Joint = Template->DefaultInstance;
			if (Joint.ConstraintBone1 == Upper)
			{
				bNeedsJointRange |= !JointIsOpenedUp(Joint, Settings.ShoulderSwingLimit, Settings.ShoulderTwistLimit);
			}
			else if (Joint.ConstraintBone1 == Lower)
			{
				bNeedsJointRange |= !JointIsOpenedUp(Joint, Settings.ElbowSwingLimit, Settings.ElbowTwistLimit);
			}
		}
		if (!bNeedsHand && !bNeedsForearm && !bNeedsJointRange)
		{
			continue;
		}
		if (!Asset)
		{
			Asset = DuplicateObject<UPhysicsAsset>(Source, Mesh);
			Asset->SetFlags(RF_Transient);
		}

		// 肩和肘按出拳需要的活动范围放开。默认锥角只有几十度，上臂抬不到肩前，
		// 收拳会停在肚子前面，出拳也只剩小臂在折。
		bool bShoulderFound = false;
		for (UPhysicsConstraintTemplate* Template : Asset->ConstraintSetup)
		{
			FConstraintInstance& Joint = Template->DefaultInstance;
			if (Joint.ConstraintBone1 == Upper)
			{
				OpenUpJoint(Joint, Settings.ShoulderSwingLimit, Settings.ShoulderTwistLimit);
				bShoulderFound = true;
			}
			else if (Joint.ConstraintBone1 == Lower)
			{
				OpenUpJoint(Joint, Settings.ElbowSwingLimit, Settings.ElbowTwistLimit);
			}
		}
		if (!bShoulderFound)
		{
			// 没找到肩关节就没法放开限位，出拳会被物理资产原本的锥角卡住。
			UE_LOG(LogDelivery, Warning, TEXT("Boxing: no %s shoulder joint owned by %s"), *Side, *Upper.ToString());
		}

		const FTransform UpperTM = Mesh->GetSocketTransform(Upper);
		const FTransform LowerTM = Mesh->GetSocketTransform(Lower);
		const FTransform HandTM = Mesh->GetSocketTransform(Hand);

		// renwu 这类资产常常只有上臂。没有手/小臂刚体时右拳 CanDrivePunch 会直接失败。
		if (bNeedsHand)
		{
			AddBoneCapsule(Asset, Hand, HandTM, FindHandTip(Mesh, Side, HandTM), 0.22f, 1.5f, 3.5f);
			UE_LOG(LogDelivery, Log, TEXT("Boxing: added runtime %s hand"), *Side);
		}
		if (bNeedsForearm)
		{
			AddBoneCapsule(Asset, Lower, LowerTM, HandTM.GetLocation(), 0.12f, 2.0f, 4.0f);

			// Replace the old hand-to-upper-arm joint with a wrist, then add an elbow.
			bool bWristFound = false;
			for (UPhysicsConstraintTemplate* Template : Asset->ConstraintSetup)
			{
				FConstraintInstance& Joint = Template->DefaultInstance;
				const bool bHandFirst = Joint.ConstraintBone1 == Hand && Joint.ConstraintBone2 == Upper;
				const bool bHandSecond = Joint.ConstraintBone2 == Hand && Joint.ConstraintBone1 == Upper;
				if (!bHandFirst && !bHandSecond) continue;
				Joint.ConstraintBone1 = Hand;
				Joint.ConstraintBone2 = Lower;
				const FTransform WorldFrame(HandTM.GetRotation(), HandTM.GetLocation());
				Joint.SetRefFrame(EConstraintFrame::Frame1, WorldFrame.GetRelativeTransform(HandTM));
				Joint.SetRefFrame(EConstraintFrame::Frame2, WorldFrame.GetRelativeTransform(LowerTM));
				Joint.SetAngularSwing1Limit(ACM_Limited, 15);
				Joint.SetAngularSwing2Limit(ACM_Limited, 15);
				Joint.SetAngularTwistLimit(ACM_Limited, 15);
				Joint.SetDisableCollision(true);
				bWristFound = true;
			}
			if (!bWristFound)
			{
				AddBallSocket(Asset, Hand, Lower, HandTM, LowerTM, 15.0f, 15.0f);
			}
			if (!HasConstraintBetween(Asset, Lower, Upper))
			{
				AddBallSocket(Asset, Lower, Upper, LowerTM, UpperTM,
					Settings.ElbowSwingLimit, Settings.ElbowTwistLimit);
			}
			Asset->DisableCollision(Asset->FindBodyIndex(Lower), Asset->FindBodyIndex(Upper));
			if (Asset->FindBodyIndex(Hand) != INDEX_NONE)
			{
				Asset->DisableCollision(Asset->FindBodyIndex(Lower), Asset->FindBodyIndex(Hand));
			}
			UE_LOG(LogDelivery, Log, TEXT("Boxing: added runtime %s elbow and forearm"), *Side);
		}
		else if (bNeedsHand && !HasConstraintBetween(Asset, Hand, Lower) && Asset->FindBodyIndex(Lower) != INDEX_NONE)
		{
			AddBallSocket(Asset, Hand, Lower, HandTM, LowerTM, 15.0f, 15.0f);
			Asset->DisableCollision(Asset->FindBodyIndex(Lower), Asset->FindBodyIndex(Hand));
		}
	}
	if (Asset)
	{
		Asset->UpdateBoundsBodiesArray();
		Mesh->SetPhysicsAsset(Asset, true);
	}
}
