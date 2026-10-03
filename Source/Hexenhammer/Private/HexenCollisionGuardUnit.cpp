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

	// The hand the blade is actually built from. While a contact owns this chain that is the HELD hand,
	// kept in the rig's own space - the mesh's - so it travels with the body and not with the animation.
	// Which is the whole point: a second swing, a stance, anything the animation does with the arm no
	// longer reaches the blade, and no flag anywhere has to know what was playing.
	const FTransform ActingHand = In.bHasHeldHand ? In.HeldHandRig : Hand;

	// Both axes in rig space: this blade's from the acting hand, the partner's brought in from the world.
	const FVector MyStart = ActingHand.TransformPosition(In.MyAxisStartInHand);
	const FVector MyEnd = ActingHand.TransformPosition(In.MyAxisEndInHand);
	const FVector TheirStart = ExecuteContext.ToVMSpace(In.PartnerAxisStartWorld);
	const FVector TheirEnd = ExecuteContext.ToVMSpace(In.PartnerAxisEndWorld);

	FVector OnMine, OnTheirs;
	FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMine, OnTheirs);

	// And where the ANIMATION wanted the blade, measured against the same partner. Not applied to
	// anything - it is the one question the animation still answers while the contact owns the chain:
	// would it still have the two blades in each other? That is what ends the contact, and it has to be
	// asked of a pose nothing is holding, because a held blade never parts from anything by itself.
	//
	// One extra segment pair per blade per frame, every frame, held contact or not. That is deliberate:
	// bodies meet because they WALK into each other as much as because an animation swings them, and a
	// check that only runs while an animation is driving would miss every one of those.
	const FVector AnimStart = Hand.TransformPosition(In.MyAxisStartInHand);
	const FVector AnimEnd = Hand.TransformPosition(In.MyAxisEndInHand);
	FVector OnAnim, OnTheirsAnim;
	FMath::SegmentDistToSegmentSafe(AnimStart, AnimEnd, TheirStart, TheirEnd, OnAnim, OnTheirsAnim);
	Result.AnimatedHandRig = Hand;

	// Where along this blade the two are closest, and whether that is at an end of either axis. Near a tip the push
	// direction turns sharply as the tip moves, and the pair reads a change of sides there as going round the tip.
	Result.ContactFromHand = FVector::Dist(ActingHand.GetLocation(), OnMine);
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

	// Which side of the partner's blade the animation wants this one on, along the pair's own direction.
	// Positive is this blade's own side, negative is through and out the far side.
	//
	// A side and not a distance. Asking "are the animated poses still touching" took the chain away on
	// every contact - during a bind that is always true - and so replaced an ordinary correction that was
	// working with a hold that flickered at a median of two to four frames. What actually needs the chain
	// taken away is one thing only: an animation trying to put this blade THROUGH the other one. Pressed
	// hard against it is not that, and the push handles it.
	//
	// No direction yet - the pair's first frame, or the pair decision switched off - and there is nothing
	// to take a side against, so the animation keeps the chain.
	const float AnimAcross = Axis.IsNearlyZero() ? Reach : FVector::DotProduct(OnAnim - OnTheirsAnim, Axis);
	Result.bAnimationThroughPartner = AnimAcross < 0.f;
	Result.bAnimationClearOfPartner = AnimAcross > Reach;

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
	Result.bHasActingPose = true;
	Result.ActingHandWorld = ExecuteContext.ToWorldSpace(ActingHand);
	Result.ActingAxisStartWorld = ExecuteContext.ToWorldSpace(MyStart);
	Result.ActingAxisEndWorld = ExecuteContext.ToWorldSpace(MyEnd);
	Result.ActingContactWorld = ExecuteContext.ToWorldSpace(OnMine);

	// No distance past which the blade is let go: the contact ends when the two volumes stop overlapping, and until
	// then the blade is brought back however far the animation has carried it - see UpdateCollisionPair.
	Result.Depth = Depth;

	// Where the last clash threw this blade, brought into rig space as a direction rather than a point.
	// It applies whether or not anything is overlapping right now, and that is the whole point of it: the
	// blade is away from its animation because it was struck, not because something is in its way this
	// frame. Without this the throw would be undone the instant it opened a gap.
	const FVector KnockRig = In.KnockOffsetWorld.IsNearlyZero()
		? FVector::ZeroVector
		: ExecuteContext.ToVMSpace(In.PartnerAxisStartWorld + In.KnockOffsetWorld) - TheirStart;

	// Being held is a reason to drive this chain in its own right, and leaving it out was the whole of why
	// the hold did nothing. The effector starts the frame at wherever the ANIMATION left the contact bone,
	// which is how the chain is handed back when there is nothing to resolve. But a held blade sits exactly
	// on the touching line - the guard put it there - so its depth hovers at zero and goes negative as often
	// as not, and on every one of those frames this early return handed the chain straight back to the
	// animation. Which is precisely the frame a swing pressed mid-contact needs to fly through.
	const bool bPenetrating = Depth > 0.f;
	if (!bPenetrating && !In.bHasHeldHand && KnockRig.IsNearlyZero())
	{
		Combat->SetHexenCollisionGuardResult(Result);
		return;
	}

	// Two different things, added: the share of the overlap this blade gives way by, which holds the two
	// apart while they are against each other, and the throw left over from the moment they met.
	// The yielding side's part is driven past the touching line by In.Overdrive, so that a lost bind ends
	// with the blade out of the way rather than resting against the one that beat it. One on the winning
	// side and one when the two are evenly matched, so a bind resolves to exactly the overlap as before.
	const FVector Correction = (bPenetrating ? Normal * (Depth * In.Share * FMath::Max(1.f, In.Overdrive)) : FVector::ZeroVector) + KnockRig;

	// Park the aimed bone on the spot of this blade that touches, turned like the hand, and send the solver that spot
	// moved out of the other blade. The chain's last piece is then the blade itself, so reaching the target turns the
	// wrist as well as carrying the arm - which is what a fencer's hand does and a bodily shift cannot.
	if (bAimContactBone)
	{
		const FTransform ContactTip(ActingHand.GetRotation(), OnMine, FVector::OneVector);
		Hierarchy->SetGlobalTransform(ContactKey, ContactTip, false, false);
		EffectorTransform = ContactTip;
	}
	EffectorTransform.AddToTranslation(Correction);

	// Back into the world for the combat component: the partner subtracts the correction next frame, and the
	// log and the debug drawing want world positions.
	const FVector HandWorld = ExecuteContext.ToWorldSpace(ActingHand.GetLocation());
	Result.bPenetrating = bPenetrating;
	Result.bCrossed = bCrossed;
	Result.NormalWorld = (ExecuteContext.ToWorldSpace(ActingHand.GetLocation() + Normal) - HandWorld).GetSafeNormal();
	Result.CorrectionWorld = ExecuteContext.ToWorldSpace(ActingHand.GetLocation() + Correction) - HandWorld;
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
