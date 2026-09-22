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
#include "Misc/PackageName.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "HAL/IConsoleManager.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/SkinnedAsset.h"

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
	struct FHexenCollisionPair
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

		/**
		 * Side and bCrossed as they stood before this frame's decision. A swing wound back this frame returns to a
		 * pose from before the one they were decided from - a pose that can have been past the other blade - so
		 * the fighter that winds it back puts these back too.
		 */
		FVector SideBefore = FVector::ZeroVector;
		bool bCrossedBefore = false;

		/**
		 * Whether the two volumes overlapped at the last decision, so the decision can tell an overlap that has just
		 * ended - the end of the contact - from blades that have not come to overlap yet.
		 */
		bool bWasOverlapping = false;
	};

	TMap<TPair<const UHexenCollisionComponent*, const UHexenCollisionComponent*>, FHexenCollisionPair> GCollisionPairs;

	/** A blade where its animation had it - as drawn, minus what its own guard added on top - with the hand that holds it. */
	struct FAnimatedVolume
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

	bool GetAnimatedVolume(UHexenCollisionComponent* Volume, FAnimatedVolume& Out)
	{
		UHexenCombatComponent* Fighter = Volume ? Volume->GetFighter() : nullptr;
		FTransform DrawnHand;
		if (!Fighter || !Fighter->GetHandTransformWorld(DrawnHand) || !Volume->GetShapeAxisWorld(Out.Start, Out.End, Out.Radius))
		{
			return false;
		}

		// Where the fighter's own guard found the animation had the blade, once it has run: exact, whatever happens to
		// the pose between the rig and the screen.
		FVector AnimatedStart, AnimatedEnd;
		FTransform AnimatedHand;
		if (Fighter->GetHexenCollisionGuardAnimatedPose(AnimatedStart, AnimatedEnd, AnimatedHand))
		{
			Out.Start = AnimatedStart;
			Out.End = AnimatedEnd;
			Out.Hand = AnimatedHand;
			Out.LocalStart = AnimatedHand.InverseTransformPosition(AnimatedStart);
			Out.LocalEnd = AnimatedHand.InverseTransformPosition(AnimatedEnd);
			return true;
		}

		// Until then, the drawn pose minus what the guard added. The guard moves the hand by a translation, so taking it
		// off again is a translation too - true of the rig's output, but the drawn pose was 10 cm and more away from that
		// output, which is why this is only the fallback.
		Out.LocalStart = DrawnHand.InverseTransformPosition(Out.Start);
		Out.LocalEnd = DrawnHand.InverseTransformPosition(Out.End);
		const FVector Correction = Fighter->GetHexenCollisionGuardCorrectionWorld();
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

	/** Whether two volumes overlap on this machine, as their overlap events last said - either one's record will do. */
	bool AreOverlapping(const UHexenCollisionComponent* A, const UHexenCollisionComponent* B)
	{
		return A->IsOverlappingVolume(B) || B->IsOverlappingVolume(A);
	}

	/**
	 * Whether two volumes overlap in the pose their rigs put out: each blade where its animation had it, moved by what its
	 * own guard added. While the guards hold two blades they leave them a skin inside touching, so this lasts exactly as
	 * long as the holding does. The drawn capsules stand in until both rigs have published a pose.
	 */
	bool AreOverlappingInRigPose(UHexenCollisionComponent* A, UHexenCollisionComponent* B)
	{
		const UHexenCombatComponent* FighterA = A->GetFighter();
		const UHexenCombatComponent* FighterB = B->GetFighter();
		FVector StartA, EndA, StartB, EndB, DrawnStart, DrawnEnd;
		FTransform HandA, HandB;
		float RadiusA = 0.f;
		float RadiusB = 0.f;
		if (!FighterA || !FighterB
			|| !FighterA->GetHexenCollisionGuardAnimatedPose(StartA, EndA, HandA)
			|| !FighterB->GetHexenCollisionGuardAnimatedPose(StartB, EndB, HandB)
			|| !A->GetShapeAxisWorld(DrawnStart, DrawnEnd, RadiusA)
			|| !B->GetShapeAxisWorld(DrawnStart, DrawnEnd, RadiusB))
		{
			return AreOverlapping(A, B);
		}

		// The guard moves the hand by a translation and keeps its turn, so the blade it puts out is the animated one moved by it.
		const FVector CorrectionA = FighterA->GetHexenCollisionGuardCorrectionWorld();
		const FVector CorrectionB = FighterB->GetHexenCollisionGuardCorrectionWorld();
		FVector OnA, OnB;
		FMath::SegmentDistToSegmentSafe(StartA + CorrectionA, EndA + CorrectionA, StartB + CorrectionB, EndB + CorrectionB, OnA, OnB);
		return FVector::Dist(OnA, OnB) < RadiusA + RadiusB;
	}

	/**
	 * How long a montage's blend-in still takes from Weight to full weight: its curve run backwards to the point where it
	 * passes Weight, and the rest of the blend time from there.
	 */
	float RemainingBlendInTime(const FAlphaBlendArgs& BlendIn, float Weight)
	{
		if (BlendIn.BlendTime <= 0.f || Weight >= 1.f)
		{
			return 0.f;
		}

		// The curves only rise, so halving finds the point; twenty halvings put it well inside a millisecond of the blend.
		float Low = 0.f;
		float High = 1.f;
		for (int32 Halving = 0; Halving < 20; ++Halving)
		{
			const float Middle = 0.5f * (Low + High);
			if (FAlphaBlend::AlphaToBlendOption(Middle, BlendIn.BlendOption, BlendIn.CustomCurve) < Weight)
			{
				Low = Middle;
			}
			else
			{
				High = Middle;
			}
		}
		return BlendIn.BlendTime * (1.f - 0.5f * (Low + High));
	}

	/**
	 * Decides, once per frame, which way apart the pair is and whether its blades have been carried through
	 * one another - from where both animations had the blades, so the two fighters' corrections do not feed
	 * back into it.
	 */
	FHexenCollisionPair& UpdateCollisionPair(UHexenCollisionComponent* A, UHexenCollisionComponent* B, float Skin, int32 SweepSteps, bool bRigPoseOverlap, bool bAuthority, bool bLog)
	{
		for (auto It = GCollisionPairs.CreateIterator(); It; ++It)
		{
			if (!It.Value().First.IsValid() || !It.Value().Second.IsValid())
			{
				It.RemoveCurrent();
			}
		}

		UHexenCollisionComponent* First = A < B ? A : B;
		UHexenCollisionComponent* Second = A < B ? B : A;
		FHexenCollisionPair& Pair = GCollisionPairs.FindOrAdd(MakeTuple(static_cast<const UHexenCollisionComponent*>(First), static_cast<const UHexenCollisionComponent*>(Second)));

		// The other fighter of the pair has already decided this frame.
		if (Pair.Frame == GFrameCounter)
		{
			return Pair;
		}
		Pair.First = First;
		Pair.Second = Second;
		Pair.Frame = GFrameCounter;
		Pair.SideBefore = Pair.Side;
		Pair.bCrossedBefore = Pair.bCrossed;

		// The contact ends where the overlap does - see the crossed branch below. On the drawn volumes, or in the pose the
		// rigs put out - see UHexenCombatComponent::bHexenCollisionGuardRigPoseOverlap.
		const bool bOverlapping = bRigPoseOverlap ? AreOverlappingInRigPose(First, Second) : AreOverlapping(First, Second);
		const bool bWasOverlapping = Pair.bWasOverlapping;
		const bool bOverlapEnded = bWasOverlapping && !bOverlapping;
		Pair.bWasOverlapping = bOverlapping;

		FAnimatedVolume FirstVolume, SecondVolume;
		if (!GetAnimatedVolume(First, FirstVolume) || !GetAnimatedVolume(Second, SecondVolume))
		{
			return Pair;
		}
		const FVector& FirstStart = FirstVolume.Start;
		const FVector& FirstEnd = FirstVolume.End;
		const FVector& SecondStart = SecondVolume.Start;
		const FVector& SecondEnd = SecondVolume.End;
		const float FirstRadius = FirstVolume.Radius;
		const float SecondRadius = SecondVolume.Radius;

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
		bool bWereTouching = false;
		const float Reach = FirstRadius + SecondRadius - Skin;
		if (Pair.bHasPrevious)
		{
			auto VolumeAt = [](const FTransform& Hand, const FAnimatedVolume& Volume, FVector& OutStart, FVector& OutEnd)
			{
				OutStart = Hand.TransformPosition(Volume.LocalStart);
				OutEnd = Hand.TransformPosition(Volume.LocalEnd);
			};

			FVector WasFirstStart, WasFirstEnd, WasSecondStart, WasSecondEnd, WasOnFirst, WasOnSecond;
			VolumeAt(Pair.PrevFirstHand, FirstVolume, WasFirstStart, WasFirstEnd);
			VolumeAt(Pair.PrevSecondHand, SecondVolume, WasSecondStart, WasSecondEnd);
			FMath::SegmentDistToSegmentSafe(WasFirstStart, WasFirstEnd, WasSecondStart, WasSecondEnd, WasOnFirst, WasOnSecond);
			bWereTouching = FVector::Dist(WasOnFirst, WasOnSecond) < Reach;
			if (!bWereTouching)
			{
				const int32 Steps = FMath::Max(1, SweepSteps);
				for (int32 Step = 1; Step <= Steps; ++Step)
				{
					const float Alpha = static_cast<float>(Step) / Steps;
					FTransform FirstHand, SecondHand;
					FirstHand.Blend(Pair.PrevFirstHand, FirstVolume.Hand, Alpha);
					SecondHand.Blend(Pair.PrevSecondHand, SecondVolume.Hand, Alpha);

					FVector StepFirstStart, StepFirstEnd, StepSecondStart, StepSecondEnd, StepOnFirst, StepOnSecond;
					VolumeAt(FirstHand, FirstVolume, StepFirstStart, StepFirstEnd);
					VolumeAt(SecondHand, SecondVolume, StepSecondStart, StepSecondEnd);
					FMath::SegmentDistToSegmentSafe(StepFirstStart, StepFirstEnd, StepSecondStart, StepSecondEnd, StepOnFirst, StepOnSecond);
					if (FVector::Dist(StepOnFirst, StepOnSecond) < Reach)
					{
						// The first step at which they touch. Rewinding to it leaves the blades just inside each
						// other - in contact, for the guard to hold from there. The last step still apart, which this
						// used to be, left them up to a sub-step short of each other, with nothing for the guard to hold.
						bSweptTouch = true;
						Pair.TouchFrame = GFrameCounter;
						Pair.TouchAlpha = static_cast<float>(Step) / Steps;
						break;
					}
				}
			}
		}
		Pair.PrevFirstHand = FirstVolume.Hand;
		Pair.PrevSecondHand = SecondVolume.Hand;
		Pair.bHasPrevious = true;

		if (Pair.Side.IsNearlyZero())
		{
			Pair.Side = Normal;
			Pair.bCrossed = false;
			if (bAuthority)
			{
				First->SetPairCrossed(false);
				Second->SetPairCrossed(false);
			}
			return Pair;
		}

		const bool bFlipped = FVector::DotProduct(Normal, Pair.Side) < 0.f;
		if (!bAuthority)
		{
			// Whether the blades were carried through one another is the server's to say. A client deciding it from
			// its own poses got it wrong far more often than the server did - and a crossing is remembered, so one
			// mistake is not one bad frame but every frame until something clears it.
			//
			// Only the memory comes over the wire. The side itself stays local: away from a crossing it is simply
			// the direction between the two closest points of this frame, which every machine works out for itself,
			// and it is what the two fighters of a pair must agree on - which they do, reading one record.
			Pair.bCrossed = First->IsPairCrossed() || Second->IsPairCrossed();
			if (!Pair.bCrossed)
			{
				Pair.Side = Normal;
			}
		}
		else if (Pair.bCrossed)
		{
			// Stays crossed until the animations bring the blades back to their own sides, or until the contact ends: the
			// volumes, which the guard holds a hair inside touching, stop overlapping. An overlap that has not begun is not
			// an end - a swing carried clean through between two frames is still being brought back.
			if (!bFlipped || bOverlapEnded)
			{
				Pair.bCrossed = false;
				Pair.Side = Normal;
			}
		}
		else if (bFlipped
			&& (bOverlapping || bWasOverlapping || bSweptTouch)
			&& (bSweptTouch || bWereTouching || (IsOnBody(OnFirst, FirstStart, FirstEnd) && IsOnBody(OnSecond, SecondStart, SecondEnd))))
		{
			// Turned past a right angle in one frame, and the animations carried them through: the sweep saw them meet on
			// the way, or they were already touching, or each blade's closest point is on the other's body. Blades in
			// contact cannot change sides without one going through the other - sliding round a tip turns the direction a
			// little each frame, and the side follows it - so a turn past a right angle while touching is a pass-through
			// even at a tip. Before, a closest point at the end of an axis always read as going round the tip, and all six
			// tip flips of the 2026-09-19 run were pushed on through.
			//
			// And the volumes have to have really been in one another, or the sweep to have caught them meeting. A
			// crossing read off poses that never touched can never be left again: it is only cleared by the animations
			// bringing the blades back or by an overlap ending, and an overlap that never began cannot end. In the
			// 2026-09-21 run one such crossing held for 1172 frames while the blades flew half a metre apart, and the
			// guard pushed harder the further they went - depth along the frozen axis grows with the distance.
			Pair.bCrossed = true;
		}
		else
		{
			// Still on the same sides, or legitimately round a tip: the direction follows the blades.
			Pair.Side = Normal;
		}

		if (bAuthority)
		{
			// Both volumes carry the same answer, so either fighter's guard finds it whichever of them ticks first,
			// and a client reads it off the pair it is actually looking at.
			First->SetPairCrossed(Pair.bCrossed);
			Second->SetPairCrossed(Pair.bCrossed);
		}

#if !UE_BUILD_SHIPPING
		// Every change of sides, and how it was read. Near a tip the closest point sits at the end of a blade's axis, and a
		// change of sides there is read as going round the tip - so where along each blade it happened is the point.
		if (bLog && bAuthority && bFlipped && !Pair.bCrossedBefore)
		{
			auto FighterName = [](UHexenCollisionComponent* Volume)
			{
				const UHexenCombatComponent* Fighter = Volume->GetFighter();
				return GetNameSafe(Fighter ? Fighter->GetOwner() : Volume->GetOwner());
			};
			UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s vs %s %s f%llu FLIP taken as %s | first %.0fcm from its hand%s, second %.0fcm from its hand%s | swept %s, were touching %s"),
				*FighterName(First), *FighterName(Second),
				(First->GetOwner() && First->GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli"), (unsigned long long)GFrameCounter,
				Pair.bCrossed ? TEXT("CARRIED THROUGH - held on its side") : TEXT("ROUND A TIP - side follows the blades"),
				FVector::Dist(FirstVolume.Hand.GetLocation(), OnFirst), IsOnBody(OnFirst, FirstStart, FirstEnd) ? TEXT("") : TEXT(" END"),
				FVector::Dist(SecondVolume.Hand.GetLocation(), OnSecond), IsOnBody(OnSecond, SecondStart, SecondEnd) ? TEXT("") : TEXT(" END"),
				bSweptTouch ? TEXT("yes") : TEXT("no"), bWereTouching ? TEXT("yes") : TEXT("no"));
		}
#endif

		return Pair;
	}
}

