#include <pch.h>
#include "IFeature_Dx12.h"
#include "State.h"

void IFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

bool IFeature_Dx12::TryResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
    const CustomOptional<int32_t, NoDefault>& InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    if (InCommandList != nullptr && InResource != nullptr && InBeforeState.has_value())
    {
        ResourceBarrier(InCommandList, InResource, (D3D12_RESOURCE_STATES)InBeforeState.value(), InAfterState);
        return true;
    }
    else
        return false;
}

bool IFeature_Dx12::TryResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
    D3D12_RESOURCE_STATES InBeforeState, const CustomOptional<int32_t, NoDefault>& InAfterState)
{
    if (InCommandList != nullptr && InResource != nullptr && InAfterState.has_value())
    {
        ResourceBarrier(InCommandList, InResource, InBeforeState, (D3D12_RESOURCE_STATES)InAfterState.value());
        return true;
    }
    else
        return false;
}

IFeature_Dx12::IFeature_Dx12() {}

IFeature_Dx12::~IFeature_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (Imgui != nullptr && Imgui.get() != nullptr)
        Imgui.reset();

    if (OutputScaler != nullptr && OutputScaler.get() != nullptr)
        OutputScaler.reset();

    if (RCAS != nullptr && RCAS.get() != nullptr)
        RCAS.reset();

    if (Bias != nullptr && Bias.get() != nullptr)
        Bias.reset();
}
