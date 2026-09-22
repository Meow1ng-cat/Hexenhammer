// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/CriticalSection.h"
#include "Engine/NetSerialization.h"
#include "HexenCombatComponent.generated.h"

class UHexenCollisionComponent;
class USkeletalMeshComponent;
class UAnimMontage;
class UAnimInstance;

/** What the rig's Hexen Collision Guard needs each frame - see UHexenCombatComponent::bUseHexenCollisionGuard. */
struct FHexenCollisionGuardInput
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
	 * fighters with opposite signs - see UHexenCombatComponent::UpdateHexenCollisionGuard.
	 */
	FVector PairAxisWorld = FVector::ZeroVector;

	/** The pair's blades have been carried through one another, and the push has to take them back rather than on. */
	bool bPairCrossed = false;
};

/** What the guard did in the last evaluation. */
struct FHexenCollisionGuardResult
{
	bool bPenetrating = false;

	/** The animation had carried the blade through the partner's, and the guard sent it back to its own side. */
	bool bCrossed = false;

	float Depth = 0.f;
	FVector NormalWorld = FVector::ZeroVector;
	FVector ContactPointWorld = FVector::ZeroVector;
	FVector CorrectionWorld = FVector::ZeroVector;

	/**
	 * How far in the blade is along the pair's direction, in cm, whether or not the guard pushes. What the release
	 * rule reads - see UHexenCombatComponent::bRetryingHeldSwing.
	 */
	float AxisDepth = 0.f;

	/** How far from the hand, in cm, this blade's closest point to the partner's blade was, in the rig's pose. */
	float ContactFromHand = -1.f;

	/** Whether that closest point was at an end of this blade's axis, or the partner's closest point at an end of theirs - a tip. */
	bool bContactAtMyEnd = false;
	bool bContactAtTheirEnd = false;

	/**
	 * Where the animation had this blade and the hand holding it, in the world, before the guard moved anything -
	 * what a partner's guard measures against and the pair decision reads. Published by the rig itself: the drawn
	 * pose minus the correction, which stood in for it, was 10-45 cm off in the 2026-09-17 run, because the drawn
	 * pose differs from what the rig puts out somewhere after the rig. False until the rig has run with a partner
	 * in range.
	 */
	bool bHasAnimatedPose = false;
	FTransform AnimatedHandWorld = FTransform::Identity;
	FVector AnimatedAxisStartWorld = FVector::ZeroVector;
	FVector AnimatedAxisEndWorld = FVector::ZeroVector;

	/**
	 * The spot on this blade's axis that touches the partner's, in the world, in the animated pose - the one point
	 * the guard asks the rig to move. The split log holds the drawn pose against it to see what delivered the
	 * correction, the arm or the wrist - see the [SPLIT] line in UpdateHexenCollisionGuard.
	 */
	FVector AnimatedContactWorld = FVector::ZeroVector;

	/**
	 * For the drift log: the animated hand in the mesh's own space, and GFrameCounter when the rig produced this
	 * result - behind the current frame when the pose was not evaluated this frame.
	 */
	FVector AnimatedHandComponent = FVector::ZeroVector;
	uint64 EvaluationFrame = 0;

	/**
	 * Where the rig had the bones from the root down to the hand, in the mesh's own space, and the transform it
	 * treats as world. The drift log holds these against the drawn pose to find where the two part company: the
	 * guard moves nothing above clavicle_r, so a difference up there is not the IK's doing.
	 */
	static constexpr int32 MaxChainBones = 12;
	FTransform ChainComponent[MaxChainBones];

	/**
	 * Each chain bone's own offset from its parent in the rig, in cm: as the hierarchy itself holds it now, and in the rig's
	 * own reference pose. Read straight from the hierarchy rather than worked out from the globals above, so the log can
	 * hold the two against each other - see the [LOCAL] line in UpdateHexenCollisionGuard.
	 */
	float ChainLocalOffset[MaxChainBones] = {};
	float ChainInitialOffset[MaxChainBones] = {};

	/** The whole local transform the rig holds for each chain bone, so the log can look for it among the mesh's own. */
	FTransform ChainLocal[MaxChainBones];

