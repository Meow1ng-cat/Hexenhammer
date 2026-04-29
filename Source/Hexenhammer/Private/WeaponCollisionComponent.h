// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/ArrowComponent.h"
#include "BodyCollisionComponent.h"
#include "HexenCollisionComponent.h"
#include "DrawDebugHelpers.h"
#include "HexenWeapon.h"
#include "HexenUtils.h"
#include "WeaponCollisionComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class UWeaponCollisionComponent : public UHexenCollisionComponent
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    int32 Mass;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    int32 Sharpness = 50.f;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    int32 Hardness = 50.f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    bool IsDoubleEdged = false;

protected:
    AHexenWeapon* WeaponActor = nullptr;
public:
    UWeaponCollisionComponent();
    //virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
    virtual void BeginPlay() override;
    void OnHexenBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
        bool bFromSweep, const FHitResult& HitResult) override;
};

