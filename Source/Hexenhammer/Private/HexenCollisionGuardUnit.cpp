// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenCollisionGuardUnit.h"
#include "HexenCombatComponent.h"
#include "Units/RigUnitContext.h"
#include "Rigs/RigHierarchy.h"
#include "CoreGlobals.h"

namespace
{
	/** Whether Point is at one of the segment's ends rather than along its body - the pair decision's IsOnBody, turned round. */
	bool IsAtSegmentEnd(const FVector& Point, const FVector& Start, const FVector& End)
	{
		const FVector Axis = End - Start;
		const double LengthSq = Axis.SizeSquared();
		if (LengthSq <= UE_KINDA_SMALL_NUMBER)
		{
			return false;
		}
		const double T = FVector::DotProduct(Point - Start, Axis) / LengthSq;
		return T <= 0.001 || T >= 0.999;
	}
}

FRigUnit_HexenCollisionGuard_Execute()
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_RIGUNIT()

	Depth = 0.f;

	URigHierarchy* Hierarchy = ExecuteContext.Hierarchy;
	if (!Hierarchy)
	{
		return;
	}

	// The animated hand - nothing in this rig has moved it yet.
	const FTransform Hand = Hierarchy->GetGlobalTransform(FRigElementKey(HandBone, ERigElementType::Bone));

	// The solver is aimed by the contact bone where the rig has one, and by the hand where it does not - see ContactBone.
	// Until a touching spot is worked out below, it is aimed where it already stands, which asks the solver for nothing.
	const FRigElementKey ContactKey(ContactBone, ERigElementType::Bone);
	const bool bAimContactBone = Hierarchy->GetIndex(ContactKey) != INDEX_NONE;
	EffectorTransform = bAimContactBone ? Hierarchy->GetGlobalTransform(ContactKey) : Hand;

	// What the rig sees before it moves anything: which frame this is, the hand, the chain from the root down to
	// it, and the transform the rig treats as world. The drift log holds all of it against the drawn pose - see
	// UHexenCombatComponent::UpdateHexenCollisionGuard.
	FHexenCollisionGuardResult Result;
	Result.EvaluationFrame = GFrameCounter;
	Result.AnimatedHandComponent = Hand.GetLocation();
	Result.RigToWorld = ExecuteContext.ToWorldSpace(FTransform::Identity);
	{
		const TArray<FName>& ChainBones = GetHexenCollisionGuardChainBones();
		Result.RigBoneCount = Hierarchy->Num(ERigElementType::Bone);
		Result.ChainNum = FMath::Min(ChainBones.Num(), FHexenCollisionGuardResult::MaxChainBones);
		for (int32 BoneIndex = 0; BoneIndex < Result.ChainNum; ++BoneIndex)
		{
			const FRigElementKey Key(ChainBones[BoneIndex], ERigElementType::Bone);
			Result.ChainComponent[BoneIndex] = Hierarchy->GetGlobalTransform(Key);
			Result.ChainLocal[BoneIndex] = Hierarchy->GetLocalTransform(Key);
			Result.ChainLocalOffset[BoneIndex] = Result.ChainLocal[BoneIndex].GetTranslation().Size();
			Result.ChainInitialOffset[BoneIndex] = Hierarchy->GetLocalTransform(Key, true).GetTranslation().Size();
			Result.ChainParent[BoneIndex] = Hierarchy->GetFirstParent(Key).Name;
		}
	}

	// Null in the rig editor's preview, which has no fighter behind it, and then the rig simply passes the
	// animation through.
	UHexenCombatComponent* Combat = UHexenCombatComponent::FindForActor(ExecuteContext.GetOwningActor());
	if (!Combat)
	{
		return;
	}

	FHexenCollisionGuardInput In;
	if (!Combat->GetHexenCollisionGuardInput(In))
	{
		Combat->SetHexenCollisionGuardResult(Result);
		return;
	}

	// Both axes in rig space: this blade's from the animated hand, the partner's brought in from the world.
	const FVector MyStart = Hand.TransformPosition(In.MyAxisStartInHand);
	const FVector MyEnd = Hand.TransformPosition(In.MyAxisEndInHand);
	const FVector TheirStart = ExecuteContext.ToVMSpace(In.PartnerAxisStartWorld);
	const FVector TheirEnd = ExecuteContext.ToVMSpace(In.PartnerAxisEndWorld);

	FVector OnMine, OnTheirs;
	FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMine, OnTheirs);

	// Where along this blade the two are closest, and whether that is at an end of either axis. Near a tip the push
	// direction turns sharply as the tip moves, and the pair reads a change of sides there as going round the tip.
	Result.ContactFromHand = FVector::Dist(Hand.GetLocation(), OnMine);
	Result.bContactAtMyEnd = IsAtSegmentEnd(OnMine, MyStart, MyEnd);
	Result.bContactAtTheirEnd = IsAtSegmentEnd(OnTheirs, TheirStart, TheirEnd);

	// Resolved to a hair inside touching rather than exactly touching, so the overlap that says the blades
	// are in contact stays on while they are pressed together - see HexenCollisionGuardSkin.
	const float Reach = In.MyRadius + In.PartnerRadius - In.Skin;

	const float Dist = FVector::Dist(OnMine, OnTheirs);

	// The pair's direction apart, in rig space. Decided once for both fighters, so the two can only push
	// apart. A direction, so it is the difference of two converted points.
	const FVector Axis = (ExecuteContext.ToVMSpace(In.PartnerAxisStartWorld + In.PairAxisWorld) - TheirStart).GetSafeNormal();
	const bool bCrossed = In.bPairCrossed && !Axis.IsNearlyZero();

	FVector Normal = Axis;
	if (Axis.IsNearlyZero())
	{
		// No decision for the pair - its very first frame, or the decision is switched off (see
		// UHexenCombatComponent::bHexenCollisionGuardPairDecision). Out along the line between the closest points.
		Normal = (OnMine - OnTheirs).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = ((MyStart + MyEnd) - (TheirStart + TheirEnd)).GetSafeNormal();
		}
		Depth = Reach - Dist;
	}
	else if (bCrossed || Dist < Reach)
	{
		// How far this blade has to go along the pair's direction to be touching on its own side. Measured
		// along that direction rather than straight out, so a blade the animation has just carried past the
		// other one is taken back instead of on: its separation along the direction is then negative.
		Depth = Reach - FVector::DotProduct(OnMine - OnTheirs, Axis);
	}
	else
	{
		Depth = Reach - Dist;
	}

	// Along the pair's direction whichever branch set Depth, so it keeps growing through a blade the animation carries
	// past the other. The release rule reads it - see UHexenCombatComponent::bRetryingHeldSwing.
	Result.AxisDepth = Axis.IsNearlyZero() ? Depth : Reach - FVector::DotProduct(OnMine - OnTheirs, Axis);

	// Where the animation had this blade and hand, before this guard moves anything - what the partner's guard
	// measures against. Published from here rather than worked out later from the drawn pose, which is not exactly
	// what this rig puts out.
	Result.bHasAnimatedPose = true;
	Result.AnimatedHandWorld = ExecuteContext.ToWorldSpace(Hand);
	Result.AnimatedAxisStartWorld = ExecuteContext.ToWorldSpace(MyStart);
	Result.AnimatedAxisEndWorld = ExecuteContext.ToWorldSpace(MyEnd);
	Result.AnimatedContactWorld = ExecuteContext.ToWorldSpace(OnMine);

	// No distance past which the blade is let go: the contact ends when the two volumes stop overlapping, and until
	// then the blade is brought back however far the animation has carried it - see UpdateCollisionPair.
	Result.Depth = Depth;
	if (Depth <= 0.f)
	{
		Combat->SetHexenCollisionGuardResult(Result);
		return;
	}

	const FVector Correction = Normal * (Depth * In.Share);

	// Park the aimed bone on the spot of this blade that touches, turned like the hand, and send the solver that spot
	// moved out of the other blade. The chain's last piece is then the blade itself, so reaching the target turns the
	// wrist as well as carrying the arm - which is what a fencer's hand does and a bodily shift cannot.
	if (bAimContactBone)
	{
		const FTransform ContactTip(Hand.GetRotation(), OnMine, FVector::OneVector);
		Hierarchy->SetGlobalTransform(ContactKey, ContactTip, false, false);
		EffectorTransform = ContactTip;
	}
	EffectorTransform.AddToTranslation(Correction);

	// Back into the world for the combat component: the partner subtracts the correction next frame, and the
	// log and the debug drawing want world positions.
	const FVector HandWorld = ExecuteContext.ToWorldSpace(Hand.GetLocation());
	Result.bPenetrating = true;
	Result.bCrossed = bCrossed;
	Result.NormalWorld = (ExecuteContext.ToWorldSpace(Hand.GetLocation() + Normal) - HandWorld).GetSafeNormal();
	Result.CorrectionWorld = ExecuteContext.ToWorldSpace(Hand.GetLocation() + Correction) - HandWorld;
	Result.ContactPointWorld = ExecuteContext.ToWorldSpace(OnMine - Normal * In.MyRadius);
	Combat->SetHexenCollisionGuardResult(Result);
}

