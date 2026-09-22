// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HexenCharacterMovementComponent.generated.h"

class UHexenCombatComponent;

/**
 * Character movement that does not carry a fighter's blade into another blade it is held against.
 *
 * TEMPORARY - the user's rule of 2026-09-19, until contact is answered by the fighters' mass, speed and energy. The
 * collision guard can only move the hand, and a body walking on hauled the arm back until it could reach no further and
 * the blade went through. So while the guard is pushing this fighter's blade out of another, the part of the fighter's own
 * movement that would drive the blade further in - input, velocity, root motion and turning - is taken away. Moving
 * sideways or away stays free. See UHexenCombatComponent::bHexenCollisionBlocksMovement.
 *
 * Runs inside the movement update, so the owning client applies it to the moves it predicts and the server to the same
 * moves when it replays them, each by its own guard's view of the contact.
 */
UCLASS()
class UHexenCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	virtual FVector ConstrainInputAcceleration(const FVector& InputAcceleration) const override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual FVector ConstrainAnimRootMotionVelocity(const FVector& RootMotionVelocity, const FVector& CurrentVelocity) const override;
	virtual FRotator ComputeOrientToMovementRotation(const FRotator& CurrentRotation, float DeltaTime, FRotator& DeltaRotation) const override;

private:
	/**
	 * While this fighter's blade is held out of another one: the horizontal direction out of the other blade, and where the
	 * two touch. False when there is nothing to stop.
	 */
	bool GetBlock(FVector& OutOutward, FVector& OutContactPoint) const;

	/** Vector less its part along -Outward - the part that would carry the blade further into the other one. */
	static FVector WithoutPartInto(const FVector& Vector, const FVector& Outward);

	/** One log line a frame at most, however many substeps the movement takes. */
	void LogStop(const TCHAR* What, float Amount, const TCHAR* Unit) const;

	/** Found on first use - a character's components do not change. */
	mutable TWeakObjectPtr<UHexenCombatComponent> Combat;

	mutable uint64 LastLoggedFrame = 0;
};
