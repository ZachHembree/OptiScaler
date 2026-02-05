#include "pch.h"
#include "Util.h"
#include "Config.h"

#include "NVNGX_DLSS.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"

#include <upscalers/FeatureProvider_Dx12.h>
#include "upscalers/dlss/DLSSFeature_Dx12.h"

#include "FG/DLSSG_Mod.h"
#include "FG/FSR3_Dx12_FG.h"
#include "FG/Upscaler_Inputs_Dx12.h"

#include <upscaler_time/UpscalerTime_Dx12.h>

#include <hooks/D3D12_Hooks.h>

#include <dxgi1_4.h>
#include <shared_mutex>
#include "detours/detours.h"
#include <ankerl/unordered_dense.h>

static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx12>> Dx12Contexts;

static ankerl::unordered_dense::map<ID3D12GraphicsCommandList*, ID3D12RootSignature*> computeSignatures;
static ankerl::unordered_dense::map<ID3D12GraphicsCommandList*, ID3D12RootSignature*> graphicSignatures;
static ID3D12Device* D3D12Device = nullptr;
static int evalCounter = 0;
static std::wstring appDataPath = L".";
static bool shutdown = false;
static inline bool _skipInit = false;

class ScopedInit
{
  private:
    bool previousState;

  public:
    ScopedInit()
    {
        previousState = _skipInit;
        _skipInit = true;
    }
    ~ScopedInit() { _skipInit = previousState; }
};

#pragma region Hooks

typedef void (*PFN_SetComputeRootSignature)(ID3D12GraphicsCommandList* commandList,
                                            ID3D12RootSignature* pRootSignature);

static PFN_SetComputeRootSignature orgSetComputeRootSignature = nullptr;
static PFN_SetComputeRootSignature orgSetGraphicRootSignature = nullptr;

static bool contextRendering = false;
static std::shared_mutex computeSigatureMutex;
static std::shared_mutex graphSigatureMutex;

static bool GetIsDlssModuleInited()
{ 
    return Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited(); 
}

static void hkSetComputeRootSignature(ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* pRootSignature)
{
    if (Config::Instance()->RestoreComputeSignature.value_or_default() && !contextRendering && commandList != nullptr &&
        pRootSignature != nullptr)
    {
        std::unique_lock<std::shared_mutex> lock(computeSigatureMutex);
        computeSignatures.insert_or_assign(commandList, pRootSignature);
    }

    orgSetComputeRootSignature(commandList, pRootSignature);
}

static void hkSetGraphicRootSignature(ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* pRootSignature)
{
    if (Config::Instance()->RestoreGraphicSignature.value_or_default() && !contextRendering && commandList != nullptr &&
        pRootSignature != nullptr)
    {
        std::unique_lock<std::shared_mutex> lock(graphSigatureMutex);
        graphicSignatures.insert_or_assign(commandList, pRootSignature);
    }

    orgSetGraphicRootSignature(commandList, pRootSignature);
}

/**
 * @brief Attaches hooks to monitor the state of the application's DX12 render pipeline and restore state as needed.
 */
static void HookToCommandList(ID3D12GraphicsCommandList* InCmdList)
{
    if (orgSetComputeRootSignature != nullptr || orgSetGraphicRootSignature != nullptr)
        return;

    PVOID* pVTable = *(PVOID**) InCmdList;

    orgSetComputeRootSignature = (PFN_SetComputeRootSignature) pVTable[29];
    orgSetGraphicRootSignature = (PFN_SetComputeRootSignature) pVTable[30];

    if (orgSetComputeRootSignature != nullptr || orgSetGraphicRootSignature != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (orgSetComputeRootSignature != nullptr)
            DetourAttach(&(PVOID&) orgSetComputeRootSignature, hkSetComputeRootSignature);

        if (orgSetGraphicRootSignature != nullptr)
            DetourAttach(&(PVOID&) orgSetGraphicRootSignature, hkSetGraphicRootSignature);

        LOG_DEBUG("Hooked SetRootSignature functions");

        DetourTransactionCommit();
    }
}

/**
 * @brief Uninstalls renderer state monitoring hooks
 */