	/** What the rig's hierarchy says each chain bone hangs from, and how many bones it has in all - the pose is only as
	 * good as the chain it is read down. Published so the log can hold the rig's own parenting against the mesh's. */
	FName ChainParent[MaxChainBones];
	int32 RigBoneCount = 0;

	int32 ChainNum = 0;
	FTransform RigToWorld = FTransform::Identity;
};

/** The bones the drift log follows, root first - see FHexenCollisionGuardResult::ChainComponent. */
const TArray<FName>& GetHexenCollisionGuardChainBones();

/** One bone of the server's drawn pose on its way to the clients: where it is in the mesh's own space and how it is turned. No scale - nothing here scales a bone. */
USTRUCT()
struct FHexenNetBoneTransform
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize10 Location = FVector::ZeroVector;

	/**
	 * The rotation's X, Y and Z scaled to 16 bits each, W made non-negative and rebuilt on arrival from the unit length:
	 * 6 bytes. An FQuat goes over the wire as three doubles, 24 bytes, and at MaxClientRate 10000 a connection could
	 * carry a full pose of both fighters only every fourth frame - the rest were skipped, which is the lag the clients
	 * showed (2026-09-22). The cost is precision where W is near zero: about 0.3 degrees, 5 mm at the end of a blade.
	 */
	UPROPERTY()
	int16 RotationX = 0;

	UPROPERTY()
	int16 RotationY = 0;

	UPROPERTY()
	int16 RotationZ = 0;

	void SetRotation(const FQuat& In);
	FQuat GetRotation() const;
};

/** What the character's movement reads from the guard, as the server had it - see UHexenCombatComponent::bUseServerGuardResult. */
USTRUCT()
struct FHexenNetGuardResult
{
	GENERATED_BODY()

	UPROPERTY()
	bool bPenetrating = false;

	UPROPERTY()
	FVector_NetQuantizeNormal NormalWorld = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize10 ContactPointWorld = FVector::ZeroVector;
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
     * Keep the blade out of the other fighter's with the rig's Hexen Collision Guard instead of the ratchet.
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
    bool bUseHexenCollisionGuard = true;

