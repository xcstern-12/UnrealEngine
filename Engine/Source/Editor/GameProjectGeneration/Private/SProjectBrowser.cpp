// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.


#include "GameProjectGenerationPrivatePCH.h"
#include "MainFrame.h"
#include "DesktopPlatformModule.h"

#define LOCTEXT_NAMESPACE "ProjectBrowser"


/**
 * Structure for project items.
 */
struct FProjectItem
{
	FText Name;
	FText Description;
	FString ProjectFile;
	TSharedPtr<FSlateBrush> ProjectThumbnail;
	bool bIsNewProjectItem;

	FProjectItem(const FText& InName, const FText& InDescription, const TSharedPtr<FSlateBrush>& InProjectThumbnail, const FString& InProjectFile, bool InIsNewProjectItem)
		: Name(InName)
		, Description(InDescription)
		, ProjectFile(InProjectFile)
		, ProjectThumbnail(InProjectThumbnail)
		, bIsNewProjectItem(InIsNewProjectItem)
	{ }
};


/**
 * Structure for project categories.
 */
struct FProjectCategory
{
	FText CategoryName;
	TSharedPtr< STileView< TSharedPtr<FProjectItem> > > ProjectTileView;
	TArray< TSharedPtr<FProjectItem> > ProjectItemsSource;
	TArray< TSharedPtr<FProjectItem> > FilteredProjectItemsSource;
};


void ProjectItemToString(const TSharedPtr<FProjectItem> InItem, TArray<FString>& OutFilterStrings)
{
	OutFilterStrings.Add(InItem->Name.ToString());
}


/* SCompoundWidget interface
 *****************************************************************************/

SProjectBrowser::SProjectBrowser()
	: ProjectItemFilter( ProjectItemTextFilter::FItemToStringArray::CreateStatic( ProjectItemToString ))
	, NumFilteredProjects(0)
{

}


BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
void SProjectBrowser::Construct( const FArguments& InArgs )
{
	NewProjectScreenRequestedDelegate = InArgs._OnNewProjectScreenRequested;
	bPreventSelectionChangeEvent = false;
	ThumbnailBorderPadding = 5;
	ThumbnailSize = 128.0f;
	bAllowProjectCreate = InArgs._AllowProjectCreate;

	// Prepare the categories box
	CategoriesBox = SNew(SVerticalBox);

	// Find all projects
	FindProjects();

	CategoriesBox->AddSlot()
	.HAlign(HAlign_Center)
	.Padding(FMargin(0.f, 25.f))
	[
		SNew(STextBlock)
		.Visibility(this, &SProjectBrowser::GetNoProjectsErrorVisibility)
		.Text(LOCTEXT("NoProjects", "You don't have any projects yet :("))
	];

	CategoriesBox->AddSlot()
	.HAlign(HAlign_Center)
	.Padding(FMargin(0.f, 25.f))
	[
		SNew(STextBlock)
		.Visibility(this, &SProjectBrowser::GetNoProjectsAfterFilterErrorVisibility)
		.Text(LOCTEXT("NoProjectsAfterFilter", "There are no projects that match the specified filter"))
	];

	ChildSlot
	[
		SNew(SVerticalBox)

		// Page description
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(10.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SelectProjectDescription", "Select an existing project from the list below."))
		]

		// Categories
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage( FEditorStyle::GetBrush("ToolPanel.GroupBorder") )
			.Padding(4)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.VAlign(VAlign_Top)
				.Padding(FMargin(5.f, 10.f))
				[
					SNew(SHorizontalBox)
					+SHorizontalBox::Slot()
					[
						SNew(SOverlay)

						+SOverlay::Slot()
						[
							SNew(SSearchBox)
							.HintText(LOCTEXT("FilterHint", "Filter Projects..."))
							.OnTextChanged(this, &SProjectBrowser::OnFilterTextChanged)
						]

						+SOverlay::Slot()
						[
							SNew(SBorder)
							.Visibility(this, &SProjectBrowser::GetFilterActiveOverlayVisibility)
							.BorderImage(FEditorStyle::Get().GetBrush("SearchBox.ActiveBorder"))
						]
					]

					+SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.OnClicked(this, &SProjectBrowser::FindProjects)
						.ToolTipText(LOCTEXT("RefreshProjectList", "Refresh the project list"))
						[
							SNew(SImage).Image(FEditorStyle::Get().GetBrush("Icons.Refresh"))
						]
					]
				]

				+ SVerticalBox::Slot()
				[
					SNew(SScrollBox)

					+ SScrollBox::Slot()
					[
						CategoriesBox.ToSharedRef()
					]
				]
			]
		]

		+ SVerticalBox::Slot()
		.Padding( 0, 40, 0, 0 )	// Lots of vertical padding before the dialog buttons at the bottom
		.AutoHeight()
		[
			SNew( SHorizontalBox )

			// Auto-load project
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(4, 0)
			[
				SNew(SCheckBox)			
				.IsChecked(GEditor->GetGameAgnosticSettings().bLoadTheMostRecentlyLoadedProjectAtStartup)
				.OnCheckStateChanged(this, &SProjectBrowser::OnAutoloadLastProjectChanged)
				.Content()
				[
					SNew(STextBlock).Text(LOCTEXT("AutoloadOnStartupCheckbox", "Always load last project on startup"))
				]
			]

			// Browse Button
			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("BrowseProjectButton", "Browse..."))
				.OnClicked(this, &SProjectBrowser::OnBrowseToProjectClicked)
				.ContentPadding( FCoreStyle::Get().GetMargin("StandardDialog.ContentPadding") )
			]

			// Open Button
			+SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("OpenProjectButton", "Open"))
				.OnClicked(this, &SProjectBrowser::HandleOpenProjectButtonClicked)
				.IsEnabled(this, &SProjectBrowser::HandleOpenProjectButtonIsEnabled)
				.ContentPadding( FCoreStyle::Get().GetMargin("StandardDialog.ContentPadding") )
			]
		]
	];

	// Select the first item in the first category
	if (ProjectCategories.Num() > 0)
	{
		const TSharedRef<FProjectCategory> Category = ProjectCategories[0];
		if ( ensure(Category->ProjectItemsSource.Num() > 0) && ensure(Category->ProjectTileView.IsValid()))
		{
			Category->ProjectTileView->SetSelection(Category->ProjectItemsSource[0], ESelectInfo::Direct);
		}
	}

	bHasProjectFiles = false;
	for (auto CategoryIt = ProjectCategories.CreateConstIterator(); CategoryIt; ++CategoryIt)
	{
		const TSharedRef<FProjectCategory>& Category = *CategoryIt;

		if (Category->ProjectItemsSource.Num() > 0)
		{
			bHasProjectFiles = true;
			break;
		}
	}
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

