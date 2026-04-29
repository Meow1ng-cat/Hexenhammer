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

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class UBodyCollisionComponent : public UHexenCollisionComponent
{
	GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    int32 Hardness = 1.f;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    int32 Resilience = 50.f; //Resistance to Cutting. If HitPircing < Resilience, hit is not valid.
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    int32 Durabulity = 50.f; //Resistance to Mauling. Bone breaking stacks count as Damage by Mauling/Durabulity

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
    bool IsAlive = true;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
    bool IsVital = false;
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
    EBodyType BodyType = EBodyType::None;


public:
    UBodyCollisionComponent();
    //virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION(BlueprintCallable, Category = "Damage")
    void HandleDamage(const EDamageType DamageType);

protected:
    virtual void BeginPlay() override;
    void OnHexenBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
        bool bFromSweep, const FHitResult& HitResult) override;

    virtual void ApplyBleeding();
    virtual void ApplyBoneBreak();
    virtual void ApplyBurning();
    virtual void ApplyFreezing();
};
