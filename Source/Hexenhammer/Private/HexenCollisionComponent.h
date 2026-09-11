// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "HexenUtils.h"
#include "Engine/NetSerialization.h"
#include "HexenCollisionComponent.generated.h"

class UHexenCombatComponent;

/**
 * Base class for melee collision volumes (weapon blades, body hitboxes).
 *
 * Owns the query shape, notices contact, and reports it. All of that is here rather than in a weapon
 * subclass because none of it is about weapons: two bodies meeting want the same thing to happen as a
 * blade meeting a blade. What a contact then MEANS - damage, a bind, a stagger - is what subclasses are
 * for.
 */
UCLASS(Abstract)
class UHexenCollisionComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    EFdlCollisionShape CollisionShape = EFdlCollisionShape::None;

    /**
     * The actual query shape, built to match CollisionShape.
     *
     * Transient on purpose: it is rebuilt from scratch in OnRegister() every time this component comes
     * up, rather than being saved into the asset. Serialising a sub-object that was spawned by an
     * editor-only PostEditChangeProperty is what made the shape survive until the editor was restarted
     * and then quietly vanish - and a component that only exists if you happened to touch a dropdown is
     * not something the rest of the system can rely on.
     */
    UPROPERTY(Transient)
    UPrimitiveComponent* CollisionObject = nullptr;

    // Size of CollisionObject - the only thing that decides how big the volume actually is. Edited here
    // instead of having to find and select the auto-created SphereCollision/CapsuleCollision/BoxCollision
    // sub-component in the component hierarchy. Only the one matching CollisionShape does anything.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision", meta = (EditCondition = "CollisionShape == EFdlCollisionShape::Sphere", EditConditionHides))
    float SphereRadius = 50.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision", meta = (EditCondition = "CollisionShape == EFdlCollisionShape::Capsule", EditConditionHides))
    float CapsuleRadius = 15.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision", meta = (EditCondition = "CollisionShape == EFdlCollisionShape::Capsule", EditConditionHides))
    float CapsuleHalfHeight = 50.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision", meta = (EditCondition = "CollisionShape == EFdlCollisionShape::Box", EditConditionHides))
    FVector BoxExtent = FVector(50.f, 50.f, 5.f);

    /**
     * What the volume is drawn in while nothing is touching it, and what it turns while something is.
     *
     * Painted onto the shape component itself rather than drawn as debug lines over it, so it costs
     * nothing per frame, needs no debug flag, and cannot disagree with the shape being queried - it is
     * that shape. Which makes it the cheapest possible answer to the first question worth asking of any
     * detection scheme: is the event arriving at all.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision|Debug")
    FColor ClearShapeColor = FColor::Yellow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision|Debug")
    FColor ContactShapeColor = FColor::Red;

    /** One line per contact with what the energy balance decided. Server-side, since that is where it is decided. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision|Debug")
    bool bLogContactBalance = false;

public:
    UHexenCollisionComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** How fast the query shape is moving, in cm/s, measured between the last two poses. Zero on volumes that do not track it - see NeedsVelocityTracking. */
    FVector GetVolumeVelocity() const { return VolumeVelocity; }

    /**
     * What this volume brings to a contact - one half of the energy balance that decides which of two
     * blades gives way.
     *
     * Two terms, and the second is the one a purely kinetic comparison was missing: a blade standing
     * still in a guard has no kinetic energy at all, so it would offer no resistance whatever and every
     * strike would sail through a stance untouched. The hold term does not depend on motion, so a guard
     * resists by being held; the kinetic term then adds whatever the blade is actually carrying on top.
     *
     * Zero on the base, which is the honest answer for a body hitbox: what a struck limb resists with is
     * not modelled yet, and pretending otherwise would put a made-up number into a real balance.
     */
    virtual float GetContactPush() const { return 0.f; }

    /** This volume's mass. Zero on the base for the same reason GetContactPush() is - a limb's mass is not modelled yet. */
    virtual float GetContactMass() const { return 0.f; }

    /** What the energy balance worked out when the current contact began. Meaningless while nothing is touching. */
    float GetContactClosingSpeed() const { return ContactClosingSpeed; }
    float GetContactYieldFraction() const { return ContactYieldFraction; }

    /** Whether anything is currently inside this volume, on this machine. */
    UFUNCTION(BlueprintPure, Category = "DamageCollision")
    bool IsOverlapping() const;

    /** Rebuilds CollisionObject to match CollisionShape (and configures its collision settings). Runs on registration, and again in-editor whenever CollisionShape changes. */
    virtual void OnRegister() override;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /**
     * Whether this volume needs to know how fast it is moving.
     *
     * False by default, and the default is what keeps this cheap: a fighter carries one or two blades
     * and dozens of body hitboxes, and only the blades are ever the moving half of a clash. Ticking is
     * also server-only - the balance is decided there and replicated, so a client measuring its own
     * slightly different speed would only be able to disagree.
     */
    virtual bool NeedsVelocityTracking() const { return false; }

    /** The shape's pose on the previous tick, and the velocity derived from it. */
    FTransform LastShapeTransform;
    FVector VolumeVelocity = FVector::ZeroVector;

    /** Bound to CollisionObject's overlap delegates in BeginPlay. */
    UFUNCTION()
    void HandleShapeBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

    UFUNCTION()
    void HandleShapeEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

    /** Whether an overlap is with something this volume has any business reacting to. Filters out the same fighter's own gear. */
    bool ShouldIgnoreOverlap(UPrimitiveComponent* OtherComp, AActor* OtherActor);

    /**
     * Everything currently inside this volume.
     *
     * A set rather than a counter, so a repeated Begin cannot leave the volume stuck in contact, and so
     * a second partner arriving needs no special handling while a first one leaving cannot clear a
     * contact the second is still holding.
     */
    TSet<TWeakObjectPtr<UPrimitiveComponent>> OverlappingShapes;

    /** Repaints CollisionObject from OverlappingShapes, dropping anything that has gone away. */
    void RefreshShapeColor();

    /**
     * Whether this volume is in contact, as the server sees it.
     *
     * Replicated with a notify rather than polled: it changes twice per clash, and having every machine
     * ask "is it still true" sixty times a second to find out is work spent on an answer that almost
     * never differs from the last one. The notify is the signal.
     *
     * Server-authoritative because this is not a visual. What the body is allowed to do during a contact
     * is the same question as whether a hit lands, and those two must not be able to disagree.
     */
    UPROPERTY(ReplicatedUsing = OnRep_InContact)
    bool bInContact = false;

    /**
     * Server-side. Re-reads the contact state from OverlappingShapes after an overlap event.
     *
     * Asked of the set rather than inferred from which event fired, so several partners at once need no
     * counting: what matters is whether anything at all is still inside, and the set already knows.
     * Clients raise the same events and deliberately do nothing with them - one machine decides, and it
     * is the one that will later decide whether a hit landed.
     */
    void UpdateContactState();

    /**
     * Where the two shapes are touching, in world space, as measured when the contact began.
     *
     * Replicated alongside bInContact and in the same bunch, so a client reading one on its notify has
     * the other. Meaningless while bInContact is false, and deliberately not cleared - a stale point
     * nobody is allowed to read costs nothing, and clearing it would be one more thing to replicate.
     */
    UPROPERTY(Replicated)
    FVector_NetQuantize100 ContactPointWorld;

    /**
     * The share of this contact that THIS volume gives way by, from the energy balance, in 0..1.
     *
     * 1 means being swept aside completely, 0 means not moving at all. Mal pare needs no special case
     * anywhere: a blow far outweighing the other side's hold drives the other side's share towards 1 and
     * its own towards 0, and simply ploughs on through. Two fighters evenly matched get a half each and
     * the blades hold one another.
     *
     * A share, not a distance. Turning it into a displacement needs a depth to multiply it by, and depth
     * is what UpdatePenetration used to measure - still parked.
     */
    UPROPERTY(Replicated)
    float ContactYieldFraction = 0.f;

    /** How fast the two volumes were closing when they met, in m/s. Kept because it is the number worth reading in a log, and because damage will want it. */
    UPROPERTY(Replicated)
    float ContactClosingSpeed = 0.f;

    /**
     * The closest approach of the two shapes' axes, split between them by radius so the point lands on
     * the touching surfaces rather than half way between the two spines - which is only the same thing
     * when both shapes are the same thickness.
     *
     * Pure geometry on two segments, no query behind it. It is measured in the pose that raised the
     * event, which is already a tick's worth of travel deeper than the moment they met; correcting for
     * that needs the previous pose, and keeping the previous pose needs a tick. Left uncorrected on
     * purpose until there is a reason to pay for one.
     */
    bool ComputeContactPoint(const UPrimitiveComponent* OtherShape, FVector& OutWorldPoint) const;