    /** This fighter's part of an overlap, 0..1. Two fighters at a half each resolve it exactly between them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0", ClampMax = "1"))
    float HexenCollisionGuardShare = 0.5f;

    /**
     * How far inside touching, in cm, the guard leaves the two capsules.
     *
     * Exactly touching is exactly the boundary of the overlap that says the blades are in contact, and a
     * boundary flickers. A little inside keeps that overlap on for as long as the blades are pressed
     * together, and it ends when the animation takes them apart.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float HexenCollisionGuardSkin = 1.f;

    /** How far away, in cm, another fighter's blade is still looked at. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float HexenCollisionGuardRange = 300.f;

    /**
     * How often, in seconds, a swing stopped against a blade looks whether it can go on. Needs
     * bPauseMontageOnContact.
     *
     * The guard only pushes back; a swing keeps coming. Two frames after first touch the animation had the
     * hand 75 cm past the other blade - more than the arm can be pulled back by, and far enough that the
     * crossing is no longer recognisable. A solid blade would simply have stopped it.
     *
     * Not how long to hold, but how often to look. When the time is up and this fighter's blade still overlaps
     * another fighter's, nothing happens, and it looks again after the same time. Once they no longer overlap, the
     * swing is let go; if it then presses on into the blade it was stopped against, it is stopped again at once -
     * see HexenCollisionGuardPressTolerance. Unlike holding until the fighter's next action, a swing whose way
     * comes clear goes on by itself.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float HexenCollisionGuardPauseSeconds = 0.5f;

    /**
     * How much deeper, in cm, a swing let go after HexenCollisionGuardPauseSeconds may press into the blade it
     * was stopped against before it is stopped again.
     *
     * Above zero, so that small movement of the held pose itself does not count as pressing on.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "0"))
    float HexenCollisionGuardPressTolerance = 2.f;

    /**
     * How finely the last frame's movement of a pair of blades is searched for the moment they met.
     *
     * A strike can carry a blade right through the other between two frames - at 19 fps a cut covers more
     * than both capsules' width in one - and then neither frame's pose shows a contact at all, only "before"
     * and "already through". The sweep finds the moment in between, and the swing is rewound to it.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard", meta = (ClampMin = "1", ClampMax = "64"))
    int32 HexenCollisionGuardSweepSteps = 12;

    /**
     * TEMPORARY, for a test: whether the rig pushes along the pair's one shared direction and remembers which side
     * each blade belongs on. Off, each rig pushes its blade out along the shortest way from where the blades are this
     * frame, deciding alone and remembering nothing. The sweep and the rewind run either way.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard")
    bool bHexenCollisionGuardPairDecision = false;

    /**
     * TEMPORARY, for a test: whether "the blades overlap" - for the hold timer's look and for the end of a pair's crossing -
     * is judged in the pose the rigs put out (the animated blades plus each guard's correction) rather than on the drawn
     * capsules. The two differ by the rig-versus-drawn mismatch, which at a tip is more than the guard's skin, and the drawn
     * overlap then ended contacts the guard was still holding. For the look, either guard of the pair still pushing counts
     * as overlap too.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard")
    bool bHexenCollisionGuardRigPoseOverlap = true;

    /**
     * TEMPORARY - the user's rule of 2026-09-19, until contact is answered by mass, speed and energy: whether a fighter
     * stops moving and turning into a blade its own blade is held against - see UHexenCharacterMovementComponent.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat|Guard")
    bool bHexenCollisionBlocksMovement = true;

    /**
     * EXPERIMENT (2026-09-22, the user's method: replicate everything, then take pieces away one at a time until it
     * breaks). The server's drawn pose, for the bones in ServerPoseBones, sent to every client and drawn there instead
     * of the client's own - by the Hexen Server Pose unit, last in the rig.
     *
     * Why the pose and not just the guard's numbers: the same fighter at the same point of the same montage is posed
     * 4-5 cm apart from the pelvis up on the server and on a client (open question 11 in Docs/HexenCollision.md), so a
     * client applying the server's correction to its own pose still draws its blade somewhere the server's is not.
     *
     * Not free: a transform per listed bone per net update, for every fighter, all the time. It is scaffolding to
     * strip down, not something to ship - the constraint against per-frame work still stands for what is kept.
     * Read on each machine from its own copy of the defaults: the server publishes only with it on, and a client draws
     * the server's pose only with it on.
     */
    UPROPERTY(EditAnywhere, Category = "Combat|Replication Experiment")
    bool bReplicateServerPose = true;

    /**
     * The bones whose pose comes from the server, root first so each one is written after the bone it hangs from.
     * Taking a bone out of the list hands it back to this machine's own animation - that is the stripping. Take a
     * parent out while keeping its child, and the child is still put where the server had it while the parent is not:
     * visibly detached, which is the point of trying it.
     */
    UPROPERTY(EditAnywhere, Category = "Combat|Replication Experiment")
    TArray<FName> ServerPoseBones = {
        FName("pelvis"), FName("spine_01"), FName("spine_02"), FName("spine_03"), FName("spine_04"), FName("spine_05"),
        FName("clavicle_r"), FName("upperarm_r"), FName("lowerarm_r"), FName("hand_r") };

    /**
     * EXPERIMENT, same method. Whether a client's movement stops at a held blade by the server's guard result rather
     * than by its own. A client predicts its own moves; predicting them against a contact it measured itself, from a
     * pose the server does not have, stops it where the server would not and gets it corrected back.
     */
    UPROPERTY(EditAnywhere, Category = "Combat|Replication Experiment")
    bool bUseServerGuardResult = true;

    /** The combat component of the fighter a rig is running for. Safe to call from the animation worker threads. */
    static UHexenCombatComponent* FindForActor(const AActor* Actor);

    /** For the rig. Worker-thread safe. False when the guard is off or no other blade is in range. */
    bool GetHexenCollisionGuardInput(FHexenCollisionGuardInput& Out) const;

    /** For the rig. Worker-thread safe. */
    void SetHexenCollisionGuardResult(const FHexenCollisionGuardResult& In);

    /** The correction the guard added in the last evaluation, in world space. */
    FVector GetHexenCollisionGuardCorrectionWorld() const;

