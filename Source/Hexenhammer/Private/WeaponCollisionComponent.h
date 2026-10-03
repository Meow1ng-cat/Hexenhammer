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
     * How much the grip adds to what the blade resists with, as a share of the blade's own figure, at
     * ordinary strength.
     *
     * A blade held firmly cannot turn about its grip freely - it has to drag some of the arm and body
     * with it, and that is what a guard resists with when it is not moving at all. 1 means an ordinary
     * fighter's grip doubles the blade; 0 means a blade nobody is really holding, free to be knocked
     * aside by anything.
     *
     * Not a defender's property: it weighs on whichever side is bracing, attacking or defending alike.
     * But it only weighs while the fighter IS bracing - a relaxed hand adds nothing, see
     * AHexenhammerCharacter::bBracing. Strength scales it, through the same factor that scales the
     * swing's speed, so that a fighter's strength enters attack and defence by exactly the same amount
     * and two equally strong fighters stay an equal match at any strength.
     *
     * Four, so that a braced grip is five times the blade's own figure. One - the first value here - made
     * a braced blade only twice as hard to move as a relaxed one, which is not mal pare: the shares came
     * out 0.33 against 0.67, and the fighter who won still gave way by a third of the overlap. Five to one
     * puts him at a sixth. Physically it is the right order too: the blade's inertia about the grip is
     * about 0.39 kg m², and two braced arms with some torso behind them are several times that, where a
     * relaxed wrist is almost nothing.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCollision")
    float GripInertiaShare = 4.f;

    virtual float GetContactInertia() const override;
    virtual float GetContactMass() const override { return Mass; }
    virtual bool IsGripBraced() const override;

protected:
    /** What the holder's grip puts behind this blade: their strength while bracing, zero while relaxed or when nobody is holding it - see AHexenhammerCharacter::GetGripFactor. */
    float GetHolderGripFactor() const;

protected:
    /** A blade is the moving half of every clash, so it is the one volume kind that has to know its own speed. */
    virtual bool NeedsVelocityTracking() const override { return true; }
};
