#pragma once
#include "SysUtils.h"
#include "IFeature_Dx12.h"
#include <inputs/NVNGX_DLSS.h>

class FeatureProvider_Dx12
{
  public:

    static bool GetFeature(
        std::string_view upscalerName, 
        UINT handleId, 
        NVSDK_NGX_Parameter* parameters,
        std::unique_ptr<IFeature_Dx12>* feature);

    static bool ChangeFeature(
        std::string_view upscalerName, 
        ID3D12Device* device, 
        ID3D12GraphicsCommandList* cmdList,
        UINT handleId, 
        NVSDK_NGX_Parameter* parameters, 
        ContextData<IFeature_Dx12>* contextData);
};
