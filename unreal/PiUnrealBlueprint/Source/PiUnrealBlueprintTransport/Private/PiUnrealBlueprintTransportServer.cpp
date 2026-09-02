#include "PiUnrealBlueprintTransportServer.h"

#include "Async/Async.h"
#include "Common/TcpListener.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PiUnrealBlueprintFraming.h"
#include "PiUnrealBlueprintService.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Aclapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogPiUnrealBlueprintServer, Log, All);

namespace PiUnrealBlueprint
{
    namespace
    {
        constexpr int32 ReceiveChunkSize = 64 * 1024;
        constexpr int32 MaxBufferedBytes = static_cast<int32>(FLengthPrefixedJsonFraming::HeaderSize + FLengthPrefixedJsonFraming::MaxPayloadSize);

        bool ConstantTimeEquals(const FString& Left, const FString& Right)
        {
            const int32 MaxLength = FMath::Max(Left.Len(), Right.Len());
            uint32 Difference = static_cast<uint32>(Left.Len() ^ Right.Len());
            for (int32 Index = 0; Index < MaxLength; ++Index)
            {
                const TCHAR LeftChar = Index < Left.Len() ? Left[Index] : 0;
                const TCHAR RightChar = Index < Right.Len() ? Right[Index] : 0;
                Difference |= static_cast<uint32>(LeftChar ^ RightChar);
            }
            return Difference == 0;
        }

        FString ProtocolMajor(const FString& Version)
        {
            FString Major;
            FString Remainder;
            return Version.Split(TEXT("."), &Major, &Remainder) ? Major : Version;
        }

        FString GenerateSecret()
        {
            FString Secret;
            for (int32 Index = 0; Index < 4; ++Index)
            {
                Secret += FGuid::NewGuid().ToString(EGuidFormats::Digits);
            }
            return Secret;
        }

        bool ApplyCurrentUserOnlyAcl(const FString& Path, const bool bDirectory, FString& OutError)
        {
#if PLATFORM_WINDOWS
            HANDLE Token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
            {
                OutError = FString::Printf(TEXT("OpenProcessToken failed with Win32 error %u."), GetLastError());
                return false;
            }

            DWORD TokenInfoSize = 0;
            GetTokenInformation(Token, TokenUser, nullptr, 0, &TokenInfoSize);
            TArray<uint8> TokenInfo;
            TokenInfo.SetNumUninitialized(static_cast<int32>(TokenInfoSize));
            if (TokenInfoSize == 0 || !GetTokenInformation(Token, TokenUser, TokenInfo.GetData(), TokenInfoSize, &TokenInfoSize))
            {
                const DWORD ErrorCode = GetLastError();
                CloseHandle(Token);
                OutError = FString::Printf(TEXT("GetTokenInformation failed with Win32 error %u."), ErrorCode);
                return false;
            }

            TOKEN_USER* User = reinterpret_cast<TOKEN_USER*>(TokenInfo.GetData());
            EXPLICIT_ACCESSW Access;
            FMemory::Memzero(Access);
            Access.grfAccessPermissions = GENERIC_ALL;
            Access.grfAccessMode = SET_ACCESS;
            Access.grfInheritance = bDirectory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
            Access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            Access.Trustee.TrusteeType = TRUSTEE_IS_USER;
            Access.Trustee.ptstrName = static_cast<LPWSTR>(User->User.Sid);

            PACL Acl = nullptr;
            const DWORD AclResult = SetEntriesInAclW(1, &Access, nullptr, &Acl);
            if (AclResult != ERROR_SUCCESS)
            {
                CloseHandle(Token);
                OutError = FString::Printf(TEXT("SetEntriesInAcl failed with Win32 error %u."), AclResult);
                return false;
            }

            const DWORD SecurityResult = SetNamedSecurityInfoW(
                const_cast<LPWSTR>(*Path),
                SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr,
                nullptr,
                Acl,
                nullptr);
            LocalFree(Acl);
            CloseHandle(Token);
            if (SecurityResult != ERROR_SUCCESS)
            {
                OutError = FString::Printf(TEXT("SetNamedSecurityInfo failed with Win32 error %u."), SecurityResult);
                return false;
            }
            OutError.Reset();
            return true;
#else
            OutError = TEXT("PiUnrealBlueprint Transport v1 supports Win64 only.");
            return false;
#endif
        }

        FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
        {
            FString Output;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
            FJsonSerializer::Serialize(Object, Writer);
            return Output;
        }

        FProtocolResponse MakeErrorResponse(const FProtocolRequest* Request, const EErrorCode Code, const FString& Message, const FString& Callsite)
        {
            FProtocolResponse Response;
            if (Request != nullptr)
            {
                Response.Id = Request->Id;
                Response.IdJsonValue = Request->IdJsonValue;
            }
            Response.Error = FProtocolError::Make(Code, Message, Callsite);
            return Response;
        }
    }

    class FTransportConnection final : public FRunnable, public TSharedFromThis<FTransportConnection, ESPMode::ThreadSafe>
    {
    public:
        using FAuthenticationCallback = TFunction<void(bool)>;

        FTransportConnection(FSocket* InSocket, FString InSessionId, FString InToken, FAuthenticationCallback InAuthenticationCallback)
            : Socket(InSocket)
            , SessionId(MoveTemp(InSessionId))
            , Token(MoveTemp(InToken))
            , AuthenticationCallback(MoveTemp(InAuthenticationCallback))
            , bStopping(false)
            , bAuthenticated(false)
            , bCloseAfterDrain(false)
        {
        }

        ~FTransportConnection() override
        {
            StopAndWait();
            if (Socket != nullptr)
            {
                Socket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
                Socket = nullptr;
            }
        }

        bool Start()
        {
            if (Socket == nullptr || Thread != nullptr)
            {
                return false;
            }
            Socket->SetNonBlocking(true);
            Thread = FRunnableThread::Create(this, TEXT("PiUnrealBlueprintConnection"), 128 * 1024, TPri_Normal);
            return Thread != nullptr;
        }

        void StopAndWait()
        {
            bStopping = true;
            if (Socket != nullptr)
            {
                Socket->Shutdown(ESocketShutdownMode::ReadWrite);
            }
            if (Thread != nullptr)
            {
                Thread->WaitForCompletion();
                delete Thread;
                Thread = nullptr;
            }
        }

        bool IsRunning() const
        {
            return !bStopping;
        }

        void Stop() override
        {
            bStopping = true;
        }

        uint32 Run() override
        {
            while (!bStopping)
            {
                FlushOutgoing();
                if (bCloseAfterDrain && Outgoing.IsEmpty())
                {
                    break;
                }

                if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(25)))
                {
                    if (Socket->GetConnectionState() == SCS_ConnectionError)
                    {
                        break;
                    }
                    continue;
                }

                uint32 PendingSize = 0;
                while (!bStopping && Socket->HasPendingData(PendingSize) && PendingSize > 0)
                {
                    const int32 AvailableCapacity = MaxBufferedBytes - ReceiveBuffer.Num();
                    if (AvailableCapacity <= 0)
                    {
                        QueueError(nullptr, EErrorCode::TransportError, TEXT("Connection receive buffer exceeded the 8 MiB frame limit."), TEXT("FTransportConnection::Run"), true);
                        break;
                    }

                    const int32 ReadSize = FMath::Min3(static_cast<int32>(PendingSize), ReceiveChunkSize, AvailableCapacity);
                    const int32 WriteOffset = ReceiveBuffer.AddUninitialized(ReadSize);
                    int32 BytesRead = 0;
                    if (!Socket->Recv(ReceiveBuffer.GetData() + WriteOffset, ReadSize, BytesRead) || BytesRead <= 0)
                    {
                        ReceiveBuffer.SetNum(WriteOffset, false);
                        bStopping = true;
                        break;
                    }
                    ReceiveBuffer.SetNum(WriteOffset + BytesRead, false);
                    DecodeFrames();
                    if (bCloseAfterDrain)
                    {
                        break;
                    }
                }
            }

