// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenCombatComponent.h"
#include "HexenCollisionComponent.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Net/UnrealNetwork.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"

UHexenCombatComponent::UHexenCombatComponent()
{
	// Ticks only while a contact is being held - see RefreshContactState. Between clashes this component
	// costs nothing at all.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// After physics, because that is the latest the character's mesh can finish. A skeletal mesh's tick
	// is held open until its parallel animation evaluation has been folded back in, and with the engine's
	// late-end option that can be as late as TG_PostPhysics. Ticking in that group rather than the
	// default one means the prerequisite added in BeginPlay never has to drag this across a group
	// boundary to be honoured.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// The ratchet is decided on the server and its target has to reach every machine that draws the
	// fighter - see ContactTarget.
	SetIsReplicatedByDefault(true);
}

void UHexenCombatComponent::BeginPlay()
{
	Super::BeginPlay();

	// The fix for the stale measurement. Without this the tick order against the mesh was undefined, and
	// on any frame this ran first it measured the hand where last frame's pose had left it - a frame
	// behind the blade the solver was actually moving. With the ABP already reading the target a frame
	// early by necessity, that made two frames of delay in the loop, and a feedback loop with that much
	// lag hunts: overshoot, stall, catch up.
	//
	// Depending on the mesh's tick is enough to get the finished pose, not merely a started one - the
	// mesh's tick completion is held until its parallel evaluation task has run (see
	// USkeletalMeshComponent, DontCompleteUntil on the tick's completion handle).
	if (USkeletalMeshComponent* Mesh = GetOwnerMesh())
	{
		AddTickPrerequisiteComponent(Mesh);
	}
#if !UE_BUILD_SHIPPING
	else
	{
		UE_LOG(LogTemp, Error, TEXT("%s on %s found no character mesh - it will measure the blade against whatever pose happens to be current."),
			*GetName(), *GetNameSafe(GetOwner()));
	}
#endif
}

void UHexenCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// A fighter removed mid-contact would otherwise leave its montage paused with nothing left that
	// could ever resume it.
	ResumePausedMontage();

	Super::EndPlay(EndPlayReason);
}

void UHexenCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHexenCombatComponent, ContactTarget);
	DOREPLIFETIME(UHexenCombatComponent, ContactStepsTaken);
}

void UHexenCombatComponent::SetVolumeContact(UHexenCollisionComponent* Volume, bool bVolumeInContact, const FVector& WorldContactPoint)
{
	if (!Volume)
	{
		return;
	}

	// Only the contact that takes the fighter from clear to engaged starts a new ratchet. A second
	// volume arriving mid-bind does not get to reset the point the first one is being pushed out from -
	// that would throw away the separation already achieved and start over.
	const bool bWasClear = ContactingVolumes.Num() == 0;

	if (bVolumeInContact)
	{
		ContactingVolumes.Add(Volume);
		if (bWasClear)
		{
			BeginContact(Volume, WorldContactPoint);
		}
	}
	else
	{
		ContactingVolumes.Remove(Volume);
	}

	RefreshContactState();
}

void UHexenCombatComponent::BeginContact(UHexenCollisionComponent* Volume, const FVector& WorldContactPoint)
{
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	if (!Mesh)
	{
		return;
	}

	ContactVolume = Volume;
	ContactStepsTaken = 0;
	++ContactBindId;

	// The one thing held for the whole bind: which part of the blade was touched. Kept in the hand's
	// frame because that is the frame it does not move in - the grip does not change while the bind
	// holds, so this stays true with no updating, and its world position falls out of the hand.
	ContactHandOffset = Mesh->GetSocketTransform(HandBoneName, RTS_World).InverseTransformPosition(WorldContactPoint);
	ContactPoint = Mesh->GetComponentTransform().InverseTransformPosition(WorldContactPoint);

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		// offAlongBlade is the sanity check on the geometry: it should look like the distance from the
		// grip to where the blades crossed. A number far larger than the weapon says the capsule is not
		// where the blade is, and everything downstream of that is being fed nonsense.
		UE_LOG(LogTemp, Warning, TEXT("[BIND] %s BEGIN with %s | offAlongBlade %.1fcm | world %s"),
			*GetNameSafe(GetOwner()), *GetNameSafe(Volume ? Volume->GetOwner() : nullptr),
			ContactHandOffset.Size(), *WorldContactPoint.ToCompactString());
	}
#endif

	if (GetOwner() && GetOwner()->HasAuthority())
	{
		StepTarget(WorldContactPoint);
	}
	else
	{
		// A client does not step - it waits for the server's target to arrive. Until it does, hold the
		// target on the blade itself, which asks the solver for nothing, rather than leave last bind's
		// target in place and have the arm flung towards somewhere that stopped meaning anything.
		ContactTarget = ContactPoint;
	}

