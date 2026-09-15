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
#include "Misc/ScopeLock.h"

namespace
{
	/**
	 * Which combat component belongs to which fighter, for the rig. A rig unit knows the actor it is running
	 * for and nothing else, runs on a worker thread, and must not go searching an actor's components there.
	 */
	FCriticalSection GCombatRegistryLock;
	TMap<const AActor*, TWeakObjectPtr<UHexenCombatComponent>> GCombatRegistry;

	/**
	 * One record per pair of blades in range of each other: which way apart they are, and whether they have
	 * been carried through one another. Keyed by the two volumes in address order, so both fighters of a
	 * pair land on the same record. Game thread only.
	 */
	struct FHexenBladePair
	{
		TWeakObjectPtr<UHexenCollisionComponent> First;
		TWeakObjectPtr<UHexenCollisionComponent> Second;

		/** Pointing from Second towards First. */
		FVector Side = FVector::ZeroVector;

		bool bCrossed = false;

		/** The frame the record was last decided in, so the second fighter of the pair reads it rather than deciding again. */
		uint64 Frame = 0;

		/** Both blades' animated hands last frame, for the sweep - each blade is rebuilt from its hand. */
		bool bHasPrevious = false;
		FTransform PrevFirstHand = FTransform::Identity;
		FTransform PrevSecondHand = FTransform::Identity;

		/** The frame in which the sweep found the blades meeting between two poses, and how far through the last frame they met. */
		uint64 TouchFrame = 0;
		float TouchAlpha = 0.f;
	};

	TMap<TPair<const UHexenCollisionComponent*, const UHexenCollisionComponent*>, FHexenBladePair> GBladePairs;

	/** A blade where its animation had it - as drawn, minus what its own guard added on top - with the hand that holds it. */
	struct FAnimatedBlade
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		float Radius = 0.f;

		/** The hand, where the animation had it. */
		FTransform Hand = FTransform::Identity;

