#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HexenUtils.h"
#include "HexenWeapon.generated.h"

UCLASS()
class  AHexenWeapon : public AActor
{
    GENERATED_BODY()

protected:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    USkeletalMeshComponent* WeaponMesh;

    /*UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    UWeaponCollisionComponent* WeaponCollision;*/

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
    FName AttachedSocketName;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon")
    EWeaponGripType GripType;

public:
    AHexenWeapon();
    void SetSocket(FName SocketName) { AttachedSocketName = SocketName; }
};