void SProjectBrowser::ConstructCategory( const TSharedRef<SVerticalBox>& InCategoriesBox, const TSharedRef<FProjectCategory>& Category ) const
{
	// Title
	InCategoriesBox->AddSlot()
	.Padding(FMargin(5.f, 0.f))
	.AutoHeight()
	[
		SNew(STextBlock)
		.Visibility(this, &SProjectBrowser::GetProjectCategoryVisibility, Category)
		.TextStyle(FEditorStyle::Get(), "GameProjectDialog.ProjectNamePathLabels")
		.Text(Category->CategoryName)
	];

	// Separator
	InCategoriesBox->AddSlot()
	.AutoHeight()
	.Padding(5.0f, 2.0f, 5.0f, 8.0f)
	[
		SNew(SSeparator)
		.Visibility(this, &SProjectBrowser::GetProjectCategoryVisibility, Category)
	];

	// Project tile view
	InCategoriesBox->AddSlot()
	.AutoHeight()
	.Padding(5.0f, 0.0f, 5.0f, 40.0f)
	[
		SAssignNew(Category->ProjectTileView, STileView<TSharedPtr<FProjectItem> >)
		.Visibility(this, &SProjectBrowser::GetProjectCategoryVisibility, Category)
		.ListItemsSource(&Category->FilteredProjectItemsSource)
		.SelectionMode(ESelectionMode::Single)
		.ClearSelectionOnClick(false)
		.OnGenerateTile(this, &SProjectBrowser::MakeProjectViewWidget)
		.OnContextMenuOpening(this, &SProjectBrowser::OnGetContextMenuContent)
		.OnMouseButtonDoubleClick(this, &SProjectBrowser::HandleProjectItemDoubleClick)
		.OnSelectionChanged(TSlateDelegates<TSharedPtr<FProjectItem>>::FOnSelectionChanged::CreateSP(this, &SProjectBrowser::HandleProjectViewSelectionChanged, Category->CategoryName))
		.ItemHeight(ThumbnailSize + ThumbnailBorderPadding + 32)
		.ItemWidth(ThumbnailSize + ThumbnailBorderPadding)
	];
}

bool SProjectBrowser::HasProjects() const
{
	return bHasProjectFiles;
}


/* SProjectBrowser implementation
 *****************************************************************************/

