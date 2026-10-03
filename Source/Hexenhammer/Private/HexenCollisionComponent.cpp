// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenCollisionComponent.h"
#include "HexenCombatComponent.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "WeaponCollisionComponent.h"

namespace
{
	/** Every volume in play, across all worlds - a PIE session runs a server and its clients side by side. Game thread only. */
	TArray<TWeakObjectPtr<UHexenCollisionComponent>> GVolumeRegistry;
}

UHexenCollisionComponent::UHexenCollisionComponent()
{
	// bInContact is decided on the server and has to reach every machine that draws this fighter.
	SetIsReplicatedByDefault(true);

	// Off unless a subclass asks for it in BeginPlay. Most volumes are body hitboxes that never move
	// under their own steam and never need to know how fast they are going.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UHexenCollisionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHexenCollisionComponent, bInContact);
	DOREPLIFETIME(UHexenCollisionComponent, ContactPointWorld);
	DOREPLIFETIME(UHexenCollisionComponent, ContactYieldFraction);
	DOREPLIFETIME(UHexenCollisionComponent, ContactClosingSpeed);
	DOREPLIFETIME(UHexenCollisionComponent, bPairCrossed);
}

void UHexenCollisionComponent::SetPairCrossed(bool bCrossed)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || bPairCrossed == bCrossed)
	{
		return;
	}

	bPairCrossed = bCrossed;
	Owner->ForceNetUpdate();
}

void UHexenCollisionComponent::BeginPlay()
{
	Super::BeginPlay();

	GVolumeRegistry.AddUnique(this);

	// CollisionObject is built and configured in OnRegister(), which has already run by now.
	if (CollisionObject)
	{
		// Bound here rather than in OnRegister() so it is tied to the component actually being in play -
		// OnRegister also runs for editor preview instances, which have no business raising events.
		CollisionObject->OnComponentBeginOverlap.AddDynamic(this, &UHexenCollisionComponent::HandleShapeBeginOverlap);
		CollisionObject->OnComponentEndOverlap.AddDynamic(this, &UHexenCollisionComponent::HandleShapeEndOverlap);
	}
#if !UE_BUILD_SHIPPING
	else
	{
		// CollisionShape was left at None, so this volume is a shape that does not exist.
		UE_LOG(LogTemp, Error, TEXT("%s on %s has no CollisionObject - CollisionShape is None."),
			*GetName(), *GetNameSafe(GetOwner()));
	}
#endif

	// Server only. The balance is decided there and replicated; a client measuring its own slightly
	// different speed could only disagree with it.
	const bool bTrackForBalance = NeedsVelocityTracking() && GetOwner() && GetOwner()->HasAuthority();
	if (bTrackForBalance)
	{
		if (CollisionObject)
		{
			LastShapeTransform = CollisionObject->GetComponentTransform();
		}

		// The volume's pose is downstream of a skeletal mesh: a blade hangs off a hand socket. Component
		// tick order is not otherwise constrained, so without this the velocity can be measured against a
		// pose that has not been evaluated yet - which at 15 m/s is half a metre of nonsense.
		if (const ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner() ? GetOwner()->GetOwner() : nullptr))
		{
			if (USkeletalMeshComponent* Mesh = OwnerCharacter->GetMesh())
			{
				AddTickPrerequisiteComponent(Mesh);
			}
		}

		SetComponentTickEnabled(true);
	}

	// So a combat component that came up after this volume starts from a known value rather than from
	// whatever it was constructed with.
	ReportContact();
}

void UHexenCollisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// The whole of the per-frame cost of this system: one transform read and one subtraction, on blades
	// only, on the server only.
	if (!CollisionObject)
	{
		return;
	}

	const FTransform Current = CollisionObject->GetComponentTransform();
	const double SafeDelta = FMath::Max<double>(DeltaTime, KINDA_SMALL_NUMBER);
	VolumeVelocity = (Current.GetLocation() - LastShapeTransform.GetLocation()) / SafeDelta;

	// How the shape turned, as an axis and an angle over the same step. Needed because a blade is swung,
	// not carried: without it the tip and the hilt are both reported as moving at the speed of the middle.
	// The shortest arc, so that a turn of 350 degrees one way is read as 10 degrees the other - which is
	// what it is.
	FQuat Turn = Current.GetRotation() * LastShapeTransform.GetRotation().Inverse();
	Turn.Normalize();
	Turn.EnforceShortestArcWith(FQuat::Identity);
	FVector TurnAxis = FVector::ZeroVector;
	double TurnAngle = 0.0;
	Turn.ToAxisAndAngle(TurnAxis, TurnAngle);
	VolumeAngularVelocity = TurnAxis * (TurnAngle / SafeDelta);

	LastShapeTransform = Current;
}

FVector UHexenCollisionComponent::GetVelocityAtPoint(const FVector& WorldPoint) const
{
	// Rigid body, so every point of it moves with the centre plus the turn about the centre. Exact, not
	// an approximation - a blade does not bend.
	if (!CollisionObject)
	{
		return VolumeVelocity;
	}

	return VolumeVelocity + FVector::CrossProduct(VolumeAngularVelocity, WorldPoint - CollisionObject->GetComponentLocation());
}

float UHexenCollisionComponent::GetContactResistanceAt(const FVector& PivotWorld, const FVector& AtWorldPoint) const
{
	// A force at r from the pivot turns the volume by r/I, so the mass it behaves as if it had there is
	// I/r²: enormous near the hand, slight at the tip. This one line is the whole of forte and foible -
	// there is no separate leverage term anywhere, and none is wanted.
	const double Lever = FMath::Max(FVector::Dist(PivotWorld, AtWorldPoint), MinLeverArm);
	return static_cast<float>(double(GetContactInertia()) / (Lever * Lever));
}

float UHexenCollisionComponent::GetYieldFractionAgainst(const UHexenCollisionComponent* Other, const FVector& MyPivot, const FVector& MyPoint, const FVector& TheirPivot, const FVector& TheirPoint) const
{
	const float Mine = GetContactResistanceAt(MyPivot, MyPoint);
	const float Theirs = Other ? Other->GetContactResistanceAt(TheirPivot, TheirPoint) : 0.f;
	const float Total = Mine + Theirs;

	// The impulse the two exchange is equal and opposite, and the speed it buys each is that impulse over
	// its own effective mass - so what each gives way by goes as the OTHER's mass over the two together.
	return (Total > KINDA_SMALL_NUMBER) ? (Theirs / Total) : 0.5f;
}

FVector UHexenCollisionComponent::GetPivotWorld() const
{
	// The weapon actor is snapped to a hand socket when it is equipped, so its origin is the grip. Taken
	// from there rather than from the shape because the shape's own offset is a content setting that has
	// been wrong before, while the hand is where the hand is.
	return GetOwner() ? GetOwner()->GetActorLocation() : GetComponentLocation();
}

void UHexenCollisionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Destroyed, dropped or unequipped mid-contact. Nothing will ever raise an EndOverlap for this volume
	// now, so without this the bones stay out of their animation for the rest of the match.
	if (bInContact)
	{
		bInContact = false;
		ReportContact();
	}

	GVolumeRegistry.RemoveAll([this](const TWeakObjectPtr<UHexenCollisionComponent>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == this;
	});

	Super::EndPlay(EndPlayReason);
}

void UHexenCollisionComponent::HandleShapeBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!OtherComp)
	{
		return;
	}

	if (ShouldIgnoreOverlap(OtherComp, OtherActor))
	{
		return;
	}

	// Deliberately not gated on authority. The colour is for whoever is looking at the screen, and in
	// PIE that is usually the client window.
	OverlappingShapes.Add(OtherComp);
	RefreshShapeColor();
	UpdateContactState();
}