static void UnhookAll()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (orgSetComputeRootSignature != nullptr)
    {
        DetourDetach(&(PVOID&) orgSetComputeRootSignature, hkSetComputeRootSignature);
        orgSetComputeRootSignature = nullptr;
    }

    if (orgSetGraphicRootSignature != nullptr)
    {
        DetourDetach(&(PVOID&) orgSetGraphicRootSignature, hkSetGraphicRootSignature);
        orgSetGraphicRootSignature = nullptr;
    }

    DetourTransactionCommit();
}

#pragma endregion

#pragma region DLSS Init Calls

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long InApplicationId,
    const wchar_t* InApplicationDataPath, 
    ID3D12Device* InDevice,
    NVSDK_NGX_Version InSDKVersion,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
        InApplicationId = app_id_override;

    State::Instance().NVNGX_ApplicationId = InApplicationId;
    State::Instance().NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath);
    State::Instance().NVNGX_Version = InSDKVersion;
    State::Instance().NVNGX_FeatureInfo = InFeatureInfo;

    if (InFeatureInfo != nullptr && InSDKVersion > 0x0000013)
        State::Instance().NVNGX_Logger = InFeatureInfo->LoggingInfo;

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init_Ext");
            auto result = NVNGXProxy::D3D12_Init_Ext()(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion,
                                                       InFeatureInfo);
            LOG_INFO("calling NVNGXProxy::D3D12_Init_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
        else
        {
            LOG_WARN("NVNGXProxy::NVNGXModule or NVNGXProxy::D3D12_Init_Ext is nullptr!");
        }
    }

    if (State::Instance().NvngxDx12Inited)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    if (State::Instance().activeFgInput == FGInput::Nukems)
    {
        DLSSGMod::InitDLSSGMod_Dx12();
        DLSSGMod::D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);
    }

    LOG_INFO("AppId: {0}", InApplicationId);
    LOG_INFO("SDK: {0:x}", (unsigned int) InSDKVersion);
    appDataPath = std::wstring(InApplicationDataPath);

    LOG_INFO("InApplicationDataPath {0}", wstring_to_string(appDataPath));

    State::Instance().NVNGX_FeatureInfo_Paths.clear();

    if (InFeatureInfo != nullptr)
    {
        for (size_t i = 0; i < InFeatureInfo->PathListInfo.Length; i++)
        {
            const wchar_t* path = InFeatureInfo->PathListInfo.Path[i];
            State::Instance().NVNGX_FeatureInfo_Paths.push_back(std::wstring(path));
        }
    }

    D3D12Device = InDevice;
    State::Instance().currentD3D12Device = InDevice;
    D3D12Hooks::HookDevice(InDevice);

    if (!State::Instance().isWorkingAsNvngx)
    {
        UpscalerTimeDx12::Init(InDevice);
    }

    State::Instance().NvngxDx12Inited = true;

    UpscalerInputsDx12::Init(InDevice);

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init(
    unsigned long long InApplicationId,
    const wchar_t* InApplicationDataPath, 
    ID3D12Device* InDevice,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init");

            auto result =
                NVNGXProxy::D3D12_Init()(InApplicationId, InApplicationDataPath, InDevice, InFeatureInfo, InSDKVersion);

            LOG_INFO("calling NVNGXProxy::D3D12_Init result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
    }

    if (State::Instance().NvngxDx12Inited)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    // if (State::Instance().activeFgInput == FGInput::Nukems)
    //{
    //     DLSSGMod::InitDLSSGMod_Dx12();
    //     DLSSGMod::D3D12_Init(InApplicationId, InApplicationDataPath, InDevice, InFeatureInfo, InSDKVersion);
    // }
    
    ScopedInit scopedInit {};
    auto result =
        NVSDK_NGX_D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);

    LOG_DEBUG("was called NVSDK_NGX_D3D12_Init_Ext");
    return result;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_ProjectID(
    const char* InProjectId,
    NVSDK_NGX_EngineType InEngineType,
    const char* InEngineVersion,
    const wchar_t* InApplicationDataPath,
    ID3D12Device* InDevice, 
    NVSDK_NGX_Version InSDKVersion,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InProjectId = project_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::D3D12_Init_ProjectID() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::D3D12_Init_ProjectID");

            auto result =
                NVNGXProxy::D3D12_Init_ProjectID()(InProjectId, InEngineType, InEngineVersion, InApplicationDataPath,
                                                   InDevice, InSDKVersion, InFeatureInfo);

            LOG_INFO("calling NVNGXProxy::D3D12_Init_ProjectID result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetDx12Inited(true);
        }
    }

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (State::Instance().NvngxDx12Inited)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    ScopedInit scopedInit {};
    auto result = NVSDK_NGX_D3D12_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);
    return result;
}