TSharedRef<ITableRow> SProjectBrowser::MakeProjectViewWidget(TSharedPtr<FProjectItem> ProjectItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	if ( !ensure(ProjectItem.IsValid()) )
	{
		return SNew( STableRow<TSharedPtr<FProjectItem>>, OwnerTable );
	}

	TSharedPtr<SWidget> Thumbnail;
	if ( ProjectItem->bIsNewProjectItem )
	{
		Thumbnail = SNew(SBox)
			.Padding(ThumbnailBorderPadding)
			[
				SNew(SBorder)
				.Padding(0)
				.BorderImage(FEditorStyle::GetBrush("MarqueeSelection"))
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.TextStyle( FEditorStyle::Get(), "GameProjectDialog.NewProjectTitle" )
						.Text( LOCTEXT("NewProjectThumbnailText", "NEW") )
					]
				]
			];
	}
	else
	{
		// Drop shadow border
		Thumbnail =	SNew(SBorder)
			.Padding(ThumbnailBorderPadding)
			.BorderImage( FEditorStyle::GetBrush("ContentBrowser.ThumbnailShadow") )
			[
				SNew(SImage).Image(this, &SProjectBrowser::GetProjectItemImage, TWeakPtr<FProjectItem>(ProjectItem))
			];
	}

	TSharedRef<ITableRow> TableRow = SNew( STableRow<TSharedPtr<FProjectItem>>, OwnerTable )
	.Style(FEditorStyle::Get(), "GameProjectDialog.TemplateListView.TableRow")
	[
		SNew(SBox)
		.HeightOverride(ThumbnailSize+ThumbnailBorderPadding+5)
		[
			SNew(SVerticalBox)

			// Thumbnail
			+SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.WidthOverride(ThumbnailSize + ThumbnailBorderPadding * 2)
				.HeightOverride(ThumbnailSize + ThumbnailBorderPadding * 2)
				[
					Thumbnail.ToSharedRef()
				]
			]

			// Name
			+SVerticalBox::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Top)
			[
				SNew(STextBlock)
				.HighlightText(this, &SProjectBrowser::GetItemHighlightText)
				.Text(ProjectItem->Name)
			]
		]
	];

	TableRow->AsWidget()->SetToolTip(MakeProjectToolTip(ProjectItem));

	return TableRow;
}


TSharedRef<SToolTip> SProjectBrowser::MakeProjectToolTip( TSharedPtr<FProjectItem> ProjectItem ) const
{
	// Create a box to hold every line of info in the body of the tooltip
	TSharedRef<SVerticalBox> InfoBox = SNew(SVerticalBox);

	if(!ProjectItem->Description.IsEmpty())
	{
		AddToToolTipInfoBox( InfoBox, LOCTEXT("ProjectTileTooltipDescription", "Description"), ProjectItem->Description );
	}

	{
		const FString ProjectPath = FPaths::GetPath(ProjectItem->ProjectFile);
		AddToToolTipInfoBox( InfoBox, LOCTEXT("ProjectTileTooltipPath", "Path"), FText::FromString(ProjectPath) );
	}

	TSharedRef<SToolTip> Tooltip = SNew(SToolTip)
	.TextMargin(1)
	.BorderImage( FEditorStyle::GetBrush("ProjectBrowser.TileViewTooltip.ToolTipBorder") )
	[
		SNew(SBorder)
		.Padding(6)
		.BorderImage( FEditorStyle::GetBrush("ProjectBrowser.TileViewTooltip.NonContentBorder") )
		[
			SNew(SVerticalBox)

			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(SBorder)
				.Padding(6)
				.BorderImage( FEditorStyle::GetBrush("ProjectBrowser.TileViewTooltip.ContentBorder") )
				[
					SNew(SVerticalBox)

					+SVerticalBox::Slot()
					.AutoHeight()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text( ProjectItem->Name )
						.Font( FEditorStyle::GetFontStyle("ProjectBrowser.TileViewTooltip.NameFont") )
					]
				]
			]

			+SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.Padding(6)
				.BorderImage( FEditorStyle::GetBrush("ProjectBrowser.TileViewTooltip.ContentBorder") )
				[
					InfoBox
				]
			]
		]
	];

	return Tooltip;
}


void SProjectBrowser::AddToToolTipInfoBox(const TSharedRef<SVerticalBox>& InfoBox, const FText& Key, const FText& Value) const
{
	InfoBox->AddSlot()
	.AutoHeight()
	.Padding(0, 1)
	[
		SNew(SHorizontalBox)

		+SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(STextBlock) .Text( FText::Format(LOCTEXT("ProjectBrowserTooltipFormat", "{0}:"), Key ) )
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]

		+SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(STextBlock) .Text( Value )
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
	];
}