bool UHexenCollisionComponent::ShouldIgnoreOverlap(UPrimitiveComponent* OtherComp, AActor* OtherActor)
{
	UHexenCollisionComponent* OtherVolume = OtherComp ? Cast<UHexenCollisionComponent>(OtherComp->GetAttachParent()) : nullptr;
	if (!OtherVolume)
	{
		// Not one of ours. Cannot currently happen, since the shape ignores every channel but its own,
		// but the day something else is put on that channel this is the line that keeps it out.
		return true;
	}

	// Anything belonging to the same fighter - an off-hand weapon, a body hitbox, or a duplicate left
	// behind by a double equip - would otherwise read as a permanent self-collision that never ends, and
	// so never releases the arm.
	//
	// Compared by which fighter each volume resolves to, NOT by actor ownership. Ownership is set some
	// time after a weapon is spawned and can fail to be set at all - the logs carried "No owning
	// connection for actor ... SetActiveWeapon will not be processed" - and an owner-based test then
	// silently lets a fighter's own duplicate weapon through as an enemy blade. Two capsules on one
	// socket sit in exactly the same place, which also makes the separation direction degenerate, so the
	// failure arrives disguised as bad geometry rather than as bad filtering.
	const UHexenCombatComponent* MyFighter = GetCombatComponent();
	const UHexenCombatComponent* TheirFighter = OtherVolume->GetCombatComponent();

	if (MyFighter && MyFighter == TheirFighter)
	{
		return true;
	}

	// Neither side resolves to a fighter - ownership is all there is to go on.
	if (!MyFighter && !TheirFighter)
	{
		AActor* const Wielder = GetOwner() ? GetOwner()->GetOwner() : nullptr;
		if (Wielder && OtherActor && OtherActor->GetOwner() == Wielder)
		{
			return true;
		}
	}

	return false;
}

void UHexenCollisionComponent::HandleShapeEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	if (!OtherComp)
	{
		return;
	}

	OverlappingShapes.Remove(OtherComp);
	RefreshShapeColor();
	UpdateContactState();
}

bool UHexenCollisionComponent::IsOverlapping() const
{
	// Validity is checked here rather than trusting Num(), because a partner torn down without an
	// EndOverlap leaves a dead entry behind, and a dead entry would hold the arm out of the animation
	// for the rest of the match.
	for (const TWeakObjectPtr<UPrimitiveComponent>& Shape : OverlappingShapes)
	{
		if (Shape.IsValid())
		{
			return true;
		}
	}
	return false;
}

bool UHexenCollisionComponent::IsOverlappingVolume(const UHexenCollisionComponent* Other) const
{
	return Other && Other->CollisionObject && OverlappingShapes.Contains(TWeakObjectPtr<UPrimitiveComponent>(Other->CollisionObject));
}

void UHexenCollisionComponent::UpdateContactState()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// Measured before the state flips, so that when the notify fires - here and on every client - the
	// point and the flag describe the same moment. Only on the way IN: a contact that is ending has no
	// point worth measuring, and overwriting it would replicate a number nobody may read.
	const bool bNowInContact = IsOverlapping();
	if (bNowInContact && !bInContact)
	{
		for (const TWeakObjectPtr<UPrimitiveComponent>& Shape : OverlappingShapes)
		{
			if (!Shape.IsValid())
			{
				continue;
			}

			FVector Measured;
			if (!ComputeContactPoint(Shape.Get(), Measured))
			{
				continue;
			}

			ContactPointWorld = Measured;
			ContactPartnerShape = Shape;

			// Once per contact, after the partner shape is pinned - the closing speed is measured along
			// the way out of that particular shape. Still inert as far as the picture goes: nothing
			// consumes ContactYieldFraction yet, so this can only add [CONTACT] lines and two replicated
			// floats, and cannot be the cause of anything seen on screen.
			MeasureContactBalance(Cast<UHexenCollisionComponent>(Shape->GetAttachParent()), Measured);
			break;
		}
	}

	SetInContact(bNowInContact);
}

