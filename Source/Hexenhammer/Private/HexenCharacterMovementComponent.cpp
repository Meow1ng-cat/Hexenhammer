// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenCharacterMovementComponent.h"
#include "HexenCombatComponent.h"
#include "CoreGlobals.h"

bool UHexenCharacterMovementComponent::GetBlock(FVector& OutOutward, FVector& OutContactPoint) const
{
	if (!Combat.IsValid())
	{
		AActor* Owner = GetOwner();
		Combat = Owner ? Owner->FindComponentByClass<UHexenCombatComponent>() : nullptr;
	}

	FVector Outward;
	if (!Combat.IsValid() || !Combat->GetHexenCollisionMovementBlock(Outward, OutContactPoint))
	{
		return false;
	}

	// The body moves along the ground, so only the horizontal part of the way out can be blocked. A way out that is nearly
	// straight up or down leaves nothing to take away, and walking then slides the blades along each other.
	Outward.Z = 0.f;
	OutOutward = Outward.GetSafeNormal();
	return !OutOutward.IsNearlyZero();
}

FVector UHexenCharacterMovementComponent::WithoutPartInto(const FVector& Vector, const FVector& Outward)
{
	const double Along = FVector::DotProduct(Vector, Outward);
	return Along < 0.0 ? Vector - Along * Outward : Vector;
}

void UHexenCharacterMovementComponent::LogStop(const TCHAR* What, float Amount, const TCHAR* Unit) const
{
#if !UE_BUILD_SHIPPING
	if (Amount < 1.f || LastLoggedFrame == GFrameCounter || !Combat.IsValid() || !Combat->bLogContactSteps)
	{
		return;
	}
	LastLoggedFrame = GFrameCounter;

	const AActor* Owner = GetOwner();
	UE_LOG(LogTemp, Warning, TEXT("[MOVE] %s %s f%llu stopped %s into the blade: %.0f %s taken away"),
		*GetNameSafe(Owner), (Owner && Owner->HasAuthority()) ? TEXT("srv") : TEXT("cli"), (unsigned long long)GFrameCounter,
		What, Amount, Unit);
#endif
}

FVector UHexenCharacterMovementComponent::ConstrainInputAcceleration(const FVector& InputAcceleration) const
{
	const FVector Constrained = Super::ConstrainInputAcceleration(InputAcceleration);
	FVector Outward, ContactPoint;
	return GetBlock(Outward, ContactPoint) ? WithoutPartInto(Constrained, Outward) : Constrained;
}

void UHexenCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);

	// The input alone is not enough: velocity already built up keeps carrying the body in while friction and braking take
	// it down - at walking speed that is most of a metre.
	FVector Outward, ContactPoint;
	if (GetBlock(Outward, ContactPoint))
	{
		const FVector Before = Velocity;
		Velocity = WithoutPartInto(Velocity, Outward);
		LogStop(TEXT("walking"), static_cast<float>((Before - Velocity).Size()), TEXT("cm/s"));
	}
}

FVector UHexenCharacterMovementComponent::ConstrainAnimRootMotionVelocity(const FVector& RootMotionVelocity, const FVector& CurrentVelocity) const
{
	const FVector Constrained = Super::ConstrainAnimRootMotionVelocity(RootMotionVelocity, CurrentVelocity);
	FVector Outward, ContactPoint;
	if (!GetBlock(Outward, ContactPoint))
	{
		return Constrained;
	}
	const FVector Kept = WithoutPartInto(Constrained, Outward);
	LogStop(TEXT("root motion"), static_cast<float>((Constrained - Kept).Size()), TEXT("cm/s"));
	return Kept;
}

FRotator UHexenCharacterMovementComponent::ComputeOrientToMovementRotation(const FRotator& CurrentRotation, float DeltaTime, FRotator& DeltaRotation) const
{
	FRotator Desired = Super::ComputeOrientToMovementRotation(CurrentRotation, DeltaTime, DeltaRotation);

	FVector Outward, ContactPoint;
	if (UpdatedComponent && GetBlock(Outward, ContactPoint))
	{
		// Turning carries the touching point round the body's vertical axis: a positive yaw moves it along Up x Arm. A turn
		// that way into the other blade is held; the other way, out of it, is left alone.
		const FVector Arm = ContactPoint - UpdatedComponent->GetComponentLocation();
		const double OutwardPerPositiveYaw = FVector::DotProduct(FVector::CrossProduct(FVector::UpVector, Arm), Outward);
		const double Turn = FRotator::NormalizeAxis(Desired.Yaw - CurrentRotation.Yaw);
		if (Turn * OutwardPerPositiveYaw < 0.0)
		{
			Desired.Yaw = CurrentRotation.Yaw;
			LogStop(TEXT("turning"), static_cast<float>(FMath::Abs(Turn)), TEXT("deg"));
		}
	}
	return Desired;
}
