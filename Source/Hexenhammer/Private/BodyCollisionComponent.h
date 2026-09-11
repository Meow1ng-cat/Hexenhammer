// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "HexenCollisionComponent.h"
#include "HexenUtils.h"
#include "BodyCollisionComponent.generated.h"

/**
 * A hitbox on a character. Purely a passive target: it never traces itself (bIsTracing stays
 * false), it's found by the attacking UWeaponCollisionComponent's own sweep instead. See
 * UWeaponCollisionComponent::ResolveBodyHit for the actual damage decision.
 */
UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class UBodyCollisionComponent : public UHexenCollisionComponent
{
	GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    float Hardness = 1.f;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    float Resilience = 50.f; //Resistance to Cutting. If HitPircing < Resilience, hit is not valid.
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    float Durabulity = 50.f; //Resistance to Mauling. Bone breaking stacks count as Damage by Mauling/Durabulity

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
    bool IsAlive = true;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
    bool IsVital = false;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
    EBodyType BodyType = EBodyType::None;


public:
    UBodyCollisionComponent();

    UFUNCTION(BlueprintCallable, Category = "Damage")
    void HandleDamage(const EDamageType DamageType);

protected:
    virtual void BeginPlay() override;

    virtual void ApplyBleeding();
    virtual void ApplyBoneBreak();
    virtual void ApplyBurning();
    virtual void ApplyFreezing();
};