            FlushOutgoing();
            if (bAuthenticated)
            {
                bAuthenticated = false;
                AuthenticationCallback(false);
            }
            bStopping = true;
            return 0;
        }

        void QueueResponse(const FProtocolResponse& Response)
        {
            TArray<uint8> Frame;
            FString Error;
            if (!FLengthPrefixedJsonFraming::Encode(Response.ToJsonString(), Frame, Error))
            {
                UE_LOG(LogPiUnrealBlueprintServer, Error, TEXT("Cannot encode JSON-RPC response: %s"), *Error);
                bStopping = true;
                return;
            }
            Outgoing.Enqueue(MoveTemp(Frame));
        }

    private:
        void DecodeFrames()
        {
            while (!bStopping && !bCloseAfterDrain)
            {
                FString Json;
                FString FramingError;
                const EFrameDecodeResult DecodeResult = FLengthPrefixedJsonFraming::TryDecode(ReceiveBuffer, Json, FramingError);
                if (DecodeResult == EFrameDecodeResult::NeedMoreData)
                {
                    return;
                }
                if (DecodeResult == EFrameDecodeResult::Invalid)
                {
                    QueueError(nullptr, EErrorCode::TransportError, FramingError, TEXT("FLengthPrefixedJsonFraming::TryDecode"), true);
                    return;
                }
                HandleJson(Json);
            }
        }

        void HandleJson(const FString& Json)
        {
            FProtocolRequest Request;
            FProtocolError ParseError;
            if (!FProtocolRequest::Parse(Json, Request, ParseError))
            {
                FProtocolResponse Response;
                Response.Error = ParseError;
                QueueResponse(Response);
                bCloseAfterDrain = true;
                return;
            }

            if (!bAuthenticated)
            {
                Authenticate(Request);
                return;
            }
            if (Request.Method == TEXT("session.authenticate"))
            {
                QueueError(&Request, EErrorCode::InvalidRequest, TEXT("This connection is already authenticated."), TEXT("FTransportConnection::HandleJson"), false);
                return;
            }

            const TWeakPtr<FTransportConnection, ESPMode::ThreadSafe> WeakConnection = AsShared();
            AsyncTask(ENamedThreads::GameThread, [WeakConnection, Request]()
            {
                const TSharedPtr<FTransportConnection, ESPMode::ThreadSafe> Connection = WeakConnection.Pin();
                if (!Connection.IsValid() || !Connection->IsRunning())
                {
                    return;
                }
                check(IsInGameThread());
                FProtocolResponse Response = FCoreService::Get().Dispatch(Request);
                Response.Id = Request.Id;
                Response.IdJsonValue = Request.IdJsonValue;
                if (Request.Method == TEXT("unreal_status") && Response.Result.IsValid())
                {
                    Response.Result->SetStringField(TEXT("serviceState"), LexToString(EServiceState::Connected));
                    Response.Result->SetBoolField(TEXT("transportAvailable"), true);
                    Response.Result->RemoveField(TEXT("unavailableReason"));
                }
                Connection->QueueResponse(Response);
            });
        }

        void Authenticate(const FProtocolRequest& Request)
        {
            if (Request.Method != TEXT("session.authenticate"))
            {
                QueueError(&Request, EErrorCode::AuthenticationRequired,
                    TEXT("The first request on a connection must be session.authenticate."),
                    TEXT("FTransportConnection::Authenticate"), true);
                return;
            }

            FString RequestSessionId;
            FString RequestToken;
            FString RequestProtocolVersion;
            FString Client;
            if (!Request.Params.IsValid()
                || !Request.Params->TryGetStringField(TEXT("editorSessionId"), RequestSessionId)
                || !Request.Params->TryGetStringField(TEXT("authToken"), RequestToken)
                || !Request.Params->TryGetStringField(TEXT("protocolVersion"), RequestProtocolVersion)
                || !Request.Params->TryGetStringField(TEXT("client"), Client)
                || Client.IsEmpty())
            {
                QueueError(&Request, EErrorCode::AuthenticationFailed,
                    TEXT("Authentication requires non-empty editorSessionId, authToken, protocolVersion, and client strings."),
                    TEXT("FTransportConnection::Authenticate"), true);
                return;
            }
            if (!ConstantTimeEquals(RequestSessionId, SessionId) || !ConstantTimeEquals(RequestToken, Token))
            {
                QueueError(&Request, EErrorCode::AuthenticationFailed,
                    TEXT("The editor session id or authentication token is invalid."),
                    TEXT("FTransportConnection::Authenticate"), true);
                return;
            }
            if (ProtocolMajor(RequestProtocolVersion) != ProtocolMajor(ProtocolVersion))
            {
                QueueError(&Request, EErrorCode::ProtocolVersionMismatch,
                    FString::Printf(TEXT("Client protocol %s is incompatible with editor protocol %s."), *RequestProtocolVersion, ProtocolVersion),
                    TEXT("FTransportConnection::Authenticate"), true);
                return;
            }

            bAuthenticated = true;
            AuthenticationCallback(true);
            FProtocolResponse Response;
            Response.Id = Request.Id;
            Response.IdJsonValue = Request.IdJsonValue;
            Response.Result = MakeShared<FJsonObject>();
            Response.Result->SetBoolField(TEXT("authenticated"), true);
            Response.Result->SetStringField(TEXT("editorSessionId"), SessionId);
            Response.Result->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
            Response.Result->SetStringField(TEXT("pluginVersion"), PluginVersion);
            QueueResponse(Response);
        }

        void QueueError(const FProtocolRequest* Request, const EErrorCode Code, const FString& Message, const FString& Callsite, const bool bClose)
        {
            QueueResponse(MakeErrorResponse(Request, Code, Message, Callsite));
            bCloseAfterDrain = bClose;
        }

        void FlushOutgoing()
        {
            TArray<uint8> Frame;
            while (!bStopping && Outgoing.Dequeue(Frame))
            {
                int32 Offset = 0;
                while (!bStopping && Offset < Frame.Num())
                {
                    if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(100)))
                    {
                        if (Socket->GetConnectionState() == SCS_ConnectionError)
                        {
                            bStopping = true;
                        }
                        continue;
                    }
                    int32 BytesSent = 0;
                    if (!Socket->Send(Frame.GetData() + Offset, Frame.Num() - Offset, BytesSent) || BytesSent <= 0)
                    {
                        bStopping = true;
                        break;
                    }
                    Offset += BytesSent;
                }
            }
        }

        FSocket* Socket = nullptr;
        FRunnableThread* Thread = nullptr;
        FString SessionId;
        FString Token;
        FAuthenticationCallback AuthenticationCallback;
        FThreadSafeBool bStopping;
        FThreadSafeBool bAuthenticated;
        FThreadSafeBool bCloseAfterDrain;
        TArray<uint8> ReceiveBuffer;
        TQueue<TArray<uint8>, EQueueMode::Mpsc> Outgoing;
    };

    class FTransportServer::FImpl
    {
    public:
        bool Start(const FTransportServerConfig& Config, FProtocolError& OutError)
        {
            check(IsInGameThread());
            if (GetState() != EServiceState::Stopped)
            {
                OutError = FProtocolError::Make(EErrorCode::TransportError, TEXT("Transport server is already started."), TEXT("FTransportServer::Start"));
                return false;
            }
            SetState(EServiceState::Starting);
            EditorSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
            AuthToken = GenerateSecret();

            ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            if (SocketSubsystem == nullptr)
            {
                return FailStart(TEXT("The platform socket subsystem is unavailable."), OutError);
            }

            TSharedRef<FInternetAddr> BindAddress = SocketSubsystem->CreateInternetAddr();
            bool bValidAddress = false;
            BindAddress->SetIp(TEXT("127.0.0.1"), bValidAddress);
            BindAddress->SetPort(0);
            if (!bValidAddress)
            {
                return FailStart(TEXT("UE4.27 could not parse the loopback address 127.0.0.1."), OutError);
            }

            ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("PiUnrealBlueprint loopback listener"), BindAddress->GetProtocolType());
            if (ListenSocket == nullptr
                || !ListenSocket->SetReuseAddr(false)
                || !ListenSocket->Bind(*BindAddress)
                || !ListenSocket->Listen(16))
            {
                return FailStart(FString::Printf(TEXT("Failed to bind the TCP listener: %s."), SocketSubsystem->GetSocketError(SocketSubsystem->GetLastErrorCode())), OutError);
            }

            Port = ListenSocket->GetPortNo();
            if (Port <= 0 || Port > 65535)
            {
                return FailStart(TEXT("The TCP listener did not receive a valid random port."), OutError);
            }

            Listener = MakeUnique<FTcpListener>(*ListenSocket, FTimespan::FromMilliseconds(50), false);
            Listener->OnConnectionAccepted().BindRaw(this, &FImpl::HandleConnectionAccepted);
            if (!Listener->IsActive())
            {
                return FailStart(TEXT("The UE4.27 TCP listener thread failed to start."), OutError);
            }

            if (Config.bWriteSessionDescriptor && !WriteSessionDescriptor(Config, OutError))
            {
                Stop();
                SetState(EServiceState::Faulted);
                return false;
            }

            SetState(EServiceState::Listening);
            CleanupTickerHandle = FTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateRaw(this, &FImpl::TickCleanup), 1.0f);
            OutError = FProtocolError();
            return true;
        }

        void Stop()
        {
            check(IsInGameThread());
            if (GetState() == EServiceState::Stopped)
            {
                return;
            }

            if (CleanupTickerHandle.IsValid())
            {
                FTicker::GetCoreTicker().RemoveTicker(CleanupTickerHandle);
                CleanupTickerHandle.Reset();
            }
            if (Listener.IsValid())
            {
                Listener->OnConnectionAccepted().Unbind();
                Listener.Reset();
            }

            TArray<TSharedPtr<FTransportConnection, ESPMode::ThreadSafe>> ConnectionsToStop;
            {
                FScopeLock Lock(&ConnectionsMutex);
                ConnectionsToStop = Connections;
                Connections.Reset();
            }
            for (const TSharedPtr<FTransportConnection, ESPMode::ThreadSafe>& Connection : ConnectionsToStop)
            {
                Connection->StopAndWait();
            }

            if (ListenSocket != nullptr)
            {
                ListenSocket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
                ListenSocket = nullptr;
            }
            if (!SessionDescriptorPath.IsEmpty())
            {
                IFileManager::Get().Delete(*SessionDescriptorPath, false, true, true);
                SessionDescriptorPath.Reset();
            }
            {
                FScopeLock Lock(&StateMutex);
                AuthenticatedConnections = 0;
                State = EServiceState::Stopped;
            }
        }

        bool HandleConnectionAccepted(FSocket* ClientSocket, const FIPv4Endpoint& RemoteEndpoint)
        {
            if (ClientSocket == nullptr || RemoteEndpoint.Address != FIPv4Address::InternalLoopback || GetState() == EServiceState::Stopped)
            {
                return false;
            }

            const TSharedPtr<FTransportConnection, ESPMode::ThreadSafe> Connection = MakeShared<FTransportConnection, ESPMode::ThreadSafe>(
                ClientSocket,
                EditorSessionId,
                AuthToken,
                [this](const bool bAuthenticated) { OnAuthenticationChanged(bAuthenticated); });
            {
                FScopeLock Lock(&ConnectionsMutex);
                Connections.Add(Connection);
            }
            if (!Connection->Start())
            {
                FScopeLock Lock(&ConnectionsMutex);
                Connections.Remove(Connection);
                return false;
            }
            return true;
        }

        bool TickCleanup(float DeltaTime)
        {
            check(IsInGameThread());
            FScopeLock Lock(&ConnectionsMutex);
            Connections.RemoveAll([](const TSharedPtr<FTransportConnection, ESPMode::ThreadSafe>& Connection)
            {
                return !Connection.IsValid() || !Connection->IsRunning();
            });
            return GetState() != EServiceState::Stopped;
        }

        void OnAuthenticationChanged(const bool bAuthenticated)
        {
            FScopeLock Lock(&StateMutex);
            AuthenticatedConnections = FMath::Max(0, AuthenticatedConnections + (bAuthenticated ? 1 : -1));
            if (State != EServiceState::Stopped && State != EServiceState::Faulted)
            {
                State = AuthenticatedConnections > 0 ? EServiceState::Connected : EServiceState::Listening;
            }
        }

        EServiceState GetState() const
        {
            FScopeLock Lock(&StateMutex);
            return State;
        }

        void SetState(const EServiceState NewState)
        {
            FScopeLock Lock(&StateMutex);
            State = NewState;
        }

        bool WriteSessionDescriptor(const FTransportServerConfig& Config, FProtocolError& OutError)
        {
            FString SessionDirectory = Config.SessionDirectoryOverride;
            if (SessionDirectory.IsEmpty())
            {
                const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
                if (LocalAppData.IsEmpty())
                {
                    OutError = FProtocolError::Make(EErrorCode::TransportError, TEXT("LOCALAPPDATA is not set; cannot publish editor session discovery."), TEXT("FTransportServer::WriteSessionDescriptor"));
                    return false;
                }
                SessionDirectory = FPaths::Combine(LocalAppData, TEXT("PiUnrealBlueprint"), TEXT("sessions"));
            }
            SessionDirectory = FPaths::ConvertRelativePathToFull(SessionDirectory);
            if (!IFileManager::Get().MakeDirectory(*SessionDirectory, true))
            {
                OutError = FProtocolError::Make(EErrorCode::TransportError,
                    FString::Printf(TEXT("Cannot create session discovery directory '%s'."), *SessionDirectory),
                    TEXT("FTransportServer::WriteSessionDescriptor"));
                return false;
            }

            FString PermissionError;
            if (!ApplyCurrentUserOnlyAcl(SessionDirectory, true, PermissionError))
            {
                OutError = FProtocolError::Make(EErrorCode::TransportError,
                    FString::Printf(TEXT("Cannot restrict session directory '%s' to the current user: %s"), *SessionDirectory, *PermissionError),
                    TEXT("FTransportServer::WriteSessionDescriptor"));
                return false;
            }

            FString Uproject = Config.UprojectOverride.IsEmpty() ? FPaths::GetProjectFilePath() : Config.UprojectOverride;
            Uproject = FPaths::ConvertRelativePathToFull(Uproject);
            FPaths::NormalizeFilename(Uproject);
            if (Uproject.IsEmpty())
            {
                OutError = FProtocolError::Make(EErrorCode::TransportError, TEXT("The current .uproject path is unavailable."), TEXT("FTransportServer::WriteSessionDescriptor"));
                return false;
            }

            TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
            Capabilities->SetStringField(TEXT("transport"), TEXT("tcp-json-rpc-2.0"));
            Capabilities->SetNumberField(TEXT("maxFrameBytes"), FLengthPrefixedJsonFraming::MaxPayloadSize);
            Capabilities->SetBoolField(TEXT("authentication"), true);
            Capabilities->SetBoolField(TEXT("gameThreadDispatch"), true);

            TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
            Descriptor->SetStringField(TEXT("editorSessionId"), EditorSessionId);
            Descriptor->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
            Descriptor->SetStringField(TEXT("uproject"), Uproject);
            Descriptor->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
            Descriptor->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
            Descriptor->SetNumberField(TEXT("port"), Port);
            Descriptor->SetStringField(TEXT("authToken"), AuthToken);
            Descriptor->SetStringField(TEXT("pluginVersion"), PluginVersion);
            Descriptor->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
            Descriptor->SetObjectField(TEXT("capabilities"), Capabilities);
            Descriptor->SetStringField(TEXT("startedAt"), FDateTime::UtcNow().ToIso8601());

            SessionDescriptorPath = FPaths::Combine(SessionDirectory, EditorSessionId + TEXT(".json"));
            const FString TemporaryPath = SessionDescriptorPath + TEXT(".tmp");
            IFileManager::Get().Delete(*TemporaryPath, false, true, true);
            if (!FFileHelper::SaveStringToFile(SerializeJsonObject(Descriptor), *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            {
                OutError = FProtocolError::Make(EErrorCode::TransportError,
                    FString::Printf(TEXT("Cannot write temporary session descriptor '%s'."), *TemporaryPath),
                    TEXT("FTransportServer::WriteSessionDescriptor"));
                return false;
            }
            if (!ApplyCurrentUserOnlyAcl(TemporaryPath, false, PermissionError))
            {
                IFileManager::Get().Delete(*TemporaryPath, false, true, true);
                OutError = FProtocolError::Make(EErrorCode::TransportError,
                    FString::Printf(TEXT("Cannot restrict session descriptor to the current user: %s"), *PermissionError),
                    TEXT("FTransportServer::WriteSessionDescriptor"));
                return false;
            }
            if (!IFileManager::Get().Move(*SessionDescriptorPath, *TemporaryPath, true, true, false, true))
            {
                IFileManager::Get().Delete(*TemporaryPath, false, true, true);
                OutError = FProtocolError::Make(EErrorCode::TransportError,
                    FString::Printf(TEXT("Cannot atomically publish session descriptor '%s'."), *SessionDescriptorPath),
                    TEXT("FTransportServer::WriteSessionDescriptor"));
                return false;
            }
            return true;
        }

        bool FailStart(const FString& Message, FProtocolError& OutError)
        {
            OutError = FProtocolError::Make(EErrorCode::TransportError, Message, TEXT("FTransportServer::Start"));
            if (Listener.IsValid())
            {
                Listener.Reset();
            }
            if (ListenSocket != nullptr)
            {
                ListenSocket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
                ListenSocket = nullptr;
            }
            SetState(EServiceState::Faulted);
            return false;
        }

        mutable FCriticalSection StateMutex;
        mutable FCriticalSection ConnectionsMutex;
        EServiceState State = EServiceState::Stopped;
        int32 AuthenticatedConnections = 0;
        int32 Port = 0;
        FString EditorSessionId;
        FString AuthToken;
        FString SessionDescriptorPath;
        FSocket* ListenSocket = nullptr;
        TUniquePtr<FTcpListener> Listener;
        FDelegateHandle CleanupTickerHandle;
        TArray<TSharedPtr<FTransportConnection, ESPMode::ThreadSafe>> Connections;
    };

    FTransportServer::FTransportServer()
        : Impl(MakeUnique<FImpl>())
    {
    }

    FTransportServer::~FTransportServer()
    {
        if (Impl.IsValid() && Impl->GetState() != EServiceState::Stopped)
        {
            check(IsInGameThread());
            Impl->Stop();
        }
    }

    bool FTransportServer::Start(const FTransportServerConfig& Config, FProtocolError& OutError)
    {
        return Impl->Start(Config, OutError);
    }

    void FTransportServer::Stop()
    {
        Impl->Stop();
    }

    EServiceState FTransportServer::GetState() const
    {
        return Impl->GetState();
    }

    int32 FTransportServer::GetPort() const
    {
        return Impl->Port;
    }

    FString FTransportServer::GetEditorSessionId() const
    {
        return Impl->EditorSessionId;
    }

    FString FTransportServer::GetAuthToken() const
    {
        return Impl->AuthToken;
    }

    FString FTransportServer::GetSessionDescriptorPath() const
    {
        return Impl->SessionDescriptorPath;
    }
}
