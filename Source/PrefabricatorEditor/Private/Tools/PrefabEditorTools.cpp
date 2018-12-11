//$ Copyright 2015-18, Code Respawn Technologies Pvt Ltd - All Rights Reserved $//

#include "Tools/PrefabEditorTools.h"

#include "PrefabActor.h"
#include "PrefabricatorAsset.h"
#include "PrefabricatorAssetUserData.h"

#include "Editor/EditorEngine.h"
#include "Engine/Selection.h"
#include "GameFramework/Actor.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "PrefabComponent.h"
#include "MemoryReader.h"
#include "ObjectAndNameAsStringProxyArchive.h"
#include "EngineUtils.h"
#include "ObjectWriter.h"
#include "ObjectReader.h"
#include "UnrealMemory.h"

DEFINE_LOG_CATEGORY_STATIC(LogPrefabEditorTools, Log, All);

bool FPrefabEditorTools::CanCreatePrefab()
{
	if (!GEditor) {
		return false;
	}

	int32 NumSelected = GEditor->GetSelectedActorCount();
	return NumSelected > 0;
}

void FPrefabEditorTools::CreatePrefab()
{
	TArray<AActor*> SelectedActors;
	GetSelectedActors(SelectedActors);

	CreatePrefabFromActors(SelectedActors);
}

void FPrefabEditorTools::CreatePrefabFromActors(const TArray<AActor*>& Actors)
{
	if (Actors.Num() == 0) {
		return;
	}

	UWorld* World = Actors[0]->GetWorld();

	FVector Pivot = FPrefabricatorAssetUtils::FindPivot(Actors);
	APrefabActor* PrefabActor = World->SpawnActor<APrefabActor>(Pivot, FRotator::ZeroRotator);

	// Find the compatible mobility for the prefab actor
	EComponentMobility::Type Mobility = FPrefabricatorAssetUtils::FindMobility(Actors);
	PrefabActor->GetRootComponent()->SetMobility(Mobility);

	UPrefabricatorAsset* PrefabAsset = CreatePrefabAsset();
	PrefabActor->PrefabComponent->PrefabAsset = PrefabAsset;

	// Attach the actors to the prefab
	if (GEditor) {
		for (AActor* Actor : Actors) {
			if (Actor->GetRootComponent()) {
				Actor->GetRootComponent()->SetMobility(Mobility);
			}

			GEditor->ParentActors(PrefabActor, Actor, NAME_None);
		}
	}

	SaveStateToPrefabAsset(PrefabActor);

	if (GEditor) {
		GEditor->SelectNone(true, true);
		GEditor->SelectActor(PrefabActor, true, true);
	}
}

void FPrefabEditorTools::GetSelectedActors(TArray<AActor*>& OutActors)
{
	if (GEditor) {
		USelection* SelectedActors = GEditor->GetSelectedActors();
		for (FSelectionIterator Iter(*SelectedActors); Iter; ++Iter)
		{
			// We only care about actors that are referenced in the world for literals, and also in the same level as this blueprint
			AActor* Actor = Cast<AActor>(*Iter);
			if (Actor) 
			{
				OutActors.Add(Actor);
			}
		}
	}
}

void FPrefabEditorTools::AssignAssetUserData(AActor* InActor, APrefabActor* Prefab)
{
	if (!InActor || !InActor->GetRootComponent()) {
		return;
	}
	
	UPrefabricatorAssetUserData* PrefabUserData = NewObject<UPrefabricatorAssetUserData>(InActor->GetRootComponent());
	PrefabUserData->PrefabActor = Prefab;
	InActor->GetRootComponent()->AddAssetUserData(PrefabUserData);
}

UPrefabricatorAsset* FPrefabEditorTools::CreatePrefabAsset()
{
	IContentBrowserSingleton& ContentBrowserSingleton = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
	TArray<FString> SelectedFolders;
	ContentBrowserSingleton.GetSelectedPathViewFolders(SelectedFolders);
	FString PrefabFolder = SelectedFolders.Num() > 0 ? SelectedFolders[0] : "/Game";
	FString PrefabPath = PrefabFolder + "/Prefab";

	FString PackageName, AssetName;
	IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.CreateUniqueAssetName(*PrefabPath, TEXT(""), PackageName, AssetName);
	UPrefabricatorAsset* PrefabAsset = Cast<UPrefabricatorAsset>(AssetTools.CreateAsset(AssetName, PrefabFolder, UPrefabricatorAsset::StaticClass(), nullptr));

	ContentBrowserSingleton.SyncBrowserToAssets(TArray<UObject*>({ PrefabAsset }));

	return PrefabAsset;
}