#if !UE_BUILD_SHIPPING
	if (!Mesh->DoesSocketExist(HandBoneName) && Mesh->GetBoneIndex(HandBoneName) == INDEX_NONE)
	{
		UE_LOG(LogTemp, Error, TEXT("%s: HandBoneName '%s' does not exist on %s - the touched spot is being measured from the mesh origin, not the hand."),
			*GetNameSafe(GetOwner()), *HandBoneName.ToString(), *Mesh->GetName());
	}
#endif
}

void UHexenCombatComponent::StepTarget(const FVector& FromWorld)
{
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	if (!Mesh || !ContactVolume.IsValid())
	{
		return;
	}

	// Asked fresh on every step rather than remembered from the moment of contact. Two blades in a bind
	// slide along and turn against one another, so the shortest way out genuinely moves; a direction
	// frozen at first touch would keep pushing where the overlap no longer is, and the ratchet would
	// grind on without ever separating them.
	FVector Normal;
	if (!ContactVolume->ComputeSeparationNormal(Normal))
	{
		// Nothing is touching any more. The EndOverlap that follows will clear this properly; until then
		// leave the target where it is rather than inventing a direction.
#if !UE_BUILD_SHIPPING
		if (bLogContactSteps)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s step %d SKIPPED - no separation direction, shapes report no overlap"),
				*GetNameSafe(GetOwner()), ContactStepsTaken + 1);
		}
#endif
		return;
	}

	ContactTarget = Mesh->GetComponentTransform().InverseTransformPosition(FromWorld + Normal * ContactStep);
	++ContactStepsTaken;

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		// The direction is the thing worth reading here. It should point out of the other blade and turn
		// gradually as the two slide; jumping about between steps means the closest-approach query is
		// ill-conditioned, which happens when the capsules are far thicker than the blades they stand for.
		UE_LOG(LogTemp, Warning, TEXT("[BIND] %s step %d | gap was %.1fcm | normal %s | +%.1fcm"),
			*GetNameSafe(GetOwner()), ContactStepsTaken, ContactGapDistance,
			*Normal.ToCompactString(), ContactStep);
	}
#endif
}

void UHexenCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	if (!Mesh)
	{
		return;
	}

	// Keeps running past the end of the contact, for as long as the blend is still letting go. The point
	// has to go on tracking the hand through the release or the rig would be blending away from a pose
	// frozen where the blade no longer is, which looks like a second, smaller teleport.
	if (ContactCount == 0)
	{
		ContactBlendAlpha = FMath::FInterpTo(ContactBlendAlpha, 0.f, DeltaTime, ContactBlendReleaseSpeed);
		if (ContactBlendAlpha < KINDA_SMALL_NUMBER)
		{
			ContactBlendAlpha = 0.f;
			ContactGapDistance = 0.f;
			SetComponentTickEnabled(false);
			return;
		}
	}

	const FTransform ComponentToWorld = Mesh->GetComponentTransform();

	// Where the touched part of the blade has got to, rebuilt from the hand. This is what the solver is
	// moving, so this is what has to be measured against the target.
	const FVector PointWorld = Mesh->GetSocketTransform(HandBoneName, RTS_World).TransformPosition(ContactHandOffset);
	const FVector TargetWorld = ComponentToWorld.TransformPosition(ContactTarget);

	ContactPoint = ComponentToWorld.InverseTransformPosition(PointWorld);
	ContactGapDistance = (TargetWorld - PointWorld).Size();

	// Arrived, so push the target one step further out. The blades come apart once enough of these have
	// accumulated, and the overlap ending is what stops it. Nothing here counts centimetres of overlap,
	// which is exactly why this needs no depth measurement at all.
	// Only while the blades are actually touching. During the release the target stays where the last
	// step left it and the blend walks the pose back to the animation; ratcheting on through the fade
	// would keep shoving a blade that has nothing left to push against.
	// And only on the server. Every machine measures the gap - the client needs it to draw and to know
	// where its own blade is - but only one of them may decide that it is time to push further.
	if (ContactCount > 0 && GetOwner() && GetOwner()->HasAuthority()
		&& ContactGapDistance <= ContactStep * ContactReachedFraction)
	{
		StepTarget(PointWorld);
	}

