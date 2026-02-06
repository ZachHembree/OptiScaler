#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// #define WIN32_NO_STATUS
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE
#include <Windows.h>
#include <string>
#include <stdint.h>
#include <libloaderapi.h>
#include <ranges>

#include <winternl.h>
#include <d3dkmthk.h>

#define NV_WINDOWS
#define NVSDK_NGX
#define NGX_ENABLE_DEPRECATED_GET_PARAMETERS
#define NGX_ENABLE_DEPRECATED_SHUTDOWN
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#define SPDLOG_USE_STD_FORMAT
#define SPDLOG_WCHAR_FILENAMES
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#include "spdlog/spdlog.h"

#define VK_USE_PLATFORM_WIN32_KHR

#define BUFFER_COUNT 4

// Enables logging of DLSS NV Parameters
// #define DLSS_PARAM_DUMP

// Enables async spdlog
// Have issues when closing
// #define LOG_ASYNC

// Enables LOG_DEBUG_ONLY logs
// #define DETAILED_DEBUG_LOGS

// Enable D3D12 Debug Layers
// #define ENABLE_DEBUG_LAYER_DX12

// Enable D3D11 Debug Layers
// #define ENABLE_DEBUG_LAYER_DX11

#ifdef ENABLE_DEBUG_LAYER_DX12
// Enable GPUValidation
// #define ENABLE_GPU_VALIDATION

#include <d3d12sdklayers.h>
#endif

inline HMODULE dllModule = nullptr;
inline HMODULE exeModule = nullptr;
inline HMODULE originalModule = nullptr;
inline HMODULE skModule = nullptr;
inline HMODULE reshadeModule = nullptr;
inline HMODULE vulkanModule = nullptr;
inline HMODULE d3d11Module = nullptr;
inline HMODULE d3d12AgilityModule = nullptr;
inline HMODULE slInterposerModule = nullptr;
inline DWORD processId;

#define LOG_TRACE(msg, ...) spdlog::trace(__FUNCTION__ " " msg, ##__VA_ARGS__)

#define LOG_DEBUG(msg, ...) spdlog::debug(__FUNCTION__ " " msg, ##__VA_ARGS__)

#ifdef DETAILED_DEBUG_LOGS
#define LOG_DEBUG_ONLY(msg, ...) spdlog::debug(__FUNCTION__ " " msg, ##__VA_ARGS__)
#else
#define LOG_DEBUG_ONLY(msg, ...)
#endif

#ifdef LOG_ASYNC
#define LOG_DEBUG_ASYNC(msg, ...) spdlog::debug(__FUNCTION__ " " msg, ##__VA_ARGS__)
#else
#define LOG_DEBUG_ASYNC(msg, ...)
#endif

#define LOG_INFO(msg, ...) spdlog::info(__FUNCTION__ " " msg, ##__VA_ARGS__)

#define LOG_WARN(msg, ...) spdlog::warn(__FUNCTION__ " " msg, ##__VA_ARGS__)

#define LOG_ERROR(msg, ...) spdlog::error(__FUNCTION__ " " msg, ##__VA_ARGS__)

#define LOG_FUNC() spdlog::trace(__FUNCTION__)

#define LOG_FUNC_RESULT(result) spdlog::trace(__FUNCTION__ " result: {0:X}", (UINT64) result)

// #define TRACKING_LOGS

#ifdef TRACKING_LOGS
#define LOG_TRACK(msg, ...) spdlog::debug(__FUNCTION__ " [RT] " msg, ##__VA_ARGS__)
#else
#define LOG_TRACK(msg, ...)
#endif

struct feature_version
{
    unsigned int major;
    unsigned int minor;
    unsigned int patch;

    bool operator==(const feature_version& other) const
    {
        return major == other.major && minor == other.minor && patch == other.patch;
    }

    bool operator!=(const feature_version& other) const { return !(*this == other); }

    bool operator<(const feature_version& other) const
    {
        if (major != other.major)
            return major < other.major;
        if (minor != other.minor)
            return minor < other.minor;
        return patch < other.patch;
    }

    bool operator>(const feature_version& other) const { return other < *this; }

    bool operator<=(const feature_version& other) const { return !(other < *this); }

    bool operator>=(const feature_version& other) const { return !(*this < other); }
};

namespace VendorId
{
enum Value : uint32_t
{
    Invalid = 0,
    Microsoft = 0x1414, // Software Render Adapter
    Nvidia = 0x10DE,
    AMD = 0x1002,
    Intel = 0x8086,
};
};