const TArray<FName>& GetHexenCollisionGuardChainBones()
{
	// Root first, so the log reads down the chain. Everything above clavicle_r is outside the guard's FABRIK, so a
	// difference there cannot be the IK.
	static const TArray<FName> Bones = { FName("root"), FName("pelvis"), FName("spine_01"), FName("spine_02"), FName("spine_03"), FName("spine_04"), FName("spine_05"), FName("clavicle_r"), FName("upperarm_r"), FName("lowerarm_r"), FName("hand_r"), FName("weapon_r") };
	return Bones;
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
	if (bUseHexenCollisionGuard)
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
	DOREPLIFETIME(UHexenCombatComponent, ServerSwingHoldSerial);
	DOREPLIFETIME(UHexenCombatComponent, bServerSwingHeld);
	DOREPLIFETIME(UHexenCombatComponent, ServerSwingHoldPosition);
	DOREPLIFETIME(UHexenCombatComponent, ServerSwingHoldMontage);
	DOREPLIFETIME(UHexenCombatComponent, ServerPose);
	DOREPLIFETIME(UHexenCombatComponent, ServerPoseFrame);
	DOREPLIFETIME(UHexenCombatComponent, ServerGuardResult);
}

void FHexenNetBoneTransform::SetRotation(const FQuat& In)
{
	FQuat Q = In.GetNormalized();
	// q and -q are the same rotation, so W can always be made non-negative and left off the wire.
	if (Q.W < 0.0)
	{
		Q = FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W);
	}
	RotationX = static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Q.X, -1.0, 1.0) * 32767.0));
	RotationY = static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Q.Y, -1.0, 1.0) * 32767.0));
	RotationZ = static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Q.Z, -1.0, 1.0) * 32767.0));
}

FQuat FHexenNetBoneTransform::GetRotation() const
{
	const double X = RotationX / 32767.0;
	const double Y = RotationY / 32767.0;
	const double Z = RotationZ / 32767.0;
	const double W = FMath::Sqrt(FMath::Max(0.0, 1.0 - (X * X + Y * Y + Z * Z)));
	return FQuat(X, Y, Z, W).GetNormalized();
}

void UHexenCombatComponent::PublishServerState()
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	// The pose as the server draws it - after its own guard and FABRIK - so a client drawing it shows the contact the
	// server resolved rather than resolving its own. Read in the mesh's own space: the two machines agree on where the
	// mesh is to the centimetre, it is the pose on it that differs.
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	if (bReplicateServerPose && Mesh)
	{
#if !UE_BUILD_SHIPPING
		// Kept to say whether this tick's pose differs from the last one as it goes over the wire: a pose that has not
		// changed is not sent, and a client holding the same pose for several frames is then right to.
		const TArray<FHexenNetBoneTransform> PreviousPose = ServerPose;
#endif
		ServerPose.SetNum(ServerPoseBones.Num());
		for (int32 Index = 0; Index < ServerPoseBones.Num(); ++Index)
		{
			const FTransform Bone = Mesh->GetSocketTransform(ServerPoseBones[Index], RTS_Component);
			ServerPose[Index].Location = Bone.GetLocation();
			ServerPose[Index].SetRotation(Bone.GetRotation());
		}
		ServerPoseFrame = static_cast<int32>(GFrameCounter);

#if !UE_BUILD_SHIPPING
		if (bLogContactSteps && ServerPose.Num() > 0)
		{
			bool bChanged = PreviousPose.Num() != ServerPose.Num();
			for (int32 Index = 0; !bChanged && Index < ServerPose.Num(); ++Index)
			{
				const FHexenNetBoneTransform& A = ServerPose[Index];
				const FHexenNetBoneTransform& B = PreviousPose[Index];
				bChanged = !FVector(A.Location).Equals(FVector(B.Location), 0.05) || A.RotationX != B.RotationX
					|| A.RotationY != B.RotationY || A.RotationZ != B.RotationZ;
			}
			const FVector Hand = ServerPose.Last().Location;
			UE_LOG(LogTemp, Warning, TEXT("[POSEPUB] %s srv f%llu | %s | %s %.1f,%.1f,%.1f"),
				*GetNameSafe(Owner), (unsigned long long)GFrameCounter, bChanged ? TEXT("changed") : TEXT("SAME"),
				*ServerPoseBones.Last().ToString(), Hand.X, Hand.Y, Hand.Z);
		}
#endif
	}
	else if (ServerPose.Num() > 0)
	{
		// Emptied rather than left standing: an empty pose is how a client learns to go back to its own.
		ServerPose.Reset();
	}

	FHexenCollisionGuardResult Result;
	{
		FScopeLock Lock(&HexenCollisionGuardLock);
		Result = HexenCollisionGuardResult;
	}
	ServerGuardResult.bPenetrating = Result.bPenetrating;
	ServerGuardResult.NormalWorld = Result.NormalWorld;
	ServerGuardResult.ContactPointWorld = Result.ContactPointWorld;

