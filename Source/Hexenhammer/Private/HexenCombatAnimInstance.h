// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "HexenCombatAnimInstance.generated.h"

/**
 * Deliberately empty, and probably not needed at all any more.
 *
 * Combat state now lives on UHexenCombatComponent, on the character, which any Animation Blueprint can
 * read without being parented to a particular class. This is kept only so an Animation Blueprint that
 * was already parented to it still loads. Reparent that asset to AnimInstance and this can go.
 */
UCLASS()
class UHexenCombatAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
};
