#include "pch.h"
#include "Util.h"
#include "Config.h"
#include "resource.h"

#include "NVNGX_DLSS.h"
#include "FG/DLSSG_Mod.h"
#include "NVNGX_Parameter.h"
#include "proxies/NVNGX_Proxy.h"

#include "upscalers/FeatureProvider_Vk.h"

#include <upscaler_time/UpscalerTime_Vk.h>

#include <vulkan/vulkan.hpp>
#include <ankerl/unordered_dense.h>

VkInstance vkInstance;
VkPhysicalDevice vkPD;
VkDevice vkDevice;
PFN_vkGetInstanceProcAddr vkGIPA;
PFN_vkGetDeviceProcAddr vkGDPA;

static ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Vk>> VkContexts;
static inline int evalCounter = 0;
static inline bool shutdown = false;
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

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Init_Ext2(
    unsigned long long InApplicationId, const wchar_t* InApplicationDataPath, VkInstance InInstance,
    VkPhysicalDevice InPD, VkDevice InDevice, PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
    NVSDK_NGX_Version InSDKVersion, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_Init_Ext2() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::VULKAN_Init_Ext2");
            auto result = NVNGXProxy::VULKAN_Init_Ext2()(InApplicationId, InApplicationDataPath, InInstance, InPD,
                                                         InDevice, InGIPA, InGDPA, InSDKVersion, InFeatureInfo);
            LOG_INFO("NVNGXProxy::VULKAN_Init_Ext2 result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetVulkanInited(true);
        }
    }

    DLSSGMod::InitDLSSGMod_Vulkan();
    DLSSGMod::VULKAN_Init_Ext2(InApplicationId, InApplicationDataPath, InInstance, InPD, InDevice, InGIPA, InGDPA,
                               InSDKVersion, InFeatureInfo);

    State::Instance().NVNGX_ApplicationId = InApplicationId;
    State::Instance().NVNGX_ApplicationDataPath = std::wstring(InApplicationDataPath);
    State::Instance().NVNGX_Version = InSDKVersion;
    State::Instance().NVNGX_FeatureInfo = InFeatureInfo;
    State::Instance().NVNGX_Version = InSDKVersion;

    State::Instance().NVNGX_FeatureInfo_Paths.clear();

    if (InFeatureInfo != nullptr)
    {
        if (InSDKVersion > 0x0000013)
            State::Instance().NVNGX_Logger = InFeatureInfo->LoggingInfo;

        // Doom Ethernal is sending junk data
        if (InFeatureInfo->PathListInfo.Length < 10)
        {
            for (size_t i = 0; i < InFeatureInfo->PathListInfo.Length; i++)
            {
                const wchar_t* path = InFeatureInfo->PathListInfo.Path[i];
                State::Instance().NVNGX_FeatureInfo_Paths.push_back(std::wstring(path));
            }
        }
    }

    LOG_INFO("InApplicationId: {0}", InApplicationId);
    LOG_INFO("InSDKVersion: {0:x}", (UINT) InSDKVersion);
    std::wstring string(InApplicationDataPath);

    LOG_DEBUG("InApplicationDataPath {0}", wstring_to_string(string));

    if (State::Instance().NVNGX_FeatureInfo_Paths.size() > 0)
    {
        for (size_t i = 0; i < State::Instance().NVNGX_FeatureInfo_Paths.size(); ++i)
        {
            LOG_DEBUG("PathListInfo[{0}]: {1}", i, wstring_to_string(State::Instance().NVNGX_FeatureInfo_Paths[i]));
        }
    }

    if (InInstance)
    {
        LOG_INFO("InInstance exist!");
        vkInstance = InInstance;
    }

    if (InPD)
    {
        LOG_INFO("InPD exist!");
        vkPD = InPD;
    }

    if (InDevice)
    {
        LOG_INFO("InDevice exist!");
        vkDevice = InDevice;
    }

    if (InGDPA)
    {
        LOG_INFO("InGDPA exist!");
        vkGDPA = InGDPA;
    }
    else
    {
        LOG_INFO("InGDPA does not exist!");
        vkGDPA = vkGetDeviceProcAddr;
    }

    if (InGIPA)
    {
        LOG_INFO("InGIPA exist!");
        vkGIPA = InGIPA;
    }
    else
    {
        LOG_INFO("InGIPA does not exist!");
        vkGIPA = vkGetInstanceProcAddr;
    }

    State::Instance().currentVkDevice = InDevice;

    UpscalerTimeVk::Init(InDevice, InPD);

    State::Instance().NvngxVkInited = true;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Init_Ext(unsigned long long InApplicationId,
                                                         const wchar_t* InApplicationDataPath, VkInstance InInstance,
                                                         VkPhysicalDevice InPD, VkDevice InDevice,
                                                         NVSDK_NGX_Version InSDKVersion,
                                                         const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InApplicationId = app_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_Init_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::VULKAN_Init_Ext");
            auto result = NVNGXProxy::VULKAN_Init_Ext()(InApplicationId, InApplicationDataPath, InInstance, InPD,
                                                        InDevice, InSDKVersion, InFeatureInfo);
            LOG_INFO("NVNGXProxy::VULKAN_Init_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetVulkanInited(true);
        }
    }

    DLSSGMod::InitDLSSGMod_Vulkan();
    DLSSGMod::VULKAN_Init_Ext(InApplicationId, InApplicationDataPath, InInstance, InPD, InDevice, InSDKVersion,
                              InFeatureInfo);

    ScopedInit scopedInit {};
    return NVSDK_NGX_VULKAN_Init_Ext2(InApplicationId, InApplicationDataPath, InInstance, InPD, InDevice,
                                      vkGetInstanceProcAddr, vkGetDeviceProcAddr, InSDKVersion, InFeatureInfo);
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Init_ProjectID_Ext(
    const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
    const wchar_t* InApplicationDataPath, VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice,
    PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA, NVSDK_NGX_Version InSDKVersion,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_Init_ProjectID_Ext() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::VULKAN_Init_ProjectID_Ext");
            auto result = NVNGXProxy::VULKAN_Init_ProjectID_Ext()(InProjectId, InEngineType, InEngineVersion,
                                                                  InApplicationDataPath, InInstance, InPD, InDevice,
                                                                  InGIPA, InGDPA, InSDKVersion, InFeatureInfo);
            LOG_INFO("NVNGXProxy::VULKAN_Init_ProjectID_Ext result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetVulkanInited(true);
        }
    }

    ScopedInit scopedInit {};
    auto result = NVSDK_NGX_VULKAN_Init_Ext2(0x1337, InApplicationDataPath, InInstance, InPD, InDevice, InGIPA, InGDPA,
                                             InSDKVersion, InFeatureInfo);

    LOG_DEBUG("InProjectId: {0}", InProjectId);
    LOG_DEBUG("InEngineType: {0}", (int) InEngineType);
    LOG_DEBUG("InEngineVersion: {0}", InEngineVersion);

    State::Instance().NVNGX_ProjectId = std::string(InProjectId);
    State::Instance().NVNGX_Engine = InEngineType;
    State::Instance().NVNGX_EngineVersion = std::string(InEngineVersion);

    return result;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Init(unsigned long long InApplicationId,
                                                     const wchar_t* InApplicationDataPath, VkInstance InInstance,
                                                     VkPhysicalDevice InPD, VkDevice InDevice,
                                                     PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
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

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_Init() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::VULKAN_Init");
            auto result = NVNGXProxy::VULKAN_Init()(InApplicationId, InApplicationDataPath, InInstance, InPD, InDevice,
                                                    InGIPA, InGDPA, InFeatureInfo, InSDKVersion);
            LOG_INFO("NVNGXProxy::VULKAN_Init result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetVulkanInited(true);
        }
    }

    DLSSGMod::InitDLSSGMod_Vulkan();
    DLSSGMod::VULKAN_Init(InApplicationId, InApplicationDataPath, InInstance, InPD, InDevice, InGIPA, InGDPA,
                          InFeatureInfo, InSDKVersion);

    ScopedInit scopedInit {};
    return NVSDK_NGX_VULKAN_Init_Ext2(InApplicationId, InApplicationDataPath, InInstance, InPD, InDevice, InGIPA,
                                      InGDPA, InSDKVersion, InFeatureInfo);
}