void UHexenCollisionComponent::SetInContact(bool bNewInContact)
{
	if (bInContact == bNewInContact)
	{
		return;
	}

	bInContact = bNewInContact;

	// A replication notify does not fire on the machine that did the writing, so the server calls it.
	// One code path for both sides.
	OnRep_InContact();
}

void UHexenCollisionComponent::OnRep_InContact()
{
	ReportContact();
}

void UHexenCollisionComponent::MeasureContactBalance(const UHexenCollisionComponent* OtherVolume, const FVector& ContactPoint)
{
	ContactYieldFraction = 0.f;
	ContactClosingSpeed = 0.f;

	if (!OtherVolume)
	{
		return;
	}

	// How fast the two were coming together, measured AT THE POINT THEY MET and only along the way out
	// of one another. Both halves of that matter: the speed of the contact point rather than of the
	// shape's middle, because a swung blade turns as much as it travels; and only the part along the
	// normal, because two blades sliding along one another at speed are not striking each other.
	const FVector Relative = GetVelocityAtPoint(ContactPoint) - OtherVolume->GetVelocityAtPoint(ContactPoint);
	FVector Normal = FVector::ZeroVector;
	const bool bHaveNormal = ContactPartnerShape.IsValid() && ComputeSeparationNormalAgainst(ContactPartnerShape.Get(), Normal);
	if (bHaveNormal)
	{
		// Normal points out of the other shape, so closing shows up as motion against it.
		ContactClosingSpeed = FMath::Max(0.f, static_cast<float>(-FVector::DotProduct(Relative, Normal))) / 100.f;
	}
	else
	{
		ContactClosingSpeed = static_cast<float>(Relative.Size()) / 100.f;
	}

	// Who gives way, and by how much. The impulse two bodies exchange is equal and opposite, and the
	// speed it buys each of them is that impulse over its own effective mass at the point of contact -
	// so the share each gives way by is the OTHER side's effective mass over the two together. That is
	// the whole model, and leverage is inside it rather than beside it: what resists at r from the grip
	// is I/r², which at 30 cm along a metre of blade is thirteen times what it is at the tip.
	//
	// Speed is deliberately absent. In a rigid collision the SHARE does not depend on how fast the two
	// were going - only on where they met and what stands behind each of them there. Speed says how hard
	// the clash was, which is ContactClosingSpeed and, later, damage. This is what the energy comparison
	// that used to stand here got wrong in both directions at once: it let a swing beat a guard for no
	// reason but being in motion, and it could not tell the strong part of a blade from the weak part at
	// all, because a turning blade carries the same energy at every point along its length.
	//
	// What still falls out for free: two fighters meeting blade-middle to blade-middle get a half each
	// and bind; catching a cut on the forte against a tip sends the tip away almost untouched, whoever
	// swung it; and a fighter who is simply stronger holds firmer, through the grip term, on both attack
	// and defence alike.
	//
	// This is the record of the moment they met, replicated and kept for damage. It is NOT what moves the
	// blades: a bind slides, the levers change with it, and a share fixed at the first frame would be
	// wrong by the second. The guard works the same sum out live every frame from the animated poses -
	// see UHexenCombatComponent::UpdateHexenCollisionGuard.
	const float MyResistance = GetContactResistance(ContactPoint);
	const float OtherResistance = OtherVolume->GetContactResistance(ContactPoint);
	const float TotalResistance = MyResistance + OtherResistance;

	if (TotalResistance > KINDA_SMALL_NUMBER)
	{
		ContactYieldFraction = OtherResistance / TotalResistance;
	}

	// And the throw: the same impulse carried on into velocity, so that the blade that loses the balance
	// is not merely held out of the way but sent out of it, and stays out for a moment afterwards. The
	// share above decides who gives way while the two are against each other; this decides what the
	// meeting itself does to them.
	if (bHaveNormal)
	{
		if (UHexenCombatComponent* Fighter = GetFighter())
		{
			Fighter->ApplyContactKnock(Normal, ContactClosingSpeed * 100.f, MyResistance, OtherResistance);
		}
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactBalance)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CONTACT] %s vs %s | closing %.1f m/s | lever %.0f vs %.0f cm | resist %.2f %s vs %.2f %s kg | yields %.2f"),
			*GetNameSafe(GetOwner()), *GetNameSafe(OtherVolume->GetOwner()),
			ContactClosingSpeed,
			FVector::Dist(GetPivotWorld(), ContactPoint), FVector::Dist(OtherVolume->GetPivotWorld(), ContactPoint),
			MyResistance, IsGripBraced() ? TEXT("BRACED") : TEXT("loose"),
			OtherResistance, OtherVolume->IsGripBraced() ? TEXT("BRACED") : TEXT("loose"),
			ContactYieldFraction);
	}
