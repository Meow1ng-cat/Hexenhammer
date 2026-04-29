#include "HexenWeapon.h"

AHexenWeapon::AHexenWeapon()
{
    PrimaryActorTick.bCanEverTick = false;

    bReplicates = true;
    SetReplicateMovement(true);

    bNetLoadOnClient = true; // Не уверен что эти два параметра нужны в true
    bNetUseOwnerRelevancy = true;

    //WeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WeaponMesh"));
    //RootComponent = WeaponMesh;

    //WeaponCollision = CreateDefaultSubobject<UWeaponCollisionComponent>(TEXT("WeaponCollision"));
    //WeaponCollision->SetupAttachment(WeaponMesh);
}