		/** The axis in the hand's frame - fixed, since the grip does not change. */
		FVector LocalStart = FVector::ZeroVector;
		FVector LocalEnd = FVector::ZeroVector;
	};

	bool GetAnimatedBlade(UHexenCollisionComponent* Blade, FAnimatedBlade& Out)
	{
		UHexenCombatComponent* Fighter = Blade ? Blade->GetFighter() : nullptr;
		FTransform DrawnHand;
		if (!Fighter || !Fighter->GetHandTransformWorld(DrawnHand) || !Blade->GetShapeAxisWorld(Out.Start, Out.End, Out.Radius))
		{
			return false;
		}

		Out.LocalStart = DrawnHand.InverseTransformPosition(Out.Start);
		Out.LocalEnd = DrawnHand.InverseTransformPosition(Out.End);

		// The guard moves the hand by a translation, so taking it off again is a translation too.
		const FVector Correction = Fighter->GetBladeGuardCorrectionWorld();
		Out.Hand = DrawnHand;
		Out.Hand.AddToTranslation(-Correction);
		Out.Start -= Correction;
		Out.End -= Correction;
		return true;
	}

	/** Whether Point lies along the body of the segment rather than at one of its ends. */
	bool IsOnBody(const FVector& Point, const FVector& Start, const FVector& End)
	{
		const FVector Axis = End - Start;
		const double LengthSq = Axis.SizeSquared();
		if (LengthSq <= UE_KINDA_SMALL_NUMBER)
		{
			return true;
		}
		const double T = FVector::DotProduct(Point - Start, Axis) / LengthSq;
		return T > 0.001 && T < 0.999;
	}

	/**
	 * Decides, once per frame, which way apart the pair is and whether its blades have been carried through
	 * one another - from where both animations had the blades, so the two fighters' corrections do not feed
	 * back into it.
	 */
	const FHexenBladePair& UpdateBladePair(UHexenCollisionComponent* A, UHexenCollisionComponent* B, float MaxPushBack, float Skin, int32 SweepSteps)
	{
		for (auto It = GBladePairs.CreateIterator(); It; ++It)
		{
			if (!It.Value().First.IsValid() || !It.Value().Second.IsValid())
			{
				It.RemoveCurrent();
			}
		}

		UHexenCollisionComponent* First = A < B ? A : B;
		UHexenCollisionComponent* Second = A < B ? B : A;
		FHexenBladePair& Pair = GBladePairs.FindOrAdd(MakeTuple(static_cast<const UHexenCollisionComponent*>(First), static_cast<const UHexenCollisionComponent*>(Second)));

		// The other fighter of the pair has already decided this frame.
		if (Pair.Frame == GFrameCounter)
		{
			return Pair;
		}
		Pair.First = First;
		Pair.Second = Second;
		Pair.Frame = GFrameCounter;

		FAnimatedBlade FirstBlade, SecondBlade;
		if (!GetAnimatedBlade(First, FirstBlade) || !GetAnimatedBlade(Second, SecondBlade))
		{
			return Pair;
		}
		const FVector& FirstStart = FirstBlade.Start;
		const FVector& FirstEnd = FirstBlade.End;
		const FVector& SecondStart = SecondBlade.Start;
		const FVector& SecondEnd = SecondBlade.End;
		const float FirstRadius = FirstBlade.Radius;
		const float SecondRadius = SecondBlade.Radius;

		FVector OnFirst, OnSecond;
		FMath::SegmentDistToSegmentSafe(FirstStart, FirstEnd, SecondStart, SecondEnd, OnFirst, OnSecond);
		FVector Normal = (OnFirst - OnSecond).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = ((FirstStart + FirstEnd) - (SecondStart + SecondEnd)).GetSafeNormal();
		}
		if (Normal.IsNearlyZero())
		{
			return Pair;
		}

		// The sweep: where between last frame and this one the two first came within reach of each other. Only
		// when they were apart last frame - inside a contact the guard already holds them.
		//
		// Each blade is carried between its two poses by its hand - position in a straight line, rotation along
		// the shortest arc - and rebuilt from the hand as it is fixed to it. A strike turns the blade about the
		// hand; moving the blade's two ends in straight lines instead cut inside the arc the tip really follows,
		// so the meeting came out late and the swing was not wound back far enough.
		bool bSweptTouch = false;
		const float Reach = FirstRadius + SecondRadius - Skin;
		if (Pair.bHasPrevious)
		{
			auto BladeAt = [](const FTransform& Hand, const FAnimatedBlade& Blade, FVector& OutStart, FVector& OutEnd)
			{
				OutStart = Hand.TransformPosition(Blade.LocalStart);
				OutEnd = Hand.TransformPosition(Blade.LocalEnd);
			};

			FVector WasFirstStart, WasFirstEnd, WasSecondStart, WasSecondEnd, WasOnFirst, WasOnSecond;
			BladeAt(Pair.PrevFirstHand, FirstBlade, WasFirstStart, WasFirstEnd);
			BladeAt(Pair.PrevSecondHand, SecondBlade, WasSecondStart, WasSecondEnd);
			FMath::SegmentDistToSegmentSafe(WasFirstStart, WasFirstEnd, WasSecondStart, WasSecondEnd, WasOnFirst, WasOnSecond);
			if (FVector::Dist(WasOnFirst, WasOnSecond) >= Reach)
			{
				const int32 Steps = FMath::Max(1, SweepSteps);
				for (int32 Step = 1; Step <= Steps; ++Step)
				{
					const float Alpha = static_cast<float>(Step) / Steps;
					FTransform FirstHand, SecondHand;
					FirstHand.Blend(Pair.PrevFirstHand, FirstBlade.Hand, Alpha);
					SecondHand.Blend(Pair.PrevSecondHand, SecondBlade.Hand, Alpha);

					FVector StepFirstStart, StepFirstEnd, StepSecondStart, StepSecondEnd, StepOnFirst, StepOnSecond;
					BladeAt(FirstHand, FirstBlade, StepFirstStart, StepFirstEnd);
					BladeAt(SecondHand, SecondBlade, StepSecondStart, StepSecondEnd);
					FMath::SegmentDistToSegmentSafe(StepFirstStart, StepFirstEnd, StepSecondStart, StepSecondEnd, StepOnFirst, StepOnSecond);
					if (FVector::Dist(StepOnFirst, StepOnSecond) < Reach)
					{
						// The last step at which they were still apart - rewinding to it leaves the blades just short
						// of touching rather than just inside.
						bSweptTouch = true;
						Pair.TouchFrame = GFrameCounter;
						Pair.TouchAlpha = static_cast<float>(Step - 1) / Steps;
						break;
					}
				}
			}
		}
		Pair.PrevFirstHand = FirstBlade.Hand;
		Pair.PrevSecondHand = SecondBlade.Hand;
		Pair.bHasPrevious = true;

		if (Pair.Side.IsNearlyZero())
		{
			Pair.Side = Normal;
			Pair.bCrossed = false;
			return Pair;
		}

		const bool bFlipped = FVector::DotProduct(Normal, Pair.Side) < 0.f;
		if (Pair.bCrossed)
		{
			// Stays crossed until the animations bring the blades back to their own sides - or have carried
			// them so far through that no blade could have been held back from there.
			const float PastBy = -FVector::DotProduct(OnFirst - OnSecond, Pair.Side);
			if (!bFlipped || PastBy > MaxPushBack)
			{
				Pair.bCrossed = false;
				Pair.Side = Normal;
			}
		}
		else if (bFlipped && (bSweptTouch || (IsOnBody(OnFirst, FirstStart, FirstEnd) && IsOnBody(OnSecond, SecondStart, SecondEnd))))
		{
			// Turned past a right angle in one frame, and either the sweep saw them meet on the way or each
			// blade's closest point is on the other's body: not a blade sliding along, and not one going round
			// a tip - the animations carried them through.
			Pair.bCrossed = true;
		}
		else
		{
			// Still on the same sides, or legitimately round a tip: the direction follows the blades.
			Pair.Side = Normal;
		}

		return Pair;
	}
}

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

	{
		FScopeLock Lock(&GCombatRegistryLock);
		GCombatRegistry.Add(GetOwner(), this);
	}

	// The guard needs the two blades' poses every frame, touching or not - it is the rig that decides
	// whether they touch.
	if (bUseBladeGuard)
	{
		SetComponentTickEnabled(true);
	}
}

void UHexenCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	{
		FScopeLock Lock(&GCombatRegistryLock);
		GCombatRegistry.Remove(GetOwner());
	}

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
	StalledFrames = 0;
	PreviousGapDistance = TNumericLimits<float>::Max();
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

	if (GetOwner() && GetOwner()->HasAuthority() && !bUseBladeGuard)
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

	if (bUseBladeGuard)
	{
		// The rig decides from the animated pose every frame; there is no target to chase and no blend to ease.
		ContactBlendAlpha = 1.f;
		UpdateBladeGuard();
		return;
	}

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
	const FTransform HandTM = Mesh->GetSocketTransform(HandBoneName, RTS_World);
	const FVector PointWorld = HandTM.TransformPosition(ContactHandOffset);
	const FVector TargetWorld = ComponentToWorld.TransformPosition(ContactTarget);

	ContactPoint = ComponentToWorld.InverseTransformPosition(PointWorld);
	ContactGapDistance = (TargetWorld - PointWorld).Size();

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps && ContactCount > 0)
	{
		// One line per frame while touching. The step lines only show that the ratchet stalled; this shows by
		// how much and on which machine. A gap that hovers around one step says the rig is aiming at the wrong
		// spot on the blade; a gap of tens of centimetres says it is not moving the blade at all.
		// Where the target sits as seen from the hand, minus where the touched spot sits in the hand: the move,
		// in the hand's own frame, that would still put the spot on the target. A vector that comes out the same
		// bind after bind says a different point of the blade is being placed on the target - a fixed offset
		// between two frames of reference, not the solver falling short.
		const FVector HandErr = HandTM.InverseTransformPosition(TargetWorld) - ContactHandOffset;
		UE_LOG(LogTemp, Warning, TEXT("[BIND] %s tick f%llu %s | gap %.2fcm | point %s | target %s | alpha %.2f | steps %d | handErr %s (%.2fcm)"),
			*GetNameSafe(GetOwner()), (unsigned long long)GFrameCounter,
			(GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli"),
			ContactGapDistance, *ContactPoint.ToCompactString(), *ContactTarget.ToCompactString(),
			ContactBlendAlpha, ContactStepsTaken, *HandErr.ToCompactString(), HandErr.Size());
	}
#endif

	// Arrived, so push the target one step further out. The blades come apart once enough of these have
	// accumulated, and the overlap ending is what stops it. Nothing here counts centimetres of overlap,
	// which is exactly why this needs no depth measurement at all.
	// Only while the blades are actually touching. During the release the target stays where the last
	// step left it and the blend walks the pose back to the animation; ratcheting on through the fade
	// would keep shoving a blade that has nothing left to push against.
	// And only on the server. Every machine measures the gap - the client needs it to draw and to know
	// where its own blade is - but only one of them may decide that it is time to push further.
	if (ContactCount > 0 && GetOwner() && GetOwner()->HasAuthority())
	{
		const float Tolerance = ContactStep * ContactReachedFraction;

		// Arrived is the clean case. Stalled is the one the solver actually produces: it settles a few
		// centimetres short of the target and stays there, so "arrived" never comes. A gap that has stopped
		// closing for ContactStallFrames frames means the solver has done what it will at this target.
		StalledFrames = (PreviousGapDistance - ContactGapDistance < Tolerance) ? StalledFrames + 1 : 0;
		PreviousGapDistance = ContactGapDistance;

		const bool bArrived = ContactGapDistance <= Tolerance;
		const bool bStalled = StalledFrames >= ContactStallFrames;
		if (bArrived || bStalled)
		{
#if !UE_BUILD_SHIPPING
			if (bLogContactSteps && !bArrived)
			{
				UE_LOG(LogTemp, Warning, TEXT("[BIND] %s stalled %d frame(s) at gap %.2fcm - stepping on"),
					*GetNameSafe(GetOwner()), StalledFrames, ContactGapDistance);
			}
#endif
			StepTarget(PointWorld);
			StalledFrames = 0;
			PreviousGapDistance = TNumericLimits<float>::Max();
		}
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
	UpdatePoseFreeze();

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
	SetComponentTickEnabled(bUseBladeGuard || ContactCount > 0 || ContactBlendAlpha > 0.f);
}

void UHexenCombatComponent::UpdateMontagePause()
{
	// The guard stops the swing on its own schedule - see BladeGuardPauseSeconds.
	if (bUseBladeGuard)
	{
		return;
	}

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

	PauseCurrentMontage();
}

void UHexenCombatComponent::PauseCurrentMontage()
{
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

void UHexenCombatComponent::UpdatePoseFreeze()
{
	if (ContactCount == 0 || !bFreezePoseOnContact || bUseBladeGuard)
	{
#if !UE_BUILD_SHIPPING
		if (bLogContactSteps && bContactPoseFrozen)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s RELEASED pose freeze"), *GetNameSafe(GetOwner()));
		}
#endif
		bContactPoseFrozen = false;
		return;
	}

	// Already holding one. A second volume joining must not retake the snapshot - by then the arm has been
	// pulled by the IK, and the held pose would jump to wherever that left it.
	if (bContactPoseFrozen)
	{
		return;
	}

	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	// The pose as last evaluated, taken on every machine at the moment it learns of the contact: the server
	// from the overlap, a client from the replicated contact state. A client therefore freezes a little
	// later than the server, and whatever the pose did in between is the difference that remains.
	AnimInstance->SavePoseSnapshot(ContactPoseSnapshotName);
	bContactPoseFrozen = true;

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BIND] %s FROZE pose as '%s'"), *GetNameSafe(GetOwner()), *ContactPoseSnapshotName.ToString());
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

UHexenCombatComponent* UHexenCombatComponent::FindForActor(const AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	FScopeLock Lock(&GCombatRegistryLock);
	const TWeakObjectPtr<UHexenCombatComponent>* Found = GCombatRegistry.Find(Actor);
	return Found ? Found->Get() : nullptr;
}

bool UHexenCombatComponent::GetBladeGuardInput(FHexenBladeGuardInput& Out) const
{
	FScopeLock Lock(&BladeGuardLock);
	if (!bUseBladeGuard || !bBladeGuardInputValid)
	{
		return false;
	}
	Out = BladeGuardInput;
	return true;
}

void UHexenCombatComponent::SetBladeGuardResult(const FHexenBladeGuardResult& In)
{
	FScopeLock Lock(&BladeGuardLock);
	BladeGuardResult = In;
}

FVector UHexenCombatComponent::GetBladeGuardCorrectionWorld() const
{
	FScopeLock Lock(&BladeGuardLock);
	return BladeGuardResult.CorrectionWorld;
}

bool UHexenCombatComponent::GetHandTransformWorld(FTransform& Out) const
{
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	if (!Mesh)
	{
		return false;
	}
	Out = Mesh->GetSocketTransform(HandBoneName, RTS_World);
	return true;
}

void UHexenCombatComponent::UpdateBladeGuard()
{
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UWorld* World = GetWorld();

	// What the rig did this frame. Read first, because it decides the side the rig is given next.
	FHexenBladeGuardResult Result;
	{
		FScopeLock Lock(&BladeGuardLock);
		Result = BladeGuardResult;
	}

	TArray<UHexenCollisionComponent*> Blades;
	UHexenCollisionComponent::GetBladeVolumes(World, Blades);

	// This fighter's blade, then the nearest blade in range that belongs to someone else.
	UHexenCollisionComponent* MyBlade = nullptr;
	for (UHexenCollisionComponent* Blade : Blades)
	{
		if (Blade->GetFighter() == this)
		{
			MyBlade = Blade;
			break;
		}
	}

	FHexenBladeGuardInput In;
	bool bValid = false;

	FVector MyStart, MyEnd, TheirStart, TheirEnd;
	float MyRadius = 0.f, TheirRadius = 0.f;
	if (Mesh && MyBlade && MyBlade->GetShapeAxisWorld(MyStart, MyEnd, MyRadius))
	{
		const FVector MyMiddle = (MyStart + MyEnd) * 0.5f;
		UHexenCollisionComponent* TheirBlade = nullptr;
		float BestDistSq = FMath::Square(BladeGuardRange);
		for (UHexenCollisionComponent* Blade : Blades)
		{
			const UHexenCombatComponent* TheirFighter = Blade->GetFighter();
			FVector Start, End;
			float Radius = 0.f;
			if (!TheirFighter || TheirFighter == this || !Blade->GetShapeAxisWorld(Start, End, Radius))
			{
				continue;
			}
			const float DistSq = FVector::DistSquared(MyMiddle, (Start + End) * 0.5f);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				TheirBlade = Blade;
				TheirStart = Start;
				TheirEnd = End;
				TheirRadius = Radius;
			}
		}

		if (TheirBlade)
		{
			// Fixed in the hand: the grip does not change, so the rig can rebuild the capsule from whatever pose
			// the animation gives the hand.
			const FTransform HandTM = Mesh->GetSocketTransform(HandBoneName, RTS_World);
			In.MyAxisStartInHand = HandTM.InverseTransformPosition(MyStart);
			In.MyAxisEndInHand = HandTM.InverseTransformPosition(MyEnd);
			In.MyRadius = MyRadius;

			// Where the partner's animation had its blade: what is drawn, minus what its own guard added on top.
			// Measuring against the drawn blade instead would have each side answer the other's correction a
			// frame late, and two halves chasing each other like that never settle on the contact.
			const UHexenCombatComponent* TheirFighter = TheirBlade->GetFighter();
			const FVector TheirCorrection = TheirFighter ? TheirFighter->GetBladeGuardCorrectionWorld() : FVector::ZeroVector;
			In.PartnerAxisStartWorld = TheirStart - TheirCorrection;
			In.PartnerAxisEndWorld = TheirEnd - TheirCorrection;
			In.PartnerRadius = TheirRadius;

			// One decision for the pair - which way apart, and whether the blades went through - taken by
			// whichever fighter ticks first this frame and read by the other. Two fighters deciding for
			// themselves could, and did, end up pushing the same way and dragging each other along.
			const FHexenBladePair& Pair = UpdateBladePair(MyBlade, TheirBlade, BladeGuardMaxPushBack, BladeGuardSkin, BladeGuardSweepSteps);
			In.PairAxisWorld = (Pair.First.Get() == MyBlade) ? Pair.Side : -Pair.Side;
			In.bPairCrossed = Pair.bCrossed;
			In.MaxPushBack = BladeGuardMaxPushBack;

			// The blades met somewhere inside the frame just gone. If a swing carried this blade there, wind it
			// back to the moment they met and stop it: from the next frame the animated blade is at the other one
			// rather than through it.
			if (Pair.TouchFrame == GFrameCounter)
			{
				RewindSwingToTouch(Pair.TouchAlpha);
			}

			In.Share = BladeGuardShare;
			In.Skin = BladeGuardSkin;
			bValid = true;
		}
	}

	{
		FScopeLock Lock(&BladeGuardLock);
		BladeGuardInput = In;
		bBladeGuardInputValid = bValid;
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		const TCHAR* Side = (GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli");
		if (Result.bPenetrating || bGuardWasPushing)
		{
			// One line per frame while the guard is pushing, and one when it lets go. A depth that keeps
			// growing while the correction keeps pace is a blade being held off; a correction of zero with a
			// depth above zero would be the guard not reaching the rig at all.
			UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu %s%s | depth %.1fcm | correction %.1fcm | normal %s"),
				*GetNameSafe(GetOwner()), Side, (unsigned long long)GFrameCounter,
				Result.bPenetrating ? (bGuardWasPushing ? TEXT("push") : TEXT("START")) : TEXT("END"),
				Result.bCrossed ? TEXT(" CROSSED") : TEXT(""),
				Result.Depth, Result.CorrectionWorld.Size(), *Result.NormalWorld.ToCompactString());
		}
	}

	if (bDrawContactPoints && Result.bPenetrating && World)
	{
		DrawDebugSphere(World, Result.ContactPointWorld, 2.f, 8, FColor::Yellow, false, -1.f);
		DrawDebugDirectionalArrow(World, Result.ContactPointWorld, Result.ContactPointWorld + Result.CorrectionWorld * 3.f, 5.f, FColor::Red, false, -1.f);
	}
#endif

	// Stop the swing at the blade the moment the guard first has to push, and let it go after a fixed time.
	// On every machine, like the guard itself - each stops the swing it is drawing.
	if (bPauseMontageOnContact && BladeGuardPauseSeconds > 0.f && World)
	{
		const double Now = World->GetTimeSeconds();
		if (Result.bPenetrating && !bGuardWasPushing && !PausedMontage.IsValid())
		{
			PauseCurrentMontage();
			GuardPauseStartTime = Now;
		}
		else if (PausedMontage.IsValid() && Now - GuardPauseStartTime >= BladeGuardPauseSeconds)
		{
			ResumePausedMontage();
		}
	}

	bGuardWasPushing = Result.bPenetrating;

	// Where the swing is now, for the next rewind to wind back towards.
	const USkeletalMeshComponent* SwingMesh = GetOwnerMesh();
	const UAnimInstance* SwingAnim = SwingMesh ? SwingMesh->GetAnimInstance() : nullptr;
	UAnimMontage* SwingMontage = SwingAnim ? SwingAnim->GetCurrentActiveMontage() : nullptr;
	LastSwingMontage = SwingMontage;
	LastSwingPosition = SwingMontage ? SwingAnim->Montage_GetPosition(SwingMontage) : 0.f;
}