// Not sure about this one, original nvngx does not export this method
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
    const wchar_t* InApplicationDataPath, ID3D12Device* InDevice, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo,
    NVSDK_NGX_Version InSDKVersion)
{
    LOG_FUNC();

    LOG_INFO("InProjectId: {0}", InProjectId);
    LOG_INFO("InEngineType: {0}", (int) InEngineType);
    LOG_INFO("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    if (State::Instance().NvngxDx12Inited)
    {
        LOG_WARN("NVNGX already inited");
        return NVSDK_NGX_Result_Success;
    }

    auto result = NVSDK_NGX_D3D12_Init_Ext(0x1337, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo);

    return result;
}

#pragma endregion

#pragma region DLSS Shutdown Calls

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Shutdown(void)
{
    shutdown = true;
    State::Instance().NvngxDx12Inited = false;

    D3D12Device = nullptr;

    State::Instance().currentFeature = nullptr;

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // UnhookAll();
    DLSSFeatureDx12::Shutdown(D3D12Device);

    // Added `&& !State::Instance().isShuttingDown` hack for crash on exit
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited() &&
        NVNGXProxy::D3D12_Shutdown() != nullptr && !State::Instance().isShuttingDown)
    {
        auto result = NVNGXProxy::D3D12_Shutdown()();
        NVNGXProxy::SetDx12Inited(false);
    }

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // HooksDx::UnHook();

    // Disabled to prevent crash
    if (State::Instance().currentFG != nullptr && State::Instance().activeFgInput == FGInput::Upscaler)
    {
        if (State::Instance().isShuttingDown)
            State::Instance().currentFG->Shutdown();
        else
            State::Instance().currentFG->DestroyFGContext();

        State::Instance().ClearCapturedHudlesses = true;
    }

    shutdown = false;

    if (State::Instance().activeFgInput == FGInput::Nukems)
        DLSSGMod::D3D12_Shutdown();

    State::Instance().NvngxDx12Inited = false;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* InDevice)
{
    shutdown = true;
    State::Instance().NvngxDx12Inited = false;

    if (State::Instance().activeFgInput == FGInput::Nukems)
        DLSSGMod::D3D12_Shutdown1(InDevice);

    // Added `&& !State::Instance().isShuttingDown` hack for crash on exit
    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited() &&
        NVNGXProxy::D3D12_Shutdown1() != nullptr && !State::Instance().isShuttingDown)
    {
        auto result = NVNGXProxy::D3D12_Shutdown1()(InDevice);
        NVNGXProxy::SetDx12Inited(false);
    }

    return NVSDK_NGX_D3D12_Shutdown();
}

#pragma endregion

#pragma region DLSS Parameter Calls

