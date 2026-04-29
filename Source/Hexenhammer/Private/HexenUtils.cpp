
//
//#pragma once
//
//#include "CoreMinimal.h"
//#include "Components/SceneComponent.h"
//#include "Components/SphereComponent.h"
//#include "Components/CapsuleComponent.h"
//#include "Components/BoxComponent.h"
//#include "Components/ArrowComponent.h"
//#include "BodyCollisionComponent.h"
//#include "HexenCollisionComponent.h"
//#include "HexenUtils.h"
//#include "WeaponCollisionComponent.generated.h"
//
//
//USTRUCT(BlueprintType)
//struct FBladeEdge
//{
//	GENERATED_BODY()
//
//	UPROPERTY(BlueprintReadWrite, EditAnywhere)
//	FVector Direction = FVector::ForwardVector;
//
//	UPROPERTY(BlueprintReadWrite, EditAnywhere)
//	int32 Piercing = 50.f;
//
//	UPROPERTY(BlueprintReadWrite, EditAnywhere)
//	int32 Damage = 50.f;
//};
//
//UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
//class UWeaponCollisionComponent : public UHexenCollisionComponent
//{
//	GENERATED_BODY()
//
//public:
//	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//	EFdlCollisionShape CollisionShape = EFdlCollisionShape::None;
//	UPROPERTY()
//	UPrimitiveComponent* CollisionObject = nullptr;
//	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//	FVector BladeDirection;
//	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//	int32 Mass;
//	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//	int32 Sharpness = 50.f;
//
//	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//	bool IsDoubleEdged = false;
//
//protected:
//	UPROPERTY()
//	FTransform LastTransform;
//	float LastDeltaTime;
//
//public:
//	UWeaponCollisionComponent();
//	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
//
//protected:
//	virtual void BeginPlay() override;
//	void SetShape();
//	UFUNCTION()
//	void OnBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
//		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
//		bool bFromSweep, const FHitResult& HitResult);
//
//#if WITH_EDITOR
//	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
//#endif
//};
//
//
//
//// Fill out your copyright notice in the Description page of Project Settings.
//
//
//#include "WeaponCollisionComponent.h"
//
//// Sets default values for this component's properties
//UWeaponCollisionComponent::UWeaponCollisionComponent()
//{
//	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
//	// off to improve performance if you don't need them.
//	PrimaryComponentTick.bCanEverTick = true;
//
//#if !UE_BUILD_SHIPPING
//	SetIsReplicatedByDefault(true); // Если в будущем вам потребуется, чтобы клиент как-то взаимодействовал с этим компонентом 
//	//(например, для клиентских предсказаний, визуальных эффектов или отладки), придётся пересмотреть это решение.
//#endif
//}
//
//void UWeaponCollisionComponent::BeginPlay()
//{
//	Super::BeginPlay();
//
//	if (GetOwner()->HasAuthority())
//	{
//		PrimaryComponentTick.SetTickFunctionEnable(true);
//
//		if (CollisionObject)
//		{
//			CollisionObject->CreationMethod = EComponentCreationMethod::Instance;
//			CollisionObject->RegisterComponent();
//			CollisionObject->SetActive(true);
//
//			CollisionObject->SetCollisionObjectType(ECC_GameTraceChannel1);
//			CollisionObject->SetCollisionResponseToAllChannels(ECR_Ignore);
//			CollisionObject->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Block);
//			CollisionObject->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Overlap);
//			CollisionObject->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
//
//			CollisionObject->SetGenerateOverlapEvents(true);
//			CollisionObject->OnComponentBeginOverlap.AddDynamic(this, &UWeaponCollisionComponent::OnBeginOverlap);
//
//			LastTransform = CollisionObject->GetComponentTransform();
//		}
//	}
//	else
//	{
//		PrimaryComponentTick.SetTickFunctionEnable(false);
//	}
//
//#if !UE_BUILD_SHIPPING
//	if (CollisionObject)
//	{
//		if (GetOwner()->HasAuthority())
//		{
//			UE_LOG(LogTemp, Warning, TEXT("CollisionShape is %d"), CollisionShape);
//			if (CollisionObject->IsRegistered())
//			{
//				UE_LOG(LogTemp, Warning, TEXT("CollisionObject is registered"));
//			}
//			else
//			{
//				UE_LOG(LogTemp, Warning, TEXT("CollisionObject is not registered"));
//			}
//		}
//
//		if (GetNetMode() != NM_DedicatedServer)
//		{
//			CollisionObject->SetHiddenInGame(false);
//			CollisionObject->SetVisibility(true);
//
//			//DebugArrow = NewObject<UArrowComponent>(this, TEXT("DebugArrow"));
//			//DebugArrow->SetupAttachment(CollisionObject); // прикрепить к коллизионному объекту
//			//DebugArrow->SetHiddenInGame(true); // по умолчанию скрыт
//			//DebugArrow->ArrowColor = FColor::Red;
//			//DebugArrow->ArrowSize = 1.0f;
//			//DebugArrow->bIsScreenSizeScaled = true;
//		}
//	}
//#endif
//}
//
//void UWeaponCollisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
//{
//	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
//
//	if (GetOwner()->HasAuthority())
//	{
//		LastDeltaTime = DeltaTime;
//		if (CollisionObject)
//		{
//			LastTransform = CollisionObject->GetComponentTransform();
//		}
//	}
//}
//
//#if WITH_EDITOR
//void UWeaponCollisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
//{
//	Super::PostEditChangeProperty(PropertyChangedEvent);
//	FName PropertyName = PropertyChangedEvent.Property ?
//		PropertyChangedEvent.Property->GetFName() : NAME_None;
//	if (PropertyName == GET_MEMBER_NAME_CHECKED(UWeaponCollisionComponent, CollisionShape))
//	{
//		SetShape();
//	}
//}
//#endif
//
//void UWeaponCollisionComponent::SetShape()
//{
//	if (IsValid(this))
//	{
//		if (CollisionShape == EFdlCollisionShape::None)
//		{
//		}
//		else if (CollisionShape == EFdlCollisionShape::Sphere)
//		{
//			CollisionObject = NewObject<USphereComponent>(this, TEXT("SphereCollision"), RF_NoFlags);
//			Cast<USphereComponent>(CollisionObject)->SetSphereRadius(50.0f);
//		}
//		else if (CollisionShape == EFdlCollisionShape::Capsule)
//		{
//			CollisionObject = NewObject<UCapsuleComponent>(this, TEXT("BoxCollision"), RF_NoFlags);
//			Cast<UCapsuleComponent>(CollisionObject)->SetCapsuleSize(50.0f, 100.0f);
//		}
//		else if (CollisionShape == EFdlCollisionShape::Box)
//		{
//			CollisionObject = NewObject<UBoxComponent>(this, TEXT("CapsuleCollision"), RF_NoFlags);
//			Cast<UBoxComponent>(CollisionObject)->SetBoxExtent(FVector(50, 50, 50));
//		}
//		else
//		{
//			UE_LOG(LogTemp, Warning, TEXT("No Shape for weapon collison!"));
//		}
//
//		if (IsValid(CollisionObject))
//		{
//			CollisionObject->SetupAttachment(this);
//		}
//	}
//}
//
//void UWeaponCollisionComponent::OnBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
//	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
//	bool bFromSweep, const FHitResult& HitResult)
//{
//
//	if (GetOwner()->HasAuthority())
//	{
//		AActor* WeaponOwner = GetOwner() ? GetOwner()->GetOwner() : nullptr;
//		if (!OtherActor || OtherActor == WeaponOwner)
//		{
//			return;
//		}
//
//		if (UBodyCollisionComponent* BodyParentComponent = Cast<UBodyCollisionComponent>(OtherComp->GetAttachParent()))
//		{
//			FVector CurrentLocation = CollisionObject->GetComponentLocation();
//			FVector LastLocation = LastTransform.GetLocation();
//			FQuat CurrentRotation = CollisionObject->GetComponentQuat();
//
//			// Calculating Velocity at the hit moment
//			FVector WeaponDeltaPos = CurrentLocation - LastLocation;
//			float WeaponDeltaTime = FMath::Max(LastDeltaTime, 0.0001f);  //Maybe its not necessary cause there is no way that BodyDeltaTime will be litteraly zero
//			FVector WeaponVelocity = WeaponDeltaPos / WeaponDeltaTime;	//Not very accurate, cause LastDeltaTime is from previos tick, but not critical... About ~10% miscalculation 
//
//			FVector BodyDeltaPos = OtherComp->GetComponentLocation() - BodyParentComponent->GetLastTransform().GetLocation();
//			float BodyDeltaTime = FMath::Max(BodyParentComponent->GetLastDeltaTime(), 0.0001f); //Maybe its not necessary cause there is no way that BodyDeltaTime will be litteraly zero
//			FVector BodyVelocity = BodyDeltaPos / BodyDeltaTime;  //Not very accurate, cause LastDeltaTime is from previos tick, but not critical... About ~10% miscalculation
//
//			FVector Velocity = WeaponVelocity - BodyVelocity;
//			FVector VelocityNormalized = Velocity.GetSafeNormal();
//			float Speed = Velocity.Size() / 100; // Divided by 100 to convert to m/s
//
//			//Calculating some kind of hit angle to reduce the damage if its necessary 
//			FHitResult SweepResult;
//			bool PerformSweep = GetWorld()->SweepSingleByChannel(
//				SweepResult,
//				LastLocation,
//				CurrentLocation,
//				CurrentRotation,
//				CollisionObject->GetCollisionObjectType(),
//				CollisionObject->GetCollisionShape()
//			);
//			if (!PerformSweep)
//			{
//				return;
//			}
//			FVector ImpactNormal = SweepResult.Normal;
//			float GlancingHitReduce = FVector::DotProduct(VelocityNormalized, ImpactNormal);
//			GlancingHitReduce = FMath::Max(0.0f, GlancingHitReduce);  //Не уверен насчет этого
//
//			// Calculating plane of blade to check, is hit by a sharpen side of the sword 
//			FVector Forward = CollisionObject->GetForwardVector();
//			FVector Up = CollisionObject->GetUpVector();
//			FVector PlaneNormal = FVector::CrossProduct(Forward, Up);
//			PlaneNormal = PlaneNormal.GetSafeNormal();
//			FVector PointOnPlane = CollisionObject->GetComponentLocation();
//			FPlane BladePlane(PointOnPlane, PlaneNormal);
//			float  FlatsideHitReduce = FVector::DotProduct(VelocityNormalized, BladePlane); //Кажется тут неправильно счтается, перепутаны плоскоти. надо перепроверить. 
//			if (IsDoubleEdged)
//			{
//				FlatsideHitReduce = FMath::Abs(FlatsideHitReduce);
//			}
//			else
//			{
//				FlatsideHitReduce = FMath::Max(0.0f, FlatsideHitReduce);
//			}
//
//			UE_LOG(LogTemp, Warning, TEXT("Hit! Speed: %f GlancingHitReduce: %f FlatsideHitReduce: %f"), Speed, GlancingHitReduce, FlatsideHitReduce);
//
//			// Calculating kinetic energy
//			float HitEnergy = 0.5f * Mass * Speed * Speed * GlancingHitReduce;
//			float HitPircing = Sharpness * HitEnergy * FlatsideHitReduce;
//
//			if (HitPircing >= BodyParentComponent->Resilience)
//			{
//				BodyParentComponent->HandleDamage(EDamageType::Cutting);
//				UE_LOG(LogTemp, Warning, TEXT("Penetreted! HitEnergy: %f HitPircing: %f"), HitEnergy, HitPircing);
//			}
//			else
//			{
//				BodyParentComponent->HandleDamage(EDamageType::Mauling);
//				UE_LOG(LogTemp, Warning, TEXT("Mauled! HitEnergy: %f HitPircing: %f"), HitEnergy, HitPircing);
//			}
//		}
//	}
//}