#endif
}

namespace
{
	/**
	 * The line down the middle of a shape, in world space - for a capsule its axis without the caps,
	 * and a single point for anything else, which is right for a sphere and serviceable for a box.
	 */
	bool GetShapeAxis(const UPrimitiveComponent* Primitive, FVector& OutStart, FVector& OutEnd, float& OutRadius)
	{
		if (!Primitive)
		{
			return false;
		}

		const FVector Center = Primitive->GetComponentLocation();
		if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Primitive))
		{
			OutRadius = Capsule->GetScaledCapsuleRadius();
			const float HalfLine = FMath::Max(0.f, Capsule->GetScaledCapsuleHalfHeight() - OutRadius);
			const FVector Axis = Primitive->GetComponentQuat().GetAxisZ();
			OutStart = Center - Axis * HalfLine;
			OutEnd = Center + Axis * HalfLine;
			return true;
		}
		if (const USphereComponent* Sphere = Cast<USphereComponent>(Primitive))
		{
			OutRadius = Sphere->GetScaledSphereRadius();
			OutStart = Center;
			OutEnd = Center;
			return true;
		}

		OutRadius = 0.f;
		OutStart = Center;
		OutEnd = Center;
		return true;
	}
}

bool UHexenCollisionComponent::GetShapeAxisWorld(FVector& OutStart, FVector& OutEnd, float& OutRadius) const
{
	return GetShapeAxis(CollisionObject, OutStart, OutEnd, OutRadius);
}

void UHexenCollisionComponent::GetGuardedVolumes(const UWorld* World, TArray<UHexenCollisionComponent*>& OutVolumes)
{
	OutVolumes.Reset();
	for (const TWeakObjectPtr<UHexenCollisionComponent>& Entry : GVolumeRegistry)
	{
		UHexenCollisionComponent* Volume = Entry.Get();
		if (Volume && Volume->GetWorld() == World && Volume->IsA<UWeaponCollisionComponent>() && Volume->CollisionObject)
		{
			OutVolumes.Add(Volume);
		}
	}
}

void UHexenCollisionComponent::ReportContact()
{
	if (UHexenCombatComponent* Combat = GetCombatComponent())
	{
		Combat->SetVolumeContact(this, bInContact, FVector(ContactPointWorld));
	}
}

