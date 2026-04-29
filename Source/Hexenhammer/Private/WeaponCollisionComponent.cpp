// Fill out your copyright notice in the Description page of Project Settings.


#include "WeaponCollisionComponent.h"

// Sets default values for this component's properties
UWeaponCollisionComponent::UWeaponCollisionComponent()
{
}

void UWeaponCollisionComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwner()->HasAuthority() && CollisionObject)
	{
		CollisionObject->OnComponentBeginOverlap.AddDynamic(this, &UWeaponCollisionComponent::OnHexenBeginOverlap);
		WeaponActor = Cast<AHexenWeapon>(GetOwner());
	}
}

void UWeaponCollisionComponent::OnHexenBeginOverlap(UPrimitiveComponent* HitComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& HitResult)
{

	if (GetOwner()->HasAuthority())
	{
		AActor* WeaponOwner = GetOwner() ? GetOwner()->GetOwner() : nullptr;
		if (!OtherActor || OtherActor == WeaponOwner)
		{
			return;
		}

		if (UHexenCollisionComponent* HexenCollisionComponent = Cast<UHexenCollisionComponent>(OtherComp->GetAttachParent()))
		{
			FVector CurrentLocation = CollisionObject->GetComponentLocation();
			FVector LastLocation = LastTransform.GetLocation();
			FQuat CurrentRotation = CollisionObject->GetComponentQuat();

			// Calculating Velocity at the hit moment
			FVector WeaponDeltaPos = CurrentLocation - LastLocation;
			float WeaponDeltaTime = FMath::Max(LastDeltaTime, 0.0001f);  //Maybe its not necessary cause there is no way that BodyDeltaTime will be litteraly zero
			FVector WeaponVelocity = WeaponDeltaPos / WeaponDeltaTime;	//Not very accurate, cause LastDeltaTime is from previos tick, but not critical... About ~10% miscalculation 

			FVector OtherCompDeltaPos = OtherComp->GetComponentLocation() - HexenCollisionComponent->GetLastTransform().GetLocation();
			float OtherCompDeltaTime = FMath::Max(HexenCollisionComponent->GetLastDeltaTime(), 0.0001f); //Maybe its not necessary cause there is no way that BodyDeltaTime will be litteraly zero
			FVector OtherCompVelocity = OtherCompDeltaPos / OtherCompDeltaTime;  //Not very accurate, cause LastDeltaTime is from previos tick, but not critical... About ~10% miscalculation

			FVector Velocity = WeaponVelocity - OtherCompVelocity;
			FVector VelocityNormalized = Velocity.GetSafeNormal();
			float Speed = Velocity.Size() / 100; // Divided by 100 to convert to m/s

			//Calculating some kind of hit angle to reduce the damage if its necessary 
			FHitResult SweepResult;
			bool PerformSweep = GetWorld()->SweepSingleByChannel(
				SweepResult,
				LastLocation,
				CurrentLocation,
				CurrentRotation,
				CollisionObject->GetCollisionObjectType(),
				CollisionObject->GetCollisionShape()
			);
			if (!PerformSweep)
			{
				return;
			}
			FVector ImpactNormal = SweepResult.Normal;
			float GlancingHitFactor = FVector::DotProduct(VelocityNormalized, ImpactNormal);
			//GlancingHitFactor = FMath::Max(0.0f, GlancingHitFactor);  //Не уверен насчет этого
	
			// Calculating plane of blade to check, is hit by a sharpen side of the sword 
			FVector Forward = CollisionObject->GetForwardVector();
			FVector Up = CollisionObject->GetUpVector();
			FVector PlaneNormal = FVector::CrossProduct(Forward, Up);
			PlaneNormal = PlaneNormal.GetSafeNormal();
			FVector PointOnPlane = CollisionObject->GetComponentLocation();
			FPlane BladePlane(PointOnPlane, PlaneNormal);
			float  FlatsideHitReduce = FVector::DotProduct(VelocityNormalized, BladePlane); //Кажется тут неправильно счтается, перепутаны плоскоти. надо перепроверить. 
			if (IsDoubleEdged)
			{
				FlatsideHitReduce = FMath::Abs(FlatsideHitReduce);
			}
			else
			{
				FlatsideHitReduce = FMath::Max(0.0f, FlatsideHitReduce);
			}

			//UE_LOG(LogTemp, Warning, TEXT("Hit! Speed: %f GlancingHitReduce: %f FlatsideHitReduce: %f"), Speed, GlancingHitFactor, FlatsideHitReduce);
			UE_LOG(LogTemp, Warning, TEXT("Hit! GlancingHitReduce: %f VelocityNormalized: %s ImpactNormal: %s"), GlancingHitFactor, *VelocityNormalized.ToString(), *ImpactNormal.ToString());

			FVector VelocityTangent = VelocityNormalized - GlancingHitFactor * ImpactNormal;
			FVector DeflectionDir = VelocityTangent.GetSafeNormal();
			//==============================================================================================================
			// Точка начала стрелок – место контакта (можно также центр коллизии)
			FVector StartLocation = SweepResult.Location;
			// Или если хотите от центра оружия:
			// FVector StartLocation = CollisionObject->GetComponentLocation();

			// Длина стрелок (в сантиметрах)
			const float ArrowLength = 80.0f;

			// Стрелка направления скорости (зелёная)
			FVector VelocityEnd = StartLocation + VelocityNormalized * ArrowLength;
			DrawDebugDirectionalArrow(GetWorld(), StartLocation, VelocityEnd, 10.f, FColor::Green, false, 2.f, 0, 2.f);

			// Стрелка нормали (красная)
			FVector NormalEnd = StartLocation + ImpactNormal * ArrowLength;
			DrawDebugDirectionalArrow(GetWorld(), StartLocation, NormalEnd, 10.f, FColor::Red, false, 2.f, 0, 2.f);

			// Стрелка отклонения (синяя)
			FVector DeflectionEnd = StartLocation + DeflectionDir * ArrowLength;
			DrawDebugDirectionalArrow(GetWorld(), StartLocation, DeflectionEnd, 10.f, FColor::Blue, false, 2.f, 0, 2.f);
			//==============================================================================================================
		
			// Calculating kinetic energy
			float HitEnergy = 0.5f * Mass * Speed * Speed * GlancingHitFactor;
			float HitPircing = Sharpness * HitEnergy * FlatsideHitReduce;
			
			bool ShuoldSlice = false;
			if (ShuoldSlice) //If weapon should slice through other component
			{
			}
			else //If weapon shouldn't slice through other component
			{

			}

			if (UBodyCollisionComponent* BodyParentComponent = Cast<UBodyCollisionComponent>(OtherComp->GetAttachParent()))
			{
				if (HitPircing >= BodyParentComponent->Resilience)
				{
					BodyParentComponent->HandleDamage(EDamageType::Cutting);
					//UE_LOG(LogTemp, Warning, TEXT("Penetreted! HitEnergy: %f HitPircing: %f"), HitEnergy, HitPircing);
				}
				else
				{
					BodyParentComponent->HandleDamage(EDamageType::Mauling);
					//UE_LOG(LogTemp, Warning, TEXT("Mauled! HitEnergy: %f HitPircing: %f"), HitEnergy, HitPircing);
				}
			}
		}
	}
}