    /** Whether the guard pushed this fighter's blade out of another one in the last evaluation. */
    bool IsHexenCollisionGuardPushing() const;

    /**
     * While the guard is pushing this fighter's blade out of another one: the direction it pushes it out in and where the
     * blades touch, in world space. What the character's movement reads - see UHexenCharacterMovementComponent.
     */
    bool GetHexenCollisionMovementBlock(FVector& OutOutward, FVector& OutContactPoint) const;

    /**
     * For the Hexen Server Pose unit. Worker-thread safe. The server's pose for the bones it lists, in the mesh's own
     * space, when this machine should draw it instead of its own - see bReplicateServerPose. False on the server, with
     * the experiment off here, and until a pose has arrived.
     */
    bool GetServerPoseForRig(TArray<FName>& OutBones, TArray<FTransform>& OutPose) const;

    /** Where this fighter's animation had its blade and hand in the last evaluation, in world space - see FHexenCollisionGuardResult::bHasAnimatedPose. False until the rig has published one. */
    bool GetHexenCollisionGuardAnimatedPose(FVector& OutStart, FVector& OutEnd, FTransform& OutHand) const;

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

    /** Pauses Montage and holds its blend weight at Weight, remembering the instance so ResumePausedMontage can let both go. */
    void HoldMontage(UAnimInstance* AnimInstance, UAnimMontage* Montage, float Weight);

    /**
     * Winds the playing montage back - its time and its blend-in - to Alpha of the way through the last frame,
     * where something stopped it, and holds it there. Cause names what stopped it, for the log. False when there
     * was no swing to wind back.
     */
    bool RewindSwingToTouch(float Alpha, const TCHAR* Cause);

    /**
     * Server-side. Puts out a hold or a release, with the montage time it happened at, for every machine to follow.
     *
     * Whether a swing is stopped is not a visual: a swing that goes on where it should have been stopped lands a
     * blow that never happened. So one machine decides it and the rest are told, the same way a contact already
     * works - see UHexenCollisionComponent::bInContact. Pushed at once rather than on the owner's next scheduled
     * update, because everything after it waits on it.
     */
    void PublishSwingHold(bool bHeld);

    /** Brings this machine's own copy of the swing to what the server last said about it. */
    void ApplyServerSwingHold();

    UFUNCTION()
    void OnRep_ServerSwingHold();

    /** Snapshots the pose when the fighter becomes engaged and lets it go when they come clear. Follows ContactCount. */
    void UpdatePoseFreeze();

    /** Game thread, every frame while the guard is on: finds this fighter's blade and the nearest other one, and publishes what the rig needs. */
    void UpdateHexenCollisionGuard();

    /** Shared with the rig, which runs on an animation worker thread. */
    mutable FCriticalSection HexenCollisionGuardLock;
    FHexenCollisionGuardInput HexenCollisionGuardInput;
    bool bHexenCollisionGuardInputValid = false;
    FHexenCollisionGuardResult HexenCollisionGuardResult;

    /** Whether the guard was pushing on the last tick, so the log can say when it starts and stops. */
    bool bGuardWasPushing = false;

    /**
     * The gap between the drawn capsules on the tick before this one - see the [GUARD] line.
     *
     * Needed because the gap on the frame a contact starts says nothing on its own: by then the guard has already
     * pushed the blades apart, and a positive gap is the guard working rather than the guard firing early. The
     * frame before is the last one nothing was moved on, so that is the one that answers whether the blades had
     * met where they are drawn.
     */
    float PreviousDrawnGap = 0.f;
    bool bHavePreviousDrawnGap = false;

    /**
     * The server's word on this fighter's swing: whether it is stopped against a blade right now, and at what
     * montage time it was stopped or let go.
     *
     * Only the server decides this. A client deciding for itself decides from its own poses, and its own poses are
     * its own: in the 2026-09-20 run the server had the blades apart for four minutes while a client had them
     * locked together, each holding a swing the other had let go. Two players would then disagree about whether a
     * blow ever landed, which is the one thing that must not happen.
     *
     * The serial carries the notify, so a hold straight after a release still arrives as its own event, and the
     * other two ride in the same bunch - a client reading one on its notify has all three.
     */
    UPROPERTY(ReplicatedUsing = OnRep_ServerSwingHold)
    uint8 ServerSwingHoldSerial = 0;

