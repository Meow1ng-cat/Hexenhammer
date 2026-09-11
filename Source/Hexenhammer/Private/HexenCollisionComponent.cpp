// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenCollisionComponent.h"
#include "HexenCombatComponent.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "Net/UnrealNetwork.h"

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
}

void UHexenCollisionComponent::BeginPlay()
{
	Super::BeginPlay();

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
	if (NeedsVelocityTracking() && GetOwner() && GetOwner()->HasAuthority())
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
	VolumeVelocity = (Current.GetLocation() - LastShapeTransform.GetLocation()) / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
	LastShapeTransform = Current;
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
			// PARKED while the rig is being debugged. It is inert either way - nothing consumes
			// ContactYieldFraction yet - so it cannot be the cause of anything being seen on screen.
			//MeasureContactBalance(Cast<UHexenCollisionComponent>(Shape->GetAttachParent()));
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

void UHexenCollisionComponent::MeasureContactBalance(const UHexenCollisionComponent* OtherVolume)
{
	ContactYieldFraction = 0.f;
	ContactClosingSpeed = 0.f;

	if (!OtherVolume)
	{
		return;
	}

	// How fast they were coming together, along the line between them. Only the closing part counts:
	// two blades sliding along one another at speed are not striking each other.
	const FVector Relative = GetVolumeVelocity() - OtherVolume->GetVolumeVelocity();
	ContactClosingSpeed = Relative.Size() / 100.f; // cm/s -> m/s

	// The whole model, and it needs no branch anywhere. Each side brings its mass times the square of a
	// speed - its own, plus the speed a freely swung blade would need to carry the same energy as the
	// grip holding it. The share each gives way by is the other side's push over the total.
	//
	// What falls out for free: a blow far outweighing the other side's hold drives its own share towards
	// zero and ploughs on through, which is mal pare; two fighters evenly matched get a half each and the
	// blades hold one another; and a parry swung INTO the strike adds its kinetic term to its hold and
	// resists harder than merely standing would, so an active defence beats a passive one.
	const float MyPush = GetContactPush();
	const float OtherPush = OtherVolume->GetContactPush();
	const float TotalPush = MyPush + OtherPush;

	if (TotalPush > KINDA_SMALL_NUMBER)
	{
		ContactYieldFraction = OtherPush / TotalPush;
	}

#if !UE_BUILD_SHIPPING
	if (bLogContactBalance)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CONTACT] %s vs %s | closing %.1f m/s | push %.0f vs %.0f | yields %.2f"),
			*GetNameSafe(GetOwner()), *GetNameSafe(OtherVolume->GetOwner()),
			ContactClosingSpeed, MyPush, OtherPush, ContactYieldFraction);
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

void UHexenCollisionComponent::ReportContact()
{
	if (UHexenCombatComponent* Combat = GetCombatComponent())
	{
		Combat->SetVolumeContact(this, bInContact, FVector(ContactPointWorld));
	}
}

bool UHexenCollisionComponent::ComputeSeparationNormalAgainst(const UPrimitiveComponent* OtherShape, FVector& OutNormal) const
{
	FVector MyStart, MyEnd, TheirStart, TheirEnd;
	float MyRadius = 0.f, TheirRadius = 0.f;
	if (!GetShapeAxis(CollisionObject, MyStart, MyEnd, MyRadius) || !GetShapeAxis(OtherShape, TheirStart, TheirEnd, TheirRadius))
	{
		return false;
	}

	FVector OnMine, OnTheirs;
	FMath::SegmentDistToSegmentSafe(MyStart, MyEnd, TheirStart, TheirEnd, OnMine, OnTheirs);

	OutNormal = (OnMine - OnTheirs).GetSafeNormal();
	if (!OutNormal.IsNearlyZero())
	{
		return true;
	}

	// The two axes cross exactly - rare, but it happens with crossed blades, and then the line between
	// the closest points has no length to take a direction from. The line between the centres keeps a
	// sane direction instead of a zero vector that would silently stop the whole mechanism.
	OutNormal = (CollisionObject->GetComponentLocation() - OtherShape->GetComponentLocation()).GetSafeNormal();
	return !OutNormal.IsNearlyZero();
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
	OutWorldPoint = Total > KINDA_SMALL_NUMBER
		? OnMine + (OnTheirs - OnMine) * (MyRadius / Total)
		: (OnMine + OnTheirs) * 0.5f;

	return true;
}

UHexenCombatComponent* UHexenCollisionComponent::GetCombatComponent()
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
