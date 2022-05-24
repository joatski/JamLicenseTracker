/*
  Copyright (C) 2022 Michael Noland

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "CoreMinimal.h"
#include "ContentBrowserMenuContexts.h"
#include "ToolMenus.h"

#include "JamAssetLicense.h"

#include "Engine/AssetManagerSettings.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Input/SEditableTextBox.h"

#include "Engine/AssetManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "SSettingsEditorCheckoutNotice.h"
#include "Logging/MessageLog.h"

#include "IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ScopedTransaction.h"

//@TODO: The asset source association is not preserved when an asset is duplicated
// (duplicating an asset doesn't copy metadata and there's currently no engine level delegate for asset or object duplication)

//@TODO: Implement the runtime enumeration of licenses that survived cooking
//  Options:
//    - Create an (editor-only) dependency from every asset to the associated license asset that
//      shares the same source URL, causing it to get cooked automatically
//    - Modify the cook rule for each individual primary asset in the asset manager to only cook
//      if any related asset is getting cooked (TBD on if we can ask that question at the time we need to)
//    - Create a single manifest asset that harvests the other licenses for things being cooked
//      (same problem as above, unsure if we have access to a cook manifest when we need it)
//  Interim/workaround:
//    - Make a manually triggered 'harvest' command that is fed an Audit_InCook style collection

#define LOCTEXT_NAMESPACE "FJamLicenseTrackerModule"

static const TCHAR* MD_AssetSourceURL = TEXT("AssetSourceURL");

class FJamLicenseTrackerEditorModule : public IModuleInterface
{
public:
	using ThisClass = FJamLicenseTrackerEditorModule;

	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		if (!IsRunningGame() && FSlateApplication::IsInitialized())
		{
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&AddAssetMenuOptions));

			// Register to get a warning on startup if settings aren't configured correctly
			UAssetManager::CallOrRegister_OnAssetManagerCreated(FSimpleMulticastDelegate::FDelegate::CreateStatic(&OnAssetManagerCreated));
		}
	}

	virtual void ShutdownModule() override
	{
	}

private:
	// Adds the options to all assets
	static void AddAssetSourceOptions(FToolMenuSection& InSection)
	{
		const TAttribute<FSlateIcon> NoIcon;

		UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
		check(Context);
		TArray<UObject*> SelectedObjects = Context->GetSelectedObjects();

		bool bAnyHaveLicense = false;
		bool bAnyMissingLicense = false;
		FString SharedLicenseAssetID;

		// See if any selected asset have a license and if all of them share the same license
		for (UObject* Obj : SelectedObjects)
		{
			if (UPackage* Package = Obj->GetOutermost())
			{
				if (UMetaData* Metadata = Package->HasMetaData() ? Package->GetMetaData() : nullptr)
				{
					const FString& LicenseAssetID = Metadata->GetValue(Obj, MD_AssetSourceURL);

					if (LicenseAssetID.IsEmpty())
					{
						bAnyMissingLicense = true;
						SharedLicenseAssetID = FString();
					}
					else
					{
						if (!bAnyHaveLicense && !bAnyMissingLicense)
						{
							SharedLicenseAssetID = LicenseAssetID;
						}
						else
						{
							if (LicenseAssetID != SharedLicenseAssetID)
							{
								SharedLicenseAssetID = FString();
							}
						}
						bAnyHaveLicense = true;
					}
				}
				else
				{
					bAnyMissingLicense = true;
				}
			}
		}

		if (!SharedLicenseAssetID.IsEmpty())
		{
			// All assets have a license set, and it's the same one, so skip the submenu and provide a direct open action
			FToolUIActionChoice OpenLicenseURLAction(FExecuteAction::CreateLambda([WeakObjects = Context->SelectedObjects, SharedLicenseAssetID]()
			{
				FPlatformProcess::LaunchURL(*SharedLicenseAssetID, nullptr, nullptr);
			}));

			InSection.AddMenuEntry(
				FName("JamLicenseAction_OpenLicenseURL"),
				LOCTEXT("OpenLicenseURL_Label", "View Source"),
				FText::Format(LOCTEXT("OpenLicenseURL_Tooltip", "Opens the source URL {0}"), FText::AsCultureInvariant(SharedLicenseAssetID)),
				NoIcon,
				OpenLicenseURLAction,
				EUserInterfaceActionType::Button);
		}
		else if (bAnyHaveLicense)
		{
			// At least one had a license, but not all of them have the same license, show a submenu to disambiguate
			InSection.AddSubMenu(
				"ViewLicenses",
				LOCTEXT("ViewLicenseMenu_Label", "View Sources"),
				LOCTEXT("ViewLicenseMenu_Tooltip", "View a list of sources that apply to the selection"),
				FNewToolMenuDelegate::CreateStatic(&ThisClass::CreateLicenseListSubmenu)
			);
		}

		// Add an option to change the license
		{
			FString StartingValue = SharedLicenseAssetID;
			if (bAnyHaveLicense && SharedLicenseAssetID.IsEmpty())
			{
				StartingValue = TEXT("[multiple values]");
			}

			auto SetLicenseURLAction = [WeakObjects = Context->SelectedObjects, StartingValue](const FText& Val, ETextCommit::Type TextCommitType)
			{
				const FString EndingValue = Val.ToString();

				if ((TextCommitType != ETextCommit::OnCleared) && (EndingValue != StartingValue))
				{
					const FScopedTransaction Transaction(LOCTEXT("SetAssetSourceTransaction", "Set Asset Source URL"));

					for (TWeakObjectPtr<UObject> WeakPtr : WeakObjects)
					{
						if (UObject* Asset = WeakPtr.Get())
						{
							if (UPackage* Package = Asset->GetOutermost())
							{
								Package->Modify();
								if (UMetaData* Metadata = Package->GetMetaData())
								{
									if (EndingValue.IsEmpty())
									{
										Metadata->RemoveValue(Asset, MD_AssetSourceURL);
									}
									else
									{
										Metadata->SetValue(Asset, MD_AssetSourceURL, *EndingValue);
									}
								}
							}
						}
					}
				}
			};

			TSharedRef<SWidget> EditURLWidget = SNew(SEditableTextBox)
				.MinDesiredWidth(128.0f)
				.Text(FText::AsCultureInvariant(StartingValue))
				.OnTextCommitted_Lambda(SetLicenseURLAction)
				.ToolTipText(LOCTEXT("LicenseURL_Tooltip", "The URL of the source for the selected assets"));

			InSection.AddEntry(FToolMenuEntry::InitWidget("LicenseURL", EditURLWidget, LOCTEXT("LicenseURL_Label", "Source URL"), /*bNoIndent=*/ true));
		}
	}

	// Adds the UJamAssetLicense specific options
	static void AddJamAssetLicenseOptions(FToolMenuSection& InSection)
	{
		UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
		check(Context);
		
		// Select associated assets option
		{
			FToolUIActionChoice SelectRelatedAssetsAction(FExecuteAction::CreateLambda([WeakObjects = Context->SelectedObjects]()
			{
				TSet<FString> AssetSourceURLs;
				for (TWeakObjectPtr<UObject> WeakPtr : WeakObjects)
				{
					if (UJamAssetLicense* LicenseAsset = Cast<UJamAssetLicense>(WeakPtr.Get()))
					{
						if (!LicenseAsset->AssetSourceURL.IsEmpty())
						{
							AssetSourceURLs.Add(LicenseAsset->AssetSourceURL);
						}
					}
				}

				IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
				const FName NAME_AssetSourceURL(MD_AssetSourceURL);

				TArray<FAssetData> PotentialAssetList;
				AssetRegistry.GetAssetsByTags({ NAME_AssetSourceURL }, /*out*/  PotentialAssetList);

				TArray<FAssetData> MatchingAssetList;
				MatchingAssetList.Reserve(PotentialAssetList.Num());
				for (const FAssetData& AssetData : PotentialAssetList)
				{
					FString TestURL;
					if (AssetData.GetTagValue(NAME_AssetSourceURL, /*out*/ TestURL))
					{
						if (AssetSourceURLs.Contains(TestURL))
						{
							MatchingAssetList.Add(AssetData);
						}
					}
				}

				if (MatchingAssetList.Num() > 0)
				{
					FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
					ContentBrowserModule.Get().SyncBrowserToAssets(MatchingAssetList, /*bAllowLockedBrowsers=*/ false, /*bFocusContentBrowser=*/ true);
				}
			}));

			InSection.AddMenuEntry(
				FName("JamAssetLicenseAction_SelectAssociatedAssets"),
				LOCTEXT("SelectAssociatedAssets_Label", "Select Associated Assets"),
				LOCTEXT("SelectAssociatedAssets_Tooltip", "Selects all assets that have the same asset source URL as this license in the Content Browser"),
				TAttribute<FSlateIcon>(),
				SelectRelatedAssetsAction,
				EUserInterfaceActionType::Button);
		}

		// Browse to the asset source itself
		{
			FToolUIActionChoice ViewAssetSourceAction(FExecuteAction::CreateLambda([WeakObjects = Context->SelectedObjects]()
			{
				TSet<FString> AssetSourceURLs;
				for (TWeakObjectPtr<UObject> WeakPtr : WeakObjects)
				{
					if (UJamAssetLicense* LicenseAsset = Cast<UJamAssetLicense>(WeakPtr.Get()))
					{
						if (!LicenseAsset->AssetSourceURL.IsEmpty())
						{
							AssetSourceURLs.Add(LicenseAsset->AssetSourceURL);
						}
					}
				}

				for (const FString& URL : AssetSourceURLs)
				{
					FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
				}
			}));

			InSection.AddMenuEntry(
				FName("JamAssetLicenseAction_ViewAssetSource"),
				LOCTEXT("ViewAssetSource_Label", "Open Asset Source URL"),
				LOCTEXT("ViewAssetSource_Tooltip", "Browses to the asset source URL associated with this license"),
				TAttribute<FSlateIcon>(),
				ViewAssetSourceAction,
				EUserInterfaceActionType::Button);
		}
	}

	static void AddAssetMenuOptions()
	{
		{
			UToolMenu* AssetContextSubMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.AssetActionsSubMenu");
			FToolMenuSection& LicenseSection = AssetContextSubMenu->AddSection("LicenseSection", LOCTEXT("LicenseSectionMenuHeading", "Asset Source (License)"));

			LicenseSection.AddDynamicEntry("AssetSourceActions", FNewToolMenuSectionDelegate::CreateStatic(&AddAssetSourceOptions));
		}

		{
			UToolMenu* AssetContextMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.JamAssetLicense");
			FToolMenuSection& AssetActionsSection = AssetContextMenu->FindOrAddSection("GetAssetActions");

			AssetActionsSection.AddDynamicEntry("JamAssetLicenseActions", FNewToolMenuSectionDelegate::CreateStatic(&AddJamAssetLicenseOptions));
		}
	}

	static void CreateLicenseListSubmenu(UToolMenu* InMenu)
	{
		FToolMenuSection& LicenseSection = InMenu->AddSection("LicensesSection", LOCTEXT("ViewLicenseSectionMenuHeading", "Sources"));
		
		// Collect license URLs
		TMap<FString, int32> URLUsageMap;
		int32 NumAssetsWithNoURL = 0;
		if (UContentBrowserAssetContextMenuContext* Context = InMenu->FindContext<UContentBrowserAssetContextMenuContext>())
		{
			for (UObject* Asset : Context->GetSelectedObjects())
			{
				if (UPackage* Package = Asset->GetOutermost())
				{
					if (UMetaData* Metadata = Package->HasMetaData() ? Package->GetMetaData() : nullptr)
					{
						const FString& LicenseAssetID = Metadata->GetValue(Asset, MD_AssetSourceURL);
						if (!LicenseAssetID.IsEmpty())
						{
							URLUsageMap.FindOrAdd(LicenseAssetID) += 1;
						}
						else
						{
							++NumAssetsWithNoURL;
						}
					}
					else
					{
						++NumAssetsWithNoURL;
					}
				}
			}
		}

		// Sort the URLs by usage
		TArray<FString> UniqueURLs;
		URLUsageMap.GenerateKeyArray(/*out*/ UniqueURLs);
		UniqueURLs.Sort([&](const FString& A, const FString& B)
		{
			const int32 CountA = URLUsageMap[A];
			const int32 CountB = URLUsageMap[B];

			if (CountA == CountB)
			{
				return A < B;
			}
			else
			{
				return CountA > CountB;
			}
		});

		// Add an option to view the license for each URL
		for (FString URL : UniqueURLs)
		{
			FToolUIActionChoice OpenLicenseURLAction(FExecuteAction::CreateLambda([URL]()
			{
				FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
			}));

			LicenseSection.AddMenuEntry(
				NAME_None,
				FText::Format(LOCTEXT("OpenSingleLicenseURL_Label", "{0}"), FText::AsCultureInvariant(URL)),
				FText::Format(LOCTEXT("OpenSingleLicenseURL_Tooltip", "Opens the license URL {0}\nApplies to {1} {1}|plural(one=asset,other=assets)"), FText::AsCultureInvariant(URL), FText::AsNumber(URLUsageMap[URL])),
				TAttribute<FSlateIcon>(),
				OpenLicenseURLAction,
				EUserInterfaceActionType::Button);
		}

		// Add a placeholder for showing how many assets didn't belong to anyone
		if (NumAssetsWithNoURL > 0)
		{
			LicenseSection.AddMenuEntry(
				NAME_None,
				LOCTEXT("AssetsWithNoLicense", "[no license]"),
				FText::Format(LOCTEXT("AssetsWithNoLicense_Tooltip", "{0} {0}|plural(one=asset has no license,other=assets have no license)"), FText::AsNumber(NumAssetsWithNoURL)),
				TAttribute<FSlateIcon>(),
				FToolUIActionChoice(),
				EUserInterfaceActionType::Button);
		}
	}

	static void ManipulateAssetManagerSettings(TFunction<void()> InnerBody)
	{
		// Check out the ini or make it writable
		UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();

		const FString& ConfigFileName = Settings->GetDefaultConfigFilename();

		bool bSuccess = false;

		FText NotificationOpText;
		if (!SettingsHelpers::IsCheckedOut(ConfigFileName, true))
		{
			FText ErrorMessage;
			bSuccess = SettingsHelpers::CheckOutOrAddFile(ConfigFileName, true, !IsRunningCommandlet(), &ErrorMessage);
			if (bSuccess)
			{
				NotificationOpText = LOCTEXT("CheckedOutAssetManagerIni", "Checked out {0}");
			}
			else
			{
				UE_LOG(LogInit, Error, TEXT("%s"), *ErrorMessage.ToString());
				bSuccess = SettingsHelpers::MakeWritable(ConfigFileName);

				if (bSuccess)
				{
					NotificationOpText = LOCTEXT("MadeWritableAssetManagerIni", "Made {0} writable (you may need to manually add to source control)");
				}
				else
				{
					NotificationOpText = LOCTEXT("FailedToTouchAssetManagerIni", "Failed to check out {0} or make it writable, so no rule was added");
				}
			}
		}
		else
		{
			NotificationOpText = LOCTEXT("UpdatedAssetManagerIni", "Updated {0}");
			bSuccess = true;
		}

		// Add the rule to project settings
		if (bSuccess)
		{
			Settings->Modify(true);

			InnerBody();

			Settings->PostEditChange();
			Settings->TryUpdateDefaultConfigFile();

			UAssetManager::Get().ReinitializeFromConfig();
		}

		// Show a message that the file was checked out/updated and must be submitted
		FNotificationInfo Info(FText::Format(NotificationOpText, FText::FromString(FPaths::GetCleanFilename(ConfigFileName))));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	static void AddJamAssetLicenseRule()
	{
		ManipulateAssetManagerSettings([]() {
			FDirectoryPath DummyPath;
			DummyPath.Path = TEXT("/Game/");

			FPrimaryAssetTypeInfo NewTypeInfo(
				UJamAssetLicense::StaticClass()->GetFName(),
				UJamAssetLicense::StaticClass(),
				/*bHasAnyBlueprintClasses=*/ false,
				/*bIsEditorOnly=*/ true,
				{ DummyPath },
				{});
			NewTypeInfo.Rules.CookRule = EPrimaryAssetCookRule::NeverCook;

			UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();
			Settings->PrimaryAssetTypesToScan.Add(NewTypeInfo);
		});
	}

	static void AddAssetLicenseToAssetRegistryRule()
	{
		ManipulateAssetManagerSettings([]() {
			UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();
			Settings->MetaDataTagsForAssetRegistry.Add(FName(MD_AssetSourceURL));
		});
	}

	static void OnAssetManagerCreated()
	{
		// Make sure there's a rule for UJamAssetLicense
		FPrimaryAssetId DummyAssetId(UJamAssetLicense::StaticClass()->GetFName(), NAME_None);
		FPrimaryAssetRules Rules = UAssetManager::Get().GetPrimaryAssetRules(DummyAssetId);
		if (Rules.IsDefault())
		{
			FMessageLog("LoadErrors").Error()
				->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MissingRuleForJamAssetLicense", "Asset Manager settings do not include an entry for assets of type {0}, which is required for automatic license tracking to function."), FText::FromName(UJamAssetLicense::StaticClass()->GetFName()))))
				->AddToken(FActionToken::Create(LOCTEXT("AddRuleForJamAssetLicense", "Add entry to PrimaryAssetTypesToScan?"), FText(),
					FOnActionTokenExecuted::CreateStatic(&ThisClass::AddJamAssetLicenseRule), true));
		}

		// Make sure the source URL is being put in the asset registry
		const FName NAME_AssetSourceURL(MD_AssetSourceURL);
		const UAssetManagerSettings* AssetManagerSettings = GetDefault<UAssetManagerSettings>();
		if (!AssetManagerSettings->MetaDataTagsForAssetRegistry.Contains(NAME_AssetSourceURL))
		{
			FMessageLog("LoadErrors").Error()
				->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MetaDataNotSavedInAssetRegistry", "Asset Manager settings does not include {0} in MetaDataTagsForAssetRegistry, which is required for automatic license tracking to function."), FText::FromName(NAME_AssetSourceURL))))
				->AddToken(FActionToken::Create(LOCTEXT("AddMetaDataToAssetRegistry", "Add entry to MetaDataTagsForAssetRegistry?"), FText(),
					FOnActionTokenExecuted::CreateStatic(&ThisClass::AddAssetLicenseToAssetRegistryRule), true));
		}
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FJamLicenseTrackerEditorModule, JamLicenseTrackerEditor)