void UHexenCombatComponent::RewindSwingToTouch(float Alpha)
{
	UWorld* World = GetWorld();
	if (!bPauseMontageOnContact || PausedMontage.IsValid() || !World)
	{
		return;
	}

	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UAnimMontage* Active = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr;

#if !UE_BUILD_SHIPPING
	const TCHAR* Side = (GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli");
#endif

	if (!Active)
	{
#if !UE_BUILD_SHIPPING
		if (bLogContactSteps)
		{
			UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu SWEEP touch at %.0f%% of the frame | no montage to rewind"),
				*GetNameSafe(GetOwner()), Side, (unsigned long long)GFrameCounter, Alpha * 100.f);
		}
#endif
		return;
	}

	// From where the montage stood last tick to where it is now, the fraction of the way the blades had come
	// when they met. A montage that was not playing last tick, or that has looped back since, has no
	// "before" to wind back towards, and is only stopped where it is.
	const float Now = AnimInstance->Montage_GetPosition(Active);
	const float Before = (LastSwingMontage.Get() == Active && LastSwingPosition <= Now) ? LastSwingPosition : Now;
	const float Target = FMath::Lerp(Before, Now, Alpha);

	AnimInstance->Montage_SetPosition(Active, Target);
	AnimInstance->Montage_Pause(Active);
	PausedMontage = Active;
	GuardPauseStartTime = World->GetTimeSeconds();

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu SWEEP touch at %.0f%% of the frame | %s rewound %.3f -> %.3f (was %.3f last frame) and paused"),
			*GetNameSafe(GetOwner()), Side, (unsigned long long)GFrameCounter, Alpha * 100.f,
			*GetNameSafe(Active), Now, Target, Before);
	}
#endif
}

USkeletalMeshComponent* UHexenCombatComponent::GetOwnerMesh() const
{
	// Not cached: a character's mesh outlives this component, but reading it is a pointer fetch, and
	// caching would only add a way to hold a stale one.
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	return OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
}