#if !UE_BUILD_SHIPPING
	// What each client connection is actually carrying. On 2026-09-22 a client's pose stood still for five frames and
	// more half of the time while the server's blade moved 33 cm on average in the same stretch - updates were being
	// skipped, not queued. A connection over its MaxClientRate is not ready to send, and its actors simply wait.
	if (bLogContactSteps && (GFrameCounter % 10) == 0)
	{
		const UWorld* World = GetWorld();
		if (const UNetDriver* Driver = World ? World->GetNetDriver() : nullptr)
		{
			for (const UNetConnection* Connection : Driver->ClientConnections)
			{
				if (!Connection)
				{
					continue;
				}
				// The engine's frame-rate cap is also the net driver's ServerTickTime, and an actor's next send is put off by
				// a random share of it - net.DisableRandomNetUpdateDelay turns that off.
				static const IConsoleVariable* DisableRandomDelay = IConsoleManager::Get().FindConsoleVariable(TEXT("net.DisableRandomNetUpdateDelay"));
				UE_LOG(LogTemp, Warning, TEXT("[NET] %s srv f%llu | %s | out %d B/s, in %d B/s | net speed %d B/s | queued %d bits | %s | engine max tick rate %.1f Hz, random update delay %s"),
					*GetNameSafe(Owner), (unsigned long long)GFrameCounter, *Connection->GetName(),
					Connection->OutBytesPerSecond, Connection->InBytesPerSecond, Connection->CurrentNetSpeed,
					Connection->QueuedBits, Connection->IsNetReady() ? TEXT("ready") : TEXT("SATURATED"),
					GEngine ? GEngine->GetMaxTickRate(World->GetDeltaSeconds(), false) : -1.f,
					(DisableRandomDelay && DisableRandomDelay->GetInt() != 0) ? TEXT("OFF") : TEXT("on"));
			}
		}
	}
#endif
}

void UHexenCombatComponent::OnRep_ServerPose()
{
	TArray<FTransform> Pose;
	Pose.Reserve(ServerPose.Num());
	for (const FHexenNetBoneTransform& Bone : ServerPose)
	{
		Pose.Add(FTransform(Bone.GetRotation(), Bone.Location));
	}

	// Asked here, on the game thread, rather than by the rig: whether a pawn is locally controlled is the controller's to
	// say, and the rig runs on a worker thread.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const bool bLocallyControlled = Pawn && Pawn->IsLocallyControlled();

	{
		FScopeLock Lock(&HexenCollisionGuardLock);
		ServerPoseForRig = MoveTemp(Pose);
		bLocallyControlledForRig = bLocallyControlled;
	}

#if !UE_BUILD_SHIPPING
	// How old the pose is when it gets here, and how long since the last one. Steady age means the pose is simply late;
	// age that keeps climbing means the connection is queueing it faster than it can send - see MaxClientRate.
	if (bLogContactSteps && ServerPose.Num() > 0)
	{
		const UWorld* World = GetWorld();
		const uint64 Now = GFrameCounter;
		const AActor* Owner = GetOwner();
		const FVector ReceivedHand = ServerPose.Last().Location;
		UE_LOG(LogTemp, Warning, TEXT("[POSEREP] %s cli/%s %s f%llu | taken on f%d, %lld frames old | %lld frames since the last one | net update %.0f Hz, min %.0f Hz | last bone %.1f,%.1f,%.1f"),
			*GetNameSafe(Owner), World ? *FPackageName::GetShortName(World->GetOutermost()->GetName()) : TEXT("noworld"),
			bLocallyControlled ? TEXT("local") : TEXT("remote"),
			(unsigned long long)Now, ServerPoseFrame, static_cast<long long>(Now) - ServerPoseFrame,
			LastServerPoseArrivalFrame > 0 ? static_cast<long long>(Now - LastServerPoseArrivalFrame) : -1LL,
			Owner ? Owner->GetNetUpdateFrequency() : -1.f, Owner ? Owner->GetMinNetUpdateFrequency() : -1.f,
			ReceivedHand.X, ReceivedHand.Y, ReceivedHand.Z);
	}
#endif
	LastServerPoseArrivalFrame = GFrameCounter;
}

bool UHexenCombatComponent::GetServerPoseForRig(TArray<FName>& OutBones, TArray<FTransform>& OutPose) const
{
	if (!bReplicateServerPose)
	{
		return false;
	}

	FScopeLock Lock(&HexenCollisionGuardLock);
	// Filled only by the notify, which never runs on the server - so the server draws its own pose, as it must.
	if (ServerPoseForRig.Num() == 0 || ServerPoseForRig.Num() != ServerPoseBones.Num())
	{
		return false;
	}
	OutBones = ServerPoseBones;
	OutPose = ServerPoseForRig;
	return true;
}

void UHexenCombatComponent::PublishSwingHold(bool bHeld)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	// The montage the hold is about: the one just held, or the one still playing when it is let go.
	float Position = ServerSwingHoldPosition;
	UAnimMontage* Held = nullptr;
	if (const USkeletalMeshComponent* Mesh = GetOwnerMesh())
	{
		if (UAnimInstance* AnimInstance = Mesh->GetAnimInstance())
		{
			Held = PausedMontage.Get();
			if (!Held)
			{
				Held = AnimInstance->GetCurrentActiveMontage();
			}
			if (Held)
			{
				Position = AnimInstance->Montage_GetPosition(Held);
			}
		}
	}

	if (bHeld && !Held)
	{
		// The guard stopped a fighter who was not swinging - the blade is simply in the way. There is nothing to hold
		// and nothing to tell anyone: a hold published with no montage behind it would reach a client that IS swinging
		// and wind that swing somewhere it was never held.
		return;
	}

	bServerSwingHeld = bHeld;
	ServerSwingHoldMontage = Held;
	ServerSwingHoldPosition = Position;
	++ServerSwingHoldSerial;
	Owner->ForceNetUpdate();

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AUTH] %s srv f%llu | swing %s at %.3f, serial %u - told to the clients"),
			*GetNameSafe(Owner), (unsigned long long)GFrameCounter,
			bHeld ? TEXT("HELD") : TEXT("let go"), Position, ServerSwingHoldSerial);
	}
#endif
}

void UHexenCombatComponent::OnRep_ServerSwingHold()
{
	ApplyServerSwingHold();
}

void UHexenCombatComponent::ApplyServerSwingHold()
{
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	UAnimMontage* Montage = PausedMontage.Get();
	if (!Montage)
	{
		Montage = AnimInstance->GetCurrentActiveMontage();
	}
	if (!Montage || Montage != ServerSwingHoldMontage)
	{
		// The swing itself has not reached this machine yet, or a different one is playing - the multicast that plays
		// it and this property travel apart. A hold with nothing to hold is left standing, and the guard's tick tries
		// it again next frame.
		return;
	}

	// Wound to the server's time either way. Held, that is where the blade met the other one; let go, it is where
	// the server had the swing when it set it going again, and the two copies carry on from the same place.
	AnimInstance->Montage_SetPosition(Montage, ServerSwingHoldPosition);

	if (bServerSwingHeld)
	{
		const FAnimMontageInstance* Instance = AnimInstance->GetActiveInstanceForMontage(Montage);
		HoldMontage(AnimInstance, Montage, Instance ? Instance->GetWeight() : 1.f);
	}
	else
	{
		ResumePausedMontage();
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AUTH] %s cli f%llu | swing %s at %.3f, serial %u - taken from the server"),
			*GetNameSafe(GetOwner()), (unsigned long long)GFrameCounter,
			bServerSwingHeld ? TEXT("HELD") : TEXT("let go"), ServerSwingHoldPosition, ServerSwingHoldSerial);
	}
#endif
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

	if (GetOwner() && GetOwner()->HasAuthority() && !bUseHexenCollisionGuard)
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

	if (bUseHexenCollisionGuard)
	{
		// The rig decides from the animated pose every frame; there is no target to chase and no blend to ease.
		ContactBlendAlpha = 1.f;
		UpdateHexenCollisionGuard();
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
	SetComponentTickEnabled(bUseHexenCollisionGuard || ContactCount > 0 || ContactBlendAlpha > 0.f);
}

