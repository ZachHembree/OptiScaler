#include <pch.h>
#include <Config.h>
#include "FSR2Feature_Dx12_212.h"
#include "MathUtils.h"

using namespace OptiMath;

bool FSR2FeatureDx12_212::Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
                               NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    Device = InDevice;

    if (InitFSR2(InParameters))
    {
        if (!Config::Instance()->OverlayMenu.value_or_default() && (Imgui == nullptr || Imgui.get() == nullptr))
            Imgui = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), InDevice);

        OutputScaler = std::make_unique<OS_Dx12>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx12>("RCAS", InDevice);
        Bias = std::make_unique<Bias_Dx12>("Bias", InDevice);

        return true;
    }

    return false;
}

bool FSR2FeatureDx12_212::Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
        return false;

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto& ngxParams = *InParameters;

    if (!RCAS->IsInit())
        cfg.RcasEnabled.set_volatile_value(false);

    if (!OutputScaler->IsInit())
        cfg.OutputScalingEnabled.set_volatile_value(false);

    Fsr212::FfxFsr2DispatchDescription params {};

    ngxParams.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &params.jitterOffset.x);
    ngxParams.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &params.jitterOffset.y);

    if (cfg.OverrideSharpness.value_or_default())
        _sharpness = cfg.Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (cfg.RcasEnabled.value_or_default())
    {
        params.enableSharpening = false;
        params.sharpness = 0.0f;
    }
    else
    {
        if (_sharpness > 1.0f)
            _sharpness = 1.0f;

        params.enableSharpening = _sharpness > 0.0f;
        params.sharpness = _sharpness;
    }

    LOG_DEBUG("Jitter Offset: {0}x{1}", params.jitterOffset.x, params.jitterOffset.y);

    unsigned int reset;
    ngxParams.Get(NVSDK_NGX_Parameter_Reset, &reset);
    params.reset = (reset == 1);

    GetRenderResolution(InParameters, &params.renderSize.width, &params.renderSize.height);

    bool useSS = cfg.OutputScalingEnabled.value_or_default() && LowResMV();

    LOG_DEBUG("Input Resolution: {0}x{1}", params.renderSize.width, params.renderSize.height);

    params.commandList = Fsr212::ffxGetCommandListDX12_212(InCommandList);

    ID3D12Resource* paramColor;
    if (ngxParams.Get(NVSDK_NGX_Parameter_Color, &paramColor) != NVSDK_NGX_Result_Success)
        ngxParams.Get(NVSDK_NGX_Parameter_Color, (void**) &paramColor);

    if (paramColor)
    {
        LOG_DEBUG("Color exist..");

        if (cfg.ColorResourceBarrier.has_value())
        {
            ResourceBarrier(InCommandList, paramColor,
                            (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else if (state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                 state.gameQuirks & GameQuirk::ForceUnrealEngine)
        {
            cfg.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);
            ResourceBarrier(InCommandList, paramColor, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        params.color = Fsr212::ffxGetResourceDX12_212(&_context, paramColor, (wchar_t*) L"FSR2_Color",
                                                      Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    else
    {
        LOG_ERROR("Color not exist!!");
        return false;
    }

    ID3D12Resource* paramVelocity;
    if (ngxParams.Get(NVSDK_NGX_Parameter_MotionVectors, &paramVelocity) != NVSDK_NGX_Result_Success)
        ngxParams.Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramVelocity);

    if (paramVelocity)
    {
        LOG_DEBUG("MotionVectors exist..");

        if (cfg.MVResourceBarrier.has_value())
            ResourceBarrier(InCommandList, paramVelocity,
                            (D3D12_RESOURCE_STATES) cfg.MVResourceBarrier.value(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        else if (state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                 state.gameQuirks & GameQuirk::ForceUnrealEngine)
        {
            cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ResourceBarrier(InCommandList, paramVelocity, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        params.motionVectors = Fsr212::ffxGetResourceDX12_212(
            &_context, paramVelocity, (wchar_t*) L"FSR2_MotionVectors", Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    else
    {
        LOG_ERROR("MotionVectors not exist!!");
        return false;
    }

    ID3D12Resource* paramOutput;
    if (ngxParams.Get(NVSDK_NGX_Parameter_Output, &paramOutput) != NVSDK_NGX_Result_Success)
        ngxParams.Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);

    if (paramOutput)
    {
        LOG_DEBUG("Output exist..");

        if (cfg.OutputResourceBarrier.has_value())
            ResourceBarrier(InCommandList, paramOutput,
                            (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (useSS)
        {
            if (OutputScaler->CreateBufferResource(Device, paramOutput, TargetWidth(), TargetHeight(),
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            {
                OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                params.output =
                    Fsr212::ffxGetResourceDX12_212(&_context, OutputScaler->Buffer(), (wchar_t*) L"FSR2_Output",
                                                   Fsr212::FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else
                params.output = Fsr212::ffxGetResourceDX12_212(&_context, paramOutput, (wchar_t*) L"FSR2_Output",
                                                               Fsr212::FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
            params.output = Fsr212::ffxGetResourceDX12_212(&_context, paramOutput, (wchar_t*) L"FSR2_Output",
                                                           Fsr212::FFX_RESOURCE_STATE_UNORDERED_ACCESS);

        if (cfg.RcasEnabled.value_or_default() &&
            (_sharpness > 0.0f || (cfg.MotionSharpnessEnabled.value_or_default() &&
                                   cfg.MotionSharpness.value_or_default() > 0.0f)) &&
            RCAS->IsInit() &&
            RCAS->CreateBufferResource(Device, (ID3D12Resource*) params.output.resource,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            params.output = Fsr212::ffxGetResourceDX12_212(&_context, RCAS->Buffer(), (wchar_t*) L"FSR2_Output",
                                                           Fsr212::FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    else
    {
        LOG_ERROR("Output not exist!!");
        return false;
    }

    ID3D12Resource* paramDepth;
    if (ngxParams.Get(NVSDK_NGX_Parameter_Depth, &paramDepth) != NVSDK_NGX_Result_Success)
        ngxParams.Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

    if (paramDepth)
    {
        LOG_DEBUG("Depth exist..");

        if (cfg.DepthResourceBarrier.has_value())
            ResourceBarrier(InCommandList, paramDepth,
                            (D3D12_RESOURCE_STATES) cfg.DepthResourceBarrier.value(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        params.depth = Fsr212::ffxGetResourceDX12_212(&_context, paramDepth, (wchar_t*) L"FSR2_Depth",
                                                      Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    else
    {
        LOG_ERROR("Depth not exist!!");
        return false;
    }

    ID3D12Resource* paramExp = nullptr;
    if (AutoExposure())
    {
        LOG_DEBUG("AutoExposure enabled!");
    }
    else
    {
        if (ngxParams.Get(NVSDK_NGX_Parameter_ExposureTexture, &paramExp) != NVSDK_NGX_Result_Success)
            ngxParams.Get(NVSDK_NGX_Parameter_ExposureTexture, (void**) &paramExp);

        if (paramExp)
        {
            LOG_DEBUG("ExposureTexture exist..");

            if (cfg.ExposureResourceBarrier.has_value())
                ResourceBarrier(InCommandList, paramExp,
                                (D3D12_RESOURCE_STATES) cfg.ExposureResourceBarrier.value(),
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            params.exposure = Fsr212::ffxGetResourceDX12_212(&_context, paramExp, (wchar_t*) L"FSR2_Exposure",
                                                             Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
        }
        else
        {
            LOG_DEBUG("AutoExposure disabled but ExposureTexture is not exist, it may cause problems!!");
            state.AutoExposure = true;
            state.changeBackend[Handle()->Id] = true;
            return true;
        }
    }

    ID3D12Resource* paramTransparency = nullptr;
    if (ngxParams.Get(OptiKeys::FSR_TransparencyAndComp, &paramTransparency) == NVSDK_NGX_Result_Success)
        ngxParams.Get(OptiKeys::FSR_TransparencyAndComp, (void**) &paramTransparency);

    ID3D12Resource* paramReactiveMask = nullptr;
    if (ngxParams.Get(OptiKeys::FSR_Reactive, &paramReactiveMask) == NVSDK_NGX_Result_Success)
        ngxParams.Get(OptiKeys::FSR_Reactive, (void**) &paramReactiveMask);

    ID3D12Resource* paramReactiveMask2 = nullptr;
    if (ngxParams.Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &paramReactiveMask2) !=
        NVSDK_NGX_Result_Success)
        ngxParams.Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void**) &paramReactiveMask2);

    if (!cfg.DisableReactiveMask.value_or(paramReactiveMask == nullptr &&
                                                          paramReactiveMask2 == nullptr))
    {
        if (paramTransparency != nullptr)
        {
            LOG_DEBUG("Using FSR transparency mask..");
            params.transparencyAndComposition = Fsr212::ffxGetResourceDX12_212(
                &_context, paramTransparency, (wchar_t*) L"FSR2_Transparency", Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
        }

        if (paramReactiveMask != nullptr)
        {
            LOG_DEBUG("Using FSR reactive mask..");
            params.reactive = Fsr212::ffxGetResourceDX12_212(&_context, paramReactiveMask, (wchar_t*) L"FSR2_Reactive",
                                                             Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
        }
        else
        {
            if (paramReactiveMask2 != nullptr)
            {
                LOG_DEBUG("Bias mask exist..");
                cfg.DisableReactiveMask.set_volatile_value(false);

                if (cfg.MaskResourceBarrier.has_value())
                    ResourceBarrier(InCommandList, paramReactiveMask2,
                                    (D3D12_RESOURCE_STATES) cfg.MaskResourceBarrier.value(),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                if (paramTransparency == nullptr && cfg.FsrUseMaskForTransparency.value_or_default())
                    params.transparencyAndComposition =
                        Fsr212::ffxGetResourceDX12_212(&_context, paramReactiveMask2, (wchar_t*) L"FSR2_Transparency",
                                                       Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);

                if (cfg.DlssReactiveMaskBias.value_or_default() > 0.0f && Bias->IsInit() &&
                    Bias->CreateBufferResource(Device, paramReactiveMask2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                    Bias->CanRender())
                {
                    Bias->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    if (Bias->Dispatch(Device, InCommandList, paramReactiveMask2,
                                       cfg.DlssReactiveMaskBias.value_or_default(), Bias->Buffer()))
                    {
                        Bias->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        params.reactive =
                            Fsr212::ffxGetResourceDX12_212(&_context, Bias->Buffer(), (wchar_t*) L"FSR2_Reactive",
                                                           Fsr212::FFX_RESOURCE_STATE_COMPUTE_READ);
                    }
                }
                else
                {
                    LOG_DEBUG("Skipping reactive mask, Bias: {0}, Bias Init: {1}, Bias CanRender: {2}",
                              cfg.DlssReactiveMaskBias.value_or_default(), Bias->IsInit(),
                              Bias->CanRender());
                }
            }
        }
    }

    _hasColor = params.color.resource != nullptr;
    _hasDepth = params.depth.resource != nullptr;
    _hasMV = params.motionVectors.resource != nullptr;
    _hasExposure = params.exposure.resource != nullptr;
    _hasTM = params.transparencyAndComposition.resource != nullptr;
    _accessToReactiveMask = paramReactiveMask != nullptr;
    _hasOutput = params.output.resource != nullptr;

    float MVScaleX = 1.0f;
    float MVScaleY = 1.0f;

    if (ngxParams.Get(NVSDK_NGX_Parameter_MV_Scale_X, &MVScaleX) == NVSDK_NGX_Result_Success &&
        ngxParams.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &MVScaleY) == NVSDK_NGX_Result_Success)
    {
        params.motionVectorScale.x = MVScaleX;
        params.motionVectorScale.y = MVScaleY;
    }
    else
    {
        LOG_WARN("Can't get motion vector scales!");

        params.motionVectorScale.x = MVScaleX;
        params.motionVectorScale.y = MVScaleY;
    }

    LOG_DEBUG("Sharpness: {0}", params.sharpness);

    if (cfg.FsrCameraNear.has_value() || !cfg.FsrUseFsrInputValues.value_or_default() ||
        ngxParams.Get(OptiKeys::FSR_NearPlane, &params.cameraNear) != NVSDK_NGX_Result_Success)
    {
        if (DepthInverted())
            params.cameraFar = cfg.FsrCameraNear.value_or_default();
        else
            params.cameraNear = cfg.FsrCameraNear.value_or_default();
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default() ||
        ngxParams.Get(OptiKeys::FSR_FarPlane, &params.cameraFar) != NVSDK_NGX_Result_Success)
    {
        if (DepthInverted())
            params.cameraNear = cfg.FsrCameraFar.value_or_default();
        else
            params.cameraFar = cfg.FsrCameraFar.value_or_default();
    }

    if (ngxParams.Get(OptiKeys::FSR_CameraFovVertical, &params.cameraFovAngleVertical) != NVSDK_NGX_Result_Success)
    {
        if (cfg.FsrVerticalFov.has_value())
            params.cameraFovAngleVertical = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
        else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
        {
            const float hFovRad = GetRadiansFromDeg(cfg.FsrHorizontalFov.value());
            params.cameraFovAngleVertical = GetVerticalFovFromHorizontal(hFovRad, (float) TargetWidth(), (float) TargetHeight());
        }
        else
            params.cameraFovAngleVertical = GetRadiansFromDeg(60);
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default() ||
        ngxParams.Get(OptiKeys::FSR_FrameTimeDelta, &params.frameTimeDelta) != NVSDK_NGX_Result_Success)
    {
        if (ngxParams.Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &params.frameTimeDelta) !=
                NVSDK_NGX_Result_Success ||
            params.frameTimeDelta < 1.0f)
            params.frameTimeDelta = (float) GetDeltaTime();
    }

    LOG_DEBUG("FrameTimeDeltaInMsec: {0}", params.frameTimeDelta);

    if (ngxParams.Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &params.preExposure) != NVSDK_NGX_Result_Success)
        params.preExposure = 1.0f;

    LOG_DEBUG("Dispatch!!");
    auto result = Fsr212::ffxFsr2ContextDispatch212(&_context, &params);

    if (result != Fsr212::FFX_OK)
    {
        LOG_ERROR("ffxFsr2ContextDispatch error: {0}", ResultToString212(result));
        return false;
    }

    // apply rcas
    if (cfg.RcasEnabled.value_or_default() &&
        (_sharpness > 0.0f || (cfg.MotionSharpnessEnabled.value_or_default() &&
                               cfg.MotionSharpness.value_or_default() > 0.0f)) &&
        RCAS->CanRender())
    {
        if (params.output.resource != RCAS->Buffer())
            ResourceBarrier(InCommandList, (ID3D12Resource*) params.output.resource,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        RcasConstants rcasConstants {};

        rcasConstants.Sharpness = _sharpness;
        rcasConstants.DisplayWidth = TargetWidth();
        rcasConstants.DisplayHeight = TargetHeight();
        ngxParams.Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
        ngxParams.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);
        rcasConstants.DisplaySizeMV = !(GetFeatureFlags() & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
        rcasConstants.RenderHeight = RenderHeight();
        rcasConstants.RenderWidth = RenderWidth();

        if (useSS)
        {
            if (!RCAS->Dispatch(Device, InCommandList, (ID3D12Resource*) params.output.resource,
                                (ID3D12Resource*) params.motionVectors.resource, rcasConstants, OutputScaler->Buffer()))
            {
                cfg.RcasEnabled.set_volatile_value(false);
                return true;
            }
        }
        else
        {
            if (!RCAS->Dispatch(Device, InCommandList, (ID3D12Resource*) params.output.resource,
                                (ID3D12Resource*) params.motionVectors.resource, rcasConstants, paramOutput))
            {
                cfg.RcasEnabled.set_volatile_value(false);
                return true;
            }
        }
    }

    if (useSS)
    {
        LOG_DEBUG("scaling output...");
        OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (!OutputScaler->Dispatch(Device, InCommandList, OutputScaler->Buffer(), paramOutput))
        {
            cfg.OutputScalingEnabled.set_volatile_value(false);
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }
    }

    // imgui
    if (!cfg.OverlayMenu.value_or_default() && _frameCount > 30)
    {
        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InCommandList, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(GetForegroundWindow(), Device);
        }
    }

    // restore resource states
    if (paramColor && cfg.ColorResourceBarrier.has_value())
        ResourceBarrier(InCommandList, paramColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value());

    if (paramVelocity && cfg.MVResourceBarrier.has_value())
        ResourceBarrier(InCommandList, paramVelocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        (D3D12_RESOURCE_STATES) cfg.MVResourceBarrier.value());

    if (paramOutput && cfg.OutputResourceBarrier.has_value())
        ResourceBarrier(InCommandList, paramOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value());

    if (paramDepth && cfg.DepthResourceBarrier.has_value())
        ResourceBarrier(InCommandList, paramDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        (D3D12_RESOURCE_STATES) cfg.DepthResourceBarrier.value());

    if (paramExp && cfg.ExposureResourceBarrier.has_value())
        ResourceBarrier(InCommandList, paramExp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        (D3D12_RESOURCE_STATES) cfg.ExposureResourceBarrier.value());

    if (paramReactiveMask && cfg.MaskResourceBarrier.has_value())
        ResourceBarrier(InCommandList, paramReactiveMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        (D3D12_RESOURCE_STATES) cfg.MaskResourceBarrier.value());

    _frameCount++;

    return true;
}

FSR2FeatureDx12_212::~FSR2FeatureDx12_212() {}

bool FSR2FeatureDx12_212::InitFSR2(const NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (Device == nullptr)
    {
        LOG_ERROR("D3D12Device is null!");
        return false;
    }

    {
        ScopedSkipSpoofing skipSpoofing {};

        const size_t scratchBufferSize = Fsr212::ffxFsr2GetScratchMemorySizeDX12_212();
        void* scratchBuffer = calloc(scratchBufferSize, 1);

        auto errorCode =
            Fsr212::ffxFsr2GetInterfaceDX12_212(&_contextDesc.callbacks, Device, scratchBuffer, scratchBufferSize);

        if (errorCode != Fsr212::FFX_OK)
        {
            LOG_ERROR("ffxGetInterfaceDX12 error: {0}", ResultToString212(errorCode));
            free(scratchBuffer);
            return false;
        }

        _contextDesc.device = Fsr212::ffxGetDeviceDX12_212(Device);
        _contextDesc.flags = 0;

        if (DepthInverted())
            _contextDesc.flags |= Fsr212::FFX_FSR2_ENABLE_DEPTH_INVERTED;

        if (AutoExposure())
            _contextDesc.flags |= Fsr212::FFX_FSR2_ENABLE_AUTO_EXPOSURE;

        if (IsHdr())
            _contextDesc.flags |= Fsr212::FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;

        if (JitteredMV())
            _contextDesc.flags |= Fsr212::FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

        if (!LowResMV())
            _contextDesc.flags |= Fsr212::FFX_FSR2_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;

        if (Config::Instance()->OutputScalingEnabled.value_or_default() && LowResMV())
        {
            float ssMulti = Config::Instance()->OutputScalingMultiplier.value_or_default();

            if (ssMulti < 0.5f)
            {
                ssMulti = 0.5f;
                Config::Instance()->OutputScalingMultiplier.set_volatile_value(ssMulti);
            }
            else if (ssMulti > 3.0f)
            {
                ssMulti = 3.0f;
                Config::Instance()->OutputScalingMultiplier.set_volatile_value(ssMulti);
            }

            _targetWidth = static_cast<unsigned int>(DisplayWidth() * ssMulti);
            _targetHeight = static_cast<unsigned int>(DisplayHeight() * ssMulti);
        }
        else
        {
            _targetWidth = DisplayWidth();
            _targetHeight = DisplayHeight();
        }

        // extended limits changes how resolution
        if (Config::Instance()->ExtendedLimits.value_or_default() && RenderWidth() > DisplayWidth())
        {
            _contextDesc.maxRenderSize.width = RenderWidth();
            _contextDesc.maxRenderSize.height = RenderHeight();

            Config::Instance()->OutputScalingMultiplier.set_volatile_value(1.0f);

            // if output scaling active let it to handle downsampling
            if (Config::Instance()->OutputScalingEnabled.value_or_default() && LowResMV())
            {
                _contextDesc.displaySize.width = _contextDesc.maxRenderSize.width;
                _contextDesc.displaySize.height = _contextDesc.maxRenderSize.height;

                // update target res
                _targetWidth = _contextDesc.maxRenderSize.width;
                _targetHeight = _contextDesc.maxRenderSize.height;
            }
            else
            {
                _contextDesc.displaySize.width = DisplayWidth();
                _contextDesc.displaySize.height = DisplayHeight();
            }
        }
        else
        {
            _contextDesc.maxRenderSize.width = TargetWidth() > DisplayWidth() ? TargetWidth() : DisplayWidth();
            _contextDesc.maxRenderSize.height = TargetHeight() > DisplayHeight() ? TargetHeight() : DisplayHeight();
            _contextDesc.displaySize.width = TargetWidth();
            _contextDesc.displaySize.height = TargetHeight();
        }

        LOG_DEBUG("ffxFsr2ContextCreate!");

        ScopedSkipHeapCapture skipHeapCapture {};
        auto ret = Fsr212::ffxFsr2ContextCreate212(&_context, &_contextDesc);

        if (ret != Fsr212::FFX_OK)
        {
            LOG_ERROR("ffxFsr2ContextCreate error: {0}", ResultToString212(ret));
            return false;
        }
    }

    SetInit(true);

    return true;
}
