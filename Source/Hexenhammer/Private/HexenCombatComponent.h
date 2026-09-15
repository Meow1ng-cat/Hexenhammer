// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/CriticalSection.h"
#include "HexenCombatComponent.generated.h"

class UHexenCollisionComponent;
class USkeletalMeshComponent;
class UAnimMontage;

/** What the rig's Hexen Blade Guard needs each frame - see UHexenCombatComponent::bUseBladeGuard. */
struct FHexenBladeGuardInput
{
	/** This blade's capsule axis in the hand bone's frame - the one frame it does not move in. */
	FVector MyAxisStartInHand = FVector::ZeroVector;
	FVector MyAxisEndInHand = FVector::ZeroVector;
	float MyRadius = 0.f;

	/** The partner's capsule axis in the world, where the partner's animation put it: its drawn pose minus the correction its own guard added. */
	FVector PartnerAxisStartWorld = FVector::ZeroVector;
	FVector PartnerAxisEndWorld = FVector::ZeroVector;
	float PartnerRadius = 0.f;

	float Share = 0.5f;
	float Skin = 0.f;

	/**
	 * The direction to push this blade in, in the world. One decision for the pair, the same for both
	 * fighters with opposite signs - see UHexenCombatComponent::UpdateBladeGuard.
	 */
	FVector PairAxisWorld = FVector::ZeroVector;

	/** The pair's blades have been carried through one another, and the push has to take them back rather than on. */
	bool bPairCrossed = false;

	/** A blade further past the other than this, in cm, is let go rather than hauled back. */
	float MaxPushBack = 0.f;
};

/** What the guard did in the last evaluation. */
struct FHexenBladeGuardResult
{
	bool bPenetrating = false;

	/** The animation had carried the blade through the partner's, and the guard sent it back to its own side. */
	bool bCrossed = false;

	float Depth = 0.f;
	FVector NormalWorld = FVector::ZeroVector;
	FVector ContactPointWorld = FVector::ZeroVector;
	FVector CorrectionWorld = FVector::ZeroVector;
};