/**
 * @brief [Deprecated NGX API] Superceeded by NVSDK_NGX_AllocateParameters and NVSDK_NGX_GetCapabilityParameters.
 *
 * Retrieves a common NVSDK parameter map for providing params to the SDK. The lifetime of this
 * map is NOT managed by the application. It is expected to be managed internally by the SDK.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // If DLSS is enabled and the real DLSS module is loaded, get native NGX table
    if (GetIsDlssModuleInited() && NVNGXProxy::D3D12_GetParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_GetParameters");
        auto result = NVNGXProxy::D3D12_GetParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_GetParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        // Copy OptiScaler config to real NGX param table
        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVPersistent);
            return NVSDK_NGX_Result_Success;
        }
    }

    // Get custom parameters if using custom backend
    static NVNGX_Parameters oldParams = NVNGX_Parameters(OptiKeys::Dx12Provider, true);
    InitNGXParameters(*OutParameters);
    *OutParameters = &oldParams;

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new NVSDK parameter map pre-populated with NGX capabilities and information about available features. 
 * The output parameter map may also be used in the same ways as a parameter map allocated with AllocateParameters().
 * The lifetime of this map is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    // Get native DLSS params if DLSS is enabled and the module is loaded
    if (GetIsDlssModuleInited() && NVNGXProxy::IsDx12Inited() && NVNGXProxy::D3D12_GetCapabilityParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_GetCapabilityParameters");
        auto result = NVNGXProxy::D3D12_GetCapabilityParameters()(OutParameters);
        LOG_INFO("Calling NVNGXProxy::D3D12_GetCapabilityParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        if (result == NVSDK_NGX_Result_Success)
        {
            // Init external NGX table with current configuration and mark as dynamic+external
            InitNGXParameters(*OutParameters);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return NVSDK_NGX_Result_Success;
        }
    }
    
    // Get custom parameters if using custom backend
    auto& params = *(new NVNGX_Parameters(OptiKeys::Dx12Provider, false));
    InitNGXParameters(&params);
    *OutParameters = &params;

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new parameter map used to provide parameters needed by the DLSS API. The lifetime of this map 
 * is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (GetIsDlssModuleInited() && NVNGXProxy::D3D12_AllocateParameters() != nullptr)
    {
        LOG_INFO("Calling NVNGXProxy::D3D12_AllocateParameters");
        auto result = NVNGXProxy::D3D12_AllocateParameters()(OutParameters);

        LOG_INFO("Calling NVNGXProxy::D3D12_AllocateParameters result: {0:X}, ptr: {1:X}", (UINT) result,
                 (UINT64) *OutParameters);

        if (result == NVSDK_NGX_Result_Success)
        {
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    auto* params = new NVNGX_Parameters(OptiKeys::Dx12Provider, false);
    *OutParameters = params;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    InitNGXParameters(InParameters);

    if (State::Instance().activeFgInput == FGInput::Nukems)
        DLSSGMod::D3D12_PopulateParameters_Impl(InParameters);

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Destroys a given input parameter map created with AllocateParameters or GetCapabilityParameters. 
 Must not be called on maps returned by GetParameters(). Unsupported tables will not be freed.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    const bool isUsingDlss = Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule();
    const bool success = TryDestroyNGXParameters(InParameters, NVNGXProxy::D3D12_DestroyParameters());

    if (isUsingDlss)
        UpscalerInputsDx12::Reset();

    return success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

#pragma endregion

#pragma region DLSS Feature Calls

static std::string_view GetUpscalerBackend()
{
    std::string_view name = OptiKeys::XeSS; // Default

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsDx12Inited())
        name = OptiKeys::DLSS;

    if (Config::Instance()->Dx12Upscaler.has_value())
        name = Config::Instance()->Dx12Upscaler.value();

    return name;
}

static bool EnsureD3D12Device(ID3D12GraphicsCommandList* cmdList)
{
    if (D3D12Device)
        return true;

    LOG_DEBUG("Get D3D12 device from InCmdList!");

    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&D3D12Device))) || !D3D12Device)
    {
        LOG_ERROR("Can't get Dx12Device from InCmdList!");
        return false;
    }

    return true;
}

static void RestoreRootSignatures(ID3D12GraphicsCommandList* cmdList)
{
    const auto& config = *Config::Instance();
    const bool restoreCompute = config.RestoreComputeSignature.value_or_default();
    const bool restoreGraphic = config.RestoreGraphicSignature.value_or_default();

    if (!restoreCompute && !restoreGraphic)
        return;

    if (restoreCompute)
    {
        if (computeSignatures.contains(cmdList))
        {
            LOG_TRACE("restore ComputeRootSig: {0:X}", (UINT64) computeSignatures[cmdList]);
            orgSetComputeRootSignature(cmdList, computeSignatures[cmdList]);
        }
        else
        {
            LOG_TRACE("Can't restore ComputeRootSig");
        }
    }

    if (restoreGraphic)
    {
        if (graphicSignatures.contains(cmdList))
        {
            LOG_TRACE("restore GraphicRootSig: {0:X}", (UINT64) graphicSignatures[cmdList]);
            orgSetGraphicRootSignature(cmdList, graphicSignatures[cmdList]);
        }
        else
        {
            LOG_TRACE("Can't restore GraphicRootSig");
        }
    }
}

static NVSDK_NGX_Result TryCreateOptiFeature(
    ID3D12GraphicsCommandList* InCmdList, 
    NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter* InParameters, 
    NVSDK_NGX_Handle** OutHandle)
{
    State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    state.api = DX12;

    const uint32_t handleId = IFeature::GetNextHandleId();
    LOG_INFO("Creating OptiScaler feature, HandleId: {}", handleId);

    // Determine backend name
    std::string featureName;
    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        featureName = GetUpscalerBackend();
        LOG_INFO("Creating {} upscaler feature", featureName);
    }
    else
    {
        featureName = OptiKeys::DLSSD;
        LOG_INFO("Creating DLSSD (Ray Reconstruction) feature");
    }

    // Root signature restoration setup
    const bool restoreCompute = cfg.RestoreComputeSignature.value_or_default();
    const bool restoreGraphic = cfg.RestoreGraphicSignature.value_or_default();
    const bool shouldRestore = restoreCompute || restoreGraphic;

    if (shouldRestore)
        contextRendering = true;

    // Create context entry
    Dx12Contexts[handleId] = {};

    // Retrieve feature implementation
    if (!FeatureProvider_Dx12::GetFeature(featureName, handleId, 
            InParameters, &Dx12Contexts[handleId].feature))
    {
        LOG_ERROR("Failed to retrieve feature implementation for '{}'", featureName);

        if (shouldRestore)
            contextRendering = false;

        Dx12Contexts.erase(handleId);
        return NVSDK_NGX_Result_Fail;
    }

    // Assign handle
    if (*OutHandle == nullptr)
        *OutHandle = new NVSDK_NGX_Handle { handleId };
    else
        (*OutHandle)->Id = handleId;

    // Ensure D3D12 device
    if (!EnsureD3D12Device(InCmdList))
    {
        LOG_ERROR("Failed to acquire D3D12 device");

        if (shouldRestore)
            contextRendering = false;

        // Partial cleanup � handle is allocated but context is incomplete
        Dx12Contexts.erase(handleId);
        return NVSDK_NGX_Result_Fail;
    }

    state.AutoExposure.reset();

    IFeature_Dx12* feature = Dx12Contexts[handleId].feature.get();

    // Initialize feature
    if (feature->Init(D3D12Device, InCmdList, InParameters))
    {
        state.currentFeature = feature;
        evalCounter = 0;
        UpscalerInputsDx12::Reset();
    }
    else
    {
        LOG_ERROR("Feature '{}' initialization failed falling back to FSR 2.1.2", featureName);
        state.newBackend = OptiKeys::FSR21;
        state.changeBackend[handleId] = true;
    }

    // Restore root signatures
    if (shouldRestore)
    {
        RestoreRootSignatures(InCmdList);
        contextRendering = false;
    }

    state.FGchanged = true;

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Instantiates a new feature based on the given unique feature ID and param table and
 * provides a handle used to reference the feature elsewhere in the API. Currently supports
 * various TSR and Frame Generation algorithms, including a special case for DLSS-RR passthrough.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter* InParameters,
    NVSDK_NGX_Handle** OutHandle)
{
    LOG_FUNC();

    if (!InCmdList)
    {
        LOG_ERROR("InCmdList is null");
        return NVSDK_NGX_Result_Fail;
    }

    if (!OutHandle)
    {
        LOG_ERROR("OutHandle is null");
        return NVSDK_NGX_Result_Fail;
    }

    HookToCommandList(InCmdList);

    const State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    // Nukem's DLSSG mod passthrough
    if (state.activeFgInput == FGInput::Nukems && DLSSGMod::isDx12Available() &&
        InFeatureID == NVSDK_NGX_Feature_FrameGeneration)
    {
        LOG_INFO("Passthrough to Nukem's DLSSG CreateFeature for FrameGeneration");

        NVSDK_NGX_Result res = DLSSGMod::D3D12_CreateFeature(InCmdList, InFeatureID, InParameters, OutHandle);

        if (*OutHandle)
            LOG_INFO("Created modded DLSSG feature with HandleId: {}", (*OutHandle)->Id);

        return res;
    }

    // Native DLSS passthrough (exclude SuperSampling and RayReconstruction)
    if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (cfg.DLSSEnabled.value_or_default() && NVNGXProxy::InitDx12(D3D12Device) &&
            NVNGXProxy::D3D12_CreateFeature() != nullptr)
        {
            LOG_INFO("Passthrough to native NGX CreateFeature for feature {}", (int) InFeatureID);

            NVSDK_NGX_Result res = NVNGXProxy::D3D12_CreateFeature()(InCmdList, InFeatureID, InParameters, OutHandle);

            if (*OutHandle)
                LOG_INFO("Native CreateFeature success, HandleId: {}", (*OutHandle)->Id);
            else
                LOG_INFO("Native CreateFeature failed: {:#x}", (uint32_t) res);

            return res;
        }

        LOG_WARN("Native DLSS passthrough not available for feature {}", (int) InFeatureID);
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }

    // OptiScaler internal handling (SuperSampling or RayReconstruction)
    return TryCreateOptiFeature(InCmdList, InFeatureID, InParameters, OutHandle);
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    LOG_FUNC();

    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;
    State::Instance().FGchanged = true;

    // Clean up framegen
    if (State::Instance().currentFG != nullptr && State::Instance().activeFgInput == FGInput::Upscaler)
    {
        State::Instance().currentFG->DestroyFGContext();
        State::Instance().ClearCapturedHudlesses = true;
        UpscalerInputsDx12::Reset();
    }

    if (!shutdown)
        LOG_INFO("releasing feature with id {0}", handleId);

    // OptiScaler handles start after this offset. If it's outside this range, it doesn't belong to OptiScaler.
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_ReleaseFeature() != nullptr)
        {
            if (!shutdown)
                LOG_INFO("calling D3D12_ReleaseFeature for ({0})", handleId);

            // Clean up real DLSS feature
            auto result = NVNGXProxy::D3D12_ReleaseFeature()(InHandle);

            if (!shutdown)
                LOG_INFO("D3D12_ReleaseFeature result for ({0}): {1:X}", handleId, (UINT) result);

            return result;
        }
        else
        {
            if (!shutdown)
                LOG_INFO("D3D12_ReleaseFeature not available for ({0})", handleId);

            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }
    // Clean up OptiScaler feature with framegen
    else if (State::Instance().activeFgInput == FGInput::Nukems && handleId >= DLSSG_MOD_ID_OFFSET)
    {
        LOG_INFO("D3D12_ReleaseFeature modded DLSSG with HandleId: {0}", handleId);
        return DLSSGMod::D3D12_ReleaseFeature(InHandle);
    }

    // Remove feature from context map
    if (auto it = Dx12Contexts.find(handleId); it != Dx12Contexts.end())
    {
        auto& entry = it->second;

        if (auto* deviceContext = entry.feature.get())
        {
            // Clear global reference if it matches
            if (deviceContext == State::Instance().currentFeature)
                State::Instance().currentFeature = nullptr;

            // Erase from map (smart pointer reset is implicit on erase)
            Dx12Contexts.erase(it);
        }
    }

    // Fallback Error Handling
    if (!shutdown)
        LOG_ERROR("can't release feature with id {0}!", handleId);

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Used by the client application to check for feature support. 
 * @param Adapter Device the feature is for.
 * @param FeatureDiscoveryInfo Specifies the feature being queried.
 * @param OutSupported Used to indicate whether a feature is supported and its requirements. 
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter* Adapter, 
    const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement* OutSupported)
{
    LOG_DEBUG("for ({0})", (int) FeatureDiscoveryInfo->FeatureID);

    if (State::Instance().activeFgInput == FGInput::Nukems)
        DLSSGMod::InitDLSSGMod_Dx12();

    if (FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling ||
        ((DLSSGMod::isDx12Available() || Config::Instance()->FGInput == FGInput::DLSSG) &&
         FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_FrameGeneration))
    {
        if (OutSupported == nullptr)
        {
            static auto tmp = NVSDK_NGX_FeatureRequirement();
            OutSupported = &tmp;
        }

        OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_Supported;
        OutSupported->MinHWArchitecture = 0;

        // Some old windows 10 os version
        strcpy_s(OutSupported->MinOSVersion, "10.0.10240.16384");
        return NVSDK_NGX_Result_Success;
    }

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() == nullptr)
        NVNGXProxy::InitNVNGX();

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_GetFeatureRequirements() != nullptr)
    {
        LOG_DEBUG("D3D12_GetFeatureRequirements for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
        auto result = NVNGXProxy::D3D12_GetFeatureRequirements()(Adapter, FeatureDiscoveryInfo, OutSupported);
        LOG_DEBUG("D3D12_GetFeatureRequirements result for ({0}): {1:X}", (int) FeatureDiscoveryInfo->FeatureID,
                  (UINT) result);
        return result;
    }
    else
    {
        LOG_DEBUG("D3D12_GetFeatureRequirements not available for ({0})", (int) FeatureDiscoveryInfo->FeatureID);
    }

    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

static NVSDK_NGX_Result TryEvaluateOptiFeature(
    ID3D12GraphicsCommandList* InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    State& state = State::Instance();
    const Config& cfg = *Config::Instance();
    const uint32_t handleId = InFeatureHandle->Id;

    auto ctxIt = Dx12Contexts.find(handleId);

    if (ctxIt == Dx12Contexts.end())
    {
        LOG_WARN("No context found for handle {}", handleId);
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    ContextData<IFeature_Dx12>& ctxData = ctxIt->second;
    IFeature_Dx12* feature = ctxData.feature.get();

    if (feature == nullptr) // Prevent source api name flicker when dlssg is active
        state.setInputApiName = state.currentInputApiName;

    const std::string_view targetApiName = state.setInputApiName.empty() ? "DLSS" : state.setInputApiName.c_str();

    if (state.currentInputApiName != targetApiName)
        state.currentInputApiName = targetApiName;

    state.setInputApiName.clear();
    evalCounter++;

    // Skip evaluation for the first N frames if configured
    if (cfg.SkipFirstFrames.has_value() && evalCounter < cfg.SkipFirstFrames.value())
        return NVSDK_NGX_Result_Success;

    if (InCallback)
        LOG_INFO("Progress callback provided but unused in synchronous OptiScaler path");

    // Resolution change detection (only for upscalers that may require recreation)
    if (feature != nullptr)
    {
        const bool isFSR31OrLater = feature->Name().starts_with("FSR") && feature->Version() >= feature_version { 3, 1, 0 };

        // FSR 3.1 supports upscaleSize that doesn't need reinit to change output resolution
        if (!isFSR31OrLater && feature->UpdateOutputResolution(InParameters))
            state.changeBackend[handleId] = true;
    }

    // Backend change or recreation requested
    if (state.changeBackend[handleId])
    {
        UpscalerInputsDx12::Reset();
        contextRendering = false;

        FeatureProvider_Dx12::ChangeFeature(state.newBackend, D3D12Device, 
            InCmdList, handleId, InParameters, &ctxData);
        feature = ctxData.feature.get();

        evalCounter = 0;
        return NVSDK_NGX_Result_Success;
    }

    // Fallback to FSR 2.1.2 if feature failed to initialize and user didn't explicitly request it
    if (!feature->IsInited() && cfg.Dx12Upscaler.value_or_default() != OptiKeys::FSR21)
    {
        LOG_WARN("Feature '{}' failed to initialize. Falling back to FSR 2.1.2", feature->Name());
        state.newBackend = OptiKeys::FSR21;
        state.changeBackend[handleId] = true;
        return NVSDK_NGX_Result_Success;
    }

    state.currentFeature = feature;
    
    // Root signature restoration setup
    const bool restoreCompute = cfg.RestoreComputeSignature.value_or_default();
    const bool restoreGraphic = cfg.RestoreGraphicSignature.value_or_default();
    const bool shouldRestore = (feature->Name() != "DLSSD") && (restoreCompute || restoreGraphic);

    if (shouldRestore)
        contextRendering = true;

    // Prepare upscaling inputs
    UpscalerInputsDx12::UpscaleStart(InCmdList, InParameters, feature);
    FSR3FG::SetUpscalerInputs(InCmdList, InParameters, feature);

    if (!state.isWorkingAsNvngx)
        UpscalerTimeDx12::UpscaleStart(InCmdList);

    // Evaluate the feature
    bool evalSuccess = false;
    {
        ScopedSkipHeapCapture skip {};
        evalSuccess = feature->Evaluate(InCmdList, InParameters);
    }

    // Cleanup on success
    if (evalSuccess)
    {
        if (!state.isWorkingAsNvngx)
            UpscalerTimeDx12::UpscaleEnd(InCmdList);

        UpscalerInputsDx12::UpscaleEnd(InCmdList, InParameters, feature);
    }
    else
    {
        LOG_ERROR("Feature evaluation failed for '{}'", feature->Name());
    }

    // Restore root signatures
    if (shouldRestore)
    {
        RestoreRootSignatures(InCmdList);
        contextRendering = false;
    }

    return evalSuccess ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

/**
 * @brief Per-frame feature execution. Runs a feature (upscaler, framegen, etc.) on a given command list using a
 * preexisting feature instance referenced by a unique handle.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    if (!InFeatureHandle)
    {
        LOG_DEBUG("InFeatureHandle is null");
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    if (!InCmdList)
    {
        LOG_ERROR("InCmdList is null");
        return NVSDK_NGX_Result_Fail;
    }

    const uint32_t handleId = InFeatureHandle->Id;
    LOG_DEBUG("Handle: {}, CmdList: {:p}", handleId, (void*) InCmdList);

    const State& state = State::Instance();
    const Config& cfg = *Config::Instance();

    // Native DLSS passthrough
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (cfg.DLSSEnabled.value_or_default() && NVNGXProxy::D3D12_EvaluateFeature() != nullptr)
        {
            LOG_DEBUG("Passthrough to native DLSS EvaluateFeature for handle {}", handleId);
            NVSDK_NGX_Result result =
                NVNGXProxy::D3D12_EvaluateFeature()(InCmdList, InFeatureHandle, InParameters, InCallback);
            LOG_DEBUG("Native DLSS EvaluateFeature result: {:#x}", (uint32_t) result);
            return result;
        }

        LOG_DEBUG("Native DLSS EvaluateFeature not available for handle {}", handleId);
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }

    // Nukem's DLSSG mod passthrough
    if (state.activeFgInput == FGInput::Nukems && handleId >= DLSSG_MOD_ID_OFFSET)
    {
        LOG_DEBUG("Passthrough to Nukem's DLSSG EvaluateFeature for handle {}", handleId);
        return DLSSGMod::D3D12_EvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    // OptiScaler internal handling
    return TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
}

#pragma endregion

#pragma region DLSS Buffer Size Call

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_GetScratchBufferSize(
    NVSDK_NGX_Feature InFeatureId,
    const NVSDK_NGX_Parameter* InParameters,
    size_t* OutSizeInBytes)
{
    if (State::Instance().activeFgInput == FGInput::Nukems && DLSSGMod::isDx12Available() &&
        InFeatureId == NVSDK_NGX_Feature_FrameGeneration)
    {
        return DLSSGMod::D3D12_GetScratchBufferSize(InFeatureId, InParameters, OutSizeInBytes);
    }

    LOG_WARN("-> 52428800");
    *OutSizeInBytes = 52428800;
    return NVSDK_NGX_Result_Success;
}

#pragma endregion