void UHexenCombatComponent::UpdateMontagePause()
{
	// The guard stops the swing on its own schedule - see HexenCollisionGuardPauseSeconds.
	if (bUseHexenCollisionGuard)
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
		// Held at the weight it has: this pause does not wind anything back.
		const FAnimMontageInstance* Instance = AnimInstance->GetActiveInstanceForMontage(Active);
		HoldMontage(AnimInstance, Active, Instance ? Instance->GetWeight() : 1.f);
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		if (Active)
		{
			const FAnimMontageInstance* Instance = AnimInstance->GetActiveInstanceForMontage(Active);
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s PAUSED montage %s at %.3f, weight %.2f"), *GetNameSafe(GetOwner()), *GetNameSafe(Active),
				AnimInstance->Montage_GetPosition(Active), Instance ? Instance->GetWeight() : -1.f);
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

void UHexenCombatComponent::HoldMontage(UAnimInstance* AnimInstance, UAnimMontage* Montage, float Weight)
{
	AnimInstance->Montage_Pause(Montage);
	PausedMontage = Montage;
	PausedMontageInstanceID = INDEX_NONE;
	bRetryingHeldSwing = false;

	// Montage_Pause stops the montage's time and nothing else. FAnimMontageInstance::Pause only clears
	// bPlaying, while UpdateWeight carries every instance's blend on towards full weight, paused or not. A
	// swing stopped inside its blend-in therefore kept moving for as long as the blend had left - in the
	// 2026-09-16 runs 15-45 cm over 0.15-0.22 s, into the other blade or back out of it, since every contact
	// fell inside the 0.25 s blend-in of a 0.51 s strike.
	//
	// So the weight is held too. A blend whose desired weight is the weight it stands at has nothing left to
	// do: SetDesiredWeight makes Weight both ends of the blend, and SetWeight(1) - which sets the blend's alpha,
	// not the weight - puts it at that end now rather than on the next update. The resume starts the blend-in again
	// from the held weight - see ResumePausedMontage. Kept above zero: a desired weight of zero is how the engine
	// marks a montage as stopped.
	// Side effect: to the engine the blend-in is complete while held, so a "blended in" event can fire here,
	// and again when the resumed blend reaches full weight.
	if (FAnimMontageInstance* Instance = AnimInstance->GetActiveInstanceForMontage(Montage))
	{
		PausedMontageInstanceID = Instance->GetInstanceID();
		Instance->SetDesiredWeight(FMath::Max(Weight, UE_KINDA_SMALL_NUMBER));
		Instance->SetWeight(1.f);
	}
}

void UHexenCombatComponent::UpdatePoseFreeze()
{
	if (ContactCount == 0 || !bFreezePoseOnContact || bUseHexenCollisionGuard)
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
	const int32 HeldInstanceID = PausedMontageInstanceID;
	PausedMontageInstanceID = INDEX_NONE;

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
		// The blend-in HoldMontage stopped goes on from the held weight to full - on the held instance only. The same
		// swing played again meanwhile is a new instance, with a blend of its own.
		//
		// Over the time the montage's own blend-in would still take from the held weight, not over the time the blend had
		// left when it was stopped. A rewind lowers the weight and gives back no time, so the resumed blend used to make up
		// the difference at once: 0.50 to 0.89 in one frame in f624 of the 2026-09-19 run, and the blade through the other
		// one on every release. Play on a playing instance leaves its position alone and blends from the current weight.
		FAnimMontageInstance* Instance = AnimInstance->GetActiveInstanceForMontage(Montage);
		float ResumeBlendTime = -1.f;
		if (Instance && Instance->GetInstanceID() == HeldInstanceID)
		{
			FAlphaBlendArgs ResumeBlend = Montage->GetBlendInArgs();
			ResumeBlend.BlendTime = RemainingBlendInTime(ResumeBlend, Instance->GetWeight());
			ResumeBlendTime = ResumeBlend.BlendTime;
			Instance->Play(Instance->GetPlayRate(), FMontageBlendSettings(ResumeBlend));
		}

		AnimInstance->Montage_Resume(Montage);

#if !UE_BUILD_SHIPPING
		if (bLogContactSteps)
		{
			UE_LOG(LogTemp, Warning, TEXT("[BIND] %s RESUMED montage %s at %.3f, weight %.2f, blending in over %.3fs"), *GetNameSafe(GetOwner()), *GetNameSafe(Montage),
				AnimInstance->Montage_GetPosition(Montage), Instance ? Instance->GetWeight() : -1.f, ResumeBlendTime);
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

bool UHexenCombatComponent::GetHexenCollisionGuardInput(FHexenCollisionGuardInput& Out) const
{
	FScopeLock Lock(&HexenCollisionGuardLock);
	if (!bUseHexenCollisionGuard || !bHexenCollisionGuardInputValid)
	{
		return false;
	}
	Out = HexenCollisionGuardInput;
	return true;
}

void UHexenCombatComponent::SetHexenCollisionGuardResult(const FHexenCollisionGuardResult& In)
{
	FScopeLock Lock(&HexenCollisionGuardLock);
	HexenCollisionGuardResult = In;
}

FVector UHexenCombatComponent::GetHexenCollisionGuardCorrectionWorld() const
{
	FScopeLock Lock(&HexenCollisionGuardLock);
	return HexenCollisionGuardResult.CorrectionWorld;
}

bool UHexenCombatComponent::IsHexenCollisionGuardPushing() const
{
	FScopeLock Lock(&HexenCollisionGuardLock);
	return HexenCollisionGuardResult.bPenetrating;
}

bool UHexenCombatComponent::GetHexenCollisionMovementBlock(FVector& OutOutward, FVector& OutContactPoint) const
{
	if (!bUseHexenCollisionGuard || !bHexenCollisionBlocksMovement)
	{
		return false;
	}

	// A client stops at the contact the server measured, not at one it measured itself - see bUseServerGuardResult.
	// Game thread only, where the net driver writes it, so no lock.
	if (bUseServerGuardResult && GetOwner() && !GetOwner()->HasAuthority())
	{
		if (!ServerGuardResult.bPenetrating)
		{
			return false;
		}
		OutOutward = ServerGuardResult.NormalWorld;
		OutContactPoint = ServerGuardResult.ContactPointWorld;
		return true;
	}

	FScopeLock Lock(&HexenCollisionGuardLock);
	if (!HexenCollisionGuardResult.bPenetrating)
	{
		return false;
	}
	OutOutward = HexenCollisionGuardResult.NormalWorld;
	OutContactPoint = HexenCollisionGuardResult.ContactPointWorld;
	return true;
}

bool UHexenCombatComponent::GetHexenCollisionGuardAnimatedPose(FVector& OutStart, FVector& OutEnd, FTransform& OutHand) const
{
	FScopeLock Lock(&HexenCollisionGuardLock);
	if (!HexenCollisionGuardResult.bHasAnimatedPose)
	{
		return false;
	}
	OutStart = HexenCollisionGuardResult.AnimatedAxisStartWorld;
	OutEnd = HexenCollisionGuardResult.AnimatedAxisEndWorld;
	OutHand = HexenCollisionGuardResult.AnimatedHandWorld;
	return true;
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

void UHexenCombatComponent::UpdateHexenCollisionGuard()
{
	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UWorld* World = GetWorld();

	// The guard itself runs on every machine - it is what moves the blade out of the other one, and each machine
	// draws its own pose. What does not run on every machine is the decision to stop a swing and to let it go
	// again: that one the server takes and the rest are told - see PublishSwingHold.
	const bool bAuthority = GetOwner() && GetOwner()->HasAuthority();

	// A hold that arrived before the swing it is about had nothing to hold. Two bools a tick to catch up with it.
	if (!bAuthority && bServerSwingHeld && !PausedMontage.IsValid())
	{
		ApplyServerSwingHold();
	}

	// What the rig did this frame. Read first, because it decides the side the rig is given next.
	FHexenCollisionGuardResult Result;
	{
		FScopeLock Lock(&HexenCollisionGuardLock);
		Result = HexenCollisionGuardResult;
	}

	TArray<UHexenCollisionComponent*> Volumes;
	UHexenCollisionComponent::GetGuardedVolumes(World, Volumes);

	// This fighter's blade, then the nearest blade in range that belongs to someone else.
	UHexenCollisionComponent* MyVolume = nullptr;
	for (UHexenCollisionComponent* Volume : Volumes)
	{
		if (Volume->GetFighter() == this)
		{
			MyVolume = Volume;
			break;
		}
	}

	FHexenCollisionGuardInput In;
	bool bValid = false;

	FVector MyStart = FVector::ZeroVector, MyEnd = FVector::ZeroVector, TheirStart = FVector::ZeroVector, TheirEnd = FVector::ZeroVector;

	// The volume TheirStart and TheirEnd were read from - for the read-timing log, which needs its owner's mesh.
	UHexenCollisionComponent* NearestPartnerVolume = nullptr;
	float MyRadius = 0.f, TheirRadius = 0.f;
	const bool bHaveMyAxis = Mesh && MyVolume && MyVolume->GetShapeAxisWorld(MyStart, MyEnd, MyRadius);
	if (bHaveMyAxis)
	{
		const FVector MyMiddle = (MyStart + MyEnd) * 0.5f;
		UHexenCollisionComponent* TheirVolume = nullptr;
		float BestDistSq = FMath::Square(HexenCollisionGuardRange);
		for (UHexenCollisionComponent* Volume : Volumes)
		{
			const UHexenCombatComponent* TheirFighter = Volume->GetFighter();
			FVector Start, End;
			float Radius = 0.f;
			if (!TheirFighter || TheirFighter == this || !Volume->GetShapeAxisWorld(Start, End, Radius))
			{
				continue;
			}
			const float DistSq = FVector::DistSquared(MyMiddle, (Start + End) * 0.5f);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				TheirVolume = Volume;
				NearestPartnerVolume = Volume;
				TheirStart = Start;
				TheirEnd = End;
				TheirRadius = Radius;
			}
		}

		if (TheirVolume)
		{
			// Fixed in the hand: the grip does not change, so the rig can rebuild the capsule from whatever pose
			// the animation gives the hand.
			const FTransform HandTM = Mesh->GetSocketTransform(HandBoneName, RTS_World);
			In.MyAxisStartInHand = HandTM.InverseTransformPosition(MyStart);
			In.MyAxisEndInHand = HandTM.InverseTransformPosition(MyEnd);
			In.MyRadius = MyRadius;

			// Where the partner's animation had its blade, not where it is drawn. Measuring against the drawn blade
			// would have each side answer the other's correction a frame late, and two halves chasing each other like
			// that never settle on the contact. Taken from what the partner's own rig published; until it has, the
			// drawn blade minus the partner's correction stands in - which was 10-45 cm off in practice.
			const UHexenCombatComponent* TheirFighter = TheirVolume->GetFighter();
			FTransform TheirAnimatedHand;
			if (!TheirFighter || !TheirFighter->GetHexenCollisionGuardAnimatedPose(In.PartnerAxisStartWorld, In.PartnerAxisEndWorld, TheirAnimatedHand))
			{
				const FVector TheirCorrection = TheirFighter ? TheirFighter->GetHexenCollisionGuardCorrectionWorld() : FVector::ZeroVector;
				In.PartnerAxisStartWorld = TheirStart - TheirCorrection;
				In.PartnerAxisEndWorld = TheirEnd - TheirCorrection;
			}
			In.PartnerRadius = TheirRadius;

			// One decision for the pair - which way apart, and whether the blades went through - taken by
			// whichever fighter ticks first this frame and read by the other. Two fighters deciding for
			// themselves could, and did, end up pushing the same way and dragging each other along.
			FHexenCollisionPair& Pair = UpdateCollisionPair(MyVolume, TheirVolume, HexenCollisionGuardSkin, HexenCollisionGuardSweepSteps, bHexenCollisionGuardRigPoseOverlap, bAuthority, bLogContactSteps);

			// Winding a swing back and holding it there is a hold like any other, so it is the server's to make. A
			// client gets the same pose by being told the montage time to wind to - see ApplyServerSwingHold.
			bool bRewound = false;
			if (bAuthority && Pair.TouchFrame == GFrameCounter)
			{
				// The blades met somewhere inside the frame just gone. If a swing carried this blade there, wind it
				// back to the moment they met and hold it: from the next frame the animated blade is at the other one
				// rather than through it.
				bRewound = RewindSwingToTouch(Pair.TouchAlpha, TEXT("SWEEP touch"));
			}
			else if (bAuthority && bRetryingHeldSwing && !PausedMontage.IsValid())
			{
				// A swing let go to try again that presses on into the blade it was held against is held again.
				// Pressing on is depth along the pair's direction, so a swing that went right through within the
				// frame still counts. And it counts only while the blades are still against each other, or went
				// through each other: a swing whose way has come clear must not be stopped by the empty place where
				// the other blade was.
				const bool bCarriedThrough = Pair.bCrossed && !Pair.bCrossedBefore;
				const bool bStillAgainst = Result.bPenetrating || bCarriedThrough;
				if (bStillAgainst && Result.AxisDepth > RetryDepth + HexenCollisionGuardPressTolerance)
				{
					// All the way back to where the swing stood last tick - the pose it was held in, when it presses on in
					// its first frame free. Winding back only to where the depth passed the limit, as a first version did,
					// left it 4-8% of a frame further on each time, and the held pose crept deeper with every retry.
					bRewound = RewindSwingToTouch(0.f, TEXT("PRESS on"));
				}
			}

			if (bRewound)
			{
				PublishSwingHold(true);

				// Side and crossed were decided this frame from the pose the swing has just been wound back from, which
				// can have been past the other blade; left as they are, the rig would push the rewound blade on through.
				// A partner that ticked first this frame has already read them, and pushes one frame by the undone decision.
				Pair.Side = Pair.SideBefore;
				Pair.bCrossed = Pair.bCrossedBefore;
			}

			// Off, neither goes to the rig, which then pushes along the shortest way out on its own - see
			// bHexenCollisionGuardPairDecision. The pair record is still kept, for the sweep.
			if (bHexenCollisionGuardPairDecision)
			{
				In.PairAxisWorld = (Pair.First.Get() == MyVolume) ? Pair.Side : -Pair.Side;
				In.bPairCrossed = Pair.bCrossed;
			}

			In.Share = HexenCollisionGuardShare;
			In.Skin = HexenCollisionGuardSkin;
			bValid = true;
		}
		else if (bAuthority)
		{
			// Nobody's blade in range any more, so there is no pair and nothing to be crossed with. Left standing, a
			// true from the last clash would meet the next partner already crossed, before a single frame is measured.
			MyVolume->SetPairCrossed(false);
		}
	}

	{
		FScopeLock Lock(&HexenCollisionGuardLock);
		HexenCollisionGuardInput = In;
		bHexenCollisionGuardInputValid = bValid;
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		// Which world this line is from. "cli" alone is not enough: a PIE session runs a server and more than one
		// client side by side, each with its own copy of every fighter in its own pose, and two lines from two
		// different clients read as one world disagreeing with itself.
		// The world's own name is the map's and is the same in every PIE world - it has to be the package, which is
		// where the UEDPIE_N prefix lives.
		const FString Side = FString::Printf(TEXT("%s/%s"),
			(GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli"),
			World ? *FPackageName::GetShortName(World->GetOutermost()->GetName()) : TEXT("noworld"));

		// When each blade was read, against when its fighter's pose was last updated. Two fighters read the same blade in the
		// same frame - its owner as "mine", the other as "theirs" - and on 2026-09-22 the two reads showed the same blade in
		// two different places, one keeping time and one behind along with what is drawn. A read before the owner's mesh
		// has finished this frame's animation sees last frame's pose; a read after sees this one. The revision number goes
		// up once per frame when the pose changes, and "evaluating" means the animation for this frame is still running.
		if (bHaveMyAxis && NearestPartnerVolume)
		{
			auto PoseState = [](const UHexenCombatComponent* Fighter) -> FString
			{
				const ACharacter* Character = Fighter ? Cast<ACharacter>(Fighter->GetOwner()) : nullptr;
				const USkeletalMeshComponent* FighterMesh = Character ? Character->GetMesh() : nullptr;
				if (!FighterMesh)
				{
					return TEXT("no mesh");
				}
				return FString::Printf(TEXT("rev %u%s"), FighterMesh->GetBoneTransformRevisionNumber(),
					FighterMesh->IsRunningParallelEvaluation() ? TEXT(" EVALUATING") : TEXT(""));
			};
			const UHexenCombatComponent* PartnerFighter = NearestPartnerVolume->GetFighter();
			UE_LOG(LogTemp, Warning, TEXT("[READS] %s %s f%llu | mine mid %s %s | theirs %s mid %s %s"),
				*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
				*((MyStart + MyEnd) * 0.5f).ToCompactString(), *PoseState(this),
				*GetNameSafe(PartnerFighter ? PartnerFighter->GetOwner() : nullptr),
				*((TheirStart + TheirEnd) * 0.5f).ToCompactString(), *PoseState(PartnerFighter));
		}

		// What the eye sees, held against what the guard measures: the gap between the two DRAWN capsules' surfaces,
		// negative when they overlap. Depth above it answers the same question about the rig's pose. The two poses are
		// not the same pose, and this pair of numbers says by how much - a push that starts while this is still several
		// centimetres positive is the guard holding blades apart that have not met where anyone can see them.
		float DrawnGap = 0.f;
		bool bHaveDrawnGap = false;
		if (bHaveMyAxis && TheirRadius > 0.f)
		{
			FVector OnMineDrawn, OnTheirsDrawn;
			FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMineDrawn, OnTheirsDrawn);
			DrawnGap = FVector::Dist(OnMineDrawn, OnTheirsDrawn) - (MyRadius + TheirRadius);
			bHaveDrawnGap = true;
		}

		if (Result.bPenetrating || bGuardWasPushing)
		{
			// One line per frame while the guard is pushing, and one when it lets go. A depth that keeps
			// growing while the correction keeps pace is a blade being held off; a correction of zero with a
			// depth above zero would be the guard not reaching the rig at all.
			UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu %s%s%s | depth %.1fcm | drawn gap %s | correction %.1fcm | normal %s | at %.0fcm from hand%s%s"),
				*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
				Result.bPenetrating ? (bGuardWasPushing ? TEXT("push") : TEXT("START")) : TEXT("END"),
				Result.bCrossed ? TEXT(" CROSSED") : TEXT(""),
				bHexenCollisionGuardPairDecision ? TEXT("") : TEXT(" NOPAIR"),
				Result.Depth,
				bHaveDrawnGap
					? (bHavePreviousDrawnGap
						? *FString::Printf(TEXT("%.1fcm, was %.1fcm a frame before"), DrawnGap, PreviousDrawnGap)
						: *FString::Printf(TEXT("%.1fcm"), DrawnGap))
					: TEXT("no partner"),
				Result.CorrectionWorld.Size(), *Result.NormalWorld.ToCompactString(),
				Result.ContactFromHand, Result.bContactAtMyEnd ? TEXT(" MY-END") : TEXT(""), Result.bContactAtTheirEnd ? TEXT(" THEIR-END") : TEXT(""));
		}

		// Every guarded volume in this world the frame a contact starts: who owns it, which fighter it answers to, and
		// where its axis is. A third capsule in the picture that sits on no blade is either a volume nobody expected
		// to be in play or the same volume read twice from two different places, and this tells the two apart.
		if (Result.bPenetrating && !bGuardWasPushing)
		{
			for (UHexenCollisionComponent* Volume : Volumes)
			{
				FVector Start, End;
				float Radius = 0.f;
				const bool bHaveAxis = Volume->GetShapeAxisWorld(Start, End, Radius);
				const UHexenCombatComponent* Fighter = Volume->GetFighter();
				UE_LOG(LogTemp, Warning, TEXT("[VOLUMES] %s %s f%llu | %s on %s | fighter %s | %s | axis %s -> %s r%.1f | len %.0fcm"),
					*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
					*Volume->GetName(), *GetNameSafe(Volume->GetOwner()),
					*GetNameSafe(Fighter ? Fighter->GetOwner() : nullptr),
					Volume == MyVolume ? TEXT("MINE") : (Fighter == this ? TEXT("also mine") : TEXT("theirs")),
					bHaveAxis ? *Start.ToCompactString() : TEXT("none"), bHaveAxis ? *End.ToCompactString() : TEXT("none"),
					Radius, bHaveAxis ? FVector::Dist(Start, End) : -1.f);
			}
		}

		// The drawn pose itself, bone by bone down the chain, in the mesh's own space so the fighter's place in the
		// world is out of it. Two machines standing at the same point of the same montage still put the hand 10 cm
		// apart, so the question is no longer whether they differ but where down the body they begin to: a difference
		// that starts at the spine is something leaning the upper body, one that starts at the shoulder is the swing.
		if (Mesh && (Result.bPenetrating || PausedMontage.IsValid() || bRetryingHeldSwing))
		{
			FString PoseReport;
			for (const FName& Bone : GetHexenCollisionGuardChainBones())
			{
				const FVector At = Mesh->GetSocketTransform(Bone, RTS_Component).GetLocation();
				PoseReport += FString::Printf(TEXT(" %s %.0f,%.0f,%.0f"), *Bone.ToString(), At.X, At.Y, At.Z);
			}
			UE_LOG(LogTemp, Warning, TEXT("[POSE] %s %s f%llu |%s"),
				*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter, *PoseReport);
		}

		PreviousDrawnGap = DrawnGap;
		bHavePreviousDrawnGap = bHaveDrawnGap;

		if (Mesh && (Result.bPenetrating || PausedMontage.IsValid() || bRetryingHeldSwing))
		{
			// What moves while a swing is held - the drift, open question 7 in Docs/HexenCollision.md. The hand where
			// the animation put it, in the mesh's own space: movement there is the pose itself, such as the stance
			// under a partly weighted montage. The mesh in the world: movement there is the fighter. And how far the
			// partner's estimate of this blade - drawn minus correction - is from where the animation had it, at each
			// end: an error there goes straight into the partner's push.
			const float EstErrStart = bHaveMyAxis ? FVector::Dist(MyStart - Result.CorrectionWorld, Result.AnimatedAxisStartWorld) : -1.f;
			const float EstErrEnd = bHaveMyAxis ? FVector::Dist(MyEnd - Result.CorrectionWorld, Result.AnimatedAxisEndWorld) : -1.f;
			const FVector MeshLocation = Mesh->GetComponentLocation();

			// And the cause of that error: whether the hand is drawn where the rig put it - the animated hand plus the
			// correction, its rotation kept - and how many frames old the rig's result is. A result from an earlier
			// frame means the pose was not evaluated this frame.
			const FTransform DrawnHand = Mesh->GetSocketTransform(HandBoneName, RTS_World);
			const float HandOffRig = Result.bHasAnimatedPose ? FVector::Dist(DrawnHand.GetLocation(), Result.AnimatedHandWorld.GetLocation() + Result.CorrectionWorld) : -1.f;
			const float HandTurnOffRig = Result.bHasAnimatedPose ? FMath::RadiansToDegrees(DrawnHand.GetRotation().AngularDistance(Result.AnimatedHandWorld.GetRotation())) : -1.f;
			const uint64 RigAge = GFrameCounter >= Result.EvaluationFrame ? GFrameCounter - Result.EvaluationFrame : 0;

			// Where this machine's copy of the swing has got to. Two machines standing at different points of the same
			// montage put the same fighter's hand in two different places, and the distance between those two places is
			// the whole divergence - so the time itself is worth having next to it rather than inferred from it.
			float MontageAt = -1.f;
			if (UAnimInstance* AnimInstance = Mesh->GetAnimInstance())
			{
				if (UAnimMontage* Active = AnimInstance->GetCurrentActiveMontage())
				{
					MontageAt = AnimInstance->Montage_GetPosition(Active);
				}
			}

			UE_LOG(LogTemp, Warning, TEXT("[DRIFT] %s %s f%llu %s | montage %.3f | handCS %.1f %.1f %.1f | mesh %.1f %.1f %.1f yaw %.1f | estErr %.1f/%.1fcm | axisDepth %.1fcm | drawn hand off rig %.1fcm %.1fdeg | rig age %llu"),
				*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
				PausedMontage.IsValid() ? TEXT("held") : (bRetryingHeldSwing ? TEXT("retry") : TEXT("free")),
				MontageAt,
				Result.AnimatedHandComponent.X, Result.AnimatedHandComponent.Y, Result.AnimatedHandComponent.Z,
				MeshLocation.X, MeshLocation.Y, MeshLocation.Z, Mesh->GetComponentRotation().Yaw,
				EstErrStart, EstErrEnd, Result.AxisDepth, HandOffRig, HandTurnOffRig, (unsigned long long)RigAge);

			// What delivered the correction: carrying the arm, or turning the wrist. The guard asks for one spot on this
			// blade - the one that touches - to move out by CorrectionWorld, and aims the rig's contact bone at it; the
			// solver is free to reach that by moving the arm, by turning the hand, or by both. Which of the two it used
			// is the whole point of aiming by the contact bone - a hand pushed bodily cannot turn the blade at all.
			//
			// Only the turn is measured against a clean baseline. Rotations reach the rig's hierarchy and its bone
			// offsets do not, so the drawn hand stands 8-12 cm away from the rig's whatever the guard does - see the
			// [LOCAL] line and question 7. That standing error is in the hand's figure below and not in the turn's,
			// which is the same local contact offset turned by the two rotations and nothing else.
			if (Result.bHasAnimatedPose && Result.bPenetrating && !Result.CorrectionWorld.IsNearlyZero())
			{
				const FVector Wanted = Result.CorrectionWorld;
				const FVector WantedDir = Wanted.GetSafeNormal();
				const FVector ContactInHand = Result.AnimatedHandWorld.InverseTransformPosition(Result.AnimatedContactWorld);
				const FVector ByTurn = DrawnHand.GetRotation().RotateVector(ContactInHand) - Result.AnimatedHandWorld.GetRotation().RotateVector(ContactInHand);
				const FVector ByHand = DrawnHand.GetLocation() - Result.AnimatedHandWorld.GetLocation();
				const float Asked = Wanted.Size();
				const float TurnAlong = FVector::DotProduct(ByTurn, WantedDir);
				const float HandAlong = FVector::DotProduct(ByHand, WantedDir);

				UE_LOG(LogTemp, Warning, TEXT("[SPLIT] %s %s f%llu | asked %.1fcm | by turn %.1fcm (%.0f%%) of %.1fcm moved, %.1fdeg | by hand %.1fcm (%.0f%%) of %.1fcm moved, carries the standing error | contact %.0fcm from hand%s"),
					*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
					Asked,
					TurnAlong, 100.f * TurnAlong / Asked, ByTurn.Size(), HandTurnOffRig,
					HandAlong, 100.f * HandAlong / Asked, ByHand.Size(),
					Result.ContactFromHand, Result.bContactAtMyEnd ? TEXT(" MY-END") : TEXT(""));
			}

			// And where the drawn pose leaves the rig's, bone by bone down the chain, both in the mesh's own space so
			// the world transform is out of it. Above clavicle_r the guard moves nothing, so a difference there is
			// somewhere else entirely; from clavicle_r down, the guard's FABRIK is in the picture. The last pair of
			// numbers is the two spaces themselves: what the mesh calls world against what the rig calls world.
			// Each bone twice: against the rig's pose of this frame, and against the rig's pose of the frame before.
			// If the drawn pose sits on the older one, it is simply a frame behind rather than changed by anything.
			const bool bHavePrevious = PreviousChainNum == Result.ChainNum && PreviousChainFrame + 1 == GFrameCounter;
			const TArray<FName>& ChainBones = GetHexenCollisionGuardChainBones();
			FString ChainReport;
			for (int32 BoneIndex = 0; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
			{
				const FTransform DrawnBone = Mesh->GetSocketTransform(ChainBones[BoneIndex], RTS_Component);
				const float OffPrevious = bHavePrevious ? FVector::Dist(DrawnBone.GetLocation(), PreviousChain[BoneIndex].GetLocation()) : -1.f;
				ChainReport += FString::Printf(TEXT(" %s %.1f|%.1f/%.0f"), *ChainBones[BoneIndex].ToString(),
					FVector::Dist(DrawnBone.GetLocation(), Result.ChainComponent[BoneIndex].GetLocation()), OffPrevious,
					FMath::RadiansToDegrees(DrawnBone.GetRotation().AngularDistance(Result.ChainComponent[BoneIndex].GetRotation())));
			}

			// Each bone's own offset from its parent, in cm, from every place it can be read: what the rig's hierarchy
			// holds (rig), the same worked out from the rig's globals (glob), the drawn pose (drawn), the rig's own
			// reference pose (init) and the mesh's (ref). The rig's numbers came out as none of the others in the
			// 2026-09-18 run - spine_02 was 10.81 against 6.80 in the rig's reference pose and 8.13 drawn - so this holds
			// all five side by side. rig against glob says whether the hierarchy's locals and globals agree at all.
			const FReferenceSkeleton* RefSkeleton = Mesh->GetSkinnedAsset() ? &Mesh->GetSkinnedAsset()->GetRefSkeleton() : nullptr;

			// The mesh's own local pose - what the animation graph put out, before it was built into the component-space
			// transforms everything else reads - and the scale the parent is drawn at. If mesh matches the rig while drawn
			// does not, nothing is wrong with the transfer and the difference is made when the component-space transforms
			// are built; a parent scale other than 1 would be how.
			// Not const: the view blocks on the evaluation task first, and that is what makes it safe to read here.
			USkeletalMeshComponent* PoseMesh = GetOwnerMesh();
			const TArrayView<const FTransform> MeshLocals = PoseMesh ? PoseMesh->GetBoneSpaceTransformsView() : TArrayView<const FTransform>();
			FString LocalReport;
			for (int32 BoneIndex = 1; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
			{
				const FTransform RigLocal = Result.ChainComponent[BoneIndex].GetRelativeTransform(Result.ChainComponent[BoneIndex - 1]);
				const FTransform DrawnLocal = Mesh->GetSocketTransform(ChainBones[BoneIndex], RTS_Component).GetRelativeTransform(Mesh->GetSocketTransform(ChainBones[BoneIndex - 1], RTS_Component));

				float RefOffset = -1.f;
				if (RefSkeleton)
				{
					const int32 RefIndex = RefSkeleton->FindBoneIndex(ChainBones[BoneIndex]);
					if (RefSkeleton->GetRefBonePose().IsValidIndex(RefIndex))
					{
						RefOffset = static_cast<float>(RefSkeleton->GetRefBonePose()[RefIndex].GetTranslation().Size());
					}
				}

				const int32 MeshBoneIndex = Mesh->GetBoneIndex(ChainBones[BoneIndex]);
				const float MeshLocalOffset = MeshLocals.IsValidIndex(MeshBoneIndex) ? static_cast<float>(MeshLocals[MeshBoneIndex].GetTranslation().Size()) : -1.f;
				const FVector ParentScale = Mesh->GetSocketTransform(ChainBones[BoneIndex - 1], RTS_Component).GetScale3D();

				LocalReport += FString::Printf(TEXT(" %s rig %.2f glob %.2f mesh %.2f drawn %.2f init %.2f ref %.2f pscale %.2f %.0fdeg"), *ChainBones[BoneIndex].ToString(),
					Result.ChainLocalOffset[BoneIndex], RigLocal.GetTranslation().Size(), MeshLocalOffset, DrawnLocal.GetTranslation().Size(),
					Result.ChainInitialOffset[BoneIndex], RefOffset, static_cast<float>(ParentScale.X),
					FMath::RadiansToDegrees(RigLocal.GetRotation().AngularDistance(DrawnLocal.GetRotation())));
			}

			UE_LOG(LogTemp, Warning, TEXT("[LOCAL] %s %s f%llu | bone offset from the rig, its globals, the drawn pose, the rig's reference pose and the mesh's, then the turn apart:%s"),
				*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter, *LocalReport);

			// Once per contact: what the rig's hierarchy hangs each chain bone from against what the mesh does, and how many
			// bones each holds. A pose read down a different chain is a different pose, however faithfully it was handed over.
			if (!bGuardWasPushing && Result.bPenetrating)
			{
				FString ParentReport;
				for (int32 BoneIndex = 0; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
				{
					const FName MeshParent = Mesh->GetParentBone(ChainBones[BoneIndex]);
					ParentReport += FString::Printf(TEXT(" %s rig<-%s mesh<-%s%s"), *ChainBones[BoneIndex].ToString(),
						*Result.ChainParent[BoneIndex].ToString(), *MeshParent.ToString(),
						(Result.ChainParent[BoneIndex] == MeshParent) ? TEXT("") : TEXT(" DIFFERENT"));
				}

				UE_LOG(LogTemp, Warning, TEXT("[RIGCHAIN] %s %s f%llu | bones in the rig %d, in the mesh %d |%s"),
					*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
					Result.RigBoneCount, Mesh->GetNumBones(), *ParentReport);

				// And where each of the rig's chain bones actually stands: the nearest bone of the drawn pose to it. If the
				// rig's hand_r sits on another bone, the pose is landing in the wrong elements of the hierarchy, and every
				// reading our unit takes by name is another bone's.
				FString SitsOnReport;
				const int32 MeshBoneNum = Mesh->GetNumBones();
				for (int32 BoneIndex = 0; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
				{
					const FVector RigPoint = Result.ChainComponent[BoneIndex].GetLocation();
					float NearestDistance = TNumericLimits<float>::Max();
					FName NearestBone;
					for (int32 MeshBone = 0; MeshBone < MeshBoneNum; ++MeshBone)
					{
						const float Distance = static_cast<float>(FVector::Dist(RigPoint, Mesh->GetBoneTransform(MeshBone, FTransform::Identity).GetLocation()));
						if (Distance < NearestDistance)
						{
							NearestDistance = Distance;
							NearestBone = Mesh->GetBoneName(MeshBone);
						}
					}
					SitsOnReport += FString::Printf(TEXT(" %s on %s %.1fcm"), *ChainBones[BoneIndex].ToString(), *NearestBone.ToString(), NearestDistance);
				}

				UE_LOG(LogTemp, Warning, TEXT("[RIGSITS] %s %s f%llu | the rig's chain bone stands nearest to this bone of the drawn pose:%s"),
					*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter, *SitsOnReport);

				// And whose pose the rig is actually holding: the bone of the mesh whose own local transform is closest to
				// the one the rig has for this bone. If the rig's spine_02 turns out to hold another bone's transform, the
				// pose is landing in the wrong elements, and every reading our unit takes by name belongs to someone else.
				FString HoldsReport;
				for (int32 BoneIndex = 1; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
				{
					const FTransform& RigLocalBone = Result.ChainLocal[BoneIndex];
					float BestScore = TNumericLimits<float>::Max();
					float BestDistance = -1.f;
					FName BestBone;
					for (int32 MeshBone = 0; MeshBone < MeshBoneNum && MeshLocals.IsValidIndex(MeshBone); ++MeshBone)
					{
						const float Distance = static_cast<float>(FVector::Dist(RigLocalBone.GetTranslation(), MeshLocals[MeshBone].GetTranslation()));
						const float Turn = FMath::RadiansToDegrees(RigLocalBone.GetRotation().AngularDistance(MeshLocals[MeshBone].GetRotation()));
						const float Score = Distance + 0.1f * Turn;
						if (Score < BestScore)
						{
							BestScore = Score;
							BestDistance = Distance;
							BestBone = Mesh->GetBoneName(MeshBone);
						}
					}
					HoldsReport += FString::Printf(TEXT(" %s holds %s %.2fcm"), *ChainBones[BoneIndex].ToString(), *BestBone.ToString(), BestDistance);
				}

				UE_LOG(LogTemp, Warning, TEXT("[RIGHOLDS] %s %s f%llu | the rig's local transform for a bone matches this bone of the mesh's own pose:%s"),
					*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter, *HoldsReport);

				// The one reference pose not yet held against the rig's: the skeleton's. A mesh carries its own, and this one
				// can differ - SKM_Manny_Simple has 89 bones where its skeleton has about 155. If the rig's offsets turn out
				// to be the skeleton's, the rig is standing in a reference pose the animation never passes through.
				const FReferenceSkeleton* SkeletonRef = (Mesh->GetSkinnedAsset() && Mesh->GetSkinnedAsset()->GetSkeleton())
					? &Mesh->GetSkinnedAsset()->GetSkeleton()->GetReferenceSkeleton() : nullptr;
				FString VectorReport;
				for (int32 BoneIndex = 1; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
				{
					const int32 MeshBoneIndex = Mesh->GetBoneIndex(ChainBones[BoneIndex]);
					const FVector MeshOffset = MeshLocals.IsValidIndex(MeshBoneIndex) ? MeshLocals[MeshBoneIndex].GetTranslation() : FVector::ZeroVector;

					FVector SkeletonOffset = FVector::ZeroVector;
					if (SkeletonRef)
					{
						const int32 SkeletonIndex = SkeletonRef->FindBoneIndex(ChainBones[BoneIndex]);
						if (SkeletonRef->GetRefBonePose().IsValidIndex(SkeletonIndex))
						{
							SkeletonOffset = SkeletonRef->GetRefBonePose()[SkeletonIndex].GetTranslation();
						}
					}

					VectorReport += FString::Printf(TEXT(" %s rig(%s) mesh(%s) skel(%s %.2f)"), *ChainBones[BoneIndex].ToString(),
						*Result.ChainLocal[BoneIndex].GetTranslation().ToCompactString(), *MeshOffset.ToCompactString(),
						*SkeletonOffset.ToCompactString(), SkeletonOffset.Size());
				}

				UE_LOG(LogTemp, Warning, TEXT("[RIGVEC] %s %s f%llu | bone offset as a vector - in the rig, in the mesh's pose and in the skeleton's reference pose:%s"),
					*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter, *VectorReport);

				// Whether each chain bone is in the pose at all. The animation is evaluated over the bones the current LOD
				// asks for, not every bone of the mesh, and a bone left out of that list is never handed to the rig - which
				// would be reason enough for the rig to be holding something else.
				UAnimInstance* PoseAnim = PoseMesh ? PoseMesh->GetAnimInstance() : nullptr;
				FString RequiredReport;
				if (PoseAnim)
				{
					const TArray<FBoneIndexType>& RequiredBones = PoseAnim->GetRequiredBones().GetBoneIndicesArray();
					for (int32 BoneIndex = 0; BoneIndex < Result.ChainNum && BoneIndex < ChainBones.Num(); ++BoneIndex)
					{
						const int32 MeshBoneIndex = Mesh->GetBoneIndex(ChainBones[BoneIndex]);
						const int32 CompactIndex = RequiredBones.IndexOfByKey(static_cast<FBoneIndexType>(MeshBoneIndex));
						RequiredReport += FString::Printf(TEXT(" %s mesh %d pose %s"), *ChainBones[BoneIndex].ToString(), MeshBoneIndex,
							CompactIndex == INDEX_NONE ? TEXT("MISSING") : *FString::FromInt(CompactIndex));
					}

					UE_LOG(LogTemp, Warning, TEXT("[RIGPOSE] %s %s f%llu | LOD %d, bones in the pose %d of the mesh's %d |%s"),
						*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
						Mesh->GetPredictedLODLevel(), RequiredBones.Num(), Mesh->GetNumBones(), *RequiredReport);
				}
			}

			// And whether physics is being blended into the drawn pose at all: that would move bones the rig never
			// touches, which is what a difference above the arm would otherwise mean.
			const FTransform MeshToWorld = Mesh->GetComponentTransform();
			const FBodyInstance* SpineBody = Mesh->GetBodyInstance(FName("spine_05"));
			const FBodyInstance* HandBody = Mesh->GetBodyInstance(HandBoneName);
			UE_LOG(LogTemp, Warning, TEXT("[CHAIN] %s %s f%llu %s | cm/deg:%s | mesh vs rig world %.1fcm %.1fdeg scale %.2f/%.2f | phys blend %d sim %d spine %.2f hand %.2f"),
				*GetNameSafe(GetOwner()), *Side, (unsigned long long)GFrameCounter,
				PausedMontage.IsValid() ? TEXT("held") : (bRetryingHeldSwing ? TEXT("retry") : TEXT("free")),
				*ChainReport,
				FVector::Dist(MeshToWorld.GetLocation(), Result.RigToWorld.GetLocation()),
				FMath::RadiansToDegrees(MeshToWorld.GetRotation().AngularDistance(Result.RigToWorld.GetRotation())),
				MeshToWorld.GetScale3D().X, Result.RigToWorld.GetScale3D().X,
				Mesh->bBlendPhysics ? 1 : 0, Mesh->IsSimulatingPhysics() ? 1 : 0,
				SpineBody ? SpineBody->PhysicsBlendWeight : -1.f, HandBody ? HandBody->PhysicsBlendWeight : -1.f);
		}
	}

	if (bDrawContactPoints && Result.bPenetrating && World)
	{
		DrawDebugSphere(World, Result.ContactPointWorld, 2.f, 8, FColor::Yellow, false, -1.f);
		DrawDebugDirectionalArrow(World, Result.ContactPointWorld, Result.ContactPointWorld + Result.CorrectionWorld * 3.f, 5.f, FColor::Red, false, -1.f);
	}

	// The drawn capsules as the code reads them, in the frame they are read: each axis in magenta, and between them
	// the shortest line - green while the two are inside one another, red while they are apart, and ringed at each
	// end by that capsule's own thickness. A picture and a number taken at two different moments prove nothing about
	// each other, so the number is drawn on the thing it is about: if the magenta lines do not lie down the middle of
	// the capsules, the code is reading a shape nobody is looking at.
	if (bDrawContactPoints && World && bHaveMyAxis && TheirRadius > 0.f)
	{
		FVector OnMineDrawn, OnTheirsDrawn;
		FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMineDrawn, OnTheirsDrawn);
		const float Gap = FVector::Dist(OnMineDrawn, OnTheirsDrawn) - (MyRadius + TheirRadius);

		DrawDebugLine(World, OnMineDrawn, OnTheirsDrawn, Gap <= 0.f ? FColor::Green : FColor::Red, false, -1.f, 0, 1.5f);
		// And each capsule rebuilt from exactly the numbers the measurement uses - centre, length and thickness. If a
		// magenta capsule does not sit on top of its yellow one, the code is reading a shape nobody is looking at, and
		// that is the whole bug rather than anything about contact.
		// Coloured by the WORLD that drew it: orange the server, magenta the first client, cyan the second. Within one
		// world both reads of a blade are identical to the millimetre (2026-09-22), so a blade showing two capsules in one
		// window is showing another world's drawing on top of its own - and the colour says whose.
		const bool bServerWorld = GetOwner() && GetOwner()->HasAuthority();
		const FColor WorldColour = bServerWorld ? FColor::Orange
			: (World->GetOutermost()->GetName().Contains(TEXT("UEDPIE_2")) ? FColor::Cyan : FColor::Magenta);
		auto DrawMeasuredCapsule = [World](const FVector& Start, const FVector& End, float Radius, const FColor& Colour)
		{
			const FVector Axis = End - Start;
			const FQuat Rotation = FRotationMatrix::MakeFromZ(Axis.GetSafeNormal()).ToQuat();
			DrawDebugCapsule(World, (Start + End) * 0.5f, static_cast<float>(Axis.Size()) * 0.5f + Radius, Radius, Rotation, Colour, false, -1.f, 0, 0.5f);
		};
		DrawMeasuredCapsule(MyStart, MyEnd, MyRadius, WorldColour);
		DrawMeasuredCapsule(TheirStart, TheirEnd, TheirRadius, WorldColour);
	}
#endif

	// Stop the swing at the blade the moment the guard first has to push, and look every HexenCollisionGuardPauseSeconds
	// whether it can go on. The server alone, from its own poses: a client that decided this for itself would sooner or
	// later disagree with the server about whether a swing ever reached its target - see PublishSwingHold.
	if (bAuthority && bPauseMontageOnContact && HexenCollisionGuardPauseSeconds > 0.f && World)
	{
		const double Now = World->GetTimeSeconds();
		if (Result.bPenetrating && !bGuardWasPushing && !PausedMontage.IsValid())
		{
			PauseCurrentMontage();
			GuardPauseStartTime = Now;
			PublishSwingHold(true);
		}
		else if (PausedMontage.IsValid() && Now - GuardPauseStartTime >= HexenCollisionGuardPauseSeconds)
		{
			// Let go only once the contact is over: this blade no longer overlaps another fighter's. The guard holds
			// the blades a hair inside touching, so the overlap lasts exactly as long as the other blade stays in the way.
			// Judged on the drawn capsules or in the rig's pose - see bHexenCollisionGuardRigPoseOverlap - and both are
			// looked at, so the log can show where they disagree.
			//
			// In the rig's pose a guard still pushing counts as contact too, whichever of the two it is: a blade that has to
			// be pushed out is in the way by definition. The rig's pose alone let go of swings the guard was still pushing
			// 4-27 cm - it pushes along the pair's direction, which can run oblique to the shortest one and leave the blades
			// further apart than touching - and each came straight back into the blade, ten times in the 2026-09-19 run.
			const UHexenCollisionComponent* StillOverlapping = nullptr;
			bool bDrawnOverlap = false;
			bool bRigPoseOverlap = false;
			bool bTheirGuardPushing = false;
			const bool bMyGuardPushing = Result.bPenetrating;
			if (MyVolume)
			{
				for (UHexenCollisionComponent* Volume : Volumes)
				{
					const UHexenCombatComponent* TheirFighter = Volume->GetFighter();
					if (TheirFighter == this)
					{
						continue;
					}
					const bool bDrawn = AreOverlapping(MyVolume, Volume);
					const bool bRigPose = AreOverlappingInRigPose(MyVolume, Volume);
					const bool bTheirs = TheirFighter && TheirFighter->IsHexenCollisionGuardPushing();
					bDrawnOverlap |= bDrawn;
					bRigPoseOverlap |= bRigPose;
					bTheirGuardPushing |= bTheirs;
					const bool bInContact = bHexenCollisionGuardRigPoseOverlap ? (bRigPose || bMyGuardPushing || bTheirs) : bDrawn;
					if (!StillOverlapping && bInContact)
					{
						StillOverlapping = Volume;
					}
				}
			}

#if !UE_BUILD_SHIPPING
			if (bLogContactSteps)
			{
				UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu LOOK after %.2fs held | %s | by %s: rig pose %s, guard pushing mine %s theirs %s, drawn %s"),
					*GetNameSafe(GetOwner()), (GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli"),
					(unsigned long long)GFrameCounter, Now - GuardPauseStartTime,
					StillOverlapping
						? *FString::Printf(TEXT("still overlapping %s - held, next look in %.2fs"), *GetNameSafe(StillOverlapping->GetOwner()), HexenCollisionGuardPauseSeconds)
						: TEXT("overlap over - swing let go"),
					bHexenCollisionGuardRigPoseOverlap ? TEXT("rig pose") : TEXT("drawn"),
					bRigPoseOverlap ? TEXT("yes") : TEXT("no"), bMyGuardPushing ? TEXT("yes") : TEXT("no"),
					bTheirGuardPushing ? TEXT("yes") : TEXT("no"), bDrawnOverlap ? TEXT("yes") : TEXT("no"));
			}
#endif

			if (StillOverlapping)
			{
				// Still in the way: stay held, and look again after the same time.
				GuardPauseStartTime = Now;
			}
			else
			{
				ResumePausedMontage();
				PublishSwingHold(false);

				// Still against the blade: from the next tick, pressing on into it stops the swing again. Already apart:
				// there is nothing to press against, and the swing simply plays on.
				bRetryingHeldSwing = Result.bPenetrating;
				RetryDepth = Result.AxisDepth;

#if !UE_BUILD_SHIPPING
				if (bLogContactSteps && bRetryingHeldSwing)
				{
					UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu RETRY from depth %.1fcm - held again past %.1fcm"),
						*GetNameSafe(GetOwner()), (GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli"),
						(unsigned long long)GFrameCounter, RetryDepth, RetryDepth + HexenCollisionGuardPressTolerance);
				}
#endif
			}
		}
	}

	// The blades the swing was let go against have come apart: nothing left to press on into, and the swing is free.
	// A swing that went through them instead has already been held again above.
	if (bRetryingHeldSwing && !PausedMontage.IsValid() && !Result.bPenetrating)
	{
		bRetryingHeldSwing = false;

#if !UE_BUILD_SHIPPING
		if (bLogContactSteps)
		{
			UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu RETRY over - the blades came apart, the swing plays on"),
				*GetNameSafe(GetOwner()), (GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli"),
				(unsigned long long)GFrameCounter);
		}
#endif
	}

	bGuardWasPushing = Result.bPenetrating;

	// EXPERIMENT - see bReplicateServerPose. Server only; returns at once anywhere else.
	PublishServerState();

	// Kept for the next tick's chain comparison - see PreviousChain.
	if (Result.ChainNum > 0)
	{
		for (int32 BoneIndex = 0; BoneIndex < Result.ChainNum; ++BoneIndex)
		{
			PreviousChain[BoneIndex] = Result.ChainComponent[BoneIndex];
		}
		PreviousChainNum = Result.ChainNum;
		PreviousChainFrame = GFrameCounter;
	}

	// Where the swing is now - its time and how far into its blend-in - for the next rewind to wind back towards.
	const USkeletalMeshComponent* SwingMesh = GetOwnerMesh();
	const UAnimInstance* SwingAnim = SwingMesh ? SwingMesh->GetAnimInstance() : nullptr;
	UAnimMontage* SwingMontage = SwingAnim ? SwingAnim->GetCurrentActiveMontage() : nullptr;
	const FAnimMontageInstance* SwingInstance = SwingMontage ? SwingAnim->GetActiveInstanceForMontage(SwingMontage) : nullptr;
	LastSwingInstanceID = SwingInstance ? SwingInstance->GetInstanceID() : INDEX_NONE;
	LastSwingPosition = SwingInstance ? SwingInstance->GetPosition() : 0.f;
	LastSwingWeight = SwingInstance ? SwingInstance->GetWeight() : 0.f;

	// A swing that has ended has nothing left to press on with.
	if (!SwingInstance)
	{
		bRetryingHeldSwing = false;
	}
}

bool UHexenCombatComponent::RewindSwingToTouch(float Alpha, const TCHAR* Cause)
{
	UWorld* World = GetWorld();
	if (!bPauseMontageOnContact || PausedMontage.IsValid() || !World)
	{
		return false;
	}

	const USkeletalMeshComponent* Mesh = GetOwnerMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UAnimMontage* Active = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr;

#if !UE_BUILD_SHIPPING
	const TCHAR* Side = (GetOwner() && GetOwner()->HasAuthority()) ? TEXT("srv") : TEXT("cli");
#endif

	FAnimMontageInstance* Instance = Active ? AnimInstance->GetActiveInstanceForMontage(Active) : nullptr;
	if (!Instance)
	{
#if !UE_BUILD_SHIPPING
		if (bLogContactSteps)
		{
			UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu %s at %.0f%% of the frame | no montage to rewind"),
				*GetNameSafe(GetOwner()), Side, (unsigned long long)GFrameCounter, Cause, Alpha * 100.f);
		}
#endif
		return false;
	}

	// From where the swing stood last tick to where it is now, the fraction of the way the blades had come
	// when they met - in its time and in its blend-in both. Inside the blend-in the pose is still coming in
	// from the stance, so the weight carries the blade as much as the time does: winding back the time alone
	// left the blade wherever the newer, heavier weight put it. A swing that was not playing last tick, or
	// that has looped back since, has no "before" to wind back towards, and is only held where it is.
	const float Now = Instance->GetPosition();
	const bool bHasBefore = LastSwingInstanceID == Instance->GetInstanceID() && LastSwingPosition <= Now;
	const float Before = bHasBefore ? LastSwingPosition : Now;
	const float Target = FMath::Lerp(Before, Now, Alpha);
	const float WeightNow = Instance->GetWeight();
	const float TargetWeight = FMath::Lerp(bHasBefore ? LastSwingWeight : WeightNow, WeightNow, Alpha);

	AnimInstance->Montage_SetPosition(Active, Target);
	HoldMontage(AnimInstance, Active, TargetWeight);
	GuardPauseStartTime = World->GetTimeSeconds();

#if !UE_BUILD_SHIPPING
	if (bLogContactSteps)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GUARD] %s %s f%llu %s at %.0f%% of the frame | %s rewound %.3f -> %.3f (was %.3f last frame), weight %.2f -> %.2f, and held"),
			*GetNameSafe(GetOwner()), Side, (unsigned long long)GFrameCounter, Cause, Alpha * 100.f,
			*GetNameSafe(Active), Now, Target, Before, WeightNow, Instance->GetWeight());
	}
#endif
	return true;
}

USkeletalMeshComponent* UHexenCombatComponent::GetOwnerMesh() const
{
	// Not cached: a character's mesh outlives this component, but reading it is a pointer fetch, and
	// caching would only add a way to hold a stale one.
	const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	return OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
}