public:
    /**
     * The direction to push this volume out of whatever it is currently touching, in world space.
     *
     * For two capsules this is not an approximation: the shortest way out of the overlap IS the line
     * between the closest points of their two axes, so the answer is exact and costs two segment
     * queries and a normalise. No physics query, no MTD, nothing to park.
     *
     * Recomputed from live geometry every time it is asked for, because two blades in a bind slide and
     * turn against one another and the way out genuinely moves. Returns false when nothing is touching.
     */
    bool ComputeSeparationNormal(FVector& OutNormal) const;

protected:
    /**
     * The shape this volume's current contact was measured against.
     *
     * Pinned so every step of a bind is taken against the same partner. OverlappingShapes is a set, and
     * a set's iteration order means nothing - so "the first shape in it" can be one partner on one step
     * and another on the next, and the push direction jumps between them. With two weapons stacked on
     * one socket that is not hypothetical: it is what the logs showed.
     */
    TWeakObjectPtr<UPrimitiveComponent> ContactPartnerShape;

    /** The separation direction against one specific shape. False when the two are in exactly the same place. */
    bool ComputeSeparationNormalAgainst(const UPrimitiveComponent* OtherShape, FVector& OutNormal) const;

public:

protected:

    /** Server-side. Fills ContactYieldFraction and ContactClosingSpeed from the two volumes' mass and motion. Runs once, when a contact begins. */
    void MeasureContactBalance(const UHexenCollisionComponent* OtherVolume);

    UFUNCTION()
    void OnRep_InContact();

    /** Server-side. Writes bInContact and, if it changed, tells everyone - including this machine, since a notify does not fire on the authority. */
    void SetInContact(bool bNewInContact);

    /** Tells the fighter's combat component what this volume is currently reporting. The only thing this volume does with its contact state. */
    void ReportContact();

    /** The fighter's combat component, resolved on demand and kept. Null when this volume is not on (or held by) a character carrying one. */
    UHexenCombatComponent* GetCombatComponent();

    TWeakObjectPtr<UHexenCombatComponent> CombatComponent;

    /** Creates CollisionObject if it's missing or is the wrong shape, then sizes and configures it. Safe to call repeatedly - it does nothing when the existing shape already matches. */
    virtual void EnsureCollisionObject();

    /** Pushes SphereRadius/CapsuleRadius&HalfHeight/BoxExtent onto whichever shape CollisionObject currently is. Called when it's built, and again whenever one of those size properties is edited. */
    virtual void ApplyShapeSize();

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
