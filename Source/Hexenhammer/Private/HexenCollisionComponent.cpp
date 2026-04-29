// Fill out your copyright notice in the Description page of Project Settings.


#include "HexenCollisionComponent.h"

// Sets default values for this component's properties
UHexenCollisionComponent::UHexenCollisionComponent()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

#if !UE_BUILD_SHIPPING
	SetIsReplicatedByDefault(true); // Если в будущем вам потребуется, чтобы клиент как-то взаимодействовал с этим компонентом 
									//(например, для клиентских предсказаний, визуальных эффектов или отладки), придётся пересмотреть это решение.
#endif
}

void UHexenCollisionComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwner()->HasAuthority())
	{
		PrimaryComponentTick.SetTickFunctionEnable(true);

		if (CollisionObject)
		{
			CollisionObject->CreationMethod = EComponentCreationMethod::Instance;
			CollisionObject->RegisterComponent();
			CollisionObject->SetActive(true);
			CollisionObject->SetCollisionObjectType(ECC_GameTraceChannel1);
			CollisionObject->SetCollisionResponseToAllChannels(ECR_Ignore);
			CollisionObject->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
			CollisionObject->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			CollisionObject->SetGenerateOverlapEvents(true);

			LastTransform = CollisionObject->GetComponentTransform();
		}
	}
	else
	{
		PrimaryComponentTick.SetTickFunctionEnable(false);
	}

#if !UE_BUILD_SHIPPING
	if (CollisionObject)
	{
		if (GetOwner()->HasAuthority())
		{
			UE_LOG(LogTemp, Warning, TEXT("CollisionShape is %d"), CollisionShape);
			if (CollisionObject->IsRegistered())
			{
				UE_LOG(LogTemp, Warning, TEXT("CollisionObject is registered"));
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("CollisionObject is not registered"));
			}
		}

		if (GetNetMode() != NM_DedicatedServer)
		{
			CollisionObject->SetHiddenInGame(false);
			CollisionObject->SetVisibility(true);
		}
	}
#endif
}

void UHexenCollisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (GetOwner()->HasAuthority())
	{
		LastDeltaTime = DeltaTime;
		if (CollisionObject)
		{
			LastTransform = CollisionObject->GetComponentTransform();
		}
	}
}


#if WITH_EDITOR
void UHexenCollisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	FName PropertyName = PropertyChangedEvent.Property ?
		PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHexenCollisionComponent, CollisionShape))
	{
		SetShape();
	}
}
#endif

void UHexenCollisionComponent::SetShape()
{
	if (IsValid(this))
	{
		if (CollisionShape == EFdlCollisionShape::None)
		{
		}
		else if (CollisionShape == EFdlCollisionShape::Sphere)
		{
			CollisionObject = NewObject<USphereComponent>(this, TEXT("SphereCollision"), RF_NoFlags);
			Cast<USphereComponent>(CollisionObject)->SetSphereRadius(50.0f);
		}
		else if (CollisionShape == EFdlCollisionShape::Capsule)
		{
			CollisionObject = NewObject<UCapsuleComponent>(this, TEXT("BoxCollision"), RF_NoFlags);
			Cast<UCapsuleComponent>(CollisionObject)->SetCapsuleSize(50.0f, 100.0f);
		}
		else if (CollisionShape == EFdlCollisionShape::Box)
		{
			CollisionObject = NewObject<UBoxComponent>(this, TEXT("CapsuleCollision"), RF_NoFlags);
			Cast<UBoxComponent>(CollisionObject)->SetBoxExtent(FVector(50, 50, 50));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("No Shape for weapon collison!"));
		}

		if (IsValid(CollisionObject))
		{
			CollisionObject->SetupAttachment(this);
		}
	}
}

void UHexenCollisionComponent::OnHexenBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& HitResult)
{
	UE_LOG(LogTemp, Error, TEXT("UHexenCollisionComponent::OnBeginOverlap has been called!"));
}