    UPROPERTY(Replicated)
    bool bServerSwingHeld = false;

    UPROPERTY(Replicated)
    float ServerSwingHoldPosition = 0.f;

    /**
     * Which swing the time above belongs to. A client winds nothing unless the montage it is playing is this one:
     * a time from one swing put into another would wind that other swing to an arbitrary place, and it would look
     * exactly like the hold misbehaving.
     */
    UPROPERTY(Replicated)
    TObjectPtr<UAnimMontage> ServerSwingHoldMontage = nullptr;

    /**
     * EXPERIMENT - the server's drawn pose for the bones it listed in ServerPoseBones, in that order, in the mesh's own
     * space. Empty while the server has the experiment off, which is how a client knows to draw its own.
     */
    UPROPERTY(ReplicatedUsing = OnRep_ServerPose)
    TArray<FHexenNetBoneTransform> ServerPose;

    UFUNCTION()
    void OnRep_ServerPose();

    /**
     * EXPERIMENT - the frame the server took ServerPose on. Every PIE world ticks in one process on one frame counter,
     * so a client can say exactly how many frames old the pose it is about to draw is - and whether that age holds
     * steady (the pose is simply late) or keeps growing (the connection is queueing it).
     */
    UPROPERTY(Replicated)
    int32 ServerPoseFrame = 0;

    /** The frame the last pose arrived on here - for how often they come. */
    uint64 LastServerPoseArrivalFrame = 0;

    /** ServerPose, turned into transforms and handed to the rig under HexenCollisionGuardLock - the rig runs on a worker thread and the net driver writes the replicated array on the game thread. */
    TArray<FTransform> ServerPoseForRig;

    /** Whether this machine controls this fighter, worked out on the game thread and handed to the rig with the pose - see bServerPoseOnOwnFighter. */
    bool bLocallyControlledForRig = false;

    /** EXPERIMENT - what the server's guard last said, for the movement of a client - see bUseServerGuardResult. */
    UPROPERTY(Replicated)
    FHexenNetGuardResult ServerGuardResult;

    /** Server-side. Puts this frame's drawn pose and guard result out for the clients - see bReplicateServerPose. */
    void PublishServerState();


    /** When the guard stopped the swing - see HexenCollisionGuardPauseSeconds. */
    double GuardPauseStartTime = 0.0;

    /**
     * True from the moment a held swing is let go to try again until the contact it was held against is over.
     * While it is, a swing pressing on past RetryDepth by HexenCollisionGuardPressTolerance is wound back and held
     * again. Cleared by any hold, by the blades coming apart, and by the swing ending.
     */
    bool bRetryingHeldSwing = false;

    /** The guard's depth along the pair's direction when the swing was let go - see bRetryingHeldSwing. */
    float RetryDepth = 0.f;

    /**
     * The chain the rig published on the previous tick, so the drift log can hold the drawn pose against both the
     * rig's pose of this frame and of the last one. A drawn pose that matches the older one is a pose a frame behind.
     */
    FTransform PreviousChain[FHexenCollisionGuardResult::MaxChainBones];
    int32 PreviousChainNum = 0;
    uint64 PreviousChainFrame = 0;

    /**
     * The montage instance that was playing on the last tick, where it was, and at what blend weight - where a
     * rewind winds back towards. Kept by instance rather than by montage: the same swing played again is a new
     * instance, and nothing of the old one's is a "before" for it.
     */
    int32 LastSwingInstanceID = INDEX_NONE;
    float LastSwingPosition = 0.f;
    float LastSwingWeight = 0.f;

    /**
     * The exact montage this component paused.
     *
     * Kept rather than paused-and-resumed blind, so that a montage which started while the contact held
     * is not resumed by a contact that never paused it, and a montage that ended on its own leaves
     * nothing behind to resume.
     */
    TWeakObjectPtr<UAnimMontage> PausedMontage;

    /** The instance HoldMontage paused - the one ResumePausedMontage gives its blend-in back to. */
    int32 PausedMontageInstanceID = INDEX_NONE;

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
