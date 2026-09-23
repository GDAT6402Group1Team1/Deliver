#include "Vehicle/DeliveryRiderAnimInstance.h"
#include "Vehicle/DeliveryMotorbike.h"
#include "DeliveryCharacter.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

namespace
{
struct FDeliveryRiderAnimProxy : FAnimInstanceProxy
{
	explicit FDeliveryRiderAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}
	DeliveryRiderPose::FRig Rig;
	DeliveryRiderPose::FSpring Pitch, Roll, Yaw, Bounce;
	DeliveryRiderPose::FSpring HeadRoll, HeadPitch;
	DeliveryRiderPose::FSpring HandSlack;
	FDeliveryRiderFrame Frame;
	TWeakObjectPtr<USkeletalMesh> CachedAsset;
	TWeakObjectPtr<AActor> LastDriver;
	float FlutterTime = 0;

	virtual void PreUpdate(UAnimInstance* Instance, float Dt) override
	{
		FAnimInstanceProxy::PreUpdate(Instance, Dt);
		const USkeletalMeshComponent* Mesh = Instance->GetSkelMeshComponent();
		USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
		if (Asset && CachedAsset.Get() != Asset)
		{
			CachedAsset = Asset;
			if (!Rig.Initialize(Asset->GetRefSkeleton()))
				UE_LOG(LogTemp, Error, TEXT("Rider rig: required seated limb/spine bones are missing in %s"), *Asset->GetName());
		}
		const ADeliveryMotorbike* Bike = Cast<ADeliveryMotorbike>(Instance->GetOwningActor());
		AActor* Driver = Bike ? Bike->GetDriver() : nullptr;
		if (LastDriver.Get() != Driver || !Driver)
		{
			Pitch = {}; Roll = {}; Yaw = {}; Bounce = {}; FlutterTime = 0;
			HeadRoll = {}; HeadPitch = {};
			HandSlack = {};
			LastDriver = Driver;
		}
		Frame = {};
		if (Bike) Bike->BuildRiderAnimationFrame(Frame);
		if (!Frame.bActive) return;
		const auto& M = Frame.Motion;
		const auto& S = Frame.Settings;
		const float Speed = FMath::Clamp(FMath::Abs(M.Speed), 0.f, 1.f);
		FlutterTime = FMath::Fmod(FlutterTime + FMath::Clamp(Dt, 0.f, .1f), 1000.f);
		// 约四成半最高车速就达到完整表现，不必一直顶着极速才能看出风摆。
		// 迎面风主要把头往后压，不应在直行时凭空制造左右摇头。
		const float WindWeight = FMath::SmoothStep(0.f, .45f, Speed);
		const float Exaggeration = FMath::Clamp(S.WindExaggeration, 0.f, 2.5f);
		const float BodyWind = S.WindSway * Exaggeration;
		const float HeadWind = S.HeadWindSway * Exaggeration;
		const float Flutter = (FMath::Sin(FlutterTime * 4.8f) + .28f * FMath::Sin(FlutterTime * 8.1f)) * WindWeight;
		const float ForeAft = FMath::Sin(FlutterTime * 3.7f + .7f) * WindWeight;
		const float ForwardWind = M.Speed > 0.f ? WindWeight : 0.f;
		const float TurnWind = FMath::Abs(M.Steer) * WindWeight;
		Pitch.Step(-Speed * S.WindLean - M.Acceleration * S.InertiaLean
			- M.Impact.X * S.ImpactLean + M.Bump * 4.f - ForwardWind * BodyWind * .2f
			+ ForeAft * BodyWind * .55f, Dt, S.ResponseFrequency, S.Damping, 26.f);
		Roll.Step(-M.Steer * Speed * S.TurnLean * 1.6f - M.Impact.Y * S.ImpactLean + Flutter * BodyWind * TurnWind * .95f,
			Dt, S.ResponseFrequency, S.Damping, 28.f);
		Yaw.Step(M.Steer * 8.f, Dt, S.ResponseFrequency * 1.15f, S.Damping, 16.f);
		Bounce.Step(-M.Bump * S.BumpHeight + Flutter * .12f, Dt, S.ResponseFrequency, S.Damping, 3.f);
		// 绕 Right 轴负向旋转是后仰。后仰基准上叠明显的前后回摆，最低仍保持后仰。
		// 后仰目标约 14～42°，与45°上限留出回弹余量，不靠连续撞限幅制造夸张感。
		// 侧倾只响应转弯/侧向撞击；加减速与颠簸仍可叠加短暂的前后惯性。
		HeadRoll.Step(-M.Steer * Speed * HeadWind * .55f - M.Impact.Y * S.ImpactLean * .5f
			- FMath::Sin(FlutterTime * 4.8f - .65f) * TurnWind * HeadWind * .6f,
			Dt, S.ResponseFrequency * .7f, S.Damping, 40.f);
		HeadPitch.Step(-ForwardWind * HeadWind * (.92f + .46f * FMath::Sin(FlutterTime * 3.7f + .05f))
			- M.Acceleration * S.InertiaLean * .35f - M.Impact.X * S.ImpactLean * .5f + M.Bump * 4.f,
			Dt, S.ResponseFrequency * .8f, S.Damping, 45.f);
		// 只放松手部5cm，腿仍贴脚踏。容差也经平滑淡入淡出，避免速度变化时肩部突然挪动。
		HandSlack.Step(WindWeight * 5.f, Dt, S.ResponseFrequency, 1.f, 5.f);
	}

	virtual bool Evaluate(FPoseContext& Output) override
	{
		Output.ResetToRefPose();
		if (!Frame.bActive || !Rig.IsReady()) return true;
		TArray<FTransform> Pose;
		Rig.Solve({ Pitch.Value, Roll.Value, Yaw.Value, Bounce.Value, HeadRoll.Value, HeadPitch.Value, HandSlack.Value }, Frame.Forward, Frame.Right, Frame.Contacts, Pose);
		// 求解器使用全骨架组件空间；输出必须转回动画系统要求的局部空间。
		// 通过 MeshPoseIndex 映射，LOD 删减骨骼时不能直接拿 compact 索引当原骨骼索引。
		const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
		for (FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex())
		{
			const int32 Bone = Bones.MakeMeshPoseIndex(Index).GetInt();
			if (!Pose.IsValidIndex(Bone)) continue;
			const int32 Parent = Rig.Parents[Bone];
			Output.Pose[Index] = Parent != INDEX_NONE ? Pose[Bone].GetRelativeTransform(Pose[Parent]) : Pose[Bone];
		}
		return true;
	}
};
}

FAnimInstanceProxy* UDeliveryRiderAnimInstance::CreateAnimInstanceProxy() { return new FDeliveryRiderAnimProxy(this); }
void UDeliveryRiderAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) { delete Proxy; }