void FPrefabEditorTools::SaveStateToPrefabAsset(APrefabActor* PrefabActor)
{
	if (!PrefabActor) {
		UE_LOG(LogPrefabEditorTools, Error, TEXT("Invalid prefab actor reference"));
		return;
	}

	UPrefabricatorAsset* PrefabAsset = PrefabActor->PrefabComponent->PrefabAsset;
	if (!PrefabAsset) {
		UE_LOG(LogPrefabEditorTools, Error, TEXT("Prefab asset is not assigned correctly"));
		return;
	}

	PrefabAsset->ActorData.Reset();

	TArray<AActor*> Children;
	GetActorChildren(PrefabActor, Children);

	for (AActor* ChildActor : Children) {
		AssignAssetUserData(ChildActor, PrefabActor);
		int32 NewItemIndex = PrefabAsset->ActorData.AddDefaulted();
		FPrefabricatorActorData& ActorData = PrefabAsset->ActorData[NewItemIndex];
		SaveStateToPrefabAsset(ChildActor, PrefabActor, ActorData);
	}
}

namespace {

	void GetPropertyData(UProperty* Property, UObject* Obj, FString& OutPropertyData) {
		Property->ExportTextItem(OutPropertyData, Property->ContainerPtrToValuePtr<void>(Obj), nullptr, Obj, PPF_None);
	}

	bool ContainsOuterParent(UObject* ObjectToTest, UObject* Outer) {
		while (ObjectToTest) {
			if (ObjectToTest == Outer) return true;
			ObjectToTest = ObjectToTest->GetOuter();
		}
		return false;
	}

	bool HasDefaultValue(UProperty* Property, UObject* ObjToSerialize) {
		FString PropertyData, TemplatePropertyData;
		GetPropertyData(Property, ObjToSerialize, PropertyData);
		GetPropertyData(Property, ObjToSerialize->GetArchetype(), TemplatePropertyData);
		return (PropertyData == TemplatePropertyData);
	}

	bool ShouldSkipSerialization(UProperty* Property, UObject* ObjToSerialize, APrefabActor* PrefabActor) {
		if (UObjectProperty* ObjProperty = Cast<UObjectProperty>(Property)) {
			UObject* PropertyObjectValue = ObjProperty->GetObjectPropertyValue_InContainer(ObjToSerialize);
			if (ContainsOuterParent(PropertyObjectValue, ObjToSerialize) ||
				ContainsOuterParent(PropertyObjectValue, PrefabActor)) {
				UE_LOG(LogPrefabEditorTools, Warning, TEXT("Skipping Property: %s"), *Property->GetName());
				return true;
			}
		}

		return false;
	}

	void DeserializeFields(UObject* InObjToDeserialize, const TArray<UPrefabricatorPropertyBase*>& InProperties) {
		TMap<FString, UPrefabricatorPropertyBase*> PropertiesByName;
		for (UPrefabricatorPropertyBase* Property : InProperties) {
			PropertiesByName.Add(Property->PropertyName, Property);
		}

		for (TFieldIterator<UProperty> PropertyIterator(InObjToDeserialize->GetClass()); PropertyIterator; ++PropertyIterator) {
			UProperty* Property = *PropertyIterator;
			if (!Property) continue;
			UPrefabricatorPropertyBase** SearchResult = PropertiesByName.Find(Property->GetName());
			if (!SearchResult) continue;
			UPrefabricatorPropertyBase* PrefabProperty = *SearchResult;

			if (UPrefabricatorAtomProperty* Atom = Cast<UPrefabricatorAtomProperty>(PrefabProperty)) {
				Property->ImportText(*Atom->ExportedValue, Property->ContainerPtrToValuePtr<void>(InObjToDeserialize), PPF_None, InObjToDeserialize);
			}
			else if (UPrefabricatorArrayProperty* Array = Cast<UPrefabricatorArrayProperty>(PrefabProperty)) {
				// ...
			}
			else if (UPrefabricatorSetProperty* Set = Cast<UPrefabricatorSetProperty>(PrefabProperty)) {
				// ...
			}
			else if (UPrefabricatorMapProperty* Map = Cast<UPrefabricatorMapProperty>(PrefabProperty)) {
				// ...
			}
		}
	}