FRigUnit_HexenServerPose_Execute()
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_RIGUNIT()

	URigHierarchy* Hierarchy = ExecuteContext.Hierarchy;
	if (!Hierarchy)
	{
		return;
	}

	const UHexenCombatComponent* Combat = UHexenCombatComponent::FindForActor(ExecuteContext.GetOwningActor());
	if (!Combat)
	{
		return;
	}

	TArray<FName> Bones;
	TArray<FTransform> Pose;
	if (!Combat->GetServerPoseForRig(Bones, Pose))
	{
		return;
	}

	// Root first, so each bone is written after the one it hangs from. Writing a bone carries everything below it along,
	// so a bone left out of the list still follows its parent into the server's pose - and the next listed bone is then
	// put exactly where the server had it, whatever its parent did. Scale is this machine's own: nothing scales a bone.
	for (int32 Index = 0; Index < Bones.Num() && Index < Pose.Num(); ++Index)
	{
		const FRigElementKey Key(Bones[Index], ERigElementType::Bone);
		if (Hierarchy->GetIndex(Key) == INDEX_NONE)
		{
			continue;
		}
		FTransform Target = Pose[Index];
		Target.SetScale3D(Hierarchy->GetGlobalTransform(Key).GetScale3D());
		Hierarchy->SetGlobalTransform(Key, Target, false, true);
	}
}
