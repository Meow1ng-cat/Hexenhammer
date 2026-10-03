// Copyright Epic Games, Inc. All Rights Reserved.

#include "HexenhammerCharacter.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "Net/UnrealNetwork.h"
#include "Hexenhammer.h"
#include "HexenCharacterMovementComponent.h"

AHexenhammerCharacter::AHexenhammerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UHexenCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 500.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character) 
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

void AHexenhammerCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() == NM_DedicatedServer)
	{
		USkeletalMeshComponent* SkeletalMesh = GetMesh();
		if (SkeletalMesh)
		{
			SkeletalMesh->SetVisibility(false);
			SkeletalMesh->bEnableUpdateRateOptimizations = false;
			SkeletalMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
	}
}

void AHexenhammerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void AHexenhammerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AHexenhammerCharacter::Move);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AHexenhammerCharacter::Look);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AHexenhammerCharacter::Look);
	}
	else
	{
		UE_LOG(LogHexenhammer, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AHexenhammerCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHexenhammerCharacter, ActiveWeapon);
	DOREPLIFETIME(AHexenhammerCharacter, Strength);
	DOREPLIFETIME(AHexenhammerCharacter, bBracing);
}

void AHexenhammerCharacter::Server_SetBracing_Implementation(bool bNewBracing)
{
	// Server RPC body - this only ever runs on the server, no HasAuthority() check needed here.
	bBracing = bNewBracing;
}

void AHexenhammerCharacter::SetStrength(int32 NewStrength)
{
	if (!HasAuthority())
	{
		UE_LOG(LogHexenhammer, Warning, TEXT("SetStrength: %s is not the server's copy of this fighter - strength is the server's to set. Ignored."), *GetName());
		return;
	}

	const int32 Clamped = FMath::Max(1, NewStrength);
	if (Clamped != NewStrength)
	{
		// Loudly, not silently: a characteristic of zero or less is a mistake somewhere upstream, and
		// quietly reading it as one would hide it.
		UE_LOG(LogHexenhammer, Warning, TEXT("SetStrength: %d is not a characteristic - strength is whole and positive. Using %d on %s."), NewStrength, Clamped, *GetName());
	}

	Strength = Clamped;
}

float AHexenhammerCharacter::GetStrengthFactor() const
{
	const int32 PointsOverReference = GetStrength() - FMath::Max(1, StrengthReference);

	// Floored above zero rather than at zero: everything downstream multiplies by this, and a factor of
	// zero would not read as "very weak" anywhere - it would read as a swing that never plays and a
	// blade with nothing behind it.
	return FMath::Max(0.01f, 1.f + PointsOverReference * StrengthPerPoint);
}

float AHexenhammerCharacter::GetAttackPlayRate() const
{
	// A rate of zero would stop the swing where it stands and never finish it - and a montage standing
	// still is exactly what a guard hold looks like, so it would read as the contact mechanic having
	// seized rather than as a bad number. A negative one would play the swing backwards. Neither is
	// allowed through, whatever is typed into the limits.
	const float Floor = FMath::Max(0.01f, MinAttackPlayRate);
	const float Ceiling = FMath::Max(Floor, MaxAttackPlayRate);

	return FMath::Clamp(GetStrengthFactor(), Floor, Ceiling);
}

void AHexenhammerCharacter::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	// route the input
	DoMove(MovementVector.X, MovementVector.Y);
}

void AHexenhammerCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	// route the input
	DoLook(LookAxisVector.X, LookAxisVector.Y);
}

void AHexenhammerCharacter::DoMove(float Right, float Forward)
{
	if (GetController() != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = GetController()->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);

		// get right vector 
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement 
		AddMovementInput(ForwardDirection, Forward);
		AddMovementInput(RightDirection, Right);
	}
}

void AHexenhammerCharacter::DoLook(float Yaw, float Pitch)
{
	if (GetController() != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void AHexenhammerCharacter::DoJumpStart()
{
	// signal the character to jump
	Jump();
}

void AHexenhammerCharacter::DoJumpEnd()
{
	// signal the character to stop jumping
	StopJumping();
}

void AHexenhammerCharacter::SetActiveWeapon_Implementation(TSubclassOf<AHexenWeapon> WeaponClass)
{
	// Server RPC body - this only ever runs on the server, no HasAuthority() check needed here.

	if (!WeaponClass)
	{
		// AHexenWeapon isn't Abstract, so SpawnActor<AHexenWeapon>(nullptr, ...) would silently spawn a
		// bare base-class weapon (no mesh, no per-weapon Blueprint setup) instead of failing loudly.
		UE_LOG(LogTemp, Warning, TEXT("SetActiveWeapon: no weapon class given, ignoring."));
		return;
	}

	if (ActiveWeapon)
	{
		ActiveWeapon->Destroy();
		ActiveWeapon = nullptr;
	}

	// Вычисляем трансформацию для спавна (обычно в нулевых координатах, так как сразу прикрепим)
	FTransform SpawnTransform = FTransform::Identity;

	// Спавним актор оружия
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = this;             
	SpawnParams.Instigator = GetInstigator(); 

	AHexenWeapon* NewWeapon = GetWorld()->SpawnActor<AHexenWeapon>(WeaponClass, SpawnTransform, SpawnParams);
	if (!NewWeapon)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to spawn weapon!"));
		return;
	}

	if (USkeletalMeshComponent* SkeletalMesh = GetMesh())
	{
		// AttachToComponent with an empty/nonexistent socket name doesn't fail - it just attaches at the
		// mesh component's own origin instead of the hand, which reads as "spawned but not attached
		// properly". Log clearly instead of leaving that to be a silent mystery.
		if (RightWeaponSocket.IsNone())
		{
			UE_LOG(LogTemp, Warning, TEXT("SetActiveWeapon: RightWeaponSocket is not set on %s - weapon will attach at the mesh's origin, not a hand socket."), *GetName());
		}
		else if (!SkeletalMesh->DoesSocketExist(RightWeaponSocket))
		{
			UE_LOG(LogTemp, Warning, TEXT("SetActiveWeapon: socket '%s' does not exist on %s's mesh - weapon will attach at the mesh's origin instead."), *RightWeaponSocket.ToString(), *GetName());
		}

		NewWeapon->AttachToComponent(SkeletalMesh, FAttachmentTransformRules::SnapToTargetIncludingScale, RightWeaponSocket);
		NewWeapon->SetSocket(RightWeaponSocket);
	}

	ActiveWeapon = NewWeapon;
}