bool UHexenCollisionComponent::ComputeAnalyticContact(const UPrimitiveComponent* OtherShape, FVector& OutPoint, FVector& OutNormal, float& OutDepth) const
{
	FVector MyStart, MyEnd, TheirStart, TheirEnd;
	float MyRadius = 0.f, TheirRadius = 0.f;
	if (!GetShapeAxis(CollisionObject, MyStart, MyEnd, MyRadius) || !GetShapeAxis(OtherShape, TheirStart, TheirEnd, TheirRadius))
	{
		return false;
	}

	FVector OnMine, OnTheirs;
	FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMine, OnTheirs);

	// Split by radius rather than halved. Half way between two spines is the touching surface only when
	// the shapes are equally thick; with a fat capsule against a thin one it sits inside the fat one.
	const float Total = MyRadius + TheirRadius;
	OutPoint = Total > KINDA_SMALL_NUMBER
		? OnMine + (OnTheirs - OnMine) * (MyRadius / Total)
		: (OnMine + OnTheirs) * 0.5f;

	OutDepth = Total - FVector::Dist(OnMine, OnTheirs);

	OutNormal = (OnMine - OnTheirs).GetSafeNormal();
	if (OutNormal.IsNearlyZero())
	{
		// The two axes cross exactly - rare, but it happens with crossed blades, and then the line between
		// the closest points has no length to take a direction from. The line between the centres keeps a
		// sane direction instead of a zero vector that would silently stop the whole mechanism.
		OutNormal = (CollisionObject->GetComponentLocation() - OtherShape->GetComponentLocation()).GetSafeNormal();
	}
	return true;
}

bool UHexenCollisionComponent::ComputeSeparationNormalAgainst(const UPrimitiveComponent* OtherShape, FVector& OutNormal) const
{
	FVector Point;
	float Depth = 0.f;
	return ComputeAnalyticContact(OtherShape, Point, OutNormal, Depth) && !OutNormal.IsNearlyZero();
}

bool UHexenCollisionComponent::ComputeSeparationNormal(FVector& OutNormal) const
{
	// The partner the bind began against, for as long as it is still touching. Every step is then taken
	// against one shape, and the direction can only change because that shape moved - never because the
	// set happened to hand back a different one this time.
	if (const UPrimitiveComponent* Partner = ContactPartnerShape.Get())
	{
		if (OverlappingShapes.Contains(ContactPartnerShape) && ComputeSeparationNormalAgainst(Partner, OutNormal))
		{
			return true;
		}
	}

	// The partner has left but something else is still inside. Better to push away from that than to
	// stop pushing, but it is a different contact and its direction owes nothing to the previous one.
	for (const TWeakObjectPtr<UPrimitiveComponent>& Shape : OverlappingShapes)
	{
		if (Shape.IsValid() && ComputeSeparationNormalAgainst(Shape.Get(), OutNormal))
		{
			return true;
		}
	}

	return false;
}

bool UHexenCollisionComponent::ComputeContactPoint(const UPrimitiveComponent* OtherShape, FVector& OutWorldPoint) const
{
	FVector Normal;
	float Depth = 0.f;
	return ComputeAnalyticContact(OtherShape, OutWorldPoint, Normal, Depth);
}

UHexenCombatComponent* UHexenCollisionComponent::GetCombatComponent() const
{
	if (CombatComponent.IsValid())
	{
		return CombatComponent.Get();
	}

	// Walk outwards until the component turns up. A body hitbox sits on the fighter itself and finds it
	// straight away; a blade sits on a weapon actor and has to go one step further - by ownership when
	// the game has set it, and by attachment when it has not yet, since a weapon is given its owner some
	// time after it is spawned into a hand.
	//
	// The step limit is against a cycle in the owner/attachment graph, which is not supposed to happen
	// and would hang here if it did.
	AActor* Actor = GetOwner();
	for (int32 Step = 0; Actor && Step < 8; ++Step)
	{
		if (UHexenCombatComponent* Found = Actor->FindComponentByClass<UHexenCombatComponent>())
		{
			CombatComponent = Found;
			break;
		}

		AActor* Next = Actor->GetOwner();
		if (!Next)
		{
			Next = Actor->GetAttachParentActor();
		}
		if (Next == Actor)
		{
			break;
		}
		Actor = Next;
	}

	return CombatComponent.Get();
}