TSharedPtr<SWidget> SProjectBrowser::OnGetContextMenuContent() const
{
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

	TSharedPtr<FProjectItem> SelectedProjectItem = GetSelectedProjectItem();
	const FText ProjectContextActionsText = (SelectedProjectItem.IsValid()) ? SelectedProjectItem->Name : LOCTEXT("ProjectActionsMenuHeading", "Project Actions");
	MenuBuilder.BeginSection("ProjectContextActions", ProjectContextActionsText);

	FFormatNamedArguments Args;
	Args.Add(TEXT("FileManagerName"), FPlatformMisc::GetFileManagerName());
	const FText ExploreToText = FText::Format(NSLOCTEXT("GenericPlatform", "ShowInFileManager", "Show In {FileManagerName}"), Args);

	MenuBuilder.AddMenuEntry(
		ExploreToText,
		LOCTEXT("FindInExplorerTooltip", "Finds this project on disk"),
		FSlateIcon(),
		FUIAction(
		FExecuteAction::CreateSP( this, &SProjectBrowser::ExecuteFindInExplorer ),
		FCanExecuteAction::CreateSP( this, &SProjectBrowser::CanExecuteFindInExplorer )
		)
		);

	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}


void SProjectBrowser::ExecuteFindInExplorer() const
{
	TSharedPtr<FProjectItem> SelectedProjectItem = GetSelectedProjectItem();
	check(SelectedProjectItem.IsValid());
	FPlatformProcess::ExploreFolder(*SelectedProjectItem->ProjectFile);
}


bool SProjectBrowser::CanExecuteFindInExplorer() const
{
	TSharedPtr<FProjectItem> SelectedProjectItem = GetSelectedProjectItem();
	return SelectedProjectItem.IsValid();
}


const FSlateBrush* SProjectBrowser::GetProjectItemImage(TWeakPtr<FProjectItem> ProjectItem) const
{
	if ( ProjectItem.IsValid() )
	{
		TSharedPtr<FProjectItem> Item = ProjectItem.Pin();
		if ( Item->ProjectThumbnail.IsValid() )
		{
			return Item->ProjectThumbnail.Get();
		}
	}
	
	return FEditorStyle::GetBrush("GameProjectDialog.DefaultGameThumbnail");
}


TSharedPtr<FProjectItem> SProjectBrowser::GetSelectedProjectItem() const
{
	for ( auto CategoryIt = ProjectCategories.CreateConstIterator(); CategoryIt; ++CategoryIt )
	{
		const TSharedRef<FProjectCategory>& Category = *CategoryIt;
		TArray< TSharedPtr<FProjectItem> > SelectedItems = Category->ProjectTileView->GetSelectedItems();
		if ( SelectedItems.Num() > 0 )
		{
			return SelectedItems[0];
		}
	}
	
	return NULL;
}


FText SProjectBrowser::GetSelectedProjectName() const
{
	TSharedPtr<FProjectItem> SelectedItem = GetSelectedProjectItem();
	if ( SelectedItem.IsValid() )
	{
		return SelectedItem->Name;
	}

	return FText::GetEmpty();
}