	void SerializeFields(UObject* ObjToSerialize, APrefabActor* PrefabActor, TArray<UPrefabricatorPropertyBase*>& OutProperties) {
		UPrefabricatorAsset* PrefabAsset = PrefabActor->PrefabComponent->PrefabAsset;
		if (!PrefabAsset) {
			return;
		}

		for (TFieldIterator<UProperty> PropertyIterator(ObjToSerialize->GetClass()); PropertyIterator; ++PropertyIterator) {
			UProperty* Property = *PropertyIterator;

			UPrefabricatorPropertyBase* PrefabProperty = nullptr;
			FString PropertyName = Property->GetName();

			if (UArrayProperty* ArrayProperty = Cast<UArrayProperty>(Property)) {
				UPrefabricatorArrayProperty* ArrayPrefabProperty = NewObject<UPrefabricatorArrayProperty>(PrefabAsset);
				ArrayPrefabProperty->PropertyName = PropertyName;

				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(ObjToSerialize));
				UProperty* ArrayItemProperty = ArrayProperty->Inner;

				for (int i = 0; i < ArrayHelper.Num(); i++) {
					void* ArrayItemData = ArrayHelper.GetRawPtr(i);
					FString ItemValue;
					ArrayItemProperty->ExportTextItem(ItemValue, ArrayItemData, nullptr, ObjToSerialize, PPF_None);
					ArrayPrefabProperty->ExportedValues.Add(ItemValue);
				}

				PrefabProperty = ArrayPrefabProperty;
			}
			else if (USetProperty* SetProperty = Cast<USetProperty>(Property)) {
				UPrefabricatorSetProperty* SetPrefabProperty = NewObject<UPrefabricatorSetProperty>(PrefabAsset);
				SetPrefabProperty->PropertyName = PropertyName;

				FScriptSetHelper SetHelper(SetProperty, SetProperty->ContainerPtrToValuePtr<void>(ObjToSerialize));
				UProperty* SetItemProperty = SetProperty->ElementProp;

				for (int i = 0; i < SetHelper.Num(); i++) {
					void* SetItemData = SetHelper.GetElementPtr(i);
					FString ItemValue;
					SetItemProperty->ExportTextItem(ItemValue, SetItemData, nullptr, ObjToSerialize, PPF_None);
					SetPrefabProperty->ExportedValues.Add(ItemValue);
				}

				PrefabProperty = SetPrefabProperty;
			}
			else if (UMapProperty* MapProperty = Cast<UMapProperty>(Property)) {
				UPrefabricatorMapProperty* MapPrefabProperty = NewObject<UPrefabricatorMapProperty>(PrefabAsset);
				MapPrefabProperty->PropertyName = PropertyName;

				FScriptMapHelper MapHelper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(ObjToSerialize));
				UProperty* MapKeyProperty = MapProperty->KeyProp;
				UProperty* MapValueProperty = MapProperty->ValueProp;
				
				for (int i = 0; i < MapHelper.Num(); i++) {
					void* KeyData = MapHelper.GetKeyPtr(i);
					void* ValueData = MapHelper.GetValuePtr(i);
					
					int32 EntryIdx = MapPrefabProperty->ExportedEntries.AddDefaulted();
					FPrefabricatorMapPropertyEntry& Entry = MapPrefabProperty->ExportedEntries[EntryIdx];
					MapKeyProperty->ExportTextItem(Entry.ExportedKey, KeyData, nullptr, ObjToSerialize, PPF_None);
					MapValueProperty->ExportTextItem(Entry.ExportedValue, ValueData, nullptr, ObjToSerialize, PPF_None);
				}

				PrefabProperty = MapPrefabProperty;
			}
			else {
				if (HasDefaultValue(Property, ObjToSerialize) || ShouldSkipSerialization(Property, ObjToSerialize, PrefabActor)) {
					continue;
				}

				FString PropertyValue;
				Property->ExportTextItem(PropertyValue, Property->ContainerPtrToValuePtr<void>(ObjToSerialize), nullptr, ObjToSerialize, PPF_None);

				UPrefabricatorAtomProperty* AtomPrefabProperty = NewObject<UPrefabricatorAtomProperty>(PrefabAsset);
				AtomPrefabProperty->PropertyName = PropertyName;
				AtomPrefabProperty->ExportedValue = PropertyValue;

				PrefabProperty = AtomPrefabProperty;
				
			}

