#include "Traffic/DeliveryTrafficCarComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeliveryTrafficFadeTest, "Delivery.Traffic.FadeMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeliveryTrafficFadeTest::RunTest(const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("test world"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	AStaticMeshActor* Car = World->SpawnActor<AStaticMeshActor>();
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Original = LoadObject<UMaterialInterface>(nullptr,
	    TEXT("/Game/materials/plastic_red.plastic_red"));
	if (!TestNotNull(TEXT("car"), Car) || !TestNotNull(TEXT("mesh"), Mesh) ||
	    !TestNotNull(TEXT("original paint"), Original)) return false;
	Car->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	Car->GetStaticMeshComponent()->SetMaterial(0, Original);
	UDeliveryTrafficCarComponent* Traffic = NewObject<UDeliveryTrafficCarComponent>(Car);
	Car->AddInstanceComponent(Traffic);
	const FVector OriginalScale = Car->GetActorScale3D();
	Traffic->bMaterialFadeReady = Traffic->PrepareFadeMaterials();
	TestTrue(TEXT("paint has fade counterpart"), Traffic->bMaterialFadeReady);
	if (!Traffic->bMaterialFadeReady) return false;
	Traffic->ApplyFadeAlpha(0.f);
	UMaterialInstanceDynamic* Fade = Cast<UMaterialInstanceDynamic>(
	    Car->GetStaticMeshComponent()->GetMaterial(0));
	float Opacity = -1.f;
	TestTrue(TEXT("first frame uses fade material"), Fade != nullptr);
	TestTrue(TEXT("first frame has opacity parameter"),
	    Fade && Fade->GetScalarParameterValue(FMaterialParameterInfo(TEXT("FadeOpacity")), Opacity));
	TestEqual(TEXT("first frame starts fully invisible"), Opacity, 0.f);
	Traffic->ApplyFadeAlpha(0.5f);
	TestTrue(TEXT("fade window uses dynamic fade material"),
	    Cast<UMaterialInstanceDynamic>(Car->GetStaticMeshComponent()->GetMaterial(0)) != nullptr);
	TestEqual(TEXT("fade does not resize collision or car"), Car->GetActorScale3D(), OriginalScale);
	Traffic->ApplyFadeAlpha(1.f);
	TestEqual(TEXT("normal driving restores exact original paint"),
	    Car->GetStaticMeshComponent()->GetMaterial(0), Original);
	return true;
}
#endif