// Fill out your copyright notice in the Description page of Project Settings.

//#pragma once
//
//#include "CoreMinimal.h"
//#include "Components/SceneComponent.h"
//#include "Components/SphereComponent.h"
//#include "Components/CapsuleComponent.h"
//#include "Components/BoxComponent.h"
//#include "HexenUtils.h"
//#include "BodyCollisionComponent.generated.h"
//
//UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
//class UBodyCollisionComponent : public UHexenCollisionComponent
//{
//    GENERATED_BODY()
//
//public:
//    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//    EFdlCollisionShape CollisionShape = EFdlCollisionShape::None;
//    UPROPERTY()
//    UPrimitiveComponent* CollisionObject = nullptr;
//    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//    int32 Resilience = 50.f; //Resistance to Cutting. If HitPircing < Resilience, hit is not valid.
//    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "DamageCollision")
//    int32 Durabulity = 50.f; //Resistance to Mauling. Bone breaking stacks count as Damage by Mauling/Durabulity
//
//    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
//    bool IsAlive = true;
//    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
//    bool IsVital = false;
//    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "BodyType")
//    EBodyType BodyType = EBodyType::None;
//
//protected:
//    FTransform LastTransform;
//    float LastDeltaTime;
//
//public:
//    UBodyCollisionComponent();
//    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
//
//    FTransform GetLastTransform() { return LastTransform; }
//    float GetLastDeltaTime() { return LastDeltaTime; }
//
//    UFUNCTION(BlueprintCallable, Category = "Damage")
//    void HandleDamage(const EDamageType DamageType);
//protected:
//    virtual void BeginPlay() override;
//    void SetShape();
//
//    virtual void ApplyBleeding();
//    virtual void ApplyBoneBreak();
//    virtual void ApplyBurning();
//    virtual void ApplyFreezing();
//
//#if WITH_EDITOR
//    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
//#endif
//};
//
//// Fill out your copyright notice in the Description page of Project Settings.
//
//
//#include "BodyCollisionComponent.h"
//
//// Sets default values for this component's properties
//UBodyCollisionComponent::UBodyCollisionComponent()
//{
//	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
//	// off to improve performance if you don't need them.
//	PrimaryComponentTick.bCanEverTick = true;
//
//#if !UE_BUILD_SHIPPING
//	SetIsReplicatedByDefault(true); // Если в будущем вам потребуется, чтобы клиент как-то взаимодействовал с этим компонентом 
//	//(например, для клиентских предсказаний, визуальных эффектов или отладки), придётся пересмотреть это решение.
//#endif
//
//}
//
//void UBodyCollisionComponent::BeginPlay()
//{
//	Super::BeginPlay();
//	if (GetOwner()->HasAuthority())
//	{
//		PrimaryComponentTick.SetTickFunctionEnable(true);
//
//		CollisionObject->CreationMethod = EComponentCreationMethod::Instance;
//		CollisionObject->RegisterComponent();
//		CollisionObject->SetActive(true);
//		CollisionObject->SetHiddenInGame(false);
//		CollisionObject->SetVisibility(true);
//
//		CollisionObject->SetCollisionObjectType(ECC_GameTraceChannel2);
//		CollisionObject->SetCollisionResponseToAllChannels(ECR_Ignore);
//		CollisionObject->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
//		CollisionObject->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block);
//		CollisionObject->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
//		CollisionObject->SetGenerateOverlapEvents(true);
//	}
//	else
//	{
//		PrimaryComponentTick.SetTickFunctionEnable(false);
//	}
//
//#if !UE_BUILD_SHIPPING
//	if (CollisionObject)
//	{
//		if (GetOwner()->HasAuthority())
//		{
//			UE_LOG(LogTemp, Warning, TEXT("CollisionShape is %d"), CollisionShape);
//			if (CollisionObject->IsRegistered())
//			{
//				UE_LOG(LogTemp, Warning, TEXT("CollisionObject is registered"));
//			}
//			else
//			{
//				UE_LOG(LogTemp, Warning, TEXT("CollisionObject is not registered"));
//			}
//		}
//
//		if (GetNetMode() != NM_DedicatedServer)
//		{
//			CollisionObject->SetHiddenInGame(false);
//			CollisionObject->SetVisibility(true);
//		}
//	}
//#endif
//
//}
//
//void UBodyCollisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
//{
//	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
//
//	if (GetOwner()->HasAuthority())
//	{
//		LastDeltaTime = DeltaTime;
//		if (CollisionObject)
//		{
//			LastTransform = CollisionObject->GetComponentTransform();
//		}
//	}
//}
//
//#if WITH_EDITOR
//void UBodyCollisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
//{
//	Super::PostEditChangeProperty(PropertyChangedEvent);
//	FName PropertyName = PropertyChangedEvent.Property ?
//		PropertyChangedEvent.Property->GetFName() : NAME_None;
//	if (PropertyName == GET_MEMBER_NAME_CHECKED(UBodyCollisionComponent, CollisionShape))
//	{
//		SetShape();
//	}
//}
//#endif
//
//void UBodyCollisionComponent::SetShape()
//{
//	if (IsValid(this))
//	{
//		if (CollisionShape == EFdlCollisionShape::None)
//		{
//		}
//		else if (CollisionShape == EFdlCollisionShape::Sphere)
//		{
//			CollisionObject = NewObject<USphereComponent>(this, TEXT("SphereCollision"), RF_NoFlags);
//			Cast<USphereComponent>(CollisionObject)->SetSphereRadius(50.0f);
//		}
//		else if (CollisionShape == EFdlCollisionShape::Capsule)
//		{
//			CollisionObject = NewObject<UCapsuleComponent>(this, TEXT("BoxCollision"), RF_NoFlags);
//			Cast<UCapsuleComponent>(CollisionObject)->SetCapsuleSize(50.0f, 100.0f);
//		}
//		else if (CollisionShape == EFdlCollisionShape::Box)
//		{
//			CollisionObject = NewObject<UBoxComponent>(this, TEXT("CapsuleCollision"), RF_NoFlags);
//			Cast<UBoxComponent>(CollisionObject)->SetBoxExtent(FVector(50, 50, 50));
//		}
//		else
//		{
//			UE_LOG(LogTemp, Warning, TEXT("No Shape for weapon collison!"));
//		}
//
//		if (IsValid(CollisionObject))
//		{
//			CollisionObject->SetupAttachment(this);
//		}
//	}
//}
//
//void UBodyCollisionComponent::HandleDamage(const EDamageType DamageType)
//{
//	switch (DamageType)
//	{
//	case EDamageType::None:
//		UE_LOG(LogTemp, Warning, TEXT("No damage type to handle"));
//		return;
//	case EDamageType::Cutting:
//		ApplyBleeding();
//		break;
//	case EDamageType::Mauling:
//		ApplyBoneBreak();
//		break;
//	case EDamageType::Igniting:
//		ApplyBurning();
//		break;
//	case EDamageType::Freezing:
//		ApplyFreezing();
//		break;
//	}
//
//	if (!IsAlive)
//	{
//		if (IsVital)
//		{
//			//make characted dead
//		}
//	}
//}
//
//void  UBodyCollisionComponent::ApplyBleeding()
//{
//}
//
//void  UBodyCollisionComponent::ApplyBoneBreak()
//{
//}
//
//void  UBodyCollisionComponent::ApplyBurning()
//{
//}
//
//void  UBodyCollisionComponent::ApplyFreezing()
//{
//}
//
