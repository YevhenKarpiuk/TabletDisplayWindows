/*++

Copyright (c) Microsoft Corporation

Abstract:

    This module contains a sample implementation of an indirect display driver. See the included README.md file and the
    various TODO blocks throughout this file and all accompanying files for information on building a production driver.

    MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"
#include "Driver.tmh"

#include <bcrypt.h>
#include <sddl.h>

#include "display/DisplayModes.h"

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

#pragma region SampleMonitors

static constexpr GUID TABLET_DISPLAY_MONITOR_CONTAINER_ID =
{ 0x05211a31, 0x67e4, 0x4536, { 0x9d, 0x42, 0xae, 0xfc, 0xdf, 0x0a, 0xf1, 0x83 } };

// Modified EDID from Dell S2719DGF. Monitor slot 0 reports it unchanged, so the first tablet
// keeps the display layout Windows saved before several tablets were supported.
static constexpr BYTE s_BaseEdid[IndirectSampleMonitor::szEdidBlock] =
{
    0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,0x24,0x1D,0x01,
    0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54,0x00,0x02,0x00,
    0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,
    0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,
    0x00,0x37,0x4A,0x51,0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,
    0x53,0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,0x00,0x28,
    0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
};

namespace
{
    struct SlotEdid
    {
        BYTE Bytes[IndirectSampleMonitor::szEdidBlock];
    };

    // Every slot has its own product code (bytes 10-11), serial number (byte 12) and checksum,
    // so Windows identifies each tablet monitor separately.
    SlotEdid MakeSlotEdid(UINT slot)
    {
        SlotEdid edid{};
        memcpy(edid.Bytes, s_BaseEdid, sizeof(edid.Bytes));
        if (slot == 0)
            return edid;
        const auto productCode = tabletdisplay::ipc::EdidProductCode(slot);
        edid.Bytes[10] = static_cast<BYTE>(productCode & 0xFF);
        edid.Bytes[11] = static_cast<BYTE>(productCode >> 8);
        edid.Bytes[12] = static_cast<BYTE>(edid.Bytes[12] + slot);
        BYTE sum = 0;
        for (size_t index = 0; index + 1 < sizeof(edid.Bytes); ++index)
            sum = static_cast<BYTE>(sum + edid.Bytes[index]);
        edid.Bytes[sizeof(edid.Bytes) - 1] = static_cast<BYTE>(0x100 - sum);
        return edid;
    }

    const SlotEdid& EdidForSlot(UINT slot)
    {
        static const SlotEdid edids[tabletdisplay::ipc::kMaxMonitors] =
        {
            MakeSlotEdid(0), MakeSlotEdid(1), MakeSlotEdid(2), MakeSlotEdid(3)
        };
        static_assert(tabletdisplay::ipc::kMaxMonitors == 4);
        return edids[slot];
    }

    GUID ContainerIdForSlot(UINT slot)
    {
        auto containerId = TABLET_DISPLAY_MONITOR_CONTAINER_ID;
        containerId.Data4[7] = static_cast<unsigned char>(containerId.Data4[7] + slot);
        return containerId;
    }
}

#pragma endregion

#pragma region TabletModes

namespace
{
    struct TabletMode
    {
        DWORD Width;
        DWORD Height;
        DWORD VSync;
    };

    // Set by the Host before monitor arrival: the tablet panel resolution and the divisor the
    // user selected, per monitor slot. IddCx asks for monitor and target modes during arrival.
    struct TabletModeRequest
    {
        UINT NativeWidth;
        UINT NativeHeight;
        UINT PreferredDivisor;
    };

    SRWLOCK s_TabletModeLock = SRWLOCK_INIT;
    TabletModeRequest s_TabletModes[tabletdisplay::ipc::kMaxMonitors] = {};

    void SetTabletModes(UINT slot, UINT nativeWidth, UINT nativeHeight, UINT preferredDivisor)
    {
        AcquireSRWLockExclusive(&s_TabletModeLock);
        s_TabletModes[slot] = { nativeWidth, nativeHeight, preferredDivisor };
        ReleaseSRWLockExclusive(&s_TabletModeLock);
    }

    // The tablet resolution divided by 1..4 keeps the tablet aspect ratio; each size is
    // offered at 60 and 30 Hz. Without a tablet resolution the legacy modes are reported.
    vector<TabletMode> CurrentTabletModes(UINT slot, DWORD* preferredIndex)
    {
        AcquireSRWLockShared(&s_TabletModeLock);
        const auto request = slot < tabletdisplay::ipc::kMaxMonitors
            ? s_TabletModes[slot]
            : TabletModeRequest{};
        ReleaseSRWLockShared(&s_TabletModeLock);
        const auto nativeWidth = request.NativeWidth;
        const auto nativeHeight = request.NativeHeight;
        const auto preferredDivisor = request.PreferredDivisor;

        vector<TabletMode> modes;
        DWORD preferred = 0;
        if (nativeWidth == 0 || nativeHeight == 0)
        {
            modes = { { 1920, 1200, 60 }, { 1920, 1200, 30 }, { 1280, 800, 60 }, { 1280, 800, 30 } };
        }
        else
        {
            for (UINT divisor = tabletdisplay::display::kMinResolutionDivisor;
                 divisor <= tabletdisplay::display::kMaxResolutionDivisor;
                 ++divisor)
            {
                const auto mode = tabletdisplay::display::ScaledMode(nativeWidth, nativeHeight, divisor);
                if (!tabletdisplay::display::IsSupportedMode(mode))
                    continue;
                bool duplicate = false;
                for (const auto& existing : modes)
                    duplicate = duplicate || (existing.Width == mode.width && existing.Height == mode.height);
                if (duplicate)
                    continue;
                if (divisor == preferredDivisor)
                    preferred = static_cast<DWORD>(modes.size());
                modes.push_back({ mode.width, mode.height, 60 });
                modes.push_back({ mode.width, mode.height, 30 });
            }
        }
        if (preferredIndex != nullptr)
            *preferredIndex = preferred;
        return modes;
    }
}

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD IddSampleDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY IddSampleDeviceD0Entry;
EVT_WDF_FILE_CLEANUP IddSampleFileCleanup;
EVT_IDD_CX_DEVICE_IO_CONTROL IddSampleDeviceIoControl;

EVT_IDD_CX_ADAPTER_INIT_FINISHED IddSampleAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES IddSampleAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION IddSampleParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES IddSampleMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES IddSampleMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN IddSampleMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN IddSampleMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,
        IddSampleDeviceAdd
    );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS IddSampleDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = IddSampleDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // IddCx redirects custom IOCTL requests to this callback. TabletDisplay uses one read-only
    // request to move the latest virtual-display frame into the interactive host process.
    IddConfig.EvtIddCxDeviceIoControl = IddSampleDeviceIoControl;

    IddConfig.EvtIddCxAdapterInitFinished = IddSampleAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = IddSampleParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = IddSampleMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = IddSampleMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = IddSampleAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = IddSampleMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = IddSampleMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(
        &FileConfig,
        WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK,
        IddSampleFileCleanup);
    WdfDeviceInitSetFileObjectConfig(
        pDeviceInit,
        &FileConfig,
        WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WdfDeviceCreateDeviceInterface(
        Device,
        &tabletdisplay::ipc::kFrameDeviceInterface,
        nullptr);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Create a new device context object and attach it to the WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_
NTSTATUS IddSampleDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    // This function is called by WDF to start the device in the fully-on power state.

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region FrameStore

namespace
{
    constexpr UINT64 SharedTransportHeartbeatTimeoutMs = 2'000;

    bool MakeSharedResourceName(wchar_t* destination, size_t destinationChars)
    {
        if (destination == nullptr || destinationChars == 0)
        {
            return false;
        }
        BYTE randomBytes[16] = {};
        if (!BCRYPT_SUCCESS(BCryptGenRandom(
                nullptr,
                randomBytes,
                sizeof(randomBytes),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        {
            return false;
        }
        const int count = swprintf_s(
            destination,
            destinationChars,
            L"Global\\TabletDisplayFrame-%08X-%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
            GetCurrentProcessId(),
            randomBytes[0], randomBytes[1], randomBytes[2], randomBytes[3],
            randomBytes[4], randomBytes[5], randomBytes[6], randomBytes[7],
            randomBytes[8], randomBytes[9], randomBytes[10], randomBytes[11],
            randomBytes[12], randomBytes[13], randomBytes[14], randomBytes[15]);
        return count > 0 && static_cast<size_t>(count) < destinationChars;
    }
}

FrameStore::FrameStore()
{
    InitializeSRWLock(&m_Lock);
}

FrameStore::~FrameStore()
{
    AcquireSRWLockExclusive(&m_Lock);
    ResetSharedTextureLocked();
    ReleaseSRWLockExclusive(&m_Lock);
}

void FrameStore::ResetSharedTextureLocked()
{
    m_SharedEnabled = false;
    m_SharedHeartbeatMs = 0;
    m_SharedFrame = {};
    m_SharedMutex.Reset();
    m_SharedTexture.Reset();
    m_SharedDevice.Reset();
    if (m_SharedHandle != nullptr)
    {
        CloseHandle(m_SharedHandle);
        m_SharedHandle = nullptr;
    }
}

bool FrameStore::CreateSharedTextureLocked(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& sourceDescription)
{
    ResetSharedTextureLocked();
    if (device == nullptr || sourceDescription.Width == 0 || sourceDescription.Height == 0)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC description = {};
    description.Width = sourceDescription.Width;
    description.Height = sourceDescription.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &texture)))
    {
        return false;
    }
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<IDXGIResource1> resource;
    if (FAILED(texture.As(&mutex)) || FAILED(texture.As(&resource)))
    {
        return false;
    }

    wchar_t resourceName[tabletdisplay::ipc::kSharedResourceNameChars] = {};
    if (!MakeSharedResourceName(resourceName, ARRAYSIZE(resourceName)))
    {
        return false;
    }

    // The driver host runs as LocalService and the TabletDisplay Host in the interactive
    // user session. Nobody else may open the virtual desktop texture by its name.
    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;IU)",
            SDDL_REVISION_1,
            &securityDescriptor,
            nullptr))
    {
        return false;
    }
    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor;

    HANDLE sharedHandle = nullptr;
    const HRESULT sharedHandleResult = resource->CreateSharedHandle(
        &securityAttributes,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        resourceName,
        &sharedHandle);
    LocalFree(securityDescriptor);
    if (FAILED(sharedHandleResult))
    {
        return false;
    }

    DXGI_ADAPTER_DESC adapterDescription = {};
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapterDescription)))
    {
        CloseHandle(sharedHandle);
        return false;
    }

    ++m_SharedGeneration;
    if (m_SharedGeneration == 0)
    {
        ++m_SharedGeneration;
    }
    m_SharedFrame = {};
    m_SharedFrame.magic = tabletdisplay::ipc::kSharedFrameMagic;
    m_SharedFrame.versionMajor = tabletdisplay::ipc::kFrameVersionMajor;
    m_SharedFrame.versionMinor = tabletdisplay::ipc::kFrameVersionMinor;
    m_SharedFrame.structureBytes = sizeof(m_SharedFrame);
    m_SharedFrame.pixelFormat = tabletdisplay::ipc::kPixelFormatBgra8;
    m_SharedFrame.width = description.Width;
    m_SharedFrame.height = description.Height;
    m_SharedFrame.adapterLuidLow = adapterDescription.AdapterLuid.LowPart;
    m_SharedFrame.adapterLuidHigh = adapterDescription.AdapterLuid.HighPart;
    m_SharedFrame.generation = m_SharedGeneration;
    m_SharedFrame.resourceNameLength = static_cast<UINT>(wcslen(resourceName));
    wcscpy_s(m_SharedFrame.resourceName, resourceName);
    m_SharedDevice = device;
    m_SharedTexture = std::move(texture);
    m_SharedMutex = std::move(mutex);
    m_SharedHandle = sharedHandle;
    return true;
}

bool FrameStore::PublishShared(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const D3D11_TEXTURE2D_DESC& sourceDescription,
    UINT64 timestampUs,
    bool* sharedOnly)
{
    if (sharedOnly == nullptr)
    {
        return false;
    }
    *sharedOnly = false;
    if (device == nullptr || context == nullptr || source == nullptr ||
        (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
    {
        return false;
    }

    AcquireSRWLockExclusive(&m_Lock);
    D3D11_TEXTURE2D_DESC sharedDescription = {};
    if (m_SharedTexture)
    {
        m_SharedTexture->GetDesc(&sharedDescription);
    }
    if (!m_SharedTexture || m_SharedDevice.Get() != device ||
        sharedDescription.Width != sourceDescription.Width ||
        sharedDescription.Height != sourceDescription.Height)
    {
        if (!CreateSharedTextureLocked(device, sourceDescription))
        {
            ReleaseSRWLockExclusive(&m_Lock);
            return false;
        }
    }

    const UINT64 nowMs = GetTickCount64();
    if (m_SharedEnabled &&
        (m_SharedHeartbeatMs == 0 || nowMs - m_SharedHeartbeatMs > SharedTransportHeartbeatTimeoutMs))
    {
        m_SharedEnabled = false;
    }
    *sharedOnly = m_SharedEnabled;

    const HRESULT acquireResult = m_SharedMutex->AcquireSync(0, 0);
    if (acquireResult == WAIT_TIMEOUT)
    {
        ReleaseSRWLockExclusive(&m_Lock);
        return true;
    }
    if (FAILED(acquireResult))
    {
        ResetSharedTextureLocked();
        *sharedOnly = false;
        ReleaseSRWLockExclusive(&m_Lock);
        return false;
    }

    context->CopyResource(m_SharedTexture.Get(), source);
    context->Flush();
    const HRESULT releaseResult = m_SharedMutex->ReleaseSync(1);
    if (FAILED(releaseResult))
    {
        ResetSharedTextureLocked();
        *sharedOnly = false;
        ReleaseSRWLockExclusive(&m_Lock);
        return false;
    }
    ++m_SharedFrame.sequence;
    m_SharedFrame.timestampUs = timestampUs;
    ReleaseSRWLockExclusive(&m_Lock);
    return true;
}

NTSTATUS FrameStore::CopyShared(
    UINT64 afterSequence,
    UINT64 afterGeneration,
    void* output,
    size_t outputBytes,
    size_t* bytesWritten)
{
    if (bytesWritten == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *bytesWritten = 0;
    if (output == nullptr || outputBytes < sizeof(m_SharedFrame))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    AcquireSRWLockExclusive(&m_Lock);
    if (m_SharedEnabled)
    {
        m_SharedHeartbeatMs = GetTickCount64();
    }
    if (!m_SharedTexture || m_SharedFrame.sequence == 0 ||
        (m_SharedFrame.generation == afterGeneration &&
         m_SharedFrame.sequence <= afterSequence))
    {
        ReleaseSRWLockExclusive(&m_Lock);
        return STATUS_DEVICE_NOT_READY;
    }
    memcpy(output, &m_SharedFrame, sizeof(m_SharedFrame));
    *bytesWritten = sizeof(m_SharedFrame);
    ReleaseSRWLockExclusive(&m_Lock);
    return STATUS_SUCCESS;
}

NTSTATUS FrameStore::SetTransport(UINT mode, UINT64 generation)
{
    AcquireSRWLockExclusive(&m_Lock);
    NTSTATUS status = STATUS_SUCCESS;
    if (mode == tabletdisplay::ipc::kFrameTransportCpu)
    {
        m_SharedEnabled = false;
        m_SharedHeartbeatMs = 0;
    }
    else if (mode == tabletdisplay::ipc::kFrameTransportSharedTexture &&
        m_SharedTexture && generation == m_SharedGeneration)
    {
        m_SharedEnabled = true;
        m_SharedHeartbeatMs = GetTickCount64();
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;
    }
    ReleaseSRWLockExclusive(&m_Lock);
    return status;
}

bool FrameStore::Publish(
    const void* pixels,
    UINT width,
    UINT height,
    UINT sourceStride,
    UINT64 timestampUs)
{
    if (pixels == nullptr || width == 0 || height == 0 ||
        width > tabletdisplay::ipc::kMaxFrameWidth ||
        height > tabletdisplay::ipc::kMaxFrameHeight)
    {
        return false;
    }

    const UINT destinationStride = width * tabletdisplay::ipc::kBytesPerPixel;
    const size_t frameBytes = static_cast<size_t>(destinationStride) * height;
    if (sourceStride < destinationStride || frameBytes > tabletdisplay::ipc::kMaxFrameBytes)
    {
        return false;
    }

    AcquireSRWLockExclusive(&m_Lock);
    try
    {
        m_Pixels.resize(frameBytes);
    }
    catch (...)
    {
        ReleaseSRWLockExclusive(&m_Lock);
        return false;
    }

    const auto* source = static_cast<const BYTE*>(pixels);
    for (UINT row = 0; row < height; ++row)
    {
        memcpy(
            m_Pixels.data() + static_cast<size_t>(row) * destinationStride,
            source + static_cast<size_t>(row) * sourceStride,
            destinationStride);
    }

    m_Header.magic = tabletdisplay::ipc::kFrameMagic;
    m_Header.versionMajor = tabletdisplay::ipc::kFrameVersionMajor;
    m_Header.versionMinor = tabletdisplay::ipc::kFrameVersionMinor;
    m_Header.headerBytes = sizeof(m_Header);
    m_Header.pixelFormat = tabletdisplay::ipc::kPixelFormatBgra8;
    m_Header.width = width;
    m_Header.height = height;
    m_Header.stride = destinationStride;
    m_Header.frameBytes = static_cast<UINT>(frameBytes);
    ++m_Header.sequence;
    m_Header.timestampUs = timestampUs;
    ReleaseSRWLockExclusive(&m_Lock);
    return true;
}

NTSTATUS FrameStore::CopyLatest(
    UINT64 afterSequence,
    void* output,
    size_t outputBytes,
    size_t* bytesWritten)
{
    if (bytesWritten == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *bytesWritten = 0;

    AcquireSRWLockShared(&m_Lock);
    if (m_Header.sequence == 0 || m_Header.sequence <= afterSequence)
    {
        ReleaseSRWLockShared(&m_Lock);
        return STATUS_DEVICE_NOT_READY;
    }

    const size_t requiredBytes = sizeof(m_Header) + m_Pixels.size();
    if (output == nullptr || outputBytes < requiredBytes)
    {
        ReleaseSRWLockShared(&m_Lock);
        return STATUS_BUFFER_TOO_SMALL;
    }

    memcpy(output, &m_Header, sizeof(m_Header));
    memcpy(static_cast<BYTE*>(output) + sizeof(m_Header), m_Pixels.data(), m_Pixels.size());
    *bytesWritten = requiredBytes;
    ReleaseSRWLockShared(&m_Lock);
    return STATUS_SUCCESS;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(
    IDDCX_SWAPCHAIN hSwapChain,
    shared_ptr<Direct3DDevice> Device,
    shared_ptr<FrameStore> Frames,
    HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_Frames(Frames),
      m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

    ComPtr<ID3D11Texture2D> StagingTexture;
    D3D11_TEXTURE2D_DESC StagingDescription = {};

    // Acquire and release buffers in a loop
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            // We must wait for a new buffer
            HANDLE WaitHandles [] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                // We have a new buffer, so try the AcquireBuffer again
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                // We need to terminate
                break;
            }
            else
            {
                // The wait was cancelled or something unexpected happened
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            // We have new frame to process, the surface has a reference on it that the driver has to release
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            ComPtr<ID3D11Texture2D> SourceTexture;
            if (SUCCEEDED(AcquiredBuffer.As(&SourceTexture)))
            {
                D3D11_TEXTURE2D_DESC SourceDescription = {};
                SourceTexture->GetDesc(&SourceDescription);
                const bool supportedFormat =
                    SourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                    SourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                const bool supportedSize =
                    SourceDescription.Width <= tabletdisplay::ipc::kMaxFrameWidth &&
                    SourceDescription.Height <= tabletdisplay::ipc::kMaxFrameHeight;
                if (supportedFormat && supportedSize)
                {
                    LARGE_INTEGER Counter = {};
                    LARGE_INTEGER Frequency = {};
                    QueryPerformanceCounter(&Counter);
                    QueryPerformanceFrequency(&Frequency);
                    const UINT64 TimestampUs = Frequency.QuadPart > 0
                        ? static_cast<UINT64>(Counter.QuadPart / Frequency.QuadPart) * 1'000'000ULL +
                          static_cast<UINT64>(Counter.QuadPart % Frequency.QuadPart) * 1'000'000ULL /
                              static_cast<UINT64>(Frequency.QuadPart)
                        : 0;
                    bool sharedOnly = false;
                    (void)m_Frames->PublishShared(
                        m_Device->Device.Get(),
                        m_Device->DeviceContext.Get(),
                        SourceTexture.Get(),
                        SourceDescription,
                        TimestampUs,
                        &sharedOnly);

                    if (!sharedOnly)
                    {
                        if (!StagingTexture ||
                            StagingDescription.Width != SourceDescription.Width ||
                            StagingDescription.Height != SourceDescription.Height ||
                            StagingDescription.Format != SourceDescription.Format)
                        {
                            StagingDescription = SourceDescription;
                            StagingDescription.BindFlags = 0;
                            StagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            StagingDescription.Usage = D3D11_USAGE_STAGING;
                            StagingDescription.MiscFlags = 0;
                            StagingDescription.ArraySize = 1;
                            StagingDescription.MipLevels = 1;
                            StagingDescription.SampleDesc.Count = 1;
                            StagingDescription.SampleDesc.Quality = 0;
                            StagingTexture.Reset();
                            (void)m_Device->Device->CreateTexture2D(
                                &StagingDescription,
                                nullptr,
                                &StagingTexture);
                        }

                        if (StagingTexture)
                        {
                            m_Device->DeviceContext->CopyResource(
                                StagingTexture.Get(),
                                SourceTexture.Get());
                            D3D11_MAPPED_SUBRESOURCE Mapped = {};
                            if (SUCCEEDED(m_Device->DeviceContext->Map(
                                    StagingTexture.Get(),
                                    0,
                                    D3D11_MAP_READ,
                                    0,
                                    &Mapped)))
                            {
                                (void)m_Frames->Publish(
                                    Mapped.pData,
                                    SourceDescription.Width,
                                    SourceDescription.Height,
                                    Mapped.RowPitch,
                                    TimestampUs);
                                m_Device->DeviceContext->Unmap(StagingTexture.Get(), 0);
                            }
                        }
                    }
                }
            }

            // We have finished processing this frame hence we release the reference on it.
            // If the driver forgets to release the reference to the surface, it will be leaked which results in the
            // surfaces being left around after swapchain is destroyed.
            // NOTE: Although in this sample we release reference to the surface here; the driver still
            // owns the Buffer.MetaData.pSurface surface until IddCxSwapChainReleaseAndAcquireBuffer returns
            // S_OK and gives us a new frame, a driver may want to use the surface in future to re-encode the desktop
            // for better quality if there is no new frame for a while
            AcquiredBuffer.Reset();

            // Indicate to OS that we have finished inital processing of the frame, it is a hint that
            // OS could start preparing another frame
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }

            // ==============================
            // TODO: Report frame statistics once the asynchronous encode/send work is completed
            //
            // Drivers should report information about sub-frame timings, like encode time, send time, etc.
            // ==============================
            // IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
        }
        else
        {
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice),
    m_Adapter{},
    m_AdapterReady{}
{
    for (auto& Slot : m_Slots)
        Slot.Frames = make_shared<FrameStore>();
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

IndirectDeviceContext::MonitorSlot* IndirectDeviceContext::LeasedSlotLocked(WDFFILEOBJECT fileObject)
{
    if (fileObject == nullptr)
        return nullptr;
    for (auto& Slot : m_Slots)
    {
        if (Slot.LeaseFile == fileObject)
            return &Slot;
    }
    return nullptr;
}

shared_ptr<FrameStore> IndirectDeviceContext::LeasedFrames(WDFFILEOBJECT fileObject)
{
    lock_guard lock(m_MonitorMutex);
    const auto* Slot = LeasedSlotLocked(fileObject);
    return Slot != nullptr ? Slot->Frames : nullptr;
}

// Frames and the frame transport belong to the process that connected the monitor, so other
// processes that can open the device interface cannot read the virtual desktop.
NTSTATUS IndirectDeviceContext::CopyLatestFrame(
    WDFFILEOBJECT fileObject,
    UINT64 afterSequence,
    void* output,
    size_t outputBytes,
    size_t* bytesWritten)
{
    const auto Frames = LeasedFrames(fileObject);
    if (!Frames)
        return STATUS_ACCESS_DENIED;
    return Frames->CopyLatest(afterSequence, output, outputBytes, bytesWritten);
}

NTSTATUS IndirectDeviceContext::CopySharedFrame(
    WDFFILEOBJECT fileObject,
    UINT64 afterSequence,
    UINT64 afterGeneration,
    void* output,
    size_t outputBytes,
    size_t* bytesWritten)
{
    const auto Frames = LeasedFrames(fileObject);
    if (!Frames)
        return STATUS_ACCESS_DENIED;
    return Frames->CopyShared(
        afterSequence, afterGeneration, output, outputBytes, bytesWritten);
}

NTSTATUS IndirectDeviceContext::SetFrameTransport(
    WDFFILEOBJECT fileObject,
    UINT mode,
    UINT64 generation)
{
    const auto Frames = LeasedFrames(fileObject);
    if (!Frames)
        return STATUS_ACCESS_DENIED;
    return Frames->SetTransport(mode, generation);
}

// requestedSlot 0 leases the first free monitor, N leases monitor N - 1.
NTSTATUS IndirectDeviceContext::SetMonitorConnection(
    WDFFILEOBJECT fileObject,
    bool connected,
    UINT nativeWidth,
    UINT nativeHeight,
    UINT preferredDivisor,
    UINT requestedSlot,
    UINT* leasedSlot)
{
    if (fileObject == nullptr)
        return STATUS_INVALID_HANDLE;

    lock_guard lock(m_MonitorMutex);
    if (!m_AdapterReady)
        return STATUS_DEVICE_NOT_READY;

    auto* Slot = LeasedSlotLocked(fileObject);
    if (connected)
    {
        if (Slot != nullptr)
        {
            if (requestedSlot != 0 && &m_Slots[requestedSlot - 1] != Slot)
                return STATUS_INVALID_PARAMETER;
        }
        else if (requestedSlot != 0)
        {
            Slot = &m_Slots[requestedSlot - 1];
            if (Slot->LeaseFile != nullptr)
                return STATUS_DEVICE_BUSY;
        }
        else
        {
            for (auto& Candidate : m_Slots)
            {
                if (Candidate.LeaseFile == nullptr)
                {
                    Slot = &Candidate;
                    break;
                }
            }
            if (Slot == nullptr)
                return STATUS_DEVICE_BUSY;
        }

        const auto SlotIndex = static_cast<UINT>(Slot - m_Slots);
        if (!Slot->Connected)
        {
            // Monitor modes are queried during arrival, so they must be known before it.
            SetTabletModes(SlotIndex, nativeWidth, nativeHeight, preferredDivisor);
            if (Slot->Monitor == nullptr)
            {
                const auto Status = CreateMonitorLocked(SlotIndex);
                if (!NT_SUCCESS(Status))
                    return Status;
            }
            if (Slot->Monitor == nullptr)
                return STATUS_DEVICE_NOT_READY;
            IDARG_OUT_MONITORARRIVAL ArrivalOut{};
            const auto Status = IddCxMonitorArrival(Slot->Monitor, &ArrivalOut);
            if (!NT_SUCCESS(Status))
            {
                WdfObjectDelete(Slot->Monitor);
                Slot->Monitor = nullptr;
                return Status;
            }
            Slot->Connected = true;
        }
        Slot->LeaseFile = fileObject;
        if (leasedSlot != nullptr)
            *leasedSlot = SlotIndex;
        return STATUS_SUCCESS;
    }

    if (Slot == nullptr)
        return STATUS_SUCCESS;

    if (Slot->Connected)
    {
        const auto Status = IddCxMonitorDeparture(Slot->Monitor);
        if (!NT_SUCCESS(Status))
            return Status;
        Slot->Connected = false;
        Slot->Monitor = nullptr;
    }
    Slot->LeaseFile = nullptr;
    return STATUS_SUCCESS;
}

void IndirectDeviceContext::ReleaseMonitorConnection(WDFFILEOBJECT fileObject) noexcept
{
    lock_guard lock(m_MonitorMutex);
    auto* Slot = LeasedSlotLocked(fileObject);
    if (Slot == nullptr)
        return;

    if (Slot->Connected && NT_SUCCESS(IddCxMonitorDeparture(Slot->Monitor)))
    {
        Slot->Connected = false;
        Slot->Monitor = nullptr;
    }
    Slot->LeaseFile = nullptr;
}

void IndirectDeviceContext::InitAdapter()
{
    // ==============================
    // TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
    // numbers are used for telemetry and may be displayed to the user in some situations.
    //
    // This is also where static per-adapter capabilities are determined.
    // ==============================

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = tabletdisplay::ipc::kMaxMonitors;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"TabletDisplay Virtual Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"Microsoft";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"TabletDisplay";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit()
{
    lock_guard lock(m_MonitorMutex);
    m_AdapterReady = true;
}

NTSTATUS IndirectDeviceContext::CreateMonitorLocked(UINT slot)
{
    // ==============================
    // TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDIDs
    // provided here are purely for demonstration.
    // Monitor manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS
    // to optimize settings like viewing distance and scale factor. Manufacturers should also use a unique serial
    // number every single device to ensure the OS can tell the monitors apart.
    // ==============================

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* Context = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        if (Context != nullptr)
            Context->Cleanup();
    };

    // Each Host lease gets a fresh IddCx monitor object; the stable container ID preserves monitor identity.
    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = slot;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = IndirectSampleMonitor::szEdidBlock;
    MonitorInfo.MonitorDescription.pData = const_cast<BYTE*>(EdidForSlot(slot).Bytes);

    // The same tablet display reconnects repeatedly, so each slot's identity must remain stable.
    MonitorInfo.MonitorContainerId = ContainerIdForSlot(slot);

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (NT_SUCCESS(Status))
    {
        // Create a new monitor context object and attach it to the Idd monitor object
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(
            MonitorCreateOut.MonitorObject,
            slot,
            m_Slots[slot].Frames);
        m_Slots[slot].Monitor = MonitorCreateOut.MonitorObject;
    }
    return Status;
}

IndirectMonitorContext::IndirectMonitorContext(
    _In_ IDDCX_MONITOR Monitor,
    UINT Slot,
    shared_ptr<FrameStore> Frames) :
    m_Monitor(Monitor), m_Slot(Slot), m_Frames(Frames)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        // Create a new swap-chain processing thread
        m_ProcessingThread.reset(new SwapChainProcessor(
            SwapChain,
            Device,
            m_Frames,
            NewFrameEvent));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Stop processing the last swap-chain
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

static UINT MonitorSlot(IDDCX_MONITOR MonitorObject)
{
    const auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    return pMonitorContextWrapper != nullptr && pMonitorContextWrapper->pContext != nullptr
        ? pMonitorContextWrapper->pContext->Slot()
        : tabletdisplay::ipc::kMaxMonitors;
}

_Use_decl_annotations_
VOID IddSampleDeviceIoControl(
    WDFDEVICE Device,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    size_t BytesWritten = 0;

    if (IoControlCode == tabletdisplay::ipc::kGetLatestFrameIoctl)
    {
        tabletdisplay::ipc::FrameRequest* FrameRequest = nullptr;
        void* Output = nullptr;
        if (InputBufferLength < sizeof(*FrameRequest) ||
            OutputBufferLength < sizeof(tabletdisplay::ipc::FrameResponseHeader))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            Status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(*FrameRequest),
                reinterpret_cast<void**>(&FrameRequest),
                nullptr);
            if (NT_SUCCESS(Status) &&
                (FrameRequest->structureBytes != sizeof(*FrameRequest) ||
                 FrameRequest->reserved != 0))
            {
                Status = STATUS_INVALID_PARAMETER;
            }
            if (NT_SUCCESS(Status))
            {
                Status = WdfRequestRetrieveOutputBuffer(
                    Request,
                    sizeof(tabletdisplay::ipc::FrameResponseHeader),
                    &Output,
                    nullptr);
            }
            if (NT_SUCCESS(Status))
            {
                auto* DeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
                if (DeviceContext == nullptr || DeviceContext->pContext == nullptr)
                {
                    Status = STATUS_DEVICE_NOT_READY;
                }
                else
                {
                    Status = DeviceContext->pContext->CopyLatestFrame(
                        WdfRequestGetFileObject(Request),
                        FrameRequest->afterSequence,
                        Output,
                        OutputBufferLength,
                        &BytesWritten);
                }
            }
        }
    }
    else if (IoControlCode == tabletdisplay::ipc::kGetSharedFrameIoctl)
    {
        tabletdisplay::ipc::SharedFrameRequest* FrameRequest = nullptr;
        void* Output = nullptr;
        if (InputBufferLength < sizeof(*FrameRequest) ||
            OutputBufferLength < sizeof(tabletdisplay::ipc::SharedFrameResponse))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            Status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(*FrameRequest),
                reinterpret_cast<void**>(&FrameRequest),
                nullptr);
            if (NT_SUCCESS(Status) &&
                (FrameRequest->structureBytes != sizeof(*FrameRequest) ||
                 FrameRequest->reserved != 0))
            {
                Status = STATUS_INVALID_PARAMETER;
            }
            if (NT_SUCCESS(Status))
            {
                Status = WdfRequestRetrieveOutputBuffer(
                    Request,
                    sizeof(tabletdisplay::ipc::SharedFrameResponse),
                    &Output,
                    nullptr);
            }
            if (NT_SUCCESS(Status))
            {
                auto* DeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
                if (DeviceContext == nullptr || DeviceContext->pContext == nullptr)
                {
                    Status = STATUS_DEVICE_NOT_READY;
                }
                else
                {
                    Status = DeviceContext->pContext->CopySharedFrame(
                        WdfRequestGetFileObject(Request),
                        FrameRequest->afterSequence,
                        FrameRequest->afterGeneration,
                        Output,
                        OutputBufferLength,
                        &BytesWritten);
                }
            }
        }
    }
    else if (IoControlCode == tabletdisplay::ipc::kSetFrameTransportIoctl)
    {
        tabletdisplay::ipc::FrameTransportRequest* TransportRequest = nullptr;
        if (InputBufferLength < sizeof(*TransportRequest))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            Status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(*TransportRequest),
                reinterpret_cast<void**>(&TransportRequest),
                nullptr);
            if (NT_SUCCESS(Status) &&
                TransportRequest->structureBytes != sizeof(*TransportRequest))
            {
                Status = STATUS_INVALID_PARAMETER;
            }
            if (NT_SUCCESS(Status))
            {
                auto* DeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
                if (DeviceContext == nullptr || DeviceContext->pContext == nullptr)
                {
                    Status = STATUS_DEVICE_NOT_READY;
                }
                else
                {
                    Status = DeviceContext->pContext->SetFrameTransport(
                        WdfRequestGetFileObject(Request),
                        TransportRequest->mode,
                        TransportRequest->generation);
                }
            }
        }
    }
    else if (IoControlCode == tabletdisplay::ipc::kSetMonitorConnectionIoctl)
    {
        tabletdisplay::ipc::MonitorConnectionRequest* ConnectionRequest = nullptr;
        if (InputBufferLength < sizeof(*ConnectionRequest))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            Status = WdfRequestRetrieveInputBuffer(
                Request,
                sizeof(*ConnectionRequest),
                reinterpret_cast<void**>(&ConnectionRequest),
                nullptr);
            if (NT_SUCCESS(Status) &&
                (ConnectionRequest->structureBytes != sizeof(*ConnectionRequest) ||
                 ConnectionRequest->connected > 1 ||
                 ConnectionRequest->slot > tabletdisplay::ipc::kMaxMonitors))
            {
                Status = STATUS_INVALID_PARAMETER;
            }
            UINT PreferredDivisor = tabletdisplay::display::kMinResolutionDivisor;
            if (NT_SUCCESS(Status))
            {
                const bool LegacyModes =
                    ConnectionRequest->nativeWidth == 0 && ConnectionRequest->nativeHeight == 0;
                if (ConnectionRequest->preferredDivisor != 0)
                    PreferredDivisor = ConnectionRequest->preferredDivisor;
                const auto NativeMode = tabletdisplay::display::Landscape(
                    ConnectionRequest->nativeWidth, ConnectionRequest->nativeHeight);
                if (PreferredDivisor > tabletdisplay::display::kMaxResolutionDivisor ||
                    (!LegacyModes && !tabletdisplay::display::IsSupportedMode(
                        tabletdisplay::display::ScaledMode(NativeMode.width, NativeMode.height, 1))))
                {
                    Status = STATUS_INVALID_PARAMETER;
                }
            }
            if (NT_SUCCESS(Status))
            {
                auto* DeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
                const auto FileObject = WdfRequestGetFileObject(Request);
                if (DeviceContext == nullptr || DeviceContext->pContext == nullptr ||
                    FileObject == nullptr)
                {
                    Status = STATUS_DEVICE_NOT_READY;
                }
                else
                {
                    UINT LeasedSlot = 0;
                    Status = DeviceContext->pContext->SetMonitorConnection(
                        FileObject,
                        ConnectionRequest->connected != 0,
                        ConnectionRequest->nativeWidth,
                        ConnectionRequest->nativeHeight,
                        PreferredDivisor,
                        ConnectionRequest->slot,
                        &LeasedSlot);
                    // Hosts that know about several monitors pass an output buffer for the slot.
                    tabletdisplay::ipc::MonitorConnectionResponse* Response = nullptr;
                    if (NT_SUCCESS(Status) && ConnectionRequest->connected != 0 &&
                        OutputBufferLength >= sizeof(*Response) &&
                        NT_SUCCESS(WdfRequestRetrieveOutputBuffer(
                            Request,
                            sizeof(*Response),
                            reinterpret_cast<void**>(&Response),
                            nullptr)))
                    {
                        *Response = {};
                        Response->slot = LeasedSlot;
                        BytesWritten = sizeof(*Response);
                    }
                }
            }
        }
    }

    WdfRequestCompleteWithInformation(Request, Status, BytesWritten);
}

_Use_decl_annotations_
VOID IddSampleFileCleanup(WDFFILEOBJECT FileObject)
{
    const auto Device = WdfFileObjectGetDevice(FileObject);
    auto* DeviceContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (DeviceContext != nullptr && DeviceContext->pContext != nullptr)
        DeviceContext->pContext->ReleaseMonitorConnection(FileObject);
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->FinishInit();
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

    // ==============================
    // TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
    // through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
    // should be turned off).
    // ==============================

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
    // this sample driver, we hard-code the EDID, so this function can generate known modes.
    // ==============================

    // The EDID only identifies the monitor slot; its modes follow that slot's tablet resolution.
    if (pInArgs->MonitorDescription.DataSize != IndirectSampleMonitor::szEdidBlock)
        return STATUS_INVALID_PARAMETER;
    UINT Slot = 0;
    for (; Slot < tabletdisplay::ipc::kMaxMonitors; Slot++)
    {
        if (memcmp(pInArgs->MonitorDescription.pData, EdidForSlot(Slot).Bytes, IndirectSampleMonitor::szEdidBlock) == 0)
            break;
    }
    // This EDID block does not belong to the monitors we reported earlier
    if (Slot == tabletdisplay::ipc::kMaxMonitors)
        return STATUS_INVALID_PARAMETER;

    DWORD PreferredModeIndex = 0;
    const auto Modes = CurrentTabletModes(Slot, &PreferredModeIndex);
    pOutArgs->MonitorModeBufferOutputCount = static_cast<UINT>(Modes.size());

    if (pInArgs->MonitorModeBufferInputCount < Modes.size())
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    else
    {
        for (size_t ModeIndex = 0; ModeIndex < Modes.size(); ModeIndex++)
        {
            pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
                Modes[ModeIndex].Width,
                Modes[ModeIndex].Height,
                Modes[ModeIndex].VSync,
                IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
            );
        }

        pOutArgs->PreferredMonitorModeIdx = PreferredModeIndex;

        return STATUS_SUCCESS;
    }
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
    // Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
    // monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
    // than an EDID, those modes would also be reported here.
    // ==============================

    DWORD PreferredModeIndex = 0;
    const auto Modes = CurrentTabletModes(MonitorSlot(MonitorObject), &PreferredModeIndex);
    pOutArgs->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(Modes.size());
    if (pInArgs->DefaultMonitorModeBufferInputCount != 0)
    {
        if (pInArgs->DefaultMonitorModeBufferInputCount < Modes.size())
            return STATUS_BUFFER_TOO_SMALL;
        for (size_t ModeIndex = 0; ModeIndex < Modes.size(); ModeIndex++)
        {
            pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
                Modes[ModeIndex].Width,
                Modes[ModeIndex].Height,
                Modes[ModeIndex].VSync,
                IDDCX_MONITOR_MODE_ORIGIN_DRIVER
            );
        }
        pOutArgs->PreferredMonitorModeIdx = PreferredModeIndex;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    vector<IDDCX_TARGET_MODE> TargetModes;

    // Create a set of modes supported for frame processing and scan-out. These are typically not based on the
    // monitor's descriptor and instead are based on the static processing capability of the device. The OS will
    // report the available set of modes for a given output as the intersection of monitor modes with target modes.

    for (const auto& Mode : CurrentTabletModes(MonitorSlot(MonitorObject), nullptr))
    {
        TargetModes.push_back(CreateIddCxTargetMode(Mode.Width, Mode.Height, Mode.VSync));
    }

    pOutArgs->TargetModeBufferOutputCount = (UINT) TargetModes.size();

    if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
    {
        copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