			if (PrefabProperty) {
				OutProperties.Add(PrefabProperty);
			}
		}
	}

	void CollectAllSubobjects(UObject* Object, TArray<UObject*>& OutSubobjectArray)
	{
		const bool bIncludedNestedObjects = true;
		GetObjectsWithOuter(Object, OutSubobjectArray, bIncludedNestedObjects);

		// Remove contained objects that are not subobjects.
		for (int32 ComponentIndex = 0; ComponentIndex < OutSubobjectArray.Num(); ComponentIndex++)
		{
			UObject* PotentialComponent = OutSubobjectArray[ComponentIndex];
			if (!PotentialComponent->IsDefaultSubobject() && !PotentialComponent->HasAnyFlags(RF_DefaultSubObject))
			{
				OutSubobjectArray.RemoveAtSwap(ComponentIndex--);
			}
		}
	}

	void DumpSerializedProperties(const TArray<UPrefabricatorPropertyBase*>& InProperties) {
		for (UPrefabricatorPropertyBase* Property : InProperties) {
			if (UPrefabricatorAtomProperty* Atom = Cast<UPrefabricatorAtomProperty>(Property)) {
				UE_LOG(LogPrefabEditorTools, Log, TEXT("%s: %s"), *Atom->PropertyName, *Atom->ExportedValue);
			}
			else if (UPrefabricatorArrayProperty* Array = Cast<UPrefabricatorArrayProperty>(Property)) {
				UE_LOG(LogPrefabEditorTools, Log, TEXT("%s: Array[%d]"), *Array->PropertyName, Array->ExportedValues.Num());
				for (int i = 0; i < Array->ExportedValues.Num(); i++) {
					UE_LOG(LogPrefabEditorTools, Log, TEXT("\t%s"), *Array->ExportedValues[i]);
				}
			}
			else if (UPrefabricatorSetProperty* Set = Cast<UPrefabricatorSetProperty>(Property)) {
				UE_LOG(LogPrefabEditorTools, Log, TEXT("%s: Set[%d]"), *Set->PropertyName, Set->ExportedValues.Num());
				for (int i = 0; i < Set->ExportedValues.Num(); i++) {
					UE_LOG(LogPrefabEditorTools, Log, TEXT("\t%s"), *Set->ExportedValues[i]);
				}
			}
			else if (UPrefabricatorMapProperty* Map = Cast<UPrefabricatorMapProperty>(Property)) {
				UE_LOG(LogPrefabEditorTools, Log, TEXT("%s: Map[%d]"), *Map->PropertyName, Map->ExportedEntries.Num());
				for (int i = 0; i < Map->ExportedEntries.Num(); i++) {
					UE_LOG(LogPrefabEditorTools, Log, TEXT("\t%s <=> %s"), *Map->ExportedEntries[i].ExportedKey, *Map->ExportedEntries[i].ExportedValue);
				}
			}
		}

	}

	void DumpSerializedData(const FPrefabricatorActorData& InActorData) {
		UE_LOG(LogPrefabEditorTools, Log, TEXT("Actor Properties: %s"), *InActorData.ClassPath);
		UE_LOG(LogPrefabEditorTools, Log, TEXT("================="));
		DumpSerializedProperties(InActorData.Properties);

		for (const FPrefabricatorComponentData& ComponentData : InActorData.Components) {
			UE_LOG(LogPrefabEditorTools, Log, TEXT(""));
			UE_LOG(LogPrefabEditorTools, Log, TEXT("Component Properties: %s"), *ComponentData.ComponentName);
			UE_LOG(LogPrefabEditorTools, Log, TEXT("================="));
			DumpSerializedProperties(ComponentData.Properties);
		}
	}
}