inline static std::string wstring_to_string(const std::wstring& wide_str)
{
    if (wide_str.empty())
        return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), static_cast<int>(wide_str.length()), nullptr, 0,
                                          nullptr, nullptr);
    if (size_needed <= 0)
        return std::string();

    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), static_cast<int>(wide_str.length()), &result[0], size_needed,
                        nullptr, nullptr);

    return result;
}

inline static std::wstring string_to_wstring(const std::string& str)
{
    if (str.empty())
        return std::wstring();

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), nullptr, 0);
    if (size_needed <= 0)
        return std::wstring();

    std::wstring result(size_needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), &result[0], size_needed);

    return result;
}

inline static void to_lower_in_place(std::string& string)
{
    std::transform(string.begin(), string.end(), string.begin(), ::tolower);
}

/**
 * @brief Common strings and identifiers used internally by OptiScaler
 */
namespace OptiKeys
{
    using CString = const char[];

    // Application name provided to upscalers
    inline constexpr CString ProjectID = "OptiScaler";

    // ID code used for the Vulkan input provider
    inline constexpr CString VkProvider = "OptiVk";
    // ID code used for the DX11 input provider
    inline constexpr CString Dx11Provider = "OptiDx11";
    // ID code used for the DX12 input provider
    inline constexpr CString Dx12Provider = "OptiDx12";

    // Upscalers

    // ID code used for the XeSS upscaler backend
    inline constexpr CString XeSS = "xess";
    // ID code used for the XeSS upscaler backend used with the DirectX 11 on 12 compatibility layer
    inline constexpr CString XeSS_11on12 = "xess_12";
    // ID code used for the FSR 2.1.x upscaler backend
    inline constexpr CString FSR21 = "fsr21";
    // ID code used for the FSR 2.1.x upscaler backend used with the DirectX 11 on 12 compatibility layer
    inline constexpr CString FSR21_11on12 = "fsr21_12";
    // ID code used for the FSR 2.2.x upscaler backend
    inline constexpr CString FSR22 = "fsr22";
    // ID code used for the FSR 2.2.x upscaler backend used with the DirectX 11 on 12 compatibility layer
    inline constexpr CString FSR22_11on12 = "fsr22_12";
    // ID code used for the FSR 3.1+ upscaler backend
    inline constexpr CString FSR31 = "fsr31";
    // ID code used for the FSR 3.1+ upscaler backend used with the DirectX 11 on 12 compatibility layer
    inline constexpr CString FSR31_11on12 = "fsr31_12";
    // ID code used for the DLSS upscaler backend
    inline constexpr CString DLSS = "dlss";
    // ID code used for the DLSS-D/Ray Reconstruction upscaler+denoiser backend
    inline constexpr CString DLSSD = "dlssd";

    // FSR resource keys

    // Key for the horizontal resolution that FSR will output
    inline constexpr CString FSR_UpscaleWidth = "FSR.upscaleSize.width"; 
    // Key for the vertical resolution that FSR will output
    inline constexpr CString FSR_UpscaleHeight = "FSR.upscaleSize.height";
    // Key for the distance to the near plane of the camera for FSR.
    inline constexpr CString FSR_NearPlane = "FSR.cameraNear";
    // Key for the distance to the far plane of the camera for FSR.
    inline constexpr CString FSR_FarPlane = "FSR.cameraFar";
    // Key for the camera field of view in the vertical direction used by FSR for reprojection
    inline constexpr CString FSR_CameraFovVertical = "FSR.cameraFovAngleVertical";
    // Key for the time elapsed since the last frame (expressed in milliseconds)
    inline constexpr CString FSR_FrameTimeDelta = "FSR.frameTimeDelta";
    // Key for the scale factor to convert view space units to meters
    inline constexpr CString FSR_ViewSpaceToMetersFactor = "FSR.viewSpaceToMetersFactor";
    // Key for the mask used to exclude semitransparencies and UI elements. Used in older FSR versions.
    // Optional in FSR 4.0+.
    inline constexpr CString FSR_TransparencyAndComp = "FSR.transparencyAndComposition";
    // Key for the motion vector attenuation mask. Used in older FSR and XeSS versions to reduce 
    // ghosting with semitransparencies and particles. Optional in FSR 4.0+.
    inline constexpr CString FSR_Reactive = "FSR.reactive";

} // namespace OptiKeys
