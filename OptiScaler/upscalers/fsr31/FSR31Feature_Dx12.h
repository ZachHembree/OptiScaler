#pragma once
#include "FSR31Feature.h"
#include <upscalers/IFeature_Dx12.h>

#include "dx12/ffx_api_dx12.h"
#include "proxies/FfxApi_Proxy.h"

struct FSRInputResourcesDx12
{
    // Primary resources
    ID3D12Resource* color;
    ID3D12Resource* velocity;
    ID3D12Resource* depth;

    // Optional resources
    ID3D12Resource* transparencyMask;
    ID3D12Resource* reactiveMask;
    ID3D12Resource* dlssBiasMaskFallback;
    ID3D12Resource* exposureMap;
};

/**
 * @brief DirectX 12 implementation of FSR 3.1/4 for OptiScaler. Translates semi-generalized
 * TSR inputs based on customized Nvidia NGX parameter tables to AMD FFX API calls.
 */
class FSR31FeatureDx12 : public FSR31Feature, public IFeature_Dx12
{
  public:
    /**
    * @brief Initializes the FSR feature, loads the FFX DX12 proxy methods,
    * and verifies if the backend module is ready.
    */
    FSR31FeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~FSR31FeatureDx12();

    feature_version Version() override { return FSR31Feature::Version(); }

    std::string Name() const override { return FSR31Feature::Name(); }

    /**
     * @brief Initializes the FFX context, selects an FSR version based on configuration and 
     availability, and initializes helper shaders.
     * @return true if initialization succeeds.
     */
    bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
              NVSDK_NGX_Parameter* InParameters) override;

    /**
     * @brief Executes the upscaling pass. Gathers input and output textures and configuration 
     * from the NGX parameter table. Includes optional, user-configurable pre and post processing 
     * steps for sharpening and scaling.
     */
    bool Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

  protected:
    // Init utils

    /**
     * @brief Initializes a compatible FSR upscaler based on NGX and OptiScaler configuratons on startup or
     * on mode changes.
     * @param InParameters DLSS-compatible configuration table
     */
    bool InitFSR3(const NVSDK_NGX_Parameter* InParameters) override;

    void SetInitFlags(const NVSDK_NGX_Parameter& ngxParams);

    void GetResolutionConfig();

    void QueryVersions();

    uint64_t GetVersionOverrideID();

    // Evaluate utils

    /**
     * @brief Prepares input textures used by FSR to produce the upscaled output.
     * @return true if successful. False if any primary resources are missing.
     */
    bool PrepareInputs(const NVSDK_NGX_Parameter& inParams, ID3D12GraphicsCommandList* InCommandList,
                       FSRInputResourcesDx12& inputs);

    /**
    * @brief Attempts to populate reactive and transparency masks for FSR input, converting/repurposing DLSS bias mask
    * if provided and configured.
    */
    void GetReactiveAndTransparencyMasks(ID3D12GraphicsCommandList* InCommandList, FSRInputResourcesDx12& inputs);

    /**
    * @brief FSR upscaling pass. Takes the provided input/output textures and applies application and user 
    * configuration options from the NGX table and manual OptiScaler configuration, respectively.
    * @return true on success
    */
    bool DispatchFSR(
        ID3D12GraphicsCommandList* InCommandList, 
        const NVSDK_NGX_Parameter& inParams,
        const FSRInputResourcesDx12& inputs,
        ID3D12Resource* dstTex);

    /**
     * @brief Reads application configuration data from the NGX table the upscaling pass and sets the appropriate FFX
     * configurations in the dispatch descriptor. Executed immediately before FSR dispatch.
     */    
    void UpdateConfiguration(const NVSDK_NGX_Parameter& inParams, ffxDispatchDescUpscale& fsrParams);

    /**
     * @brief Applies optional post-processing to FSR output if configured. Includes options for post-process RCAS, 
     FSR output rescaling and ImGui compositing.
     */
    void PostProcess(
        const NVSDK_NGX_Parameter& inParams, 
        bool useSS,
        ID3D12GraphicsCommandList* InCommandList, 
        ID3D12Resource* motionVectors,
        ID3D12Resource* fsrDstTex,
        ID3D12Resource* dstTex);
};