#include "CodexUnrealViewport.h"

#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SEditorViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UnrealClient.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

namespace CodexUnrealBlueprint
{
    namespace
    {
        struct FViewportEntry
        {
            FString Id;
            FEditorViewportClient* Client = nullptr;
            TWeakPtr<SEditorViewport> Widget;
        };

        TArray<FViewportEntry> Entries;

        bool Fail(FProtocolError& Error, EErrorCode Code, const FString& Message, const TCHAR* Callsite)
        {
            Error = FProtocolError::Make(Code, Message, Callsite);
            return false;
        }

        bool Fields(const TSharedRef<FJsonObject>& Object, const TSet<FString>& Allowed, FProtocolError& Error)
        {
            for (const auto& Field : Object->Values)
            {
                if (!Allowed.Contains(Field.Key))
                    return Fail(Error, EErrorCode::UnknownField, FString::Printf(TEXT("Unexpected viewport field '%s'."), *Field.Key), TEXT("FViewportService::Fields"));
            }
            return true;
        }

        bool RequiredString(const TSharedRef<FJsonObject>& Object, const TCHAR* Name, FString& Out, FProtocolError& Error)
        {
            if (!Object->TryGetStringField(Name, Out) || Out.TrimStartAndEnd().IsEmpty())
                return Fail(Error, EErrorCode::InvalidArgument, FString::Printf(TEXT("%s must be a non-empty string."), Name), TEXT("FViewportService::RequiredString"));
            return true;
        }

        bool Number(const TSharedRef<FJsonObject>& Object, const TCHAR* Name, float& Out, FProtocolError& Error)
        {
            double Value = 0;
            if (!Object->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt)
                return Fail(Error, EErrorCode::InvalidArgument, FString::Printf(TEXT("%s must be a finite float."), Name), TEXT("FViewportService::Number"));
            Out = static_cast<float>(Value);
            return true;
        }

        bool Vector(const TSharedRef<FJsonObject>& Object, const TCHAR* Name, FVector& Out, FProtocolError& Error)
        {
            const TSharedPtr<FJsonObject>* Value = nullptr;
            if (!Object->TryGetObjectField(Name, Value) || !Value || !Value->IsValid())
                return Fail(Error, EErrorCode::InvalidArgument, FString::Printf(TEXT("%s must be an x/y/z object."), Name), TEXT("FViewportService::Vector"));
            return Fields(Value->ToSharedRef(), {TEXT("x"), TEXT("y"), TEXT("z")}, Error)
                && Number(Value->ToSharedRef(), TEXT("x"), Out.X, Error)
                && Number(Value->ToSharedRef(), TEXT("y"), Out.Y, Error)
                && Number(Value->ToSharedRef(), TEXT("z"), Out.Z, Error);
        }

        TSharedRef<FJsonObject> VectorJson(const FVector& Value)
        {
            auto Json = MakeShared<FJsonObject>();
            Json->SetNumberField(TEXT("x"), Value.X);
            Json->SetNumberField(TEXT("y"), Value.Y);
            Json->SetNumberField(TEXT("z"), Value.Z);
            return Json;
        }

        TSharedRef<FJsonObject> CameraJson(const FEditorViewportClient& Client)
        {
            auto Json = MakeShared<FJsonObject>();
            Json->SetObjectField(TEXT("location"), VectorJson(Client.GetViewLocation()));
            Json->SetObjectField(TEXT("lookAt"), VectorJson(Client.GetLookAtLocation()));
            auto Rotation = MakeShared<FJsonObject>();
            Rotation->SetNumberField(TEXT("pitch"), Client.GetViewRotation().Pitch);
            Rotation->SetNumberField(TEXT("yaw"), Client.GetViewRotation().Yaw);
            Rotation->SetNumberField(TEXT("roll"), Client.GetViewRotation().Roll);
            Json->SetObjectField(TEXT("rotation"), Rotation);
            Json->SetNumberField(TEXT("orthoZoom"), Client.GetOrthoZoom());
            Json->SetNumberField(TEXT("fieldOfView"), Client.ViewFOV);
            return Json;
        }

