#include <pch.h>
#include "FeatureProvider_Vk.h"

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"

#include "upscalers/fsr2/FSR2Feature_Vk.h"
#include "upscalers/dlss/DLSSFeature_Vk.h"
#include "upscalers/dlssd/DLSSDFeature_Vk.h"
#include "upscalers/fsr2_212/FSR2Feature_Vk_212.h"
#include "upscalers/fsr31/FSR31Feature_Vk.h"
#include "upscalers/xess/XeSSFeature_Vk.h"

bool FeatureProvider_Vk::GetFeature(std::string_view upscalerName, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                    std::unique_ptr<IFeature_Vk>* feature)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    do
    {
        if (upscalerName == OptiKeys::XeSS)
        {
            *feature = std::make_unique<XeSSFeature_Vk>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR21)
        {
            *feature = std::make_unique<FSR2FeatureVk212>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR22)
        {
            *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
            break;
        }
        else if (upscalerName == OptiKeys::FSR31)
        {
            *feature = std::make_unique<FSR31FeatureVk>(handleId, parameters);
            break;
        }

        if (cfg.DLSSEnabled.value_or_default())
        {
            if (upscalerName == OptiKeys::DLSS && state.NVNGX_DLSS_Path.has_value())
            {
                *feature = std::make_unique<DLSSFeatureVk>(handleId, parameters);
                break;
            }
            else if (upscalerName == OptiKeys::DLSSD && state.NVNGX_DLSSD_Path.has_value())
            {
                *feature = std::make_unique<DLSSDFeatureVk>(handleId, parameters);
                break;
            }
            else
            {
                *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
            }
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        }

    } while (false);

    if (!(*feature)->ModuleLoaded())
    {
        (*feature).reset();
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        upscalerName = OptiKeys::FSR22;
    }
    else
    {
        cfg.VulkanUpscaler = (std::string)upscalerName;
    }

    auto result = (*feature)->ModuleLoaded();

    if (result)
    {
        if (upscalerName == OptiKeys::DLSSD)
            upscalerName = OptiKeys::DLSS;

        cfg.VulkanUpscaler = (std::string)upscalerName;
    }

    return result;
}

bool FeatureProvider_Vk::ChangeFeature(std::string_view upscalerName, VkInstance instance, VkPhysicalDevice pd,
                                       VkDevice device, VkCommandBuffer cmdBuffer, PFN_vkGetInstanceProcAddr gipa,
                                       PFN_vkGetDeviceProcAddr gdpa, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                       ContextData<IFeature_Vk>* contextData)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    if (state.newBackend == "" ||
        (!cfg.DLSSEnabled.value_or_default() && state.newBackend == OptiKeys::DLSS))
        state.newBackend = cfg.VulkanUpscaler.value_or_default();

    contextData->changeBackendCounter++;

    LOG_INFO("changeBackend is true, counter: {0}", contextData->changeBackendCounter);

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

            contextData->createParams = isPassthrough ? parameters : GetNGXParameters(OptiKeys::VkProvider, false);
            contextData->createParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dc->GetFeatureFlags());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Width, dc->RenderWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Height, dc->RenderHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutWidth, dc->DisplayWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutHeight, dc->DisplayHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, dc->PerfQualityValue());

            LOG_DEBUG("sleeping before reset of current feature for 1000ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            contextData->feature.reset();
            contextData->feature = nullptr;

            state.currentFeature = nullptr;
        }
        else
        {
            LOG_ERROR("can't find handle {0} in VkContexts!", handleId);

            state.newBackend = "";
            state.changeBackend[handleId] = false;

            if (contextData->createParams != nullptr)
            {
                TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::VULKAN_DestroyParameters());
                contextData->createParams = nullptr;
            }

            contextData->changeBackendCounter = 0;
        }

        return NVSDK_NGX_Result_Success;
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
        // next frame create context
        auto initResult = false;
        {
            ScopedSkipSpoofing skipSpoofing;
            initResult =
                contextData->feature->Init(instance, pd, device, cmdBuffer, gipa, gdpa, contextData->createParams);
        }

        contextData->changeBackendCounter = 0;

        if (!initResult || !contextData->feature->ModuleLoaded())
        {
            LOG_ERROR("init failed with {0} feature", state.newBackend);

            if (state.newBackend != OptiKeys::DLSSD)
            {
                if (cfg.VulkanUpscaler == OptiKeys::DLSS)
                {
                    state.newBackend = OptiKeys::XeSS;
                }
                else
                {
                    state.newBackend = OptiKeys::FSR21;
                }
            }
            else
            {
                // Retry DLSSD
                state.newBackend = OptiKeys::DLSSD;
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

        if (contextData->createParams->Get(OptiKeys::ProjectID, &optiParam) == NVSDK_NGX_Result_Success && optiParam == 1)
        {
            TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::VULKAN_DestroyParameters());
            contextData->createParams = nullptr;
        }
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    return true;
}
