// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HexenCollisionComponent.h"
#include "WeaponCollisionComponent.generated.h"

/**
 * The blade's collision volume.
 *
 * Everything about noticing a contact lives in the base. What is here is what a weapon has that a body
 * part does not: a mass, a grip holding it, and therefore a side of the energy balance.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class UWeaponCollisionComponent : public UHexenCollisionComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision")
    float Mass = 1.f;

    /**
     * How firmly this blade is held to the pose the animation wants it in, expressed as the speed a
     * freely swung blade would need to carry the same energy.
     *
     * Not a defender's property - every blade is always being pulled towards its animated pose, whether
     * that pose is mid-swing or standing in a guard, so this applies to both sides of a bind. It is what
     * a guard resists with when the blade is not moving at all, and also what drags a deflected strike
     * back onto its line.
     *
     * A speed rather than an energy so it compares directly against the kinetic term with no conversion,
     * and so the number means something you can picture: at 10, a fighter holds their blade about as
     * firmly as a blade swung at 10 m/s pushes.
     *
     * A placeholder. It will come from the character's Strength once that exists.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision")
    float HoldSpeed = 10.f;

    virtual float GetContactPush() const override;
    virtual float GetContactMass() const override { return Mass; }

protected:
    /** A blade is the moving half of every clash, so it is the one volume kind that has to know its own speed. */
    virtual bool NeedsVelocityTracking() const override { return true; }
};