        void RefreshEntries()
        {
            check(IsInGameThread());
            if (!GEditor) { Entries.Empty(); return; }
            const auto& Clients = GEditor->GetAllViewportClients();
            // Match the Slate lifetime too: an allocator may reuse a closed client's address.
            Entries.RemoveAll([&Clients](const FViewportEntry& Entry)
            {
                return !Entry.Widget.IsValid() || !Clients.Contains(Entry.Client)
                    || Entry.Client->GetEditorViewportWidget() != Entry.Widget.Pin();
            });
            for (FEditorViewportClient* Client : Clients)
            {
                if (!Client || !Client->GetEditorViewportWidget().IsValid()) continue;
                if (!Entries.ContainsByPredicate([Client](const FViewportEntry& Entry) { return Entry.Client == Client; }))
                {
                    FViewportEntry Entry;
                    Entry.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
                    Entry.Client = Client;
                    Entry.Widget = Client->GetEditorViewportWidget();
                    Entries.Add(MoveTemp(Entry));
                }
            }
        }

        FViewportEntry* Resolve(const FString& Id, FProtocolError& Error)
        {
            RefreshEntries();
            FViewportEntry* Entry = Entries.FindByPredicate([&Id](const FViewportEntry& Item) { return Item.Id == Id; });
            if (!Entry) Fail(Error, EErrorCode::ViewportNotFound,
                FString::Printf(TEXT("Viewport '%s' is closed or belongs to another Editor session; list viewports again."), *Id), TEXT("FViewportService::Resolve"));
            return Entry;
        }

        bool ContainsWidget(const TSharedRef<SWidget>& Parent, const TSharedPtr<SWidget>& Target)
        {
            if (Parent == Target) return true;
            FChildren* Children = Parent->GetChildren();
            for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
                if (ContainsWidget(Children->GetChildAt(Index), Target)) return true;
            return false;
        }

        FBlueprintEditor* BlueprintEditorFor(const FViewportEntry& Entry)
        {
            UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            if (!Subsystem || Entry.Widget.Pin()->GetType() != FName(TEXT("SSCSEditorViewport"))) return nullptr;
            for (UObject* Asset : Subsystem->GetAllEditedAssets())
            {
                IAssetEditorInstance* Editor = Subsystem->FindEditorForAsset(Asset, false);
                if (!Cast<UBlueprint>(Asset) || !Editor || Editor->GetEditorName() != FName(TEXT("BlueprintEditor"))) continue;
                auto* BlueprintEditor = static_cast<FBlueprintEditor*>(Editor);
                // SSCSEditorViewport inherits SEditorViewport directly; its public getter also identifies a closed tab.
                if (static_cast<const void*>(BlueprintEditor->GetSCSViewport().Get()) == static_cast<const void*>(Entry.Widget.Pin().Get()))
                    return BlueprintEditor;
            }
            return nullptr;
        }

        TArray<TSharedPtr<FJsonValue>> AssetPaths(const FViewportEntry& Entry)
        {
            TSet<FString> Paths;
            if (FBlueprintEditor* BlueprintEditor = BlueprintEditorFor(Entry))
                if (UBlueprint* Blueprint = BlueprintEditor->GetBlueprintObj()) Paths.Add(Blueprint->GetPathName());
            if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                for (UObject* Asset : Subsystem->GetAllEditedAssets())
                {
                    IAssetEditorInstance* Editor = Subsystem->FindEditorForAsset(Asset, false);
                    TSharedPtr<FTabManager> Manager = Editor ? Editor->GetAssociatedTabManager() : nullptr;
                    TSharedPtr<SDockTab> Owner = Manager.IsValid() ? Manager->GetOwnerTab() : nullptr;
                    if (Owner.IsValid() && ContainsWidget(Owner.ToSharedRef(), Entry.Widget.Pin())) Paths.Add(Asset->GetPathName());
                }
            }
            // Blueprint component preview worlds identify the exact generating asset even in a detached tab.
            if (!Entry.Client->IsLevelEditorClient() && Entry.Client->GetWorld())
            {
                for (TActorIterator<AActor> It(Entry.Client->GetWorld()); It; ++It)
                    if (UBlueprint* Blueprint = Cast<UBlueprint>(It->GetClass()->ClassGeneratedBy)) Paths.Add(Blueprint->GetPathName());
            }
            TArray<FString> Sorted = Paths.Array(); Sorted.Sort();
            TArray<TSharedPtr<FJsonValue>> Json;
            for (const FString& Path : Sorted) Json.Add(MakeShared<FJsonValueString>(Path));
            return Json;
        }