FReply SProjectBrowser::FindProjects()
{
	ProjectCategories.Empty();
	CategoriesBox->ClearChildren();

	const FText CreateProjectCategoryName = LOCTEXT("NewProjectCategoryName", "Create Project");
	const FText MyProjectsCategoryName = LOCTEXT("MyProjectsCategoryName", "My Projects");
	const FText ProjectsCategoryName = LOCTEXT("ProjectsCategoryName", "My Projects");
	const FText SampleGamesCategoryName = LOCTEXT("SampleGamesCategoryName", "Sample Games");
	const FText DemoletsCategoryName = LOCTEXT("DemoletsCategoryName", "Demolets");
	const FText ShowcasesCategoryName = LOCTEXT("ShowcasesCategoryName", "Showcases");

	//// Add the option to create a new game to the "Create Project" category.
	//if (bAllowProjectCreate)
	//{
	//	const bool bIsNewProjectItem = true;
	//	TSharedRef<FProjectItem> NewProjectItem = MakeShareable(new FProjectItem(LOCTEXT("CreateNewProjectItem", "New Project"), FText(), NULL, TEXT(""), bIsNewProjectItem));
	//	AddProjectToCategory(NewProjectItem, CreateProjectCategoryName);
	//}

	// Form a list of all known project files.
	TMap<FString, FText> DiscoveredProjectFilesToCategory;

	// Start with recents
	for (auto RecentIt = GEditor->GetGameAgnosticSettings().RecentlyOpenedProjectFiles.CreateConstIterator(); RecentIt; ++RecentIt)
	{
		const FString File = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(**RecentIt);
		DiscoveredProjectFilesToCategory.Add(File, MyProjectsCategoryName);
	}

	// Form a list of folders that may contain project files and their category names.
	struct FFolderToCategory
	{
		FString Folder;
		FText Category;

		FFolderToCategory(const FString& InFolder, const FText& InCategory)
			: Folder(InFolder), Category(InCategory)
		{}
	};

	TArray<FFolderToCategory> ProjectFileDiscoverFoldersToCategory;

	// Add the default creation path, in case you've never created a project before or want to include this path anyway
	ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(GameProjectUtils::GetDefaultProjectCreationPath(), MyProjectsCategoryName));

	// Add in every path that the user has ever created a project file. This is to catch new projects showing up in the user's project folders
	for (auto CreatedPathIt = GEditor->GetGameAgnosticSettings().CreatedProjectPaths.CreateConstIterator(); CreatedPathIt; ++CreatedPathIt)
	{
		ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(*CreatedPathIt, MyProjectsCategoryName));
	}

	// @todo discover projects in the "holding tank" perhaps
	// Hard-coding a few paths for now
	ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(FPaths::RootDir(), ProjectsCategoryName));
	ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(FPaths::Combine(*FPaths::RootDir(), TEXT("Samples")), SampleGamesCategoryName));
	ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(FPaths::Combine(*FPaths::RootDir(), TEXT("Samples"), TEXT("SampleGames")), SampleGamesCategoryName));
	ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(FPaths::Combine(*FPaths::RootDir(), TEXT("Samples"), TEXT("Showcases")), ShowcasesCategoryName));
	ProjectFileDiscoverFoldersToCategory.Add(FFolderToCategory(FPaths::Combine(*FPaths::RootDir(), TEXT("Samples"), TEXT("Demolets")), DemoletsCategoryName));

	// Discover all unique project files in all discover folders
	for (auto RootFolderIt = ProjectFileDiscoverFoldersToCategory.CreateConstIterator(); RootFolderIt; ++RootFolderIt)
	{
		const FString Root = (*RootFolderIt).Folder;
		const FText Category = (*RootFolderIt).Category;
		const FString RootSearchString = Root / TEXT("*");
		TArray<FString> PotentialProjectFolders;
		IFileManager::Get().FindFiles(PotentialProjectFolders, *RootSearchString, /*Files=*/false, /*Directories=*/true);
		for (auto FolderIt = PotentialProjectFolders.CreateConstIterator(); FolderIt; ++FolderIt)
		{
			const FString FolderName = Root / (*FolderIt);
			const FString ProjectSearchString = FolderName / TEXT("*.") + IProjectManager::GetProjectFileExtension();
			TArray<FString> FoundProjectFiles;
			IFileManager::Get().FindFiles(FoundProjectFiles, *ProjectSearchString, /*Files=*/true, /*Directories=*/false);
			for (auto ProjectFilenameIt = FoundProjectFiles.CreateConstIterator(); ProjectFilenameIt; ++ProjectFilenameIt)
			{
				const FString PotentiallyRelativeFile = FolderName / (*ProjectFilenameIt);
				const FString AbsoluteFile = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*PotentiallyRelativeFile);
				DiscoveredProjectFilesToCategory.Add(AbsoluteFile, Category);
			}
		}
	}

	// Add all discovered projects to the list
	for ( auto ProjectFilenameIt = DiscoveredProjectFilesToCategory.CreateConstIterator(); ProjectFilenameIt; ++ProjectFilenameIt )
	{
		const FString& ProjectFilename = ProjectFilenameIt.Key();
		const FText& DetectedCategory = ProjectFilenameIt.Value();
		if ( FPaths::FileExists(ProjectFilename) )
		{
			const bool bPromptIfSavedWithNewerVersionOfEngine = false;
			FProjectStatus ProjectStatus;
			if (IProjectManager::Get().QueryStatusForProject(ProjectFilename, FDesktopPlatformModule::Get()->GetCurrentEngineIdentifier(), ProjectStatus))
			{
				// @todo localized project name
				const FText ProjectName = FText::FromString(ProjectStatus.Name);
				const FText ProjectDescription = FText::FromString(ProjectStatus.Description);

				TSharedPtr<FSlateDynamicImageBrush> DynamicBrush;
				const FString ThumbnailPNGFile = FPaths::GetBaseFilename(ProjectFilename, false) + TEXT(".png");
				const FString AutoScreenShotPNGFile = FPaths::Combine(*FPaths::GetPath(ProjectFilename), TEXT("Saved"), TEXT("AutoScreenshot.png"));
				FString PNGFileToUse;
				if ( FPaths::FileExists(*ThumbnailPNGFile) )
				{
					PNGFileToUse = ThumbnailPNGFile;
				}
				else if ( FPaths::FileExists(*AutoScreenShotPNGFile) )
				{
					PNGFileToUse = AutoScreenShotPNGFile;
				}

				if ( !PNGFileToUse.IsEmpty() )
				{
					const FName BrushName = FName(*PNGFileToUse);
					DynamicBrush = MakeShareable( new FSlateDynamicImageBrush(BrushName, FVector2D(128,128) ) );
				}

				FText ProjectCategory;
				if ( ProjectStatus.bSignedSampleProject )
				{
					if(ProjectStatus.Category == "Sample Games")
					{
						ProjectCategory = SampleGamesCategoryName;
					}
					else if(ProjectStatus.Category == "Demolets")
					{
						ProjectCategory = DemoletsCategoryName;
					}
					else if(ProjectStatus.Category == "Showcases")
					{
						ProjectCategory = ShowcasesCategoryName;
					}
				}
				else
				{
					ProjectCategory = (ProjectStatus.Category.IsEmpty()) ? DetectedCategory : FText::FromString(ProjectStatus.Category);
				}

				if ( ProjectCategory.IsEmpty() )
				{
					ProjectCategory = MyProjectsCategoryName;
				}

				const bool bIsNewProjectItem = false;
				TSharedRef<FProjectItem> NewProjectItem = MakeShareable( new FProjectItem(ProjectName, ProjectDescription, DynamicBrush, ProjectFilename, bIsNewProjectItem ) );
				AddProjectToCategory(NewProjectItem, ProjectCategory);
			}
		}
	}

	// Make sure the category order is "Create Project", "My Projects", "Projects", "Sample Games", "Demolets", "Showcases" then all remaining categories in alphabetical order
	TSharedPtr<FProjectCategory> CreateProjectCategory;
	TSharedPtr<FProjectCategory> MyProjectsCategory;
	TSharedPtr<FProjectCategory> ProjectsCategory;
	TSharedPtr<FProjectCategory> SamplesCategory;
	TSharedPtr<FProjectCategory> DemoletsCategory;
	TSharedPtr<FProjectCategory> ShowcasesCategory;

	for ( int32 CategoryIdx = ProjectCategories.Num() - 1; CategoryIdx >= 0; --CategoryIdx )
	{
		TSharedRef<FProjectCategory> Category = ProjectCategories[CategoryIdx];
		if ( Category->CategoryName.EqualTo(CreateProjectCategoryName) )
		{
			CreateProjectCategory = Category;
			ProjectCategories.RemoveAt(CategoryIdx);
		}
		else if ( Category->CategoryName.EqualTo(MyProjectsCategoryName) )
		{
			MyProjectsCategory = Category;
			ProjectCategories.RemoveAt(CategoryIdx);
		}
		else if ( Category->CategoryName.EqualTo(ProjectsCategoryName) )
		{
			ProjectsCategory = Category;
			ProjectCategories.RemoveAt(CategoryIdx);
		}
		else if ( Category->CategoryName.EqualTo(SampleGamesCategoryName) )
		{
			SamplesCategory = Category;
			ProjectCategories.RemoveAt(CategoryIdx);
		}
		else if ( Category->CategoryName.EqualTo(DemoletsCategoryName) )
		{
			DemoletsCategory = Category;
			ProjectCategories.RemoveAt(CategoryIdx);
		}
		else if ( Category->CategoryName.EqualTo(ShowcasesCategoryName) )
		{
			ShowcasesCategory = Category;
			ProjectCategories.RemoveAt(CategoryIdx);
		}
	}

	// Sort categories
	struct FCompareCategories
	{
		FORCEINLINE bool operator()( const TSharedRef<FProjectCategory>& A, const TSharedRef<FProjectCategory>& B ) const
		{
			return A->CategoryName.CompareToCaseIgnored(B->CategoryName) < 0;
		}
	};
	ProjectCategories.Sort( FCompareCategories() );

	// Now readd the built-in categories (last added is first in the list)
	if ( DemoletsCategory.IsValid() )
	{
		ProjectCategories.Insert(DemoletsCategory.ToSharedRef(), 0);
	}
	if ( ShowcasesCategory.IsValid() )
	{
		ProjectCategories.Insert(ShowcasesCategory.ToSharedRef(), 0);
	}
	if ( SamplesCategory.IsValid() )
	{
		ProjectCategories.Insert(SamplesCategory.ToSharedRef(), 0);
	}
	if ( ProjectsCategory.IsValid() )
	{
		ProjectCategories.Insert(ProjectsCategory.ToSharedRef(), 0);
	}
	if ( MyProjectsCategory.IsValid() )
	{
		ProjectCategories.Insert(MyProjectsCategory.ToSharedRef(), 0);
	}
	if ( CreateProjectCategory.IsValid() )
	{
		ProjectCategories.Insert(CreateProjectCategory.ToSharedRef(), 0);
	}

	// Sort each individual category
	struct FCompareProjectItems
	{
		FORCEINLINE bool operator()( const TSharedPtr<FProjectItem>& A, const TSharedPtr<FProjectItem>& B ) const
		{
			return A->Name.CompareToCaseIgnored(B->Name) < 0;
		}
	};
	for ( int32 CategoryIdx = 0; CategoryIdx < ProjectCategories.Num(); ++CategoryIdx )
	{
		TSharedRef<FProjectCategory> Category = ProjectCategories[CategoryIdx];
		Category->ProjectItemsSource.Sort( FCompareProjectItems() );
	}

	PopulateFilteredProjectCategories();

	for (auto CategoryIt = ProjectCategories.CreateConstIterator(); CategoryIt; ++CategoryIt)
	{
		const TSharedRef<FProjectCategory>& Category = *CategoryIt;
		ConstructCategory(CategoriesBox.ToSharedRef(), Category);
	}

	return FReply::Handled();
}


