// Fill out your copyright notice in the Description page of Project Settings.


#include "BodyCollisionComponent.h"

// Sets default values for this component's properties
UBodyCollisionComponent::UBodyCollisionComponent()
{
}

void UBodyCollisionComponent::BeginPlay()
{
	Super::BeginPlay();
	// Nothing else to do here - this is a passive hitbox, see header comment. It never calls
	// StartTrace(), so UHexenCollisionComponent::TickComponent never sweeps it; it's only ever
	// found as the *target* of a weapon's sweep, in UWeaponCollisionComponent::OnHexenHit.
}

void UBodyCollisionComponent::HandleDamage(const EDamageType DamageType)
{
	switch (DamageType)
	{
	case EDamageType::None:
		UE_LOG(LogTemp, Warning, TEXT("No damage type to handle"));
		return;
	case EDamageType::Cutting:
		ApplyBleeding();
		break;
	case EDamageType::Mauling:
		ApplyBoneBreak();
		break;
	case EDamageType::Igniting:
		ApplyBurning();
		break;
	case EDamageType::Freezing:
		ApplyFreezing();
		break;
	}

	if (!IsAlive)
	{
		if (IsVital)
		{
			//make characted dead
		}
	}
}

void  UBodyCollisionComponent::ApplyBleeding()
{
}

void  UBodyCollisionComponent::ApplyBoneBreak()
{
}

void  UBodyCollisionComponent::ApplyBurning()
{
}

void  UBodyCollisionComponent::ApplyFreezing()
{
}
