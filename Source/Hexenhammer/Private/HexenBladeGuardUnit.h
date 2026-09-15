// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Units/RigUnit.h"
#include "HexenBladeGuardUnit.generated.h"

/**
 * Keeps this fighter's blade out of the other fighter's the way a solid blade would: moves the hand only
 * when the animated pose would put the two blade capsules into one another, and then by exactly this
 * fighter's share of the overlap. Does nothing at all otherwise - it can push a blade out, never pull one in.
 *
 * Runs inside the rig, on the animated pose, before any IK. So what it tests is where the animation wants
 * the blade this frame, not where last frame's correction left it. Testing the corrected pose - which the
 * overlap events on the real capsules do - feeds the correction back into its own input a frame late, and
 * a loop like that pumps: pushed out, released, animated back in, pushed out again.
 *
 * Which way apart, and whether the blades have been carried through one another, is decided once for the
 * pair on the game thread and handed to both fighters' rigs with opposite signs. This unit only works out
 * how far along that direction its own blade has to go, from its own animated pose. Two rigs deciding the
 * direction for themselves could push the same way and drag each other along, and could disagree about a
 * crossing so that only one of them pushed back.
 *
 * Reads what it needs from the fighter's UHexenCombatComponent - this blade's capsule in the hand's frame,
 * the partner's capsule as the partner's animation had it, and the pair's decision - and writes back what it
 * did, so the partner can take it off again next frame. Feed EffectorTransform to a FABRIK that ends at HandBone.
 */
USTRUCT(meta = (DisplayName = "Hexen Blade Guard", Category = "Hexenhammer", Keywords = "Blade,Contact,Collision,Bind,Guard"))
struct FRigUnit_HexenBladeGuard : public FRigUnitMutable
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	/** The bone the weapon is held in. Must be the combat component's HandBoneName. */
	UPROPERTY(meta = (Input))
	FName HandBone = FName("hand_r");

	/** Where the hand has to go: the animated hand, pushed out by this fighter's share of the overlap. Global (rig) space. */
	UPROPERTY(meta = (Output))
	FTransform EffectorTransform = FTransform::Identity;

	/** How far the two capsules overlap in the animated pose, in cm. Zero or less means they do not, and EffectorTransform is the animated hand. */
	UPROPERTY(meta = (Output))
	float Depth = 0.f;
};
