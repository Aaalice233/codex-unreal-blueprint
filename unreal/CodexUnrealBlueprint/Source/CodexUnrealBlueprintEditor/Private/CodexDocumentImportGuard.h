#pragma once

#include "CoreMinimal.h"

class SWindow;

namespace CodexUnrealBlueprint
{
    class FDocumentImportGuard
    {
    public:
        ~FDocumentImportGuard();
        void Start();
        void Stop();

        static bool IsGeneratedDocumentPath(const FString& PackagePath);

    private:
        void TickModalLoop(float DeltaTime);

        FDelegateHandle ModalLoopHandle;
        TWeakPtr<SWindow> InspectedWindow;
    };
}