void UHexenCollisionComponent::RefreshShapeColor()
{
#if !UE_BUILD_SHIPPING
	// Anything torn down without an EndOverlap would otherwise hold this red forever.
	for (auto It = OverlappingShapes.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}

	if (UShapeComponent* Shape = Cast<UShapeComponent>(CollisionObject))
	{
		const FColor Wanted = OverlappingShapes.Num() > 0 ? ContactShapeColor : ClearShapeColor;
		if (Shape->ShapeColor != Wanted)
		{
			Shape->ShapeColor = Wanted;
			Shape->MarkRenderStateDirty();
		}
	}
#endif
}

void UHexenCollisionComponent::OnRegister()
{
	Super::OnRegister();

	// Not on the class default object / archetypes - those have no world or owner to attach a real
	// component to, and every actual instance builds its own below anyway.
	if (GetOwner() && !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		EnsureCollisionObject();
	}
}

void UHexenCollisionComponent::EnsureCollisionObject()
{
	UClass* WantedClass = nullptr;
	switch (CollisionShape)
	{
	case EFdlCollisionShape::Sphere:  WantedClass = USphereComponent::StaticClass();  break;
	case EFdlCollisionShape::Capsule: WantedClass = UCapsuleComponent::StaticClass(); break;
	case EFdlCollisionShape::Box:     WantedClass = UBoxComponent::StaticClass();     break;
	default: break;
	}

	// Shape changed (or was cleared) - throw away what's there before building the replacement.
	if (CollisionObject && CollisionObject->GetClass() != WantedClass)
	{
		CollisionObject->DestroyComponent();
		CollisionObject = nullptr;
	}

	if (!WantedClass)
	{
		return;
	}

	if (!CollisionObject)
	{
		CollisionObject = NewObject<UPrimitiveComponent>(GetOwner(), WantedClass, NAME_None, RF_Transient);
		CollisionObject->CreationMethod = EComponentCreationMethod::Instance;
		CollisionObject->SetupAttachment(this);
		CollisionObject->RegisterComponent();
	}

	ApplyShapeSize();

	// The shape lives on its own channel that everything else ignores, and is QueryOnly, so it can never
	// push anything or be pushed. What it responds with on that channel is a detection decision rather
	// than a shape one, and is the next thing to settle.
	CollisionObject->SetCollisionObjectType(ECC_GameTraceChannel1);
	CollisionObject->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionObject->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	CollisionObject->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	// Required for the events to be raised at all, and both sides of every pair have to have it.
	CollisionObject->SetGenerateOverlapEvents(true);

#if !UE_BUILD_SHIPPING
	// Visible everywhere but a dedicated server, so the shape can actually be seen while tuning it.
	if (GetNetMode() != NM_DedicatedServer)
	{
		CollisionObject->SetHiddenInGame(false);
		CollisionObject->SetVisibility(true);
	}

	// Yellow from the moment the shape exists, so "not touching" and "not yet initialised" never look
	// the same - a shape still wearing UShapeComponent's default pink never got this far.
	if (UShapeComponent* Shape = Cast<UShapeComponent>(CollisionObject))
	{
		Shape->ShapeColor = ClearShapeColor;
	}
#endif
}

void UHexenCollisionComponent::ApplyShapeSize()
{
	if (USphereComponent* Sphere = Cast<USphereComponent>(CollisionObject))
	{
		Sphere->SetSphereRadius(SphereRadius);
	}
	else if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(CollisionObject))
	{
		Capsule->SetCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
	}
	else if (UBoxComponent* Box = Cast<UBoxComponent>(CollisionObject))
	{
		Box->SetBoxExtent(BoxExtent);
	}
}

#if WITH_EDITOR
void UHexenCollisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.Property
		? PropertyChangedEvent.Property->GetFName()
		: NAME_None;

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHexenCollisionComponent, CollisionShape))
	{
		EnsureCollisionObject();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UHexenCollisionComponent, SphereRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UHexenCollisionComponent, CapsuleRadius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UHexenCollisionComponent, CapsuleHalfHeight) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UHexenCollisionComponent, BoxExtent))
	{
		ApplyShapeSize();
	}
}
#endif
