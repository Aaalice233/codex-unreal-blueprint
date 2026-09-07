#pragma once

#include "CoreMinimal.h"
#include "Factories/CSVImportFactory.h"
#include "CodexInteractiveCSVImportTestFactory.generated.h"

UCLASS(Transient)
class UCodexInteractiveCSVImportTestFactory : public UCSVImportFactory
{
    GENERATED_BODY()

public:
    UCodexInteractiveCSVImportTestFactory(const FObjectInitializer& ObjectInitializer)
        : Super(ObjectInitializer)
    {
        // 测试工厂不得参与用户正常文件的导入器选择。
        bEditorImport = false;
    }

    // 测试进入真实交互分支，避免全局自动化标记跳过弹窗。
    virtual bool IsAutomatedImport() const override { return false; }
};
