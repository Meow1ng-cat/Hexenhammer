#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HexenUtils.h"
#include "HexenWeapon.generated.h"

class USkeletalMeshComponent;
class UWeaponCollisionComponent;

UCLASS()
class  AHexenWeapon : public AActor
{
    GENERATED_BODY()

protected:
    /**
     * Bare scene component used as the actor's root, i.e. the thing that actually gets attached to the
     * character's hand socket. It exists purely so WeaponMesh does NOT have to be the root: a root
     * component's transform is the actor's transform, so Unreal greys it out and it can't be offset in
     * the Blueprint editor. With this in between, WeaponMesh's location/rotation stay editable, which is
     * how you line the weapon up with the hand without having to re-pivot the mesh asset itself.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    USceneComponent* WeaponRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    USkeletalMeshComponent* WeaponMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UWeaponCollisionComponent* WeaponCollision;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
    FName AttachedSocketName;

    /** Socket/bone on WeaponMesh that WeaponCollision (the blade's trace shape) is attached to. Set per weapon Blueprint - a hammer's socket sits somewhere very different from a dagger's. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
    FName BladeSocketName;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
    EWeaponGripType GripType;

public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    FVector TipOffset;

public:
    AHexenWeapon();
    virtual void PostInitializeComponents() override;
    virtual void BeginPlay() override;

    void SetSocket(FName SocketName) { AttachedSocketName = SocketName; }

    UFUNCTION(BlueprintCallable, Category = "Components")
    UWeaponCollisionComponent* GetWeaponCollision() const { return WeaponCollision; }
};