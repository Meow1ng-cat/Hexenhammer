// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenBladeGuardUnit.h"
#include "HexenCombatComponent.h"
#include "Units/RigUnitContext.h"
#include "Rigs/RigHierarchy.h"

FRigUnit_HexenBladeGuard_Execute()
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
	EffectorTransform = Hand;

	// Null in the rig editor's preview, which has no fighter behind it, and then the rig simply passes the
	// animation through.
	UHexenCombatComponent* Combat = UHexenCombatComponent::FindForActor(ExecuteContext.GetOwningActor());
	if (!Combat)
	{
		return;
	}

	FHexenBladeGuardInput In;
	if (!Combat->GetBladeGuardInput(In))
	{
		Combat->SetBladeGuardResult(FHexenBladeGuardResult());
		return;
	}

	// Both axes in rig space: this blade's from the animated hand, the partner's brought in from the world.
	const FVector MyStart = Hand.TransformPosition(In.MyAxisStartInHand);
	const FVector MyEnd = Hand.TransformPosition(In.MyAxisEndInHand);
	const FVector TheirStart = ExecuteContext.ToVMSpace(In.PartnerAxisStartWorld);
	const FVector TheirEnd = ExecuteContext.ToVMSpace(In.PartnerAxisEndWorld);

	FVector OnMine, OnTheirs;
	FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMine, OnTheirs);

	// Resolved to a hair inside touching rather than exactly touching, so the overlap that says the blades
	// are in contact stays on while they are pressed together - see BladeGuardSkin.
	const float Reach = In.MyRadius + In.PartnerRadius - In.Skin;

	const float Dist = FVector::Dist(OnMine, OnTheirs);

	// The pair's direction apart, in rig space. Decided once for both fighters, so the two can only push
	// apart. A direction, so it is the difference of two converted points.
	const FVector Axis = (ExecuteContext.ToVMSpace(In.PartnerAxisStartWorld + In.PairAxisWorld) - TheirStart).GetSafeNormal();
	const bool bCrossed = In.bPairCrossed && !Axis.IsNearlyZero();

	FVector Normal = Axis;
	if (Axis.IsNearlyZero())
	{
		// No decision for the pair yet - its very first frame. Out along the line between the closest points.
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

	// So far through that no blade could have been held back from there - let it go rather than haul the arm.
	if (Depth > Reach + In.MaxPushBack)
	{
		Depth = 0.f;
	}

	FHexenBladeGuardResult Result;
	Result.Depth = Depth;
	if (Depth <= 0.f)
	{
		Combat->SetBladeGuardResult(Result);
		return;
	}

	const FVector Correction = Normal * (Depth * In.Share);
	EffectorTransform.AddToTranslation(Correction);

	// Back into the world for the combat component: the partner subtracts the correction next frame, and the
	// log and the debug drawing want world positions.
	const FVector HandWorld = ExecuteContext.ToWorldSpace(Hand.GetLocation());
	Result.bPenetrating = true;
	Result.bCrossed = bCrossed;
	Result.NormalWorld = (ExecuteContext.ToWorldSpace(Hand.GetLocation() + Normal) - HandWorld).GetSafeNormal();
	Result.CorrectionWorld = ExecuteContext.ToWorldSpace(Hand.GetLocation() + Correction) - HandWorld;
	Result.ContactPointWorld = ExecuteContext.ToWorldSpace(OnMine - Normal * In.MyRadius);
	Combat->SetBladeGuardResult(Result);
}
