#include <pch.h>
#include "FeatureProvider_Dx11.h"

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"

#include "upscalers/dlss/DLSSFeature_Dx11.h"
#include "upscalers/dlssd/DLSSDFeature_Dx11.h"
#include "upscalers/fsr2/FSR2Feature_Dx11.h"
#include "upscalers/fsr2/FSR2Feature_Dx11On12.h"
#include "upscalers/fsr2_212/FSR2Feature_Dx11On12_212.h"
#include "upscalers/fsr31/FSR31Feature_Dx11.h"
#include "upscalers/fsr31/FSR31Feature_Dx11On12.h"
#include "upscalers/xess/XeSSFeature_Dx11.h"
#include "upscalers/xess/XeSSFeature_Dx11on12.h"

bool FeatureProvider_Dx11::GetFeature(std::string_view upscalerName, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                      std::unique_ptr<IFeature_Dx11>* feature)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    do
    {
        if (upscalerName == OptiKeys::XeSS)
        {
            *feature = std::make_unique<XeSSFeature_Dx11>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::XeSS_11on12)
        {
            *feature = std::make_unique<XeSSFeatureDx11on12>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR21_11on12)
        {
            *feature = std::make_unique<FSR2FeatureDx11on12_212>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR22)
        {
            *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR22_11on12)
        {
            *feature = std::make_unique<FSR2FeatureDx11on12>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR31)
        {
            *feature = std::make_unique<FSR31FeatureDx11>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR31_11on12)
        {
            *feature = std::make_unique<FSR31FeatureDx11on12>(handleId, parameters);
            break;
        }

        if (cfg.DLSSEnabled.value_or_default())
        {
            if (upscalerName == OptiKeys::DLSS && state.NVNGX_DLSS_Path.has_value())
            {
                *feature = std::make_unique<DLSSFeatureDx11>(handleId, parameters);
                break;
            }
            else if (upscalerName == OptiKeys::DLSSD && state.NVNGX_DLSSD_Path.has_value())
            {
                *feature = std::make_unique<DLSSDFeatureDx11>(handleId, parameters);
                break;
            }
            else
            {
                *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
            }
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
        }

    } while (false);

    if (!(*feature)->ModuleLoaded())
    {
        (*feature).reset();
        *feature = std::make_unique<FSR2FeatureDx11>(handleId, parameters);
        upscalerName = OptiKeys::FSR22;
    }
    else
    {
        cfg.Dx11Upscaler = (std::string)upscalerName;
    }

    auto result = (*feature)->ModuleLoaded();

    if (result)
    {
        if (upscalerName == OptiKeys::DLSSD)
            upscalerName = OptiKeys::DLSS;

        cfg.Dx11Upscaler = (std::string)upscalerName;
    }

    return result;
}

bool FeatureProvider_Dx11::ChangeFeature(
    std::string_view upscalerName, 
    ID3D11Device* device,
    ID3D11DeviceContext* devContext, 
    UINT handleId,
    NVSDK_NGX_Parameter* parameters, 
    ContextData<IFeature_Dx11>* contextData)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    if (state.newBackend == "" ||  (!cfg.DLSSEnabled.value_or_default() && state.newBackend == OptiKeys::DLSS))
        state.newBackend = cfg.Dx11Upscaler.value_or_default();

    contextData->changeBackendCounter++;

    // first release everything
    if (contextData->changeBackendCounter == 1)
    {
        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {0}", state.newBackend);

            auto* dc = contextData->feature.get();
            // Use given params if using DLSS passthrough
            const std::string_view backend = state.newBackend;
            const bool isPassthrough = backend == OptiKeys::DLSSD || backend == OptiKeys::DLSS;

            contextData->createParams = isPassthrough ? parameters : GetNGXParameters(OptiKeys::Dx11Provider, false);
            contextData->createParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dc->GetFeatureFlags());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Width, dc->RenderWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Height, dc->RenderHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutWidth, dc->DisplayWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutHeight, dc->DisplayHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, dc->PerfQualityValue());

            LOG_TRACE("sleeping before reset of current feature for 1000ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            contextData->feature.reset();
            contextData->feature = nullptr;

            state.currentFeature = nullptr;
        }
        else
        {
            LOG_ERROR("can't find handle {0} in Dx11Contexts!", handleId);

            state.newBackend = "";
            state.changeBackend[handleId] = false;

            if (contextData->createParams != nullptr)
            {
                TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::D3D11_DestroyParameters());
                contextData->createParams = nullptr;
            }

            contextData->changeBackendCounter = 0;
        }

        return true;
    }

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

    if (contextData->changeBackendCounter == 3)
    {
        // then init and continue
        auto initResult = contextData->feature->Init(device, devContext, contextData->createParams);

        if (cfg.Dx11DelayedInit.value_or_default())
        {
            LOG_TRACE("sleeping after new Init of new feature for 1000ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        contextData->changeBackendCounter = 0;

        if (!initResult || !contextData->feature->ModuleLoaded())
        {
            LOG_ERROR("init failed with {0} feature", state.newBackend);

            if (state.newBackend != OptiKeys::DLSSD)
            {
                state.newBackend = OptiKeys::FSR22;
                state.changeBackend[handleId] = true;
            }
            else
            {
                state.newBackend = "";
                state.changeBackend[handleId] = false;
                return false;
            }
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed", state.newBackend);

            state.newBackend = "";
            state.changeBackend[handleId] = false;
        }

        // if opti nvparam release it
        int optiParam = 0;

        if (contextData->createParams->Get(OptiKeys::ProjectID.data(), &optiParam) == NVSDK_NGX_Result_Success &&
            optiParam == 1)
        {
            TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::D3D11_DestroyParameters());
            contextData->createParams = nullptr;
        }
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    return true;
}
