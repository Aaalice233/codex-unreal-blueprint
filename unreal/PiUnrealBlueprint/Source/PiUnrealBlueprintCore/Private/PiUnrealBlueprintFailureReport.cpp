#include "PiUnrealBlueprintFailureReport.h"

#include "PiUnrealBlueprintSourceControl.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"

namespace PiUnrealBlueprint
{
    namespace
    {
        FString QuoteCommandArgument(const FString& Value)
        {
            FString Escaped = Value;
            Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
            return FString::Printf(TEXT("\"%s\""), *Escaped);
        }

        TArray<TSharedPtr<FJsonValue>> FailureReportStringsToJson(const TArray<FString>& Values)
        {
            TArray<TSharedPtr<FJsonValue>> Json;
            for (const FString& Value : Values)
            {
                Json.Add(MakeShared<FJsonValueString>(Value));
            }
            return Json;
        }
    }

    const TCHAR* LexToString(const EWriteAssetState State)
    {
        switch (State)
        {
        case EWriteAssetState::Modified: return TEXT("modified");
        case EWriteAssetState::Saved: return TEXT("saved");
        case EWriteAssetState::NotSaved: return TEXT("notSaved");
        case EWriteAssetState::Unknown: return TEXT("unknown");
        default: return TEXT("unknown");
        }
    }

    const TCHAR* LexToString(const EWorkingCopyKind Kind)
    {
        switch (Kind)
        {
        case EWorkingCopyKind::Git: return TEXT("git");
        case EWorkingCopyKind::Svn: return TEXT("svn");
        default: return TEXT("none");
        }
    }

    TSharedRef<FJsonObject> FWriteAssetFailureState::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("packageName"), PackageName);
        Json->SetStringField(TEXT("filename"), Filename);
        Json->SetStringField(TEXT("state"), LexToString(State));
        Json->SetStringField(TEXT("beforeHash"), BeforeHash);
        Json->SetStringField(TEXT("lastConfirmedHash"), LastConfirmedHash);
        Json->SetBoolField(TEXT("saveAttempted"), bSaveAttempted);
        Json->SetBoolField(TEXT("saveSucceeded"), bSaveSucceeded);
        Json->SetBoolField(TEXT("reloadVerified"), bReloadVerified);
        return Json;
    }

    void FWriteFailureReport::Finalize(const FString& ProjectDirectory)
    {
        ReadOnlyInspectionCommands.Reset();
        ManualRecoveryAdvice.Reset();
        WorkingCopy = FWriteSourceControl::DetectWorkingCopy(ProjectDirectory, WorkingCopyRoot);

        TArray<FString> AffectedFiles;
        for (const FWriteAssetFailureState& Asset : Assets)
        {
            if (!Asset.Filename.IsEmpty())
            {
                AffectedFiles.AddUnique(FPaths::ConvertRelativePathToFull(Asset.Filename));
            }
        }

        if (WorkingCopy == EWorkingCopyKind::Git)
        {
            for (const FString& Filename : AffectedFiles)
            {
                ReadOnlyInspectionCommands.Add(FString::Printf(TEXT("git -C %s status --short -- %s"),
                    *QuoteCommandArgument(WorkingCopyRoot), *QuoteCommandArgument(Filename)));
                ReadOnlyInspectionCommands.Add(FString::Printf(TEXT("git -C %s diff -- %s"),
                    *QuoteCommandArgument(WorkingCopyRoot), *QuoteCommandArgument(Filename)));
            }
            ManualRecoveryAdvice.Add(TEXT("Review every listed file with the read-only Git commands before restoring anything."));
            ManualRecoveryAdvice.Add(TEXT("After review, restore only the confirmed files manually with your normal Git client; do not reset the working copy."));
        }
        else if (WorkingCopy == EWorkingCopyKind::Svn)
        {
            for (const FString& Filename : AffectedFiles)
            {
                ReadOnlyInspectionCommands.Add(FString::Printf(TEXT("svn status %s"), *QuoteCommandArgument(Filename)));
                ReadOnlyInspectionCommands.Add(FString::Printf(TEXT("svn diff %s"), *QuoteCommandArgument(Filename)));
            }
            ManualRecoveryAdvice.Add(TEXT("Review every listed file with the read-only SVN commands before restoring anything."));
            ManualRecoveryAdvice.Add(TEXT("After review, revert only the confirmed files manually with your normal SVN client; never revert the whole working copy."));
        }
        else
        {
            ManualRecoveryAdvice.Add(TEXT("No Git or SVN working copy was detected. Preserve the listed files and inspect them before making further edits."));
            ManualRecoveryAdvice.Add(TEXT("This plugin does not create backups and cannot safely restore these packages automatically."));
        }
    }

    TSharedRef<FJsonObject> FWriteFailureReport::ToJson() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetBoolField(TEXT("partial"), bPartial);
        Json->SetBoolField(TEXT("stateUnknown"), bStateUnknown);
        Json->SetStringField(TEXT("failedPhase"), FailedPhase);
        Json->SetStringField(TEXT("message"), Message);
        Json->SetStringField(TEXT("workingCopy"), LexToString(WorkingCopy));
        Json->SetStringField(TEXT("workingCopyRoot"), WorkingCopyRoot);

        TArray<TSharedPtr<FJsonValue>> AssetJson;
        for (const FWriteAssetFailureState& Asset : Assets)
        {
            AssetJson.Add(MakeShared<FJsonValueObject>(Asset.ToJson()));
        }
        Json->SetArrayField(TEXT("assets"), AssetJson);
        Json->SetArrayField(TEXT("readOnlyInspectionCommands"), FailureReportStringsToJson(ReadOnlyInspectionCommands));
        Json->SetArrayField(TEXT("manualRecoveryAdvice"), FailureReportStringsToJson(ManualRecoveryAdvice));
        return Json;
    }
}
