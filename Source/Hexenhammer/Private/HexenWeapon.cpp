#include "HexenWeapon.h"
#include "Components/SkeletalMeshComponent.h"
#include "WeaponCollisionComponent.h"

AHexenWeapon::AHexenWeapon()
{
    PrimaryActorTick.bCanEverTick = false;

    bReplicates = true;
    // A weapon is always attached to a socket, never moves independently - so its position must be
    // synced purely via attachment replication (parent + socket + relative transform), NOT via
    // ReplicatedMovement's independent absolute-position replication. Having both enabled is a classic
    // source of "spawns in the right place, then floats in place instead of following the owner" bugs:
    // a stale ReplicatedMovement update (from the moment it was spawned at FTransform::Identity, before
    // being attached) can race with/override the attachment-driven position on clients.
    SetReplicateMovement(false);

    bNetLoadOnClient = true; // Не уверен что эти два параметра нужны в true
    bNetUseOwnerRelevancy = true;

    SetActorEnableCollision(true);

    // See WeaponRoot's comment: the root is deliberately NOT the mesh, so the mesh stays movable in the editor.
    WeaponRoot = CreateDefaultSubobject<USceneComponent>(TEXT("WeaponRoot"));
    RootComponent = WeaponRoot;

    WeaponMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WeaponMesh"));
    WeaponMesh->SetupAttachment(WeaponRoot);
    // The mesh is just for rendering - hit detection is WeaponCollision's job (see UHexenCollisionComponent),
    // so there's no reason for the render mesh itself to collide with anything.
    WeaponMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    WeaponCollision = CreateDefaultSubobject<UWeaponCollisionComponent>(TEXT("WeaponCollision"));
    // Attached to the mesh root for now; re-attached to the correct blade socket in
    // PostInitializeComponents() once BladeSocketName's per-Blueprint default value is actually available.
    WeaponCollision->SetupAttachment(WeaponMesh);
}

void AHexenWeapon::BeginPlay()
{
    Super::BeginPlay();

    // A child component normally follows its parent rigidly - but a few things override that and leave
    // the mesh floating in place while the rest of the weapon follows the hand. Rather than leaving that
    // to be guessed at, name whichever one is in play. Runs here (not in the constructor) so Blueprint
    // defaults, which are applied after the C++ constructor, are already in effect.
    if (WeaponMesh)
    {
        // WeaponMesh is intentionally not the root (see WeaponRoot) - what matters is that it's somewhere
        // under it, since AActor::AttachToComponent only attaches the RootComponent to the hand socket.
        // Anything outside that branch of the hierarchy simply stays where it was spawned.
        if (WeaponMesh != GetRootComponent() && !WeaponMesh->IsAttachedTo(GetRootComponent()))
        {
            UE_LOG(LogTemp, Warning, TEXT("%s: WeaponMesh is not attached under the RootComponent ('%s') - it won't follow the character."),
                *GetClass()->GetName(), *GetNameSafe(GetRootComponent()));
        }
        if (WeaponMesh->IsSimulatingPhysics())
        {
            UE_LOG(LogTemp, Warning, TEXT("%s: WeaponMesh is simulating physics - the physics solver drives its world transform, so it will NOT follow the socket it's attached to."),
                *GetClass()->GetName());
        }
        if (WeaponMesh->IsUsingAbsoluteLocation() || WeaponMesh->IsUsingAbsoluteRotation())
        {
            UE_LOG(LogTemp, Warning, TEXT("%s: WeaponMesh has absolute location/rotation enabled - it ignores its parent's transform by design."),
                *GetClass()->GetName());
        }
    }
}

void AHexenWeapon::PostInitializeComponents()
{
    Super::PostInitializeComponents();

    if (WeaponCollision && WeaponMesh)
    {
        if (BladeSocketName.IsNone())
        {
            UE_LOG(LogTemp, Warning, TEXT("%s: BladeSocketName is not set - WeaponCollision stays attached at WeaponMesh's origin instead of the blade."), *GetClass()->GetName());
        }
        else if (!WeaponMesh->DoesSocketExist(BladeSocketName))
        {
            UE_LOG(LogTemp, Warning, TEXT("%s: socket '%s' does not exist on WeaponMesh - WeaponCollision stays attached at its origin instead."), *GetClass()->GetName(), *BladeSocketName.ToString());
        }
        else
        {
            WeaponCollision->AttachToComponent(WeaponMesh, FAttachmentTransformRules::KeepRelativeTransform, BladeSocketName);
        }
    }
}
