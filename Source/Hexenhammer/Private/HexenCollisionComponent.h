#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "HexenUtils.h"
#include "HexenCollisionComponent.generated.h"

UCLASS(Abstract)
class UHexenCollisionComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
    EFdlCollisionShape CollisionShape = EFdlCollisionShape::None;
    UPROPERTY()
    UPrimitiveComponent* CollisionObject = nullptr;
protected:
    UPROPERTY()
    FTransform LastTransform;
    float LastDeltaTime;

public:
    UHexenCollisionComponent();
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    FTransform GetLastTransform() { return LastTransform; }
    float GetLastDeltaTime() { return LastDeltaTime; }

protected:
    virtual void BeginPlay() override;
    virtual void SetShape();
    UFUNCTION()
    virtual void OnHexenBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
        UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
        bool bFromSweep, const FHitResult& HitResult);

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};

