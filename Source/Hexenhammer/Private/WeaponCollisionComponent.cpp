// Fill out your copyright notice in the Description page of Project Settings.


#include "WeaponCollisionComponent.h"

float UWeaponCollisionComponent::GetContactPush() const
{
	// HoldSpeed is in m/s and velocity in cm/s, so the grip term is converted rather than the motion -
	// squaring a large number is no worse than squaring a small one, and leaving velocity untouched
	// keeps it the same quantity everything else in this file measures.
	const float HoldTerm = FMath::Square(HoldSpeed * 100.f);
	return 0.5f * Mass * (GetVolumeVelocity().SizeSquared() + HoldTerm);
}
