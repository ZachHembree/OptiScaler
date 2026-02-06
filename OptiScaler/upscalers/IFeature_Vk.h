#pragma once
#include <vulkan/vulkan.hpp>
#include "IFeature.h"

#include <shaders/rcas/RCAS_Vk.h>
#include <shaders/output_scaling/OS_Vk.h>

class IFeature_Vk : public virtual IFeature
{
  private:
  protected:
    VkInstance Instance = nullptr;
    VkPhysicalDevice PhysicalDevice = nullptr;
    VkDevice Device = nullptr;
    PFN_vkGetInstanceProcAddr GIPA = nullptr;
    PFN_vkGetDeviceProcAddr GDPA = nullptr;

    std::unique_ptr<RCAS_Vk> RCAS;
    std::unique_ptr<OS_Vk> OS;

    IFeature_Vk() {}

  public:
    virtual bool Init(VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice, VkCommandBuffer InCmdList,
                      PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                      NVSDK_NGX_Parameter* InParameters) = 0;
    virtual bool Evaluate(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) = 0;

    virtual ~IFeature_Vk() {}
};