void FPrefabEditorTools::SaveStateToPrefabAsset(AActor* InActor, APrefabActor* PrefabActor, FPrefabricatorActorData& OutActorData)
{
	FTransform InversePrefabTransform = PrefabActor->GetTransform().Inverse();
	FTransform LocalTransform = InActor->GetTransform() * InversePrefabTransform;
	OutActorData.RelativeTransform = LocalTransform;
	OutActorData.ClassPath = InActor->GetClass()->GetPathName();

	SerializeFields(InActor, PrefabActor, OutActorData.Properties);

	TArray<UActorComponent*> Components;
	InActor->GetComponents(Components);

	for (UActorComponent* Component : Components) {
		int32 ComponentDataIdx = OutActorData.Components.AddDefaulted();
		FPrefabricatorComponentData& ComponentData = OutActorData.Components[ComponentDataIdx];
		ComponentData.ComponentName = Component->GetPathName(InActor);
		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component)) {
			ComponentData.RelativeTransform = SceneComponent->GetComponentTransform();
		}
		else {
			ComponentData.RelativeTransform = FTransform::Identity;
		}
		SerializeFields(Component, PrefabActor, ComponentData.Properties);
	}

	DumpSerializedData(OutActorData);
}

void FPrefabEditorTools::LoadStateFromPrefabAsset(AActor* InActor, APrefabActor* PrefabActor, const FPrefabricatorActorData& InActorData)
{
	DeserializeFields(InActor, InActorData.Properties);

	TMap<FString, UActorComponent*> ComponentsByName;
	for (UActorComponent* Component : InActor->GetComponents()) {
		FString ComponentPath = Component->GetPathName(InActor);
		ComponentsByName.Add(ComponentPath, Component);
	}

	for (const FPrefabricatorComponentData& ComponentData : InActorData.Components) {
		if (UActorComponent** SearchResult = ComponentsByName.Find(ComponentData.ComponentName)) {
			UActorComponent* Component = *SearchResult;
			DeserializeFields(Component, ComponentData.Properties);
		}
	}
}

void FPrefabEditorTools::GetActorChildren(AActor* InParent, TArray<AActor*>& OutChildren)
{
	InParent->GetAttachedActors(OutChildren);
}

void FPrefabEditorTools::LoadStateFromPrefabAsset(APrefabActor* PrefabActor)
{
	if (!PrefabActor) {
		UE_LOG(LogPrefabEditorTools, Error, TEXT("Invalid prefab actor reference"));
		return;
	}

	UPrefabricatorAsset* PrefabAsset = PrefabActor->PrefabComponent->PrefabAsset;
	if (!PrefabAsset) {
		UE_LOG(LogPrefabEditorTools, Error, TEXT("Prefab asset is not assigned correctly"));
		return;
	}

	TMap<UClass*, TArray<AActor*>> ExistingActorPool;

	TArray<AActor*> Children;
	GetActorChildren(PrefabActor, Children);

	for (AActor* ChildActor : Children) {
		TArray<AActor*>& ActorsByClass = ExistingActorPool.FindOrAdd(ChildActor->GetClass());
		ActorsByClass.Add(ChildActor);
	}

	for (FPrefabricatorActorData& ActorItemData : PrefabAsset->ActorData) {
		UClass* ActorClass = LoadObject<UClass>(nullptr, *ActorItemData.ClassPath);
		if (!ActorClass) return;

		UWorld* World = PrefabActor->GetWorld();
		AActor* ChildActor = nullptr;
		TArray<AActor*>& ActorPoolByClass = ExistingActorPool.FindOrAdd(ActorClass);
		if (ActorPoolByClass.Num() > 0) {
			ChildActor = ActorPoolByClass[0];
			ActorPoolByClass.RemoveAt(0);
		}
		else {
			// The pool is exhausted for this class type. Spawn a new actor
			ChildActor = World->SpawnActor<AActor>(ActorClass);
		}

		// Load the saved data into the actor
		LoadStateFromPrefabAsset(ChildActor, PrefabActor, ActorItemData);

		GEditor->ParentActors(PrefabActor, ChildActor, NAME_None);
		AssignAssetUserData(ChildActor, PrefabActor);

		// Set the transform
		FTransform WorldTransform = ActorItemData.RelativeTransform * PrefabActor->GetTransform();
		ChildActor->SetActorTransform(WorldTransform);
	}

	// Delete the unused actors from the pool
	for (auto& Entry : ExistingActorPool) {
		TArray<AActor*>& ActorsByClass = Entry.Value;
		for (AActor* Actor : ActorsByClass) {
			if (Actor && Actor->GetRootComponent()) {
				UPrefabricatorAssetUserData* PrefabUserData = Actor->GetRootComponent()->GetAssetUserData<UPrefabricatorAssetUserData>();
				if (PrefabUserData && PrefabUserData->PrefabActor == PrefabActor) {
					Actor->Destroy();
				}
			}
		}
	}
	ExistingActorPool.Reset();
}

