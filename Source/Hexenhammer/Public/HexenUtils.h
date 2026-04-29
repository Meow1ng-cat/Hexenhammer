#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "HexenUtils.generated.h"

UENUM(BlueprintType)
enum class EFdlCollisionShape : uint8
{
	None UMETA(DisplayName = "None"),
	Sphere UMETA(DisplayName = "FdlSphere"),
	Capsule UMETA(DisplayName = "FdlCapsule"),
	Box UMETA(DisplayName = "FdlBox")
};

UENUM(BlueprintType)
enum class EBodyType : uint8
{
	None UMETA(DisplayName = "None"),
	Vital UMETA(DisplayName = "Vital"),
	Breath UMETA(DisplayName = "Breath"),
	Arm UMETA(DisplayName = "Arm"),
	Leg UMETA(DisplayName = "Leg")
};

UENUM(BlueprintType)
enum class EDamageType : uint8
{
	None UMETA(DisplayName = "None"),
	Cutting UMETA(DisplayName = "Cutting"),
	Mauling UMETA(DisplayName = "Mauling"),
	Igniting UMETA(DisplayName = "Igniting"),
	Freezing UMETA(DisplayName = "Freezing")
};

UENUM(BlueprintType)
enum class EWeaponGripType : uint8
{
	None UMETA(DisplayName = "None"),
	SingleHanded UMETA(DisplayName = "SingleHanded"),
	DoubleHanded UMETA(DisplayName = "DoubleHanded"),
	Bow UMETA(DisplayName = "Bow"),
};

UENUM(BlueprintType)
enum class EWeaponCategory : uint8
{
	Melee,
	Ranged,
	Magic
};

UENUM(BlueprintType)
enum class EWeaponType : uint8
{
	Sword,
	Axe,
	Hammer,
	Pike,
	Dagger,
	//Add more melee weapon here
	ShortBow,
	LongBow,
	Crossbow,
	//Add more range weapon here
	Staff,
	Wand
	//Add more magic weapon here
};
//
//USTRUCT(BlueprintType)
//struct FContactInfo
//{
//    GENERATED_BODY()
//    FVector Point;          // Точка контакта
//    FVector Normal;         // Нормаль (направление от A к B)
//    float PenetrationDepth; // Глубина проникновения
//};
//
//UCLASS()
//class UHexenUtils : public UBlueprintFunctionLibrary
//{
//    GENERATED_BODY()
//
//public:
//    UFUNCTION(BlueprintCallable, Category = "Hexen|Collision", meta = (DevelopmentOnly))
//    static FContactInfo GetPrimitiveComponentsIntersection(
//        UPrimitiveComponent* FirstComponent, 
//        UPrimitiveComponent* SecondComponent);
//
//    // OBB Utils
//    UFUNCTION(BlueprintCallable, Category = "Hexen|Collision", meta = (DevelopmentOnly))
//    static FContactInfo GetBoxBoxIntersectionSAT(
//        const FTransform& BoxATransform, const FVector& BoxAExtent,
//        const FTransform& BoxBTransform, const FVector& BoxBExtent);
////
////    UFUNCTION(BlueprintCallable, Category = "Hexen|Collision")
////    static FVector GetClosestPointOnOBB(
////        const FVector& Point,
////        const FTransform& BoxTransform,
////        const FVector& BoxExtent);
////
////    // Sphere Utils  
////    UFUNCTION(BlueprintCallable, Category = "Hexen|Collision")
////    static TArray<FVector> GetSphereBoxIntersectionPointsSimple(
////        const FVector& SphereCenter, float SphereRadius,
////        const FTransform& BoxTransform, const FVector& BoxExtent);
////
////    // Capsule Utils
////    UFUNCTION(BlueprintCallable, Category = "Hexen|Collision")
////    static TArray<FVector> GetCapsuleBoxIntersectionPoints(
////        const FHexenCapsule& Capsule,
////        const FTransform& BoxTransform,
////        const FVector& BoxExtent);
////
////    // Хелперы (приватные)
//private:
//    static TArray<FVector> GetBoxCorners(const FVector& ExtentMin, const FVector& ExtentMax);
//    static TArray<FVector> GetBoxAxes(const FTransform& Transform);
//    static FVector FindContactPoint(const TArray<FVector>& CornersA,
//        const TArray<FVector>& CornersB,
//        const FVector& Normal,
//        float PenetrationDepth);
//};