void SProjectBrowser::AddProjectToCategory(const TSharedRef<FProjectItem>& ProjectItem, const FText& ProjectCategory)
{
	for ( auto CategoryIt = ProjectCategories.CreateConstIterator(); CategoryIt; ++CategoryIt )
	{
		const TSharedRef<FProjectCategory>& Category = *CategoryIt;
		if ( Category->CategoryName.EqualToCaseIgnored(ProjectCategory) )
		{
			Category->ProjectItemsSource.Add(ProjectItem);
			return;
		}
	}

	TSharedRef<FProjectCategory> NewCategory = MakeShareable( new FProjectCategory );
	NewCategory->CategoryName = ProjectCategory;
	NewCategory->ProjectItemsSource.Add(ProjectItem);
	ProjectCategories.Add(NewCategory);
}


void SProjectBrowser::PopulateFilteredProjectCategories()
{
	NumFilteredProjects = 0;
	for (auto& Category : ProjectCategories)
	{
		Category->FilteredProjectItemsSource.Empty();

		for (auto& ProjectItem : Category->ProjectItemsSource)
		{
			if (ProjectItemFilter.PassesFilter(ProjectItem))
			{
				Category->FilteredProjectItemsSource.Add(ProjectItem);
				++NumFilteredProjects;
			}
		}

		if (Category->ProjectTileView.IsValid())
		{
			Category->ProjectTileView->RequestListRefresh();
		}
	}
}

FReply SProjectBrowser::OnKeyDown( const FGeometry& MyGeometry, const FKeyboardEvent& InKeyboardEvent )
{
	if (InKeyboardEvent.GetKey() == EKeys::F5)
	{
		return FindProjects();
	}

	return FReply::Unhandled();
}


bool SProjectBrowser::OpenProject( const FString& ProjectFile )
{
	FText FailReason;

	if (!GameProjectUtils::OpenProject(ProjectFile, FailReason))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FailReason);

		return false;
	}

	return true;
}


void SProjectBrowser::OpenSelectedProject( )
{
	if ( CurrentSelectedProjectPath.IsEmpty() )
	{
		return;
	}

	OpenProject( CurrentSelectedProjectPath.ToString() );
}

/* SProjectBrowser event handlers
 *****************************************************************************/

FReply SProjectBrowser::HandleOpenProjectButtonClicked( )
{
	OpenSelectedProject();

	return FReply::Handled();
}


bool SProjectBrowser::HandleOpenProjectButtonIsEnabled( ) const
{
	return !CurrentSelectedProjectPath.IsEmpty();
}


void SProjectBrowser::HandleProjectItemDoubleClick( TSharedPtr<FProjectItem> TemplateItem )
{
	OpenSelectedProject();
}

FReply SProjectBrowser::OnBrowseToProjectClicked()
{
	const FString ProjectFileDescription = LOCTEXT( "FileTypeDescription", "Unreal Project File" ).ToString();
	const FString ProjectFileExtension = FString::Printf(TEXT("*.%s"), *IProjectManager::GetProjectFileExtension());
	const FString FileTypes = FString::Printf( TEXT("%s (%s)|%s"), *ProjectFileDescription, *ProjectFileExtension, *ProjectFileExtension );

	// Find the first valid project file to select by default
	FString DefaultFolder = FEditorDirectories::Get().GetLastDirectory(ELastDirectory::PROJECT);
	for ( auto ProjectIt = GEditor->GetGameAgnosticSettings().RecentlyOpenedProjectFiles.CreateConstIterator(); ProjectIt; ++ProjectIt )
	{
		if ( IFileManager::Get().FileSize(**ProjectIt) > 0 )
		{
			// This is the first uproject file in the recents list that actually exists
			DefaultFolder = FPaths::GetPath(*ProjectIt);
			break;
		}
	}

	// Prompt the user for the filenames
	TArray<FString> OpenFilenames;
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	bool bOpened = false;
	if ( DesktopPlatform )
	{
		void* ParentWindowWindowHandle = NULL;

		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
		const TSharedPtr<SWindow>& MainFrameParentWindow = MainFrameModule.GetParentWindow();
		if ( MainFrameParentWindow.IsValid() && MainFrameParentWindow->GetNativeWindow().IsValid() )
		{
			ParentWindowWindowHandle = MainFrameParentWindow->GetNativeWindow()->GetOSWindowHandle();
		}

		bOpened = DesktopPlatform->OpenFileDialog(
			ParentWindowWindowHandle,
			LOCTEXT("OpenProjectBrowseTitle", "Open Project").ToString(),
			DefaultFolder,
			TEXT(""),
			FileTypes,
			EFileDialogFlags::None,
			OpenFilenames
		);
	}

	if ( bOpened && OpenFilenames.Num() > 0 )
	{
		HandleProjectViewSelectionChanged( NULL, ESelectInfo::Direct, FText() );

		FString Path = OpenFilenames[0];
		if ( FPaths::IsRelative( Path ) )
		{
			Path = FPaths::ConvertRelativePathToFull( Path );
		}

		CurrentSelectedProjectPath = FText::FromString( Path );

		OpenSelectedProject();
	}

	return FReply::Handled();
}


void SProjectBrowser::HandleProjectViewSelectionChanged(TSharedPtr<FProjectItem> ProjectItem, ESelectInfo::Type SelectInfo, FText CategoryName)
{
	if ( !bPreventSelectionChangeEvent )
	{
		TGuardValue<bool> SelectionEventGuard(bPreventSelectionChangeEvent, true);

		for ( auto CategoryIt = ProjectCategories.CreateConstIterator(); CategoryIt; ++CategoryIt )
		{
			const TSharedRef<FProjectCategory>& Category = *CategoryIt;
			if ( Category->ProjectTileView.IsValid() && !Category->CategoryName.EqualToCaseIgnored(CategoryName) )
			{
				Category->ProjectTileView->ClearSelection();
			}
		}

		const TSharedPtr<FProjectItem> SelectedItem = GetSelectedProjectItem();
		if ( SelectedItem.IsValid() && SelectedItem != CurrentlySelectedItem )
		{
			CurrentSelectedProjectPath = FText::FromString( SelectedItem->ProjectFile );
		}
	}
}

void SProjectBrowser::OnFilterTextChanged(const FText& InText)
{
	ProjectItemFilter.SetRawFilterText(InText);
	PopulateFilteredProjectCategories();
}

void SProjectBrowser::OnAutoloadLastProjectChanged(ESlateCheckBoxState::Type NewState)
{
	UEditorGameAgnosticSettings &Settings = GEditor->AccessGameAgnosticSettings();
	Settings.bLoadTheMostRecentlyLoadedProjectAtStartup = (NewState == ESlateCheckBoxState::Checked);

	UProperty* AutoloadProjectProperty = FindField<UProperty>(Settings.GetClass(), "bLoadTheMostRecentlyLoadedProjectAtStartup");
	if (AutoloadProjectProperty != NULL)
	{
		FPropertyChangedEvent PropertyUpdateStruct(AutoloadProjectProperty);
		Settings.PostEditChangeProperty(PropertyUpdateStruct);
	}
}

EVisibility SProjectBrowser::GetProjectCategoryVisibility(const TSharedRef<FProjectCategory> InCategory) const
{
	if (NumFilteredProjects == 0)
	{
		return EVisibility::Collapsed;
	}
	return InCategory->FilteredProjectItemsSource.Num() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SProjectBrowser::GetNoProjectsErrorVisibility() const
{
	return bHasProjectFiles ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SProjectBrowser::GetNoProjectsAfterFilterErrorVisibility() const
{
	return (bHasProjectFiles && NumFilteredProjects == 0) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SProjectBrowser::GetFilterActiveOverlayVisibility() const
{
	return ProjectItemFilter.GetRawFilterText().IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
}

FText SProjectBrowser:: GetItemHighlightText() const
{
	return ProjectItemFilter.GetRawFilterText();
}

#undef LOCTEXT_NAMESPACE