/**
 * One per fighter. Collects what all of that fighter's collision volumes report, and turns it into the
 * handful of values the Animation Blueprint reads.
 *
 * It exists because there was nowhere for a per-fighter answer to live. Every volume used to write the
 * blend value itself, so a blade holding a bind and a hip hitbox brushing a wall wrote to the same
 * place and the last one to move won. A count cannot be got wrong that way.
 *
 * The contact count is derived locally from each volume's replicated state, so every machine reaches the
 * same number. The ratchet is NOT: its direction comes from live geometry, and two machines looking at
 * the same clash one frame apart can disagree about which way is out - the logs had a server and a
 * client pushing the same blade in opposite directions. So only the server steps it, and ContactTarget
 * is replicated to everyone else.
 *
 * Read from the AnimGraph without reparenting anything: in Blueprint Initialize Animation take
 * Get Owning Actor -> Get Component By Class, keep it, and read these in Blueprint Update Animation.
 * That works with any Animation Blueprint, including ones from a marketplace pack.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class UHexenCombatComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UHexenCombatComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** The bone the weapon is held in - the pivot the blade turns about, and the frame the touched spot is remembered in. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    FName HandBoneName = FName("hand_r");

    /**
     * How far, in cm, each push moves the target away from the blades' current contact.
     *
     * The mechanism is a ratchet, not a single correction. The target is set one step out along the way
     * out of the overlap; when the solver has brought the blade to it, a new target is set one step
     * further, and so on until the shapes actually come apart and EndOverlap says so.
     *
     * That termination condition is what makes this work without measuring anything. Nothing here knows
     * or needs to know how deep the blades are in one another - it pushes until the geometry stops
     * complaining. Small steps separate smoothly and slowly; large ones fling.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat", meta = (ClampMin = "0.1"))
    float ContactStep = 5.f;

    /**
     * How close to the target counts as having arrived, as a share of ContactStep.
     *
     * A share rather than its own distance so it cannot be set larger than the step itself - which would
     * mean arriving before setting off, and the ratchet would run away at one step per frame regardless
     * of whether the arm was keeping up.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat", meta = (ClampMin = "0.01", ClampMax = "0.9"))
    float ContactReachedFraction = 0.25f;

    /**
     * How many frames in a row the gap may stop closing before the ratchet steps anyway.
     *
     * Arriving within ContactReachedFraction of the target is the clean case, but the solver does not always
     * get there: it can settle a few centimetres short and stay, and then "arrived" never comes and the
     * ratchet never takes its second step. A gap that has stopped closing for this many frames means the
     * solver has done what it will at this target, so the target moves on regardless.
     *
     * Keeps the guard the arrival rule was there for: the target still only moves once the arm has stopped
     * catching up, so a hand that cannot follow at all walks the target out one step per this many frames
     * rather than one per frame.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat", meta = (ClampMin = "1"))
    int32 ContactStallFrames = 3;

    /**
     * How fast the blend lets go once the blades have come apart, as an FInterpTo speed.
     *
     * Only the release is eased. The other edge stays instant on purpose: a contact lasts a frame or
     * two, and a blend that faded IN over a tenth of a second would spend the whole contact fading and
     * never reach full strength while there was anything to correct. Coming out there is nothing left
     * to be late for, and the snap back to the animated pose is what reads as a teleport.
     *
     * Higher is snappier. Around 6 settles in roughly a third of a second; much lower and the arm stays
     * visibly out of its animation after the blades have plainly separated.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat", meta = (ClampMin = "0.1"))
    float ContactBlendReleaseSpeed = 6.f;

    /**
     * 1 while any of this fighter's volumes is in contact, then eased back to 0 once they part. Drive
     * the blend or the Control Rig node's Alpha with this.
     *
     * Note what an Alpha of 1 does and does not do. It selects the rig's output over the animation; it
     * does not stop the animation. The rig is handed the animated pose fresh every evaluation, so a rig
     * that does nothing hands it straight back and the swing carries on.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    float ContactBlendAlpha = 0.f;

    /** How many of this fighter's volumes are in contact right now. An alpha stuck at 1 and a count of 3 are different problems. */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    int32 ContactCount = 0;

    /**
     * Where the touched spot of the blade is now, in the mesh's component space. Place weaponContactBone
     * here before solving.
     *
     * It is a material point: what was remembered at contact is WHICH PART of the blade was touched, and
     * this is that part's current position, rebuilt each frame from the hand. So it rides forward with
     * the swing, and closes on the target as the solver does its work.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    FVector ContactPoint = FVector::ZeroVector;

    /**
     * Where to pull it to, in the mesh's component space. Solve weaponContactBone to here.
     *
     * Deliberately the same space as ContactPoint. Handing the rig two values in two different frames -
     * which an earlier version did - is a mistake with no symptom of its own: it looks exactly like the
     * solver misbehaving. In component space both go straight into Control Rig's GlobalSpace with no
     * conversion left to get wrong.
     */
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Combat")
    FVector ContactTarget = FVector::ZeroVector;

    /** How far the blade still is from the current step's target, in cm. Should fall towards zero, then jump by ContactStep as the ratchet advances. */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    float ContactGapDistance = 0.f;

    /**
     * Goes up by one every time a new bind begins, and never goes down. Feed it to the Control Rig that
     * freezes the arm chain, and have the rig capture a fresh pose whenever it differs from the last one
     * it saw.
     *
     * Exists because the rig cannot tell a new bind from an old one on its own. A Control Rig node only
     * runs while its alpha is above zero - AnimNode_ControlRigBase skips it entirely otherwise - so any
     * "was I frozen last time" the rig keeps in a variable goes stale across the gap between two binds.
     * The second bind finds the flag still set from the end of the first, never captures, and the arm is
     * frozen into a pose from a clash that is already over.
     *
     * Deliberately NOT replicated. The rig only ever compares it with itself on the same machine, so the
     * absolute value is meaningless and only the moment it changes matters - and that moment has to be
     * the same one at which ContactBlendAlpha jumps to 1 on that machine. Both happen in the same call
     * here, on every machine, driven by the same replicated contact state. A replicated counter would
     * arrive on its own schedule, on a different actor's channel, and could land a frame after the alpha.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    int32 ContactBindId = 0;

    /** How many steps this bind has taken. Rising steadily means the blades are being separated; stuck at 1 means the solver is not moving the blade at all. */
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Combat")
    int32 ContactStepsTaken = 0;

    /**
     * One line per event in the life of a bind: where it began, every step the ratchet takes, and what
     * it added up to when the blades came apart.
     *
     * Separate from the drawing flag because they answer different questions. The drawing shows where
     * things are right now; the log shows the whole sequence afterwards, which is the only way to see
     * whether the steps were being taken at all and in what direction.
     *
     * Not per frame - a step only happens when the target has been reached, so a whole bind is a handful
     * of lines rather than a wall of them.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Debug")
    bool bLogContactSteps = false;

    /** Draws the two rig points and prints the figures. The tick it needs is already running while a contact holds, so this costs only the drawing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Debug")
    bool bDrawContactPoints = false;

    /**
     * Whether a contact stops the swing, by pausing the montage that is playing it.
     *
     * Pausing stops time rather than the pose. Holding the bones still leaves the montage running
     * underneath, so the moment the hold is released the arm jumps to wherever the swing got to in the
     * meantime; a paused montage has nothing to catch up on and carries on from the same frame.
     *
     * Per fighter, here, and not per volume: the pause is one montage shared by everything the fighter
     * carries, so it has to be driven by the fighter's combined contact state. When every volume paused
     * and resumed it independently, a hip hitbox coming clear would resume a swing the blade was still
     * holding.
     *
     * Only montages are stopped. A swing driven by a state machine or a blend space has no montage to
     * pause and plays on - the log says so when it happens.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    bool bPauseMontageOnContact = true;

    /**
     * Whether a contact also holds the whole pose still, montage or not.
     *
     * The montage pause only covers swings that are montages, and most binds are held from a stance or on
     * the move, with no montage playing. Then the pose kept animating underneath - on each machine at its
     * own phase - so the blade a client drew sat on average 7 cm (up to 46) from where the server had it,
     * and the server's target hung beside the client's blade instead of on it.
     *
     * The freeze is a pose snapshot taken when this machine learns of the contact. The AnimGraph shows it
     * while bContactPoseFrozen is set, and the blade IK keeps solving on top of it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    bool bFreezePoseOnContact = true;

    /** The name the frozen pose is saved under. The Pose Snapshot node in the AnimGraph must use the same one. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
    FName ContactPoseSnapshotName = FName("ContactFreeze");

    /** True while the snapshot holds the pose. Drive the AnimGraph's Blend Poses by bool with this. */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    bool bContactPoseFrozen = false;

    /**
     * Keep the blade out of the other fighter's with the rig's Hexen Blade Guard instead of the ratchet.
     *
     * The guard works like a solid surface rather than a target: every frame it looks at where the
     * animation wants the blade and moves it only if that would put it into the other blade, by exactly the
     * overlap. It can push, never pull, and it keeps nothing from one frame to the next - so there is no
     * target to hold the blade after the contact and no release to wait out.
     *
     * While on, the ratchet, the montage pause and the pose freeze all stand down. Stopping the animation
     * would hold the blades pressed together with nothing left that could part them: the guard only ever
     * lets go when the animation takes the blade away.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard")
    bool bUseBladeGuard = true;

    /** This fighter's part of an overlap, 0..1. Two fighters at a half each resolve it exactly between them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0", ClampMax = "1"))
    float BladeGuardShare = 0.5f;

    /**
     * How far inside touching, in cm, the guard leaves the two capsules.
     *
     * Exactly touching is exactly the boundary of the overlap that says the blades are in contact, and a
     * boundary flickers. A little inside keeps that overlap on for as long as the blades are pressed
     * together, and it ends when the animation takes them apart.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float BladeGuardSkin = 1.f;

    /** How far away, in cm, another fighter's blade is still looked at. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float BladeGuardRange = 300.f;

    /**
     * How long, in seconds, the swing stays stopped once the guard first pushes. Needs bPauseMontageOnContact.
     *
     * The guard only pushes back; a strike keeps coming. Two frames after first touch the animation had the
     * hand 75 cm past the other blade - more than the arm can be pulled back by, and far enough that the
     * crossing is no longer recognisable. A solid blade would simply have stopped it. A fixed time is a
     * stand-in for what should really let go - the fighter's next action.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float BladeGuardPauseSeconds = 0.5f;

    /**
     * How far past the other blade, in cm, a blade can have been carried and still be brought back.
     *
     * Beyond it the guard lets go. No blade could have been held back from there, and hauling the arm that
     * far reads as the arm being yanked rather than the blade being stopped.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float BladeGuardMaxPushBack = 40.f;

    /**
     * How finely the last frame's movement of a pair of blades is searched for the moment they met.
     *
     * A strike can carry a blade right through the other between two frames - at 19 fps a cut covers more
     * than both capsules' width in one - and then neither frame's pose shows a contact at all, only "before"
     * and "already through". The sweep finds the moment in between, and the swing is rewound to it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "1", ClampMax = "64"))
    int32 BladeGuardSweepSteps = 12;

    /** The combat component of the fighter a rig is running for. Safe to call from the animation worker threads. */
    static UHexenCombatComponent* FindForActor(const AActor* Actor);

    /** For the rig. Worker-thread safe. False when the guard is off or no other blade is in range. */
    bool GetBladeGuardInput(FHexenBladeGuardInput& Out) const;

    /** For the rig. Worker-thread safe. */
    void SetBladeGuardResult(const FHexenBladeGuardResult& In);

    /** The correction the guard added in the last evaluation, in world space - what a partner takes off this blade to see where the animation had it. */
    FVector GetBladeGuardCorrectionWorld() const;

    /** The hand the weapon is held in, as drawn, in world space. Game thread. */
    bool GetHandTransformWorld(FTransform& Out) const;

    /** Called by a collision volume whenever its own contact state changes. Adding twice or removing something that was never added are both harmless. */
    void SetVolumeContact(UHexenCollisionComponent* Volume, bool bVolumeInContact, const FVector& WorldContactPoint);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Pauses the swing when the fighter becomes engaged, lets it go when they come clear. Follows ContactCount. */
    void UpdateMontagePause();

    /** Releases the pause this component placed, if it placed one. Safe to call at any time. */
    void ResumePausedMontage();

    /** Pauses whatever montage is playing and remembers it. Logs when there is none to pause. */
    void PauseCurrentMontage();

    /** Winds the playing montage back to Alpha of the way through the last frame, where the sweep found the blades meeting, and pauses it there. */
    void RewindSwingToTouch(float Alpha);

    /** Snapshots the pose when the fighter becomes engaged and lets it go when they come clear. Follows ContactCount. */
    void UpdatePoseFreeze();

    /** Game thread, every frame while the guard is on: finds this fighter's blade and the nearest other one, and publishes what the rig needs. */
    void UpdateBladeGuard();

    /** Shared with the rig, which runs on an animation worker thread. */
    mutable FCriticalSection BladeGuardLock;
    FHexenBladeGuardInput BladeGuardInput;
    bool bBladeGuardInputValid = false;
    FHexenBladeGuardResult BladeGuardResult;

    /** Whether the guard was pushing on the last tick, so the log can say when it starts and stops. */
    bool bGuardWasPushing = false;


    /** When the guard stopped the swing - see BladeGuardPauseSeconds. */
    double GuardPauseStartTime = 0.0;

    /** The montage that was playing on the last tick, and where it was - where a rewind winds back towards. */
    TWeakObjectPtr<UAnimMontage> LastSwingMontage;
    float LastSwingPosition = 0.f;

    /**
     * The exact montage this component paused.
     *
     * Kept rather than paused-and-resumed blind, so that a montage which started while the contact held
     * is not resumed by a contact that never paused it, and a montage that ended on its own leaves
     * nothing behind to resume.
     */
    TWeakObjectPtr<UAnimMontage> PausedMontage;

    /** The volumes currently reporting contact. A set rather than a counter so a repeated report cannot drift the total, which is the failure this component was made to prevent. */
    TSet<TWeakObjectPtr<UHexenCollisionComponent>> ContactingVolumes;

    /** Recomputes the exposed state and turns the tick on or off with it. */
    void RefreshContactState();

    /** Remembers which part of the blade was touched, and sets the first step. */
    void BeginContact(UHexenCollisionComponent* Volume, const FVector& WorldContactPoint);

    /** Puts the target one step further along the current way out. Asks the volume for the direction every time, because two blades in a bind slide and the way out moves with them. */
    void StepTarget(const FVector& FromWorld);

    /**
     * Which part of the blade was touched, in HandBoneName's local space. The one thing held across the whole bind.
     *
     * Blueprint-readable so the rig can place its effector bone in the hand's own frame - the one frame in which
     * this spot does not move, whatever pose the arm happens to be in. Set on every machine when a bind begins;
     * not replicated, for the same reason ContactBindId is not.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Combat")
    FVector ContactHandOffset = FVector::ZeroVector;

    /** The gap as it stood last frame, to tell whether it is still closing. */
    float PreviousGapDistance = 0.f;

    /** Frames in a row in which the gap has failed to close by at least the arrival tolerance. */
    int32 StalledFrames = 0;

    /** The volume holding this contact - asked for the separation direction on every step. */
    TWeakObjectPtr<UHexenCollisionComponent> ContactVolume;

    USkeletalMeshComponent* GetOwnerMesh() const;
};
