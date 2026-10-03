// Fill out your copyright notice in the Description page of Project Settings.


#include "WeaponCollisionComponent.h"
#include "HexenCombatComponent.h"
#include "HexenhammerCharacter.h"

float UWeaponCollisionComponent::GetHolderGripFactor() const
{
	if (const UHexenCombatComponent* Fighter = GetCombatComponent())
	{
		if (const AHexenhammerCharacter* Holder = Cast<AHexenhammerCharacter>(Fighter->GetOwner()))
		{
			return Holder->GetGripFactor();
		}
	}

	// Nobody holding it: a blade on the ground, or one whose owner has not been set yet - which does
	// happen, a weapon is given its owner some time after it is spawned into a hand. Either way there is
	// no arm braced behind it, and it resists with its own inertia and nothing more.
	return 0.f;
}

bool UWeaponCollisionComponent::IsGripBraced() const
{
	return GetHolderGripFactor() > 0.f;
}

float UWeaponCollisionComponent::GetContactInertia() const
{
	FVector AxisStart = FVector::ZeroVector;
	FVector AxisEnd = FVector::ZeroVector;
	float Radius = 0.f;
	if (Mass <= 0.f || !GetShapeAxisWorld(AxisStart, AxisEnd, Radius))
	{
		return 0.f;
	}

	const FVector Pivot = GetPivotWorld();

	// The blade's length as it swings: from the hand to the far cap. Not the axis' own length - the axis
	// stops a radius short of the shape at each end (HalfLine = HalfHeight - Radius), and it starts some
	// way out from the hand rather than at it.
	const double Reach = FMath::Max(FVector::Dist(Pivot, AxisStart), FVector::Dist(Pivot, AxisEnd)) + Radius;
	const double Length = FMath::Max(Reach, MinLeverArm);

	// A uniform rod turning about its end, plus whatever a braced grip drags along with it. A real sword
	// is not uniform - its mass sits nearer the hand than the middle, which makes the true figure smaller
	// - so this is the heavy end of the range, and a knob to turn once there are numbers to turn it
	// against.
	//
	// The grip term is nothing at all unless the fighter is bracing: a blade carried in a relaxed hand
	// has only itself in the way, and gives. See AHexenhammerCharacter::bBracing.
	const double BladeInertia = double(Mass) * Length * Length / 3.0;
	return static_cast<float>(BladeInertia * (1.0 + double(FMath::Max(0.f, GripInertiaShare)) * double(GetHolderGripFactor())));
}