NVSDK_NGX_API NVSDK_NGX_Result
NVSDK_NGX_VULKAN_Init_ProjectID(const char* InProjectId, NVSDK_NGX_EngineType InEngineType, const char* InEngineVersion,
                                const wchar_t* InApplicationDataPath, VkInstance InInstance, VkPhysicalDevice InPD,
                                VkDevice InDevice, PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                                NVSDK_NGX_Version InSDKVersion, const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && !_skipInit)
    {
        if (Config::Instance()->UseGenericAppIdWithDlss.value_or_default())
            InProjectId = project_id_override;

        if (NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_Init_ProjectID() != nullptr)
        {
            LOG_INFO("calling NVNGXProxy::VULKAN_Init_ProjectID");
            auto result = NVNGXProxy::VULKAN_Init_ProjectID()(InProjectId, InEngineType, InEngineVersion,
                                                              InApplicationDataPath, InInstance, InPD, InDevice, InGIPA,
                                                              InGDPA, InSDKVersion, InFeatureInfo);
            LOG_INFO("NVNGXProxy::VULKAN_Init_ProjectID result: {0:X}", (UINT) result);

            if (result == NVSDK_NGX_Result_Success)
                NVNGXProxy::SetVulkanInited(true);
        }
    }

    ScopedInit scopedInit {};
    return NVSDK_NGX_VULKAN_Init_ProjectID_Ext(InProjectId, InEngineType, InEngineVersion, InApplicationDataPath,
                                               InInstance, InPD, InDevice, InGIPA, InGDPA, InSDKVersion, InFeatureInfo);
}

