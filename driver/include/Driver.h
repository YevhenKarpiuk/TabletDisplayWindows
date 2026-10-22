#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Trace.h"
#include "ipc/FrameTransport.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct IndirectSampleMonitor
        {
            static constexpr size_t szEdidBlock = 128;
            static constexpr size_t szModeList = 4;

            const BYTE pEdidBlock[szEdidBlock];
            const struct SampleMonitorMode {
                DWORD Width;
                DWORD Height;
                DWORD VSync;
            } pModeList[szModeList];
            const DWORD ulPreferredModeIdx;
        };

        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        class FrameStore
        {
        public:
            FrameStore();
            ~FrameStore();

            bool Publish(
                const void* pixels,
                UINT width,
                UINT height,
                UINT sourceStride,
                UINT64 timestampUs);
            NTSTATUS CopyLatest(
                UINT64 afterSequence,
                void* output,
                size_t outputBytes,
                size_t* bytesWritten);
            bool PublishShared(
                ID3D11Device* device,
                ID3D11DeviceContext* context,
                ID3D11Texture2D* source,
                const D3D11_TEXTURE2D_DESC& sourceDescription,
                UINT64 timestampUs,
                bool* sharedOnly);
            NTSTATUS CopyShared(
                UINT64 afterSequence,
                UINT64 afterGeneration,
                void* output,
                size_t outputBytes,
                size_t* bytesWritten);
            NTSTATUS SetTransport(UINT mode, UINT64 generation);
            NTSTATUS CopyFrameEvent(void* output, size_t outputBytes, size_t* bytesWritten);

        private:
            bool CreateSharedTextureLocked(
                ID3D11Device* device,
                const D3D11_TEXTURE2D_DESC& sourceDescription);
            void ResetSharedTextureLocked();

            SRWLOCK m_Lock;
            tabletdisplay::ipc::FrameResponseHeader m_Header;
            std::vector<BYTE> m_Pixels;
            Microsoft::WRL::ComPtr<ID3D11Device> m_SharedDevice;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> m_SharedTexture;
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_SharedMutex;
            HANDLE m_SharedHandle{};
            tabletdisplay::ipc::SharedFrameResponse m_SharedFrame;
            UINT64 m_SharedGeneration{};
            UINT64 m_SharedHeartbeatMs{};
            HANDLE m_FrameEvent{};
            tabletdisplay::ipc::FrameEventResponse m_FrameEventName;
            bool m_SharedEnabled{};
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(
                IDDCX_SWAPCHAIN hSwapChain,
                std::shared_ptr<Direct3DDevice> Device,
                std::shared_ptr<FrameStore> Frames,
                HANDLE NewFrameEvent);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            std::shared_ptr<FrameStore> m_Frames;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };

        /// <summary>
        /// Provides a sample implementation of an indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();
            NTSTATUS CopyLatestFrame(
                WDFFILEOBJECT fileObject,
                UINT64 afterSequence,
                void* output,
                size_t outputBytes,
                size_t* bytesWritten);
            NTSTATUS CopySharedFrame(
                WDFFILEOBJECT fileObject,
                UINT64 afterSequence,
                UINT64 afterGeneration,
                void* output,
                size_t outputBytes,
                size_t* bytesWritten);
            NTSTATUS SetFrameTransport(WDFFILEOBJECT fileObject, UINT mode, UINT64 generation);
            NTSTATUS CopyFrameEvent(
                WDFFILEOBJECT fileObject,
                void* output,
                size_t outputBytes,
                size_t* bytesWritten);
            NTSTATUS SetMonitorConnection(
                WDFFILEOBJECT fileObject,
                bool connected,
                UINT nativeWidth,
                UINT nativeHeight,
                UINT preferredDivisor,
                UINT requestedSlot,
                UINT* leasedSlot);
            void ReleaseMonitorConnection(WDFFILEOBJECT fileObject) noexcept;

        protected:
            // One virtual monitor per tablet; the Host file object that leased it owns its frames.
            struct MonitorSlot
            {
                IDDCX_MONITOR Monitor{};
                std::shared_ptr<FrameStore> Frames;
                WDFFILEOBJECT LeaseFile{};
                bool Connected{};
            };

            NTSTATUS CreateMonitorLocked(UINT slot);
            std::shared_ptr<FrameStore> LeasedFrames(WDFFILEOBJECT fileObject);
            MonitorSlot* LeasedSlotLocked(WDFFILEOBJECT fileObject);

            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
            std::mutex m_MonitorMutex;
            MonitorSlot m_Slots[tabletdisplay::ipc::kMaxMonitors];
            bool m_AdapterReady;
        };

        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(
                _In_ IDDCX_MONITOR Monitor,
                UINT Slot,
                std::shared_ptr<FrameStore> Frames);
            virtual ~IndirectMonitorContext();

            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();
            UINT Slot() const noexcept { return m_Slot; }

        private:
            IDDCX_MONITOR m_Monitor;
            UINT m_Slot;
            std::shared_ptr<FrameStore> m_Frames;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        } ;
    }
}
