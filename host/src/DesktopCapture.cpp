#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "DesktopCapture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tabletdisplay::host
{
    namespace
    {
        [[noreturn]] void ThrowHr(const char* operation, HRESULT hr)
        {
            char value[16]{};
            sprintf_s(value, "%08X", static_cast<unsigned>(hr));
            throw std::runtime_error(std::string(operation) + " failed, HRESULT 0x" + value);
        }

        void CheckHr(HRESULT hr, const char* operation)
        {
            if (FAILED(hr)) ThrowHr(operation, hr);
        }
    }

    struct DesktopCapture::Impl
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t sourceWidth{};
        std::uint32_t sourceHeight{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGIOutputDuplication> duplication;
        ComPtr<ID3D11Texture2D> staging;
        std::vector<std::uint8_t> pixels;
        bool hasFrame{};
    };

    DesktopCapture::DesktopCapture(std::uint32_t width, std::uint32_t height)
        : m_impl(std::make_unique<Impl>())
    {
        if (width == 0 || height == 0 || width > 16'384 || height > 16'384)
            throw std::invalid_argument("Invalid desktop capture dimensions");
        m_impl->width = width;
        m_impl->height = height;
        m_impl->pixels.resize(static_cast<std::size_t>(width) * height * 4U);

        ComPtr<IDXGIFactory1> factory;
        CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> selectedAdapter;
        ComPtr<IDXGIOutput> selectedOutput;
        DXGI_OUTPUT_DESC selectedDescription{};
        for (UINT adapterIndex = 0; ; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const auto adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
            CheckHr(adapterResult, "EnumAdapters1");
            for (UINT outputIndex = 0; ; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                const auto outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND) break;
                CheckHr(outputResult, "EnumOutputs");
                DXGI_OUTPUT_DESC description{};
                CheckHr(output->GetDesc(&description), "IDXGIOutput::GetDesc");
                if (description.AttachedToDesktop &&
                    description.DesktopCoordinates.left <= 0 &&
                    description.DesktopCoordinates.top <= 0 &&
                    description.DesktopCoordinates.right > 0 &&
                    description.DesktopCoordinates.bottom > 0)
                {
                    selectedAdapter = std::move(adapter);
                    selectedOutput = std::move(output);
                    selectedDescription = description;
                    break;
                }
            }
            if (selectedOutput) break;
        }
        if (!selectedAdapter || !selectedOutput)
            throw std::runtime_error("Primary DXGI output was not found");

        CheckHr(D3D11CreateDevice(
            selectedAdapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &m_impl->device,
            nullptr,
            &m_impl->context), "D3D11CreateDevice");

        ComPtr<IDXGIOutput1> output1;
        CheckHr(selectedOutput.As(&output1), "IDXGIOutput1");
        CheckHr(output1->DuplicateOutput(m_impl->device.Get(), &m_impl->duplication), "DuplicateOutput");

        m_impl->sourceWidth = static_cast<std::uint32_t>(
            selectedDescription.DesktopCoordinates.right - selectedDescription.DesktopCoordinates.left);
        m_impl->sourceHeight = static_cast<std::uint32_t>(
            selectedDescription.DesktopCoordinates.bottom - selectedDescription.DesktopCoordinates.top);

        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = m_impl->sourceWidth;
        texture.Height = m_impl->sourceHeight;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_STAGING;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CheckHr(m_impl->device->CreateTexture2D(&texture, nullptr, &m_impl->staging), "CreateTexture2D(staging)");
    }

    DesktopCapture::~DesktopCapture() = default;

    CapturedFrame DesktopCapture::Capture()
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        const auto acquireResult = m_impl->duplication->AcquireNextFrame(
            m_impl->hasFrame ? 0U : 5'000U,
            &frameInfo,
            &resource);
        if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT && m_impl->hasFrame)
        {
            return {
                m_impl->pixels,
                m_impl->width,
                m_impl->height,
                static_cast<std::size_t>(m_impl->width) * 4U
            };
        }
        CheckHr(acquireResult, "AcquireNextFrame");

        struct FrameRelease
        {
            IDXGIOutputDuplication* duplication{};
            ~FrameRelease()
            {
                if (duplication != nullptr) (void)duplication->ReleaseFrame();
            }
        } release{ m_impl->duplication.Get() };

        ComPtr<ID3D11Texture2D> frameTexture;
        CheckHr(resource.As(&frameTexture), "Desktop frame texture");
        m_impl->context->CopyResource(m_impl->staging.Get(), frameTexture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHr(m_impl->context->Map(m_impl->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");
        struct Unmap
        {
            ID3D11DeviceContext* context{};
            ID3D11Texture2D* texture{};
            ~Unmap()
            {
                if (context != nullptr) context->Unmap(texture, 0);
            }
        } unmap{ m_impl->context.Get(), m_impl->staging.Get() };

        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t y = 0; y < m_impl->height; ++y)
        {
            const auto sourceY = static_cast<std::uint64_t>(y) * m_impl->sourceHeight / m_impl->height;
            const auto* sourceRow = source + sourceY * mapped.RowPitch;
            auto* destination = m_impl->pixels.data() +
                static_cast<std::size_t>(y) * m_impl->width * 4U;
            for (std::uint32_t x = 0; x < m_impl->width; ++x)
            {
                const auto sourceX = static_cast<std::uint64_t>(x) * m_impl->sourceWidth / m_impl->width;
                std::copy_n(sourceRow + sourceX * 4U, 4, destination + x * 4U);
            }
        }

        m_impl->hasFrame = true;
        return {
            m_impl->pixels,
            m_impl->width,
            m_impl->height,
            static_cast<std::size_t>(m_impl->width) * 4U
        };
    }
}