/**
 * @brief [Deprecated NGX API] Superceeded by NVSDK_NGX_AllocateParameters and NVSDK_NGX_GetCapabilityParameters.
 *
 * Retrieves a common NVSDK parameter map for providing params to the SDK. The lifetime of this
 * map is NOT managed by the application. It is expected to be managed internally by the SDK.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::VULKAN_GetParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::VULKAN_GetParameters");
        auto result = NVNGXProxy::VULKAN_GetParameters()(OutParameters);
        LOG_INFO("NVNGXProxy::VULKAN_GetParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            InitNGXParameters(*OutParameters);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVPersistent);
            return result;
        }
    }

    // Get custom parameters if using custom backend
    static NVNGX_Parameters oldParams = NVNGX_Parameters(OptiKeys::VkProvider, true);
    InitNGXParameters(*OutParameters);
    *OutParameters = &oldParams;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
    const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo, uint32_t* OutExtensionCount,
    VkExtensionProperties** OutExtensionProperties)
{
    LOG_DEBUG("FeatureID: {0}", (UINT) FeatureDiscoveryInfo->FeatureID);

    if (OutExtensionProperties == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::VULKAN_GetFeatureInstanceExtensionRequirements() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::VULKAN_GetFeatureInstanceExtensionRequirements");
        auto result = NVNGXProxy::VULKAN_GetFeatureInstanceExtensionRequirements()(
            FeatureDiscoveryInfo, OutExtensionCount, OutExtensionProperties);
        LOG_INFO("NVNGXProxy::VULKAN_GetFeatureInstanceExtensionRequirements result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            if (*OutExtensionCount > 0)
                LOG_DEBUG("required extensions: {0}", *OutExtensionCount);

            return result;
        }
    }

    if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_GetFeatureInstanceExtensionRequirements() != nullptr)
    {
        LOG_INFO("returning original needed extensions");
        return NVNGXProxy::VULKAN_GetFeatureInstanceExtensionRequirements()(FeatureDiscoveryInfo, OutExtensionCount,
                                                                            OutExtensionProperties);
    }
    else
    {
        LOG_DEBUG("OutExtensionCount != nullptr: {}", OutExtensionCount != nullptr);

        if ((FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling ||
             FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_FrameGeneration) &&
            OutExtensionCount != nullptr)
        {
            if (OutExtensionProperties == nullptr)
            {
                if (Config::Instance()->VulkanExtensionSpoofing.value_or_default())
                {
                    LOG_INFO("returning 3 extensions are needed");
                    *OutExtensionCount = 3;
                }
                else
                {
                    LOG_INFO("returning no extensions are needed");
                    *OutExtensionCount = 0;
                }
            }
            else if (*OutExtensionCount == 3 && Config::Instance()->VulkanExtensionSpoofing.value_or_default())
            {
                LOG_INFO("returning extension infos");

                std::memset((*OutExtensionProperties)[0].extensionName, 0,
                            sizeof(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[0].extensionName,
                            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
                (*OutExtensionProperties)[0].specVersion = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION;

                std::memset((*OutExtensionProperties)[1].extensionName, 0,
                            sizeof(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[1].extensionName,
                            VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
                (*OutExtensionProperties)[1].specVersion = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_SPEC_VERSION;

                std::memset((*OutExtensionProperties)[2].extensionName, 0,
                            sizeof(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[2].extensionName,
                            VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
                (*OutExtensionProperties)[2].specVersion = VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_SPEC_VERSION;
            }

            return NVSDK_NGX_Result_Success;
        }
    }

    LOG_INFO("returning no extensions are needed");

    if (OutExtensionCount != nullptr)
        *OutExtensionCount = 0;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
    VkInstance Instance, VkPhysicalDevice PhysicalDevice, const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo,
    uint32_t* OutExtensionCount, VkExtensionProperties** OutExtensionProperties)
{
    LOG_DEBUG("FeatureID: {0}", (UINT) FeatureDiscoveryInfo->FeatureID);

    if (OutExtensionProperties == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::VULKAN_GetFeatureDeviceExtensionRequirements() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::VULKAN_GetFeatureDeviceExtensionRequirements");
        auto result = NVNGXProxy::VULKAN_GetFeatureDeviceExtensionRequirements()(
            Instance, PhysicalDevice, FeatureDiscoveryInfo, OutExtensionCount, OutExtensionProperties);
        LOG_INFO("NVNGXProxy::VULKAN_GetFeatureDeviceExtensionRequirements result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            if (*OutExtensionCount > 0)
                LOG_DEBUG("required extensions: {0}", *OutExtensionCount);

            return result;
        }
    }

    if (NVNGXProxy::NVNGXModule() != nullptr && NVNGXProxy::VULKAN_GetFeatureDeviceExtensionRequirements() != nullptr)
    {
        LOG_INFO("returning original needed extensions");
        return NVNGXProxy::VULKAN_GetFeatureDeviceExtensionRequirements()(
            Instance, PhysicalDevice, FeatureDiscoveryInfo, OutExtensionCount, OutExtensionProperties);
    }
    else
    {
        LOG_DEBUG("OutExtensionCount != nullptr: {}", OutExtensionCount != nullptr);

        if ((FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling ||
             FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_FrameGeneration) &&
            OutExtensionCount != nullptr)
        {
            if (OutExtensionProperties == nullptr)
            {
                if (Config::Instance()->VulkanExtensionSpoofing.value_or_default())
                {
                    LOG_INFO("returning 4 extensions are needed!");
                    *OutExtensionCount = 4;
                }
                else
                {
                    LOG_INFO("returning no extensions are needed!");
                    *OutExtensionCount = 0;
                }
            }
            else if (*OutExtensionCount == 4 && Config::Instance()->VulkanExtensionSpoofing.value_or_default())
            {
                LOG_INFO("returning extension infos");

                std::memset((*OutExtensionProperties)[0].extensionName, 0, sizeof(VK_NVX_BINARY_IMPORT_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[0].extensionName, VK_NVX_BINARY_IMPORT_EXTENSION_NAME);
                (*OutExtensionProperties)[0].specVersion = VK_NVX_BINARY_IMPORT_SPEC_VERSION;

                std::memset((*OutExtensionProperties)[1].extensionName, 0,
                            sizeof(VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[1].extensionName, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME);
                (*OutExtensionProperties)[1].specVersion = VK_NVX_IMAGE_VIEW_HANDLE_SPEC_VERSION;

                std::memset((*OutExtensionProperties)[2].extensionName, 0,
                            sizeof(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[2].extensionName, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
                (*OutExtensionProperties)[2].specVersion = VK_EXT_BUFFER_DEVICE_ADDRESS_SPEC_VERSION;

                std::memset((*OutExtensionProperties)[3].extensionName, 0,
                            sizeof(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));
                std::strcpy((*OutExtensionProperties)[3].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
                (*OutExtensionProperties)[3].specVersion = VK_KHR_PUSH_DESCRIPTOR_SPEC_VERSION;
            }

            return NVSDK_NGX_Result_Success;
        }
    }

    LOG_INFO("returning no extensions are needed!");

    if (OutExtensionCount != nullptr)
        *OutExtensionCount = 0;

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Allocates a new parameter map used to provide parameters needed by the DLSS API. The lifetime of this map
 * is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_AllocateParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::VULKAN_AllocateParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::VULKAN_AllocateParameters");
        auto result = NVNGXProxy::VULKAN_AllocateParameters()(OutParameters);
        LOG_INFO("NVNGXProxy::VULKAN_AllocateParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    auto* params = new NVNGX_Parameters(OptiKeys::VkProvider, false);
    *OutParameters = params;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetFeatureRequirements(
    VkInstance VulkanInstance, VkPhysicalDevice PhysicalDevice,
    const NVSDK_NGX_FeatureDiscoveryInfo* FeatureDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported)
{
    LOG_DEBUG("for FeatureID: {0}", (int) FeatureDiscoveryInfo->FeatureID);

    DLSSGMod::InitDLSSGMod_Vulkan();

    if (FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_SuperSampling ||
        (DLSSGMod::isVulkanAvailable() && FeatureDiscoveryInfo->FeatureID == NVSDK_NGX_Feature_FrameGeneration))
    {
        if (OutSupported == nullptr)
            OutSupported = new NVSDK_NGX_FeatureRequirement();

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
        LOG_DEBUG("calling NVNGXProxy::VULKAN_GetFeatureRequirements");
        auto result = NVNGXProxy::VULKAN_GetFeatureRequirements()(VulkanInstance, PhysicalDevice, FeatureDiscoveryInfo,
                                                                  OutSupported);
        LOG_DEBUG("NVNGXProxy::VULKAN_GetFeatureRequirements result {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
            LOG_DEBUG("FeatureSupported: {0}", (UINT) OutSupported->FeatureSupported);

        return result;
    }
    else
    {
        LOG_DEBUG("VULKAN_GetFeatureRequirements not available for FeatureID: {0}",
                  (int) FeatureDiscoveryInfo->FeatureID);
    }

    OutSupported->FeatureSupported = NVSDK_NGX_FeatureSupportResult_AdapterUnsupported;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

/**
 * @brief Allocates a new NVSDK parameter map pre-populated with NGX capabilities and information about available
 * features. The output parameter map may also be used in the same ways as a parameter map allocated with
 * AllocateParameters(). The lifetime of this map is managed by the calling application with DestroyParameters().
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetCapabilityParameters(NVSDK_NGX_Parameter** OutParameters)
{
    LOG_FUNC();

    if (OutParameters == nullptr)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::NVNGXModule() != nullptr &&
        NVNGXProxy::IsVulkanInited() && NVNGXProxy::VULKAN_GetCapabilityParameters() != nullptr)
    {
        LOG_INFO("calling NVNGXProxy::VULKAN_GetCapabilityParameters");
        auto result = NVNGXProxy::VULKAN_GetCapabilityParameters()(OutParameters);
        LOG_INFO("calling NVNGXProxy::VULKAN_GetCapabilityParameters result: {0:X}", (UINT) result);

        if (result == NVSDK_NGX_Result_Success)
        {
            // Init external NGX table with current configuration and mark as dynamic+external
            InitNGXParameters(*OutParameters);
            SetNGXParamAllocType(*(*OutParameters), NGX_AllocTypes::NVDynamic);
            return result;
        }
    }

    // Get custom parameters if using custom backend
    auto& params = *(new NVNGX_Parameters(OptiKeys::VkProvider, false));
    InitNGXParameters(&params);
    *OutParameters = &params;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_PopulateParameters_Impl(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    InitNGXParameters(InParameters);

    DLSSGMod::VULKAN_PopulateParameters_Impl(InParameters);

    return NVSDK_NGX_Result_Success;
}

/**
 * @brief Destroys a given input parameter map created with AllocateParameters or GetCapabilityParameters.
 Must not be called on maps returned by GetParameters(). Unsupported tables will not be freed.
 */
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_DestroyParameters(NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (InParameters == nullptr)
        return NVSDK_NGX_Result_Fail;

    const bool success = TryDestroyNGXParameters(InParameters, NVNGXProxy::VULKAN_DestroyParameters());

    return success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetScratchBufferSize(NVSDK_NGX_Feature InFeatureId,
                                                                     const NVSDK_NGX_Parameter* InParameters,
                                                                     size_t* OutSizeInBytes)
{
    if (DLSSGMod::isVulkanAvailable() && InFeatureId == NVSDK_NGX_Feature_FrameGeneration)
    {
        return DLSSGMod::VULKAN_GetScratchBufferSize(InFeatureId, InParameters, OutSizeInBytes);
    }

    LOG_WARN("-> 52428800");

    *OutSizeInBytes = 52428800;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_CreateFeature1(VkDevice InDevice, VkCommandBuffer InCmdList,
                                                               NVSDK_NGX_Feature InFeatureID,
                                                               NVSDK_NGX_Parameter* InParameters,
                                                               NVSDK_NGX_Handle** OutHandle)
{
    if (DLSSGMod::isVulkanAvailable() && InFeatureID == NVSDK_NGX_Feature_FrameGeneration)
    {
        auto result = DLSSGMod::VULKAN_CreateFeature1(InDevice, InCmdList, InFeatureID, InParameters, OutHandle);
        LOG_INFO("Creating new modded DLSSG feature with HandleId: {0}", (*OutHandle)->Id);
        return result;
    }
    else if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() &&
            NVNGXProxy::InitVulkan(vkInstance, vkPD, vkDevice, vkGIPA, vkGDPA) &&
            NVNGXProxy::VULKAN_CreateFeature1() != nullptr)
        {
            auto result =
                NVNGXProxy::VULKAN_CreateFeature1()(InDevice, InCmdList, InFeatureID, InParameters, OutHandle);
            LOG_INFO("VULKAN_CreateFeature1 result for ({0}): {1:X}", (int) InFeatureID, (UINT) result);
            return result;
        }
        else
        {
            LOG_ERROR("Can't create this feature ({0})!", (int) InFeatureID);
            return NVSDK_NGX_Result_Fail;
        }
    }

    // Create feature
    auto handleId = IFeature::GetNextHandleId();
    LOG_INFO("HandleId: {0}", handleId);

    if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)
    {
        std::string_view upscalerChoice = OptiKeys::FSR22; // Default XeSS

        // If original NVNGX available use DLSS as base upscaler
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsVulkanInited())
            upscalerChoice = OptiKeys::DLSS;

        if (Config::Instance()->VulkanUpscaler.has_value())
            upscalerChoice = Config::Instance()->VulkanUpscaler.value();

        LOG_INFO("Creating new {} upscaler", upscalerChoice);

        VkContexts[handleId] = {};

        if (!FeatureProvider_Vk::GetFeature(upscalerChoice, handleId, InParameters, &VkContexts[handleId].feature))
        {
            LOG_ERROR("Upscaler can't created");
            return NVSDK_NGX_Result_Fail;
        }
    }
    else if (InFeatureID == NVSDK_NGX_Feature_RayReconstruction)
    {
        LOG_INFO("creating new DLSSD feature");

        VkContexts[handleId] = {};

        if (!FeatureProvider_Vk::GetFeature(OptiKeys::DLSSD, handleId, InParameters, &VkContexts[handleId].feature))
        {
            LOG_ERROR("DLSSD can't created");
            return NVSDK_NGX_Result_Fail;
        }
    }

    State::Instance().api = Vulkan;
    auto deviceContext = VkContexts[handleId].feature.get();
    *OutHandle = deviceContext->Handle();

    State::Instance().AutoExposure.reset();

    {
        ScopedSkipSpoofing skipSpoofing;
        if (deviceContext->Init(vkInstance, vkPD, InDevice, InCmdList, vkGIPA, vkGDPA, InParameters))
        {
            State::Instance().currentFeature = deviceContext;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            evalCounter = 0;

            return NVSDK_NGX_Result_Success;
        }
    }

    LOG_ERROR("CreateFeature failed");
    return NVSDK_NGX_Result_FAIL_PlatformError;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_CreateFeature(VkCommandBuffer InCmdBuffer,
                                                              NVSDK_NGX_Feature InFeatureID,
                                                              NVSDK_NGX_Parameter* InParameters,
                                                              NVSDK_NGX_Handle** OutHandle)
{
    LOG_FUNC();

    if (DLSSGMod::isVulkanAvailable() && InFeatureID == NVSDK_NGX_Feature_FrameGeneration)
    {
        auto result = DLSSGMod::VULKAN_CreateFeature(InCmdBuffer, InFeatureID, InParameters, OutHandle);
        LOG_INFO("Creating new modded DLSSG feature with HandleId: {0}", (*OutHandle)->Id);
        return result;
    }
    else if (InFeatureID != NVSDK_NGX_Feature_SuperSampling && InFeatureID != NVSDK_NGX_Feature_RayReconstruction)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() &&
            NVNGXProxy::InitVulkan(vkInstance, vkPD, vkDevice, vkGIPA, vkGDPA) &&
            NVNGXProxy::VULKAN_CreateFeature() != nullptr)
        {
            auto result = NVNGXProxy::VULKAN_CreateFeature()(InCmdBuffer, InFeatureID, InParameters, OutHandle);
            LOG_INFO("VULKAN_CreateFeature result for ({0}): {1:X}", (int) InFeatureID, (UINT) result);
            return result;
        }
    }

    return NVSDK_NGX_VULKAN_CreateFeature1(vkDevice, InCmdBuffer, InFeatureID, InParameters, OutHandle);
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle* InHandle)
{
    if (!InHandle)
        return NVSDK_NGX_Result_Success;

    auto handleId = InHandle->Id;
    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::VULKAN_ReleaseFeature() != nullptr)
        {
            auto result = NVNGXProxy::VULKAN_ReleaseFeature()(InHandle);

            if (!shutdown)
                LOG_INFO("VULKAN_ReleaseFeature result for ({0}): {1:X}", handleId, (UINT) result);

            return result;
        }
        else
        {
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }
    else if (handleId >= DLSSG_MOD_ID_OFFSET)
    {
        LOG_INFO("VULKAN_ReleaseFeature modded DLSSG with HandleId: {0}", handleId);
        return DLSSGMod::VULKAN_ReleaseFeature(InHandle);
    }

    if (!shutdown)
        LOG_INFO("releasing feature with id {0}", handleId);

    if (auto deviceContext = VkContexts[handleId].feature.get(); deviceContext)
    {
        if (deviceContext == State::Instance().currentFeature)
            State::Instance().currentFeature = nullptr;

        vkDeviceWaitIdle(vkDevice);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        VkContexts[handleId].feature.reset();
        auto it = std::find_if(VkContexts.begin(), VkContexts.end(),
                               [&handleId](const auto& p) { return p.first == handleId; });
        VkContexts.erase(it);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_EvaluateFeature(VkCommandBuffer InCmdList,
                                                                const NVSDK_NGX_Handle* InFeatureHandle,
                                                                NVSDK_NGX_Parameter* InParameters,
                                                                PFN_NVSDK_NGX_ProgressCallback InCallback)
{
    if (InFeatureHandle == nullptr)
    {
        LOG_DEBUG("InFeatureHandle is null");
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    else
    {
        LOG_DEBUG("Handle: {0}", InFeatureHandle->Id);
    }

    if (InCmdList == nullptr)
    {
        LOG_ERROR("InCmdList is null!!!");
        return NVSDK_NGX_Result_Fail;
    }

    auto handleId = InFeatureHandle->Id;
    if (VkContexts[handleId].feature == nullptr) // prevent source api name flicker when dlssg is active
        State::Instance().setInputApiName = State::Instance().currentInputApiName;

    if (State::Instance().setInputApiName.length() == 0)
    {
        if (std::strcmp(State::Instance().currentInputApiName.c_str(), "DLSS") != 0)
        {
            State::Instance().AutoExposure.reset();
            State::Instance().currentInputApiName = "DLSS";
        }
    }
    else
    {
        if (std::strcmp(State::Instance().currentInputApiName.c_str(), State::Instance().setInputApiName.c_str()) != 0)
        {
            State::Instance().AutoExposure.reset();
            State::Instance().currentInputApiName = State::Instance().setInputApiName;
        }
    }

    State::Instance().setInputApiName.clear();

    if (handleId < DLSS_MOD_ID_OFFSET)
    {
        if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::VULKAN_EvaluateFeature() != nullptr)
        {
            LOG_DEBUG("VULKAN_EvaluateFeature for ({0})", handleId);
            auto result = NVNGXProxy::VULKAN_EvaluateFeature()(InCmdList, InFeatureHandle, InParameters, InCallback);
            LOG_INFO("VULKAN_EvaluateFeature result for ({0}): {1:X}", handleId, (UINT) result);
            return result;
        }
        else
        {
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        }
    }
    else if (handleId >= DLSSG_MOD_ID_OFFSET)
    {
        return DLSSGMod::VULKAN_EvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
    }

    evalCounter++;
    if (Config::Instance()->SkipFirstFrames.has_value() && evalCounter < Config::Instance()->SkipFirstFrames.value())
        return NVSDK_NGX_Result_Success;

    if (InCallback)
        LOG_WARN("callback exist");

    IFeature_Vk* deviceContext = nullptr;
    auto contextData = &VkContexts[handleId];

    if (State::Instance().changeBackend[handleId])
    {
        FeatureProvider_Vk::ChangeFeature(State::Instance().newBackend, vkInstance, vkPD, vkDevice, InCmdList, vkGIPA,
                                          vkGDPA, handleId, InParameters, contextData);
        evalCounter = 0;

        return NVSDK_NGX_Result_Success;
    }

    deviceContext = VkContexts[handleId].feature.get();
    State::Instance().currentFeature = deviceContext;

    if (!deviceContext->IsInited() && Config::Instance()->VulkanUpscaler.value_or_default() != OptiKeys::FSR22)
    {
        State::Instance().newBackend = OptiKeys::FSR22;
        State::Instance().changeBackend[handleId] = true;
        return NVSDK_NGX_Result_Success;
    }

    UpscalerTimeVk::UpscaleStart(InCmdList);

    auto upscaleResult = deviceContext->Evaluate(InCmdList, InParameters);

    UpscalerTimeVk::UpscaleEnd(InCmdList);

    return upscaleResult ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Shutdown(void)
{
    shutdown = true;

    // for (auto const& [key, val] : VkContexts) {
    //     if (val.feature)
    //         NVSDK_NGX_VULKAN_ReleaseFeature(val.feature->Handle());
    // }

    // VkContexts.clear();

    vkInstance = nullptr;
    vkPD = nullptr;
    vkDevice = nullptr;

    State::Instance().currentFeature = nullptr;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsVulkanInited() &&
        NVNGXProxy::VULKAN_Shutdown() != nullptr)
    {
        auto result = NVNGXProxy::VULKAN_Shutdown()();
        NVNGXProxy::SetVulkanInited(false);
    }

    // Unhooking and cleaning stuff causing issues during shutdown.
    // Disabled for now to check if it cause any issues
    // MenuOverlayVk::UnHookVk();

    DLSSGMod::VULKAN_Shutdown();

    shutdown = false;
    State::Instance().NvngxVkInited = false;

    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Shutdown1(VkDevice InDevice)
{
    shutdown = true;

    if (Config::Instance()->DLSSEnabled.value_or_default() && NVNGXProxy::IsVulkanInited() &&
        NVNGXProxy::VULKAN_Shutdown1() != nullptr)
    {
        auto result = NVNGXProxy::VULKAN_Shutdown1()(InDevice);
        NVNGXProxy::SetVulkanInited(false);
    }

    DLSSGMod::VULKAN_Shutdown1(InDevice);

    shutdown = false;

    return NVSDK_NGX_VULKAN_Shutdown();
}