#if !UE_BUILD_SHIPPING
	if (bDrawContactPoints)
	{
		const FVector DrawTarget = ComponentToWorld.TransformPosition(ContactTarget);
		DrawDebugSphere(GetWorld(), PointWorld, 4.f, 12, FColor::Yellow, false, -1.f, 0, 1.f);
		DrawDebugSphere(GetWorld(), DrawTarget, 4.f, 12, FColor::Red, false, -1.f, 0, 1.f);
		DrawDebugLine(GetWorld(), PointWorld, DrawTarget, FColor::Orange, false, -1.f, 0, 2.f);

		if (GEngine)
		{
			// steps is the number to watch. Climbing means the solver is moving the blade and the ratchet
			// is doing its job. Stuck at 1 with a gap that never closes means the rig is not moving
			// anything, and no amount of tuning ContactStep will change that.
			GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this), 0.f, FColor::Green,
				FString::Printf(TEXT("[%s] contacts %d | steps %d | gap %.1fcm | step %.1fcm"),
					*GetNameSafe(GetOwner()), ContactCount, ContactStepsTaken, ContactGapDistance, ContactStep));
		}
	}
#endif
}

void UHexenCombatComponent::RefreshContactState()
{
	// A volume destroyed without reporting - a weapon dropped, a limb dismembered later on - would
	// otherwise keep the count above zero for the rest of the match, and the arm out of its animation
	// with it.
	for (auto It = ContactingVolumes.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}

	ContactCount = ContactingVolumes.Num();

	// On every machine, not just the server: the montage plays everywhere, and ContactCount is derived
	// from replicated state, so each machine pauses the same swing at the same contact.
	UpdateMontagePause();

	// Snapped on, eased off - the easing is the tick's job, so nothing here writes a zero.
	if (ContactCount > 0)
	{
		ContactBlendAlpha = 1.f;
	}

	if (ContactCount == 0)
	{
#if !UE_BUILD_SHIPPING
		if (bLogContactSteps && ContactStepsTaken > 0)
		{
			// The summary line. steps x ContactStep is roughly how far the blade was pushed before the
			// shapes came apart, so it is also a measure of how deep they had got - which is the number
			// this whole design avoids having to measure directly.
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s END after %d step(s), ~%.1fcm pushed out"),
				*GetNameSafe(GetOwner()), ContactStepsTaken, ContactStepsTaken * ContactStep);
		}
#endif
		ContactVolume = nullptr;
	}

	// Stays on through the release, and switches itself off in the tick once the blend has reached zero.
	SetComponentTickEnabled(ContactCount > 0 || ContactBlendAlpha > 0.f);
}

void UHexenCombatComponent::UpdateMontagePause()
{
	if (ContactCount == 0)
	{
		ResumePausedMontage();
		return;
	}

	// Already holding one - a second volume joining the contact changes nothing about the swing.
	if (!bPauseMontageOnContact || PausedMontage.IsValid())
	{
		return;
	}

	// Always the main graph's instance: montages are played and stopped there, and a post-process
	// instance plays none.
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UAnimMontage* Active = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr;

	if (Active)
	{
		AnimInstance->Montage_Pause(Active);
		PausedMontage = Active;
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		if (Active)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s PAUSED montage %s"), *GetNameSafe(GetOwner()), *GetNameSafe(Active));
		}
		else
		{
			// Worth knowing rather than guessing: a guard pose, or a swing that is not a montage, has
			// nothing here to stop.
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s no active montage - nothing to pause"), *GetNameSafe(GetOwner()));
		}
	}
#endif
}

void UHexenCombatComponent::ResumePausedMontage()
{
	UAnimMontage* Montage = PausedMontage.Get();
	PausedMontage = nullptr;

	if (!Montage)
	{
		return;
	}

	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;

	// IsActive, NOT IsPlaying. A paused montage instance is active but reports IsPlaying() as false -
	// FAnimMontageInstance::IsPlaying() is IsValid() && bPlaying, and pausing clears bPlaying. Testing
	// IsPlaying() here, which an earlier version did, meant the pause was never lifted: the check failed
	// on precisely the montage it was meant to resume.
	//
	// IsActive still excludes the case worth excluding - a montage interrupted and replaced while the
	// contact held has no instance any more, and resuming it would restart something already overruled.
	if (AnimInstance && AnimInstance->Montage_IsActive(Montage))
	{
		AnimInstance->Montage_Resume(Montage);

#if !UE_BUILD_SHIPPING
		if (bLogContactSteps)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s RESUMED montage %s"), *GetNameSafe(GetOwner()), *GetNameSafe(Montage));
		}
#endif
	}
}

USkeletalMeshComponent* UHexenCombatComponent::GetOwnerMesh() const
{
	// Not cached: a character's mesh outlives this component, but reading it is a pointer fetch, and
	// caching would only add a way to hold a stale one.
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	return OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
}
