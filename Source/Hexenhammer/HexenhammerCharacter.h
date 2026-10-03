// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Logging/LogMacros.h"
#include "HexenWeapon.h"
#include "HexenhammerCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UInputAction;
struct FInputActionValue;

DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, Log, All);

/**
 *  A simple player-controllable third person character
 *  Implements a controllable orbiting camera
 */
UCLASS(abstract)
class AHexenhammerCharacter : public ACharacter
{
	GENERATED_BODY()

	/** Camera boom positioning the camera behind the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* FollowCamera;
	
protected:

	/** Jump Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* JumpAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MoveAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* LookAction;

	/** Mouse Look Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	UInputAction* MouseLookAction;

	UPROPERTY(EditAnywhere, Category = "Weapon")
	FName RightWeaponSocket;
	UPROPERTY(EditAnywhere, Category = "Weapon")
	FName LeftWeaponSocket;

	public:
	UPROPERTY(Replicated, BlueprintReadWrite, Category = "Weapon")
	AHexenWeapon* ActiveWeapon = nullptr;

public:

	/**
	 * Strength - the first of the four base characteristics (strength, agility, endurance, intelligence).
	 * The other three belong here beside it.
	 *
	 * A characteristic counts points, so it is a whole positive number by definition: 2.5 strength and
	 * -3 strength are not weak values, they are meaningless ones. The editor is clamped at 1, and every
	 * read goes through GetStrength(), which clamps again - a Blueprint default, a save or a data table
	 * can still hand over anything.
	 *
	 * The server owns it and replicates it. What it drives - the swing's speed - is NOT replicated:
	 * every machine works the same rate out of the same number, see GetAttackPlayRate().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, Category = "Attributes", meta = (ClampMin = "1", UIMin = "1"))
	int32 Strength = 10;

	/**
	 * The strength that counts as ordinary - the one whose factor is exactly one. Below it a fighter is
	 * slower and holds looser, above it faster and firmer. Ten, so that today's fighter behaves as before.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes", meta = (ClampMin = "1", UIMin = "1"))
	int32 StrengthReference = 10;

	/** What one point of strength is worth. 0.05 is five percent a point. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes", meta = (ClampMin = "0.0"))
	float StrengthPerPoint = 0.05f;

	/**
	 * Floor and ceiling for the speed, so that no amount of strength turns a swing into a frame or a
	 * standstill. These three are tuning, not state: they come from the class defaults and are the same
	 * on every machine, so they are not replicated.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes", meta = (ClampMin = "0.01"))
	float MinAttackPlayRate = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes", meta = (ClampMin = "0.01"))
	float MaxAttackPlayRate = 2.0f;

	/**
	 * Whether this fighter is bracing - holding the blade against something, rather than merely carrying it.
	 *
	 * A relaxed hand puts almost nothing behind the blade: push on it and it gives, because only the
	 * blade's own inertia is in the way. Setting a guard means tensing the arms, and then the body is
	 * behind the blade and it is hard or impossible to push through. That is the difference this turns on
	 * and off, and it is why the grip must not simply be on all the time: a blade that resists as if
	 * braced while its owner is standing about is a blade nobody can ever beat aside.
	 *
	 * The server's word, like everything a contact is decided by - set it with Server_SetBracing. A test
	 * switch for now, a key in the Blueprint, until whatever actually puts a guard up drives it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, Category = "Attributes")
	bool bBracing = false;

	/** This fighter's strength: whole, and at least one, whatever is stored. */
	UFUNCTION(BlueprintPure, Category = "Attributes")
	int32 GetStrength() const { return FMath::Max(1, Strength); }

	/** Whether this fighter is bracing right now - see bBracing. */
	UFUNCTION(BlueprintPure, Category = "Attributes")
	bool IsBracing() const { return bBracing; }

	/**
	 * What this fighter's grip puts behind the blade right now: their strength while braced, nothing at
	 * all while relaxed. The blade multiplies its own GripInertiaShare by this.
	 */
	UFUNCTION(BlueprintPure, Category = "Attributes")
	float GetGripFactor() const { return bBracing ? GetStrengthFactor() : 0.f; }

	/**
	 * Server RPC: call it from the owning client - a key in the Blueprint - and it forwards to the server
	 * on its own. Don't gate the call behind a HasAuthority() check on the caller's side; that would only
	 * skip the forwarding.
	 */
	UFUNCTION(BlueprintCallable, Server, Reliable, Category = "Attributes")
	void Server_SetBracing(bool bNewBracing);

	/**
	 * Server only. A client that set its own strength would disagree with the server about how fast its
	 * swing plays until the next replication overwrote it, so the call is refused there rather than
	 * half-applied. Fractions cannot get in - the parameter is whole - and anything below one is clamped.
	 */
	UFUNCTION(BlueprintCallable, Category = "Attributes")
	void SetStrength(int32 NewStrength);

	/**
	 * What strength multiplies. One number, used everywhere strength acts.
	 *
	 * That it is ONE number is the point, not tidiness. The contact balance compares the two fighters as
	 * a ratio, and a factor shared by both sides cancels out of a ratio: with a single factor, two
	 * fighters of equal strength are an equal fight at strength 5 and at strength 50, and only the
	 * difference between them decides anything. Give the swing's speed one factor and the grip another,
	 * and that stops being true - the fight tilts one way or the other as the numbers grow, for no
	 * reason anyone chose.
	 */
	UFUNCTION(BlueprintPure, Category = "Attributes")
	float GetStrengthFactor() const;

	/**
	 * How fast this fighter's swing plays: the strength factor, held inside the limits. The same on every
	 * machine, because Strength is replicated and this is worked out from it - the swing is started
	 * locally on each machine by the attack multicast, and each one asks this.
	 */
	UFUNCTION(BlueprintPure, Category = "Attributes")
	float GetAttackPlayRate() const;

public:

	/** Constructor. The movement component is UHexenCharacterMovementComponent - see there. */
	AHexenhammerCharacter(const FObjectInitializer& ObjectInitializer);
	virtual void Tick(float DeltaTime) override;

protected:

	virtual void BeginPlay() override;
	/** Initialize input action bindings */
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

public:

	/** Handles move inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoMove(float Right, float Forward);

	/** Handles look inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoLook(float Yaw, float Pitch);

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpStart();

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpEnd();

public:

	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }

protected:
	/**
	 * Spawns and equips WeaponClass, replacing any current weapon. This is a Server RPC: call it from
	 * any client that owns this pawn (e.g. from input) and it forwards to the server automatically -
	 * the body only ever actually runs there. Don't gate calls to it behind a manual HasAuthority()
	 * check on the caller's side; that would just skip the RPC forwarding.
	 */
	UFUNCTION(BlueprintCallable, Server, Reliable, Category = "Weapon")
	void SetActiveWeapon(TSubclassOf<AHexenWeapon> WeaponClass);
};

