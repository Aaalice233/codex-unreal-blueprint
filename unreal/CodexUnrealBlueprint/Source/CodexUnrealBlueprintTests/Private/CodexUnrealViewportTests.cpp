#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CodexUnrealViewport.h"
#include "CodexUnrealBlueprintService.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"
#include "UObject/UObjectIterator.h"

using namespace CodexUnrealBlueprint;

namespace
{
    class SCodexCameraTestViewport : public SEditorViewport
    {
    public:
        SLATE_BEGIN_ARGS(SCodexCameraTestViewport) {} SLATE_END_ARGS()
        void Construct(const FArguments&) { SEditorViewport::Construct(SEditorViewport::FArguments()); }
        TSharedPtr<FEditorViewportClient> Client;
    protected:
        virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
        {
            Client = MakeShared<FEditorViewportClient>(nullptr, &Scene, SharedThis(this));
            Client->SetViewportType(LVT_Perspective);
            Client->SetViewLocation(FVector(100, 0, 0));
            Client->SetViewRotation(FRotator(0, 180, 0));
            Client->SetLookAtLocation(FVector::ZeroVector);
            return Client.ToSharedRef();
        }
    private:
        FPreviewScene Scene{FPreviewScene::ConstructionValues()};
    };

    TSharedRef<FJsonObject> Vec(float X, float Y, float Z)
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("x"), X); Json->SetNumberField(TEXT("y"), Y); Json->SetNumberField(TEXT("z"), Z); return Json;
    }

    TSharedRef<FJsonObject> Params(const FString& Id, const FString& Action)
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("viewportId"), Id); Json->SetStringField(TEXT("action"), Action);
        Json->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString()); return Json;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexViewportControlTest, "CodexUnrealBlueprint.Editor.Viewport.CameraAndBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexViewportControlTest::RunTest(const FString& Parameters)
{
    if (!TestTrue(TEXT("interactive Slate is available"), GEditor && FSlateApplication::IsInitialized() && !GEditor->PlayWorld)) return false;
    TMap<TWeakObjectPtr<UPackage>, bool> DirtyBefore;
    for (TObjectIterator<UPackage> It; It; ++It) DirtyBefore.Add(*It, It->IsDirty());
    TSharedPtr<SCodexCameraTestViewport> Widget = SNew(SCodexCameraTestViewport);
    FString Id;
    const auto ViewportList = FViewportService::List();
    for (const auto& Value : ViewportList->GetArrayField(TEXT("viewports")))
        if (Value->AsObject()->GetStringField(TEXT("widgetType")) == TEXT("SCodexCameraTestViewport"))
        {
            Id = Value->AsObject()->GetStringField(TEXT("viewportId"));
            TestFalse(TEXT("unattached viewport is not reported as visible"), Value->AsObject()->GetBoolField(TEXT("visible")));
        }
    if (!TestFalse(TEXT("test viewport has an opaque id"), Id.IsEmpty())) return false;
    FProtocolError Error;
    TSharedPtr<FJsonObject> Result;
    auto Zoom = Params(Id, TEXT("zoom")); Zoom->SetNumberField(TEXT("factor"), 0.5);
    if (!TestTrue(TEXT("perspective zoom succeeds"), FViewportService::Control(Zoom, Result, Error))) return false;
    TestTrue(TEXT("zoom halves distance to lookAt"), Widget->Client->GetViewLocation().Equals(FVector(50, 0, 0)));
    const auto OriginalCamera = Result->GetObjectField(TEXT("previousCamera"));
    auto Orbit = Params(Id, TEXT("orbit")); Orbit->SetNumberField(TEXT("yaw"), 90); Orbit->SetNumberField(TEXT("pitch"), 0);
    TestTrue(TEXT("orbit succeeds"), FViewportService::Control(Orbit, Result, Error));
    AddInfo(FString::Printf(TEXT("Orbit radius: %.9g (expected 50)."), Widget->Client->GetViewLocation().Size()));
    TestEqual(TEXT("orbit preserves radius"), Widget->Client->GetViewLocation().Size(), 50.f);
    auto Pan = Params(Id, TEXT("pan")); Pan->SetObjectField(TEXT("delta"), Vec(10, 20, 0));
    const FVector OffsetBefore = Widget->Client->GetViewLocation() - Widget->Client->GetLookAtLocation();
    TestTrue(TEXT("pan succeeds"), FViewportService::Control(Pan, Result, Error));
    TestTrue(TEXT("pan moves camera and pivot together"), (Widget->Client->GetViewLocation() - Widget->Client->GetLookAtLocation()).Equals(OffsetBefore));
    auto Restore = Params(Id, TEXT("set_camera")); Restore->SetObjectField(TEXT("camera"), OriginalCamera);
    TestTrue(TEXT("previousCamera restores the pose"), FViewportService::Control(Restore, Result, Error));
    TestTrue(TEXT("original position restored"), Widget->Client->GetViewLocation().Equals(FVector(100, 0, 0)));

    auto BadCamera = MakeShared<FJsonObject>(); BadCamera->SetObjectField(TEXT("location"), Vec(999, 999, 999)); BadCamera->SetNumberField(TEXT("fieldOfView"), 180);
    Restore->SetObjectField(TEXT("camera"), BadCamera);
    TestFalse(TEXT("invalid camera rejected before movement"), FViewportService::Control(Restore, Result, Error));
    TestTrue(TEXT("failed request leaves camera unchanged"), Widget->Client->GetViewLocation().Equals(FVector(100, 0, 0)));
    Zoom->SetNumberField(TEXT("factor"), 0);
    TestFalse(TEXT("zero zoom rejected"), FViewportService::Control(Zoom, Result, Error));
    auto Frame = Params(Id, TEXT("frame")); Frame->SetObjectField(TEXT("boundsMin"), Vec(-50, -50, -50)); Frame->SetObjectField(TEXT("boundsMax"), Vec(50, 50, 50));
    TestTrue(TEXT("frame succeeds"), FViewportService::Control(Frame, Result, Error));
    TestTrue(TEXT("frame has finite nonzero camera distance"), !Widget->Client->GetViewLocation().ContainsNaN() && Widget->Client->GetViewLocation().Size() > 0);
    Widget->Client->SetViewportType(LVT_OrthoXY);
    const float OldOrthoZoom = Widget->Client->GetOrthoZoom(); Zoom->SetNumberField(TEXT("factor"), 2);
    TestTrue(TEXT("orthographic zoom succeeds"), FViewportService::Control(Zoom, Result, Error));
    TestEqual(TEXT("orthographic zoom uses extent"), Widget->Client->GetOrthoZoom(), OldOrthoZoom * 2);
    TestFalse(TEXT("orbit rejects orthographic view"), FViewportService::Control(Orbit, Result, Error));

    auto Capture = MakeShared<FJsonObject>(); Capture->SetStringField(TEXT("viewportId"), Id);
    Capture->SetStringField(TEXT("outputPath"), TEXT("C:/Project/Content/Unsafe.png"));
    TestFalse(TEXT("Content output rejected"), FViewportService::Capture(Capture, Result, Error));
    TestEqual(TEXT("invalid output code"), Error.Code, EErrorCode::InvalidArgument);
    Capture->SetStringField(TEXT("outputPath"), TEXT("relative.png"));
    TestFalse(TEXT("relative output rejected"), FViewportService::Capture(Capture, Result, Error));
    Capture->SetStringField(TEXT("outputPath"), TEXT("C:/CodexViewportTest/hidden.png"));
    TestFalse(TEXT("viewport without a render area cannot produce an image"), FViewportService::Capture(Capture, Result, Error));
    TestEqual(TEXT("no fabricated capture success"), Error.Code, EErrorCode::ViewportUnavailable);
    Widget->Client.Reset(); Widget.Reset();
    TestFalse(TEXT("closed viewport id rejected"), FViewportService::Control(Zoom, Result, Error));
    TestEqual(TEXT("stale id error"), Error.Code, EErrorCode::ViewportNotFound);
    for (const auto& Before : DirtyBefore)
        if (UPackage* Package = Before.Key.Get()) TestEqual(FString::Printf(TEXT("dirty state unchanged: %s"), *Package->GetName()), Package->IsDirty(), Before.Value);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodexViewportRequestIdTest, "CodexUnrealBlueprint.Editor.Viewport.RequestIdRequired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCodexViewportRequestIdTest::RunTest(const FString& Parameters)
{
    FProtocolRequest Request; Request.Id = TEXT("viewport-missing-id"); Request.Method = TEXT("unreal.viewport.control");
    Request.Params = MakeShared<FJsonObject>(); Request.Params->SetStringField(TEXT("action"), TEXT("zoom"));
    const FProtocolResponse Response = FCoreService::Get().Dispatch(Request);
    TestTrue(TEXT("missing requestId is an error"), Response.Error.IsSet());
    if (Response.Error.IsSet()) TestEqual(TEXT("stable requestId code"), Response.Error.GetValue().Code, EErrorCode::RequestIdRequired);
    return true;
}

#endif