        bool IsVisible(const FViewportEntry& Entry)
        {
            const FIntPoint Size = Entry.Client->Viewport ? Entry.Client->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
            FWidgetPath Path;
            return Entry.Client->IsVisible() && Size.X > 0 && Size.Y > 0
                && FSlateApplication::Get().GeneratePathToWidgetUnchecked(Entry.Widget.Pin().ToSharedRef(), Path);
        }

        TSharedRef<FJsonObject> EntryJson(const FViewportEntry& Entry)
        {
            auto Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("viewportId"), Entry.Id);
            Json->SetStringField(TEXT("kind"), Entry.Client->IsLevelEditorClient() ? TEXT("level") : TEXT("preview"));
            Json->SetStringField(TEXT("widgetType"), Entry.Widget.Pin()->GetTypeAsString());
            Json->SetBoolField(TEXT("perspective"), Entry.Client->IsPerspective());
            Json->SetArrayField(TEXT("assetPaths"), AssetPaths(Entry));
            if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Entry.Widget.Pin().ToSharedRef()))
                Json->SetStringField(TEXT("windowTitle"), Window->GetTitle().ToString());
            const FIntPoint Size = Entry.Client->Viewport ? Entry.Client->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
            Json->SetBoolField(TEXT("visible"), IsVisible(Entry));
            Json->SetNumberField(TEXT("width"), Size.X);
            Json->SetNumberField(TEXT("height"), Size.Y);
            Json->SetObjectField(TEXT("camera"), CameraJson(*Entry.Client));
            return Json;
        }

        bool EditorReady(FProtocolError& Error)
        {
            check(IsInGameThread());
            if (!GEditor || !FSlateApplication::IsInitialized() || IsRunningCommandlet())
                return Fail(Error, EErrorCode::ViewportUnavailable, TEXT("A graphical Unreal Editor is required; commandlets have no viewport to capture."), TEXT("FViewportService::EditorReady"));
            if (GEditor->PlayWorld)
                return Fail(Error, EErrorCode::ViewportUnavailable, TEXT("Stop PIE or simulation before controlling Editor viewports."), TEXT("FViewportService::EditorReady"));
            return true;
        }

        bool ParseCamera(const TSharedRef<FJsonObject>& Json, FVector& Location, FRotator& Rotation,
            FVector& LookAt, float& Zoom, float& Fov, FProtocolError& Error)
        {
            if (!Fields(Json, {TEXT("location"), TEXT("rotation"), TEXT("lookAt"), TEXT("orthoZoom"), TEXT("fieldOfView")}, Error)) return false;
            if (Json->Values.Num() == 0) return Fail(Error, EErrorCode::InvalidArgument, TEXT("camera must contain at least one field."), TEXT("FViewportService::ParseCamera"));
            if (Json->HasField(TEXT("location")) && !Vector(Json, TEXT("location"), Location, Error)) return false;
            if (Json->HasField(TEXT("lookAt")) && !Vector(Json, TEXT("lookAt"), LookAt, Error)) return false;
            if (Json->HasField(TEXT("orthoZoom")) && !Number(Json, TEXT("orthoZoom"), Zoom, Error)) return false;
            if (Json->HasField(TEXT("fieldOfView")) && !Number(Json, TEXT("fieldOfView"), Fov, Error)) return false;
            if (Json->HasField(TEXT("rotation")))
            {
                const TSharedPtr<FJsonObject>* Value = nullptr;
                if (!Json->TryGetObjectField(TEXT("rotation"), Value) || !Value || !Value->IsValid())
                    return Fail(Error, EErrorCode::InvalidArgument, TEXT("rotation must be a pitch/yaw/roll object."), TEXT("FViewportService::ParseCamera"));
                if (!Fields(Value->ToSharedRef(), {TEXT("pitch"), TEXT("yaw"), TEXT("roll")}, Error)
                    || !Number(Value->ToSharedRef(), TEXT("pitch"), Rotation.Pitch, Error)
                    || !Number(Value->ToSharedRef(), TEXT("yaw"), Rotation.Yaw, Error)
                    || !Number(Value->ToSharedRef(), TEXT("roll"), Rotation.Roll, Error)) return false;
            }
            if (Zoom <= 0 || Fov <= 0 || Fov >= 180)
                return Fail(Error, EErrorCode::InvalidArgument, TEXT("orthoZoom must be positive and fieldOfView must be between 0 and 180 degrees."), TEXT("FViewportService::ParseCamera"));
            return true;
        }
    }

    TSharedRef<FJsonObject> FViewportService::List()
    {
        RefreshEntries();
        auto Result = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Viewports;
        for (const FViewportEntry& Entry : Entries) Viewports.Add(MakeShared<FJsonValueObject>(EntryJson(Entry)));
        Result->SetArrayField(TEXT("viewports"), Viewports);
        Result->SetStringField(TEXT("scope"), TEXT("Native 3D Editor viewports; excludes UMG Designer, graph editors, PIE and operating-system windows."));
        return Result;
    }

    bool FViewportService::Capture(const TSharedRef<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
    {
        if (!Fields(Params, {TEXT("viewportId"), TEXT("outputPath")}, Error) || !EditorReady(Error)) return false;
        FString Id, Path;
        if (!RequiredString(Params, TEXT("viewportId"), Id, Error) || !RequiredString(Params, TEXT("outputPath"), Path, Error)) return false;
        if (FPaths::IsRelative(Path) || !FPaths::GetExtension(Path).Equals(TEXT("png"), ESearchCase::IgnoreCase))
            return Fail(Error, EErrorCode::InvalidArgument, TEXT("outputPath must be an absolute .png filename outside Unreal Content directories."), TEXT("FViewportService::Capture"));
        Path = FPaths::ConvertRelativePathToFull(Path); FPaths::NormalizeFilename(Path); FPaths::CollapseRelativeDirectories(Path);
        if (Path.Contains(TEXT("/Content/"), ESearchCase::IgnoreCase) || IFileManager::Get().FileExists(*Path))
            return Fail(Error, EErrorCode::InvalidArgument, TEXT("Screenshots cannot overwrite existing files or be written under a Content directory."), TEXT("FViewportService::Capture"));
        FViewportEntry* Entry = Resolve(Id, Error); if (!Entry) return false;
        FViewport* Viewport = Entry->Client->Viewport;
        if (!IsVisible(*Entry))
            return Fail(Error, EErrorCode::ViewportUnavailable, TEXT("The viewport is hidden or has no render area; activate its tab before capturing."), TEXT("FViewportService::Capture"));
        // Draw after camera changes instead of returning an old render target.
        Entry->Client->Invalidate(); Viewport->Draw(false);
        TArray<FColor> Pixels;
        if (!GetViewportScreenShot(Viewport, Pixels))
            return Fail(Error, EErrorCode::ViewportCaptureFailed, TEXT("GetViewportScreenShot could not read the Editor render target."), TEXT("GetViewportScreenShot"));
        const FIntPoint Size = Viewport->GetSizeXY();
        if (Pixels.Num() != static_cast<int64>(Size.X) * Size.Y)
            return Fail(Error, EErrorCode::ViewportCaptureFailed, TEXT("Readback pixel count does not match the viewport dimensions."), TEXT("FViewportService::Capture"));
        for (FColor& Pixel : Pixels) Pixel.A = 255;
        TArray<uint8> Png; FImageUtils::CompressImageArray(Size.X, Size.Y, Pixels, Png);
        if (Png.Num() == 0 || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
            || !FFileHelper::SaveArrayToFile(Png, *Path, &IFileManager::Get(), FILEWRITE_NoReplaceExisting))
            return Fail(Error, EErrorCode::ViewportCaptureFailed, FString::Printf(TEXT("PNG encoding or writing failed: %s"), *Path), TEXT("FImageUtils::CompressImageArray / FFileHelper::SaveArrayToFile"));
        Result = EntryJson(*Entry);
        Result->SetStringField(TEXT("filePath"), Path);
        Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
        Result->SetStringField(TEXT("evidence"), TEXT("editor-viewport-pixels"));
        Result->SetNumberField(TEXT("byteLength"), Png.Num());
        return true;
    }

    bool FViewportService::Control(const TSharedRef<FJsonObject>& Params, TSharedPtr<FJsonObject>& Result, FProtocolError& Error)
    {
        if (!EditorReady(Error)) return false;
        FString Action;
        if (!RequiredString(Params, TEXT("action"), Action, Error)) return false;
        if (Action == TEXT("open"))
        {
            FString AssetPath;
            if (!Fields(Params, {TEXT("requestId"), TEXT("action"), TEXT("assetPath")}, Error)
                || !RequiredString(Params, TEXT("assetPath"), AssetPath, Error)) return false;
            UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
            if (!Asset)
            {
                Fail(Error, EErrorCode::AssetNotFound, TEXT("The requested asset does not exist."), TEXT("FViewportService::Control"));
                Error.AssetPath = AssetPath; return false;
            }
            UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
            if (!Subsystem || !Subsystem->OpenEditorForAsset(Asset, EToolkitMode::Standalone, nullptr, false))
                return Fail(Error, EErrorCode::ViewportUnavailable, TEXT("The native asset editor could not be opened."), TEXT("UAssetEditorSubsystem::OpenEditorForAsset"));
            Result = List(); Result->SetStringField(TEXT("openedAssetPath"), Asset->GetPathName());
            return true;
        }
        TSet<FString> Allowed = {TEXT("requestId"), TEXT("action"), TEXT("viewportId")};
        if (Action == TEXT("set_camera")) Allowed.Add(TEXT("camera"));
        else if (Action == TEXT("pan")) Allowed.Add(TEXT("delta"));
        else if (Action == TEXT("orbit")) { Allowed.Add(TEXT("yaw")); Allowed.Add(TEXT("pitch")); }
        else if (Action == TEXT("zoom")) Allowed.Add(TEXT("factor"));
        else if (Action == TEXT("frame")) { Allowed.Add(TEXT("boundsMin")); Allowed.Add(TEXT("boundsMax")); }
        else if (Action != TEXT("activate")) return Fail(Error, EErrorCode::InvalidArgument, TEXT("Unknown viewport action."), TEXT("FViewportService::Control"));
        if (!Fields(Params, Allowed, Error)) return false;
        FString Id; if (!RequiredString(Params, TEXT("viewportId"), Id, Error)) return false;
        FViewportEntry* Entry = Resolve(Id, Error); if (!Entry) return false;
        FEditorViewportClient& Client = *Entry->Client;
        const auto Before = CameraJson(Client);
        const FViewportCameraTransform PreviousTransform = Client.GetViewTransform();
        FVector Location = Client.GetViewLocation(), LookAt = Client.GetLookAtLocation();
        FRotator Rotation = Client.GetViewRotation();
        float Zoom = Client.GetOrthoZoom(), Fov = Client.ViewFOV;
        if (Action == TEXT("activate"))
        {
            // A Blueprint creates its preview client even when its Viewport tab is closed.
            if (FBlueprintEditor* BlueprintEditor = BlueprintEditorFor(*Entry))
            {
                const TSharedPtr<FTabManager> Manager = BlueprintEditor->GetAssociatedTabManager();
                if (!Manager.IsValid() || !Manager->HasTabSpawner(FBlueprintEditorTabs::SCSViewportID)
                    || !Manager->TryInvokeTab(FBlueprintEditorTabs::SCSViewportID).IsValid())
                    return Fail(Error, EErrorCode::ViewportUnavailable, TEXT("This Blueprint editor mode has no component preview tab."), TEXT("FTabManager::TryInvokeTab"));
            }
            FWidgetPath Path;
            if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(Entry->Widget.Pin().ToSharedRef(), Path, EVisibility::All))
                return Fail(Error, EErrorCode::ViewportUnavailable, TEXT("The viewport no longer belongs to a live Slate tab."), TEXT("FSlateApplication::GeneratePathToWidgetUnchecked"));
            for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
            {
                const FArrangedWidget& Widget = Path.Widgets[Index];
                if (Widget.Widget->GetType() == FName(TEXT("SDockTab"))) StaticCastSharedRef<SDockTab>(Widget.Widget)->ActivateInParent(ETabActivationCause::SetDirectly);
            }
            if (TSharedPtr<SWindow> Window = Path.GetWindow()) Window->BringToFront();
            Client.Invalidate();
            Result = EntryJson(*Entry); Result->SetObjectField(TEXT("previousCamera"), Before);
            Result->SetStringField(TEXT("action"), Action);
            return true;
        }
        else if (Action == TEXT("set_camera"))
        {
            const TSharedPtr<FJsonObject>* Camera = nullptr;
            if (!Params->TryGetObjectField(TEXT("camera"), Camera) || !Camera || !Camera->IsValid())
                return Fail(Error, EErrorCode::InvalidArgument, TEXT("camera must be an object."), TEXT("FViewportService::Control"));
            if (!ParseCamera(Camera->ToSharedRef(), Location, Rotation, LookAt, Zoom, Fov, Error)) return false;
        }
        else if (Action == TEXT("pan"))
        {
            FVector Delta; if (!Vector(Params, TEXT("delta"), Delta, Error)) return false;
            const FRotationMatrix Matrix(Rotation);
            const FVector WorldDelta = Matrix.GetScaledAxis(EAxis::Y) * Delta.X + Matrix.GetScaledAxis(EAxis::Z) * Delta.Y + Matrix.GetScaledAxis(EAxis::X) * Delta.Z;
            Location += WorldDelta; LookAt += WorldDelta;
        }
        else if (Action == TEXT("orbit"))
        {
            if (!Client.IsPerspective()) return Fail(Error, EErrorCode::InvalidArgument, TEXT("orbit requires a perspective viewport."), TEXT("FViewportService::Control"));
            float Yaw, Pitch;
            if (!Number(Params, TEXT("yaw"), Yaw, Error) || !Number(Params, TEXT("pitch"), Pitch, Error)) return false;
            const float Distance = FVector::Distance(Location, LookAt);
            if (Distance <= SMALL_NUMBER) return Fail(Error, EErrorCode::InvalidArgument, TEXT("Set a distinct lookAt point before orbiting."), TEXT("FViewportService::Control"));
            Rotation.Yaw += Yaw; Rotation.Pitch += Pitch; Rotation.Normalize();
            Location = LookAt - Rotation.Vector() * Distance;
        }
        else if (Action == TEXT("zoom"))
        {
            float Factor; if (!Number(Params, TEXT("factor"), Factor, Error)) return false;
            if (Factor <= 0) return Fail(Error, EErrorCode::InvalidArgument, TEXT("zoom factor must be positive; below 1 moves closer, above 1 moves farther."), TEXT("FViewportService::Control"));
            if (Client.IsPerspective())
            {
                if (Location.Equals(LookAt)) return Fail(Error, EErrorCode::InvalidArgument, TEXT("Set a distinct lookAt point before zooming."), TEXT("FViewportService::Control"));
                Location = LookAt + (Location - LookAt) * Factor;
            }
            else Zoom *= Factor;
        }
        else if (Action == TEXT("frame"))
        {
            FVector Min, Max;
            if (!Vector(Params, TEXT("boundsMin"), Min, Error) || !Vector(Params, TEXT("boundsMax"), Max, Error)) return false;
            if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z || Min.Equals(Max))
                return Fail(Error, EErrorCode::InvalidArgument, TEXT("frame requires ordered, non-empty bounds."), TEXT("FViewportService::Control"));
            Client.FocusViewportOnBox(FBox(Min, Max), true);
            Location = Client.GetViewLocation(); Rotation = Client.GetViewRotation(); LookAt = Client.GetLookAtLocation(); Zoom = Client.GetOrthoZoom();
        }
        if (Location.ContainsNaN() || LookAt.ContainsNaN() || Rotation.ContainsNaN() || !FMath::IsFinite(Zoom) || Zoom <= 0)
        {
            // Native framing can change the transform before its computed result is validated.
            Client.GetViewTransform() = PreviousTransform;
            return Fail(Error, EErrorCode::InvalidArgument, TEXT("Camera arithmetic overflowed; use smaller deltas or a positive representable zoom."), TEXT("FViewportService::Control"));
        }
        Client.SetViewLocation(Location); Client.SetViewRotation(Rotation); Client.SetLookAtLocation(LookAt);
        Client.SetOrthoZoom(Zoom); Client.ViewFOV = Fov; Client.Invalidate();
        Result = EntryJson(*Entry); Result->SetObjectField(TEXT("previousCamera"), Before);
        Result->SetStringField(TEXT("action"), Action);
        return true;
    }
}
