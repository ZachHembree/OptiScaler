#include "pch.h"
#include "FeatureProvider_Dx12.h"

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"

#include "upscalers/dlss/DLSSFeature_Dx12.h"
#include "upscalers/dlssd/DLSSDFeature_Dx12.h"
#include "upscalers/fsr2/FSR2Feature_Dx12.h"
#include "upscalers/fsr2_212/FSR2Feature_Dx12_212.h"
#include "upscalers/fsr31/FSR31Feature_Dx12.h"
#include "upscalers/xess/XeSSFeature_Dx12.h"
#include "FeatureProvider_Dx11.h"

bool FeatureProvider_Dx12::GetFeature(
    std::string_view upscalerName, 
    UINT handleId, 
    NVSDK_NGX_Parameter* parameters,
    std::unique_ptr<IFeature_Dx12>* feature)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();
    ScopedSkipHeapCapture skipHeapCapture {};
    std::string_view config_upscaler(upscalerName);

    if (upscalerName == "xess")
    {
        *feature = std::make_unique<XeSSFeatureDx12>(handleId, parameters);
    }
    else if (upscalerName == "fsr21")
    {
        *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
    }
    else if (upscalerName == "fsr22")
    {
        *feature = std::make_unique<FSR2FeatureDx12>(handleId, parameters);
    }
    else if (upscalerName == "fsr31")
    {
        *feature = std::make_unique<FSR31FeatureDx12>(handleId, parameters);
    }
    else if (cfg.DLSSEnabled.value_or_default())
    {
        if (upscalerName == "dlss" && state.NVNGX_DLSS_Path.has_value())
        {
            *feature = std::make_unique<DLSSFeatureDx12>(handleId, parameters);
        }
        else if (upscalerName == "dlssd" && state.NVNGX_DLSSD_Path.has_value())
        {
            *feature = std::make_unique<DLSSDFeatureDx12>(handleId, parameters);
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
            config_upscaler = "fsr21";
        }
    }
    else
    {
        *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
        config_upscaler = "fsr21";
    }

    bool loaded = (*feature)->ModuleLoaded();

    if (!loaded)
    {
        *feature = std::make_unique<FSR2FeatureDx12_212>(handleId, parameters);
        config_upscaler = "fsr21";
        loaded = true; // Assuming the fallback always loads successfully
    }

    // Handle display name normalization for DLSSD
    if (config_upscaler == "dlssd")
        config_upscaler = "dlss";

    cfg.Dx12Upscaler = std::string(config_upscaler);

    return loaded;
}

bool FeatureProvider_Dx12::ChangeFeature(
    std::string_view upscalerName, 
    ID3D12Device* device,                                   
    ID3D12GraphicsCommandList* cmdList, 
    UINT handleId,
    NVSDK_NGX_Parameter* parameters, 
    ContextData<IFeature_Dx12>* contextData)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    if (!state.changeBackend[handleId])
        return false;

    const bool isDlssBeingEnabled = !cfg.DLSSEnabled.value_or_default() && state.newBackend == "dlss";

    // If no name or if dlss is being enabled use the configured upscaler name
    if (state.newBackend == "" || isDlssBeingEnabled)
        state.newBackend = cfg.Dx12Upscaler.value_or_default();

    contextData->changeBackendCounter++;

    LOG_INFO("changeBackend is true, counter: {0}", contextData->changeBackendCounter);

    // first release everything
    if (contextData->changeBackendCounter == 1)
    {
        if (state.currentFG != nullptr && state.currentFG->IsActive() &&
            state.activeFgInput == FGInput::Upscaler)
        {
            state.currentFG->DestroyFGContext();
            state.FGchanged = true;
            state.ClearCapturedHudlesses = true;
        }

        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {}", state.newBackend);

            auto* dc = contextData->feature.get();
            // Use given params if using DLSS passthrough
            const std::string_view backend = state.newBackend;
            const bool isPassthrough = backend == "dlssd" || backend == "dlss";

            contextData->createParams = isPassthrough ? parameters : GetNGXParameters("OptiDx12", false);
            contextData->createParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dc->GetFeatureFlags());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Width, dc->RenderWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Height, dc->RenderHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutWidth, dc->DisplayWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutHeight, dc->DisplayHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, dc->PerfQualityValue());

            if (state.gameQuirks & GameQuirk::FastFeatureReset)
            {
                LOG_DEBUG("sleeping before reset of current feature for 100ms (Fast Feature Reset)");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else
            {
                LOG_DEBUG("sleeping before reset of current feature for 1000ms");
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }

            contextData->feature.reset();
            contextData->feature = nullptr;

            state.currentFeature = nullptr;
        }
        else // Clean up state if no feature is set
        {
            LOG_ERROR("can't find handle {0} in Dx12Contexts!", handleId);

            state.newBackend = "";
            state.changeBackend[handleId] = false;

            if (contextData->createParams != nullptr)
            {
                TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::D3D12_DestroyParameters());
                contextData->createParams = nullptr;
            }

            contextData->changeBackendCounter = 0;
        }

        return true;
    }

    // create new feature
    if (contextData->changeBackendCounter == 2)
    {
        LOG_INFO("Creating new {} upscaler", state.newBackend);
        contextData->feature.reset();

        if (!GetFeature(state.newBackend, handleId, contextData->createParams, &contextData->feature))
        {
            LOG_ERROR("Upscaler can't created");
            return false;
        }

        return true;
    }

    // init feature
    if (contextData->changeBackendCounter == 3)
    {
        auto initResult = contextData->feature->Init(device, cmdList, contextData->createParams);

        contextData->changeBackendCounter = 0;

        if (!initResult)
        {
            LOG_ERROR("init failed with {0} feature", state.newBackend);

            if (state.newBackend != "dlssd")
            {
                if (cfg.Dx12Upscaler == "dlss")
                    state.newBackend = "xess";
                else
                    state.newBackend = "fsr21";
            }
            else
            {
                // Retry DLSSD
                state.newBackend = "dlssd";
            }

            state.changeBackend[handleId] = true;
            return NVSDK_NGX_Result_Success;
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed", state.newBackend);

            state.newBackend = "";
            state.changeBackend[handleId] = false;
        }

        // If this is an OptiScaler fake NVNGX param table, delete it
        int optiParam = 0;

        if (contextData->createParams->Get("OptiScaler", &optiParam) == NVSDK_NGX_Result_Success && optiParam == 1)
        {
            TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::D3D12_DestroyParameters());
            contextData->createParams = nullptr;
        }
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        state.currentFG->UpdateTarget();

    return true;
}
