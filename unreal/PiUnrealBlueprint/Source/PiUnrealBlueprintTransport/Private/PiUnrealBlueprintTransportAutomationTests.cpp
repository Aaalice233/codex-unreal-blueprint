#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PiUnrealBlueprintFraming.h"
#include "PiUnrealBlueprintTransportServer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace PiUnrealBlueprint
{
    namespace
    {
        bool SendFrame(FSocket& Socket, const FString& Json, FString& OutError)
        {
            TArray<uint8> Frame;
            if (!FLengthPrefixedJsonFraming::Encode(Json, Frame, OutError))
            {
                return false;
            }
            int32 Offset = 0;
            while (Offset < Frame.Num())
            {
                int32 Sent = 0;
                if (!Socket.Send(Frame.GetData() + Offset, Frame.Num() - Offset, Sent) || Sent <= 0)
                {
                    OutError = TEXT("The automation client could not send a complete frame.");
                    return false;
                }
                Offset += Sent;
            }
            return true;
        }

        struct FTransportAutomationFixture
        {
            TUniquePtr<FTransportServer> Server;
            FSocket* ClientSocket = nullptr;
            TArray<uint8> ReceiveBuffer;
            double Deadline = 0.0;
            int32 Phase = 0;

            ~FTransportAutomationFixture()
            {
                if (ClientSocket != nullptr)
                {
                    ClientSocket->Close();
                    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
                }
                if (Server.IsValid() && Server->GetState() != EServiceState::Stopped)
                {
                    Server->Stop();
                }
            }
        };

        bool ConnectClient(FTransportAutomationFixture& Fixture, FString& OutError)
        {
            ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            Fixture.ClientSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("PiUnrealBlueprint automation client"), NAME_None);
            TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
            bool bValid = false;
            Address->SetIp(TEXT("127.0.0.1"), bValid);
            Address->SetPort(Fixture.Server->GetPort());
            if (!bValid || Fixture.ClientSocket == nullptr || !Fixture.ClientSocket->Connect(*Address))
            {
                OutError = TEXT("A real TCP automation client could not connect.");
                return false;
            }
            Fixture.ClientSocket->SetNonBlocking(true);
            return true;
        }

        bool TryReceiveJson(FTransportAutomationFixture& Fixture, FString& OutJson, FString& OutError)
        {
            uint32 Pending = 0;
            while (Fixture.ClientSocket->HasPendingData(Pending) && Pending > 0)
            {
                const int32 Offset = Fixture.ReceiveBuffer.AddUninitialized(static_cast<int32>(Pending));
                int32 Read = 0;
                if (!Fixture.ClientSocket->Recv(Fixture.ReceiveBuffer.GetData() + Offset, static_cast<int32>(Pending), Read) || Read <= 0)
                {
                    Fixture.ReceiveBuffer.SetNum(Offset, false);
                    OutError = TEXT("The automation client could not receive the response frame.");
                    return false;
                }
                Fixture.ReceiveBuffer.SetNum(Offset + Read, false);
            }
            const EFrameDecodeResult Result = FLengthPrefixedJsonFraming::TryDecode(Fixture.ReceiveBuffer, OutJson, OutError);
            return Result == EFrameDecodeResult::Complete;
        }
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiTransportDescriptorLifecycleTest,
        "PiUnrealBlueprint.Transport.SessionDescriptorLifecycle",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    bool FPiTransportDescriptorLifecycleTest::RunTest(const FString& Parameters)
    {
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PiUnrealBlueprint"), TEXT("AutomationSessions"), FGuid::NewGuid().ToString());
        FTransportServer Server;
        FTransportServerConfig Config;
        Config.SessionDirectoryOverride = Directory;
        Config.UprojectOverride = FPaths::GetProjectFilePath();
        FProtocolError Error;
        if (!TestTrue(TEXT("A real loopback server starts"), Server.Start(Config, Error)))
        {
            AddError(Error.Message);
            return false;
        }

        const FString DescriptorPath = Server.GetSessionDescriptorPath();
        TestTrue(TEXT("A session descriptor is published"), IFileManager::Get().FileExists(*DescriptorPath));
        FString DescriptorJson;
        TestTrue(TEXT("The descriptor is readable by the current user"), FFileHelper::LoadFileToString(DescriptorJson, *DescriptorPath));
        TSharedPtr<FJsonObject> Descriptor;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DescriptorJson);
        TestTrue(TEXT("The descriptor is valid JSON"), FJsonSerializer::Deserialize(Reader, Descriptor) && Descriptor.IsValid());
        if (Descriptor.IsValid())
        {
            TestEqual(TEXT("The listener is loopback-only"), Descriptor->GetStringField(TEXT("host")), FString(TEXT("127.0.0.1")));
            TestEqual(TEXT("The descriptor reports the bound random port"), static_cast<int32>(Descriptor->GetNumberField(TEXT("port"))), Server.GetPort());
            TestEqual(TEXT("The descriptor reports the editor session"), Descriptor->GetStringField(TEXT("editorSessionId")), Server.GetEditorSessionId());
            TestEqual(TEXT("The descriptor reports the protocol"), Descriptor->GetStringField(TEXT("protocolVersion")), FString(ProtocolVersion));
            TestTrue(TEXT("The descriptor includes a non-trivial authentication token"), Descriptor->GetStringField(TEXT("authToken")).Len() >= 64);
            TestTrue(TEXT("The descriptor includes capabilities"), Descriptor->HasTypedField<EJson::Object>(TEXT("capabilities")));
        }

        Server.Stop();
        TestFalse(TEXT("Clean shutdown removes the descriptor"), IFileManager::Get().FileExists(*DescriptorPath));
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiTransportRealRpcTest,
        "PiUnrealBlueprint.Transport.RealTcpAuthenticationAndRpc",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FPollPiTransportRpc, TSharedPtr<FTransportAutomationFixture>, Fixture, FAutomationTestBase*, Test);

    bool FPollPiTransportRpc::Update()
    {
        FString Json;
        FString Error;
        if (!TryReceiveJson(*Fixture, Json, Error))
        {
            if (!Error.IsEmpty())
            {
                Test->AddError(Error);
                return true;
            }
            if (FPlatformTime::Seconds() > Fixture->Deadline)
            {
                Test->AddError(TEXT("Timed out waiting for a real TCP JSON-RPC response."));
                return true;
            }
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            Test->AddError(TEXT("The real TCP response was not valid JSON."));
            return true;
        }
        if (Fixture->Phase == 0)
        {
            const TSharedPtr<FJsonObject>* RpcError = nullptr;
            Test->TestTrue(TEXT("An invalid token is explicitly rejected"), Root->TryGetObjectField(TEXT("error"), RpcError) && RpcError != nullptr);
            if (RpcError != nullptr)
            {
                const TSharedPtr<FJsonObject>* ErrorData = nullptr;
                Test->TestTrue(TEXT("Authentication rejection includes structured data"), (*RpcError)->TryGetObjectField(TEXT("data"), ErrorData) && ErrorData != nullptr);
                if (ErrorData != nullptr)
                {
                    Test->TestEqual(TEXT("Authentication rejection has a stable code"), (*ErrorData)->GetStringField(TEXT("code")), FString(TEXT("AuthenticationFailed")));
                }
            }
            Fixture->ClientSocket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Fixture->ClientSocket);
            Fixture->ClientSocket = nullptr;
            Fixture->ReceiveBuffer.Reset();
            if (!ConnectClient(*Fixture, Error))
            {
                Test->AddError(Error);
                return true;
            }
            const FString Authenticate = FString::Printf(
                TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"session.authenticate\",\"params\":{\"editorSessionId\":\"%s\",\"authToken\":\"%s\",\"protocolVersion\":\"%s\",\"client\":\"automation-test\"}}"),
                *Fixture->Server->GetEditorSessionId(), *Fixture->Server->GetAuthToken(), ProtocolVersion);
            if (!SendFrame(*Fixture->ClientSocket, Authenticate, Error))
            {
                Test->AddError(Error);
                return true;
            }
            Fixture->Phase = 1;
            Fixture->Deadline = FPlatformTime::Seconds() + 5.0;
            return false;
        }
        if (Root->HasField(TEXT("error")))
        {
            Test->AddError(FString::Printf(TEXT("The real TCP request failed: %s"), *Json));
            return true;
        }

        if (Fixture->Phase == 1)
        {
            Test->TestEqual(TEXT("Authentication preserves a numeric JSON-RPC id"), Root->GetNumberField(TEXT("id")), 1.0);
            const TSharedPtr<FJsonObject>* Result = nullptr;
            Test->TestTrue(TEXT("Authentication returns a result"), Root->TryGetObjectField(TEXT("result"), Result) && Result != nullptr);
            if (Result != nullptr)
            {
                Test->TestTrue(TEXT("The token authenticates the connection"), (*Result)->GetBoolField(TEXT("authenticated")));
            }
            Fixture->Phase = 2;
            Fixture->Deadline = FPlatformTime::Seconds() + 5.0;
            if (!SendFrame(*Fixture->ClientSocket, TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"unreal_status\",\"params\":{}}"), Error))
            {
                Test->AddError(Error);
                return true;
            }
            return false;
        }

        Test->TestEqual(TEXT("Dispatched RPC preserves a numeric JSON-RPC id"), Root->GetNumberField(TEXT("id")), 2.0);
        const TSharedPtr<FJsonObject>* Result = nullptr;
        Test->TestTrue(TEXT("Game-thread Core dispatch returns a status result"), Root->TryGetObjectField(TEXT("result"), Result) && Result != nullptr);
        if (Result != nullptr)
        {
            Test->TestTrue(TEXT("The real Core service answered"), (*Result)->GetBoolField(TEXT("coreAvailable")));
            Test->TestTrue(TEXT("Status reports the real Transport"), (*Result)->GetBoolField(TEXT("transportAvailable")));
            Test->TestEqual(TEXT("Status reports an authenticated connection"), (*Result)->GetStringField(TEXT("serviceState")), FString(TEXT("Connected")));
        }
        Fixture->ClientSocket->Close();
        Fixture->Server->Stop();
        return true;
    }

    bool FPiTransportRealRpcTest::RunTest(const FString& Parameters)
    {
        TSharedPtr<FTransportAutomationFixture> Fixture = MakeShared<FTransportAutomationFixture>();
        Fixture->Server = MakeUnique<FTransportServer>();
        FTransportServerConfig Config;
        Config.bWriteSessionDescriptor = false;
        FProtocolError StartError;
        if (!TestTrue(TEXT("A real test server starts"), Fixture->Server->Start(Config, StartError)))
        {
            AddError(StartError.Message);
            return false;
        }

        FString SendError;
        if (!TestTrue(TEXT("A real TCP client connects"), ConnectClient(*Fixture, SendError)))
        {
            AddError(SendError);
            return false;
        }

        const FString InvalidAuthenticate = FString::Printf(
            TEXT("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"session.authenticate\",\"params\":{\"editorSessionId\":\"%s\",\"authToken\":\"wrong-token\",\"protocolVersion\":\"%s\",\"client\":\"automation-test\"}}"),
            *Fixture->Server->GetEditorSessionId(), ProtocolVersion);
        if (!TestTrue(TEXT("The invalid authentication frame is sent"), SendFrame(*Fixture->ClientSocket, InvalidAuthenticate, SendError)))
        {
            AddError(SendError);
            return false;
        }
        Fixture->Deadline = FPlatformTime::Seconds() + 5.0;
        ADD_LATENT_AUTOMATION_COMMAND(FPollPiTransportRpc(Fixture, this));
        return true;
    }

    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPiTransportRejectsInvalidUtf8Test,
        "PiUnrealBlueprint.Transport.RejectsInvalidUtf8",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    bool FPiTransportRejectsInvalidUtf8Test::RunTest(const FString& Parameters)
    {
        TArray<uint8> Buffer = { 0, 0, 0, 2, 0xc0, 0xaf };
        FString Json;
        FString Error;
        TestTrue(TEXT("Malformed UTF-8 is rejected"), FLengthPrefixedJsonFraming::TryDecode(Buffer, Json, Error) == EFrameDecodeResult::Invalid);
        TestTrue(TEXT("The framing error is explicit"), Error.Contains(TEXT("UTF-8")));
        return true;
    }
}

#endif
