#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MediaFoundationH264Encoder.h"

#include <windows.h>
#include <codecapi.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <propvarutil.h>
#include <wrl.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace tabletdisplay::host
{
    namespace
    {
        [[noreturn]] void ThrowHr(const char* operation, HRESULT hr)
        {
            throw std::runtime_error(std::string(operation) + " failed, HRESULT 0x" +
                [] (HRESULT value)
                {
                    char text[16]{};
                    sprintf_s(text, "%08X", static_cast<unsigned>(value));
                    return std::string(text);
                }(hr));
        }

        void CheckHr(HRESULT hr, const char* operation)
        {
            if (FAILED(hr)) ThrowHr(operation, hr);
        }

        [[nodiscard]] std::string Utf8(std::wstring_view value)
        {
            if (value.empty()) return {};
            const auto count = WideCharToMultiByte(
                CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) return "Media Foundation H.264 encoder";
            std::string result(static_cast<std::size_t>(count), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                result.data(), count, nullptr, nullptr);
            return result;
        }

        struct ActivatedTransform
        {
            ComPtr<IMFTransform> transform;
            std::string name;
            bool hardware{};
        };

        [[nodiscard]] std::optional<ActivatedTransform> EnumerateEncoder(bool hardware)
        {
            MFT_REGISTER_TYPE_INFO input{ MFMediaType_Video, MFVideoFormat_NV12 };
            MFT_REGISTER_TYPE_INFO output{ MFMediaType_Video, MFVideoFormat_H264 };
            IMFActivate** activations{};
            UINT32 count{};
            const auto flags = hardware
                ? static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER)
                : static_cast<UINT32>(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER);
            const auto hr = MFTEnumEx(
                MFT_CATEGORY_VIDEO_ENCODER,
                flags,
                &input,
                &output,
                &activations,
                &count);
            if (FAILED(hr) || count == 0)
            {
                if (activations != nullptr) CoTaskMemFree(activations);
                return std::nullopt;
            }

            std::optional<ActivatedTransform> result;
            for (UINT32 index = 0; index < count && !result.has_value(); ++index)
            {
                wchar_t* friendlyName{};
                UINT32 friendlyNameLength{};
                std::string name = hardware ? "Hardware H.264 MFT" : "Software H.264 MFT";
                if (SUCCEEDED(activations[index]->GetAllocatedString(
                        MFT_FRIENDLY_NAME_Attribute, &friendlyName, &friendlyNameLength)) &&
                    friendlyName != nullptr)
                {
                    name = Utf8(std::wstring_view(friendlyName, friendlyNameLength));
                    CoTaskMemFree(friendlyName);
                }
                ComPtr<IMFTransform> transform;
                if (SUCCEEDED(activations[index]->ActivateObject(IID_PPV_ARGS(&transform))))
                    result = ActivatedTransform{ std::move(transform), std::move(name), hardware };
            }
            for (UINT32 index = 0; index < count; ++index) activations[index]->Release();
            CoTaskMemFree(activations);
            return result;
        }

        void SetCodecBool(ICodecAPI* codec, const GUID& property, bool value) noexcept
        {
            if (codec == nullptr) return;
            VARIANT variant{};
            VariantInit(&variant);
            variant.vt = VT_BOOL;
            variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
            (void)codec->SetValue(&property, &variant);
            VariantClear(&variant);
        }

        void SetCodecUint32(ICodecAPI* codec, const GUID& property, std::uint32_t value) noexcept
        {
            if (codec == nullptr) return;
            VARIANT variant{};
            VariantInit(&variant);
            variant.vt = VT_UI4;
            variant.ulVal = value;
            (void)codec->SetValue(&property, &variant);
            VariantClear(&variant);
        }
    }

    struct MediaFoundationH264Encoder::Impl
    {
        explicit Impl(EncoderOptions requested)
            : options(requested)
        {
            if (options.width == 0 || options.height == 0 || options.fps == 0 || options.bitrate == 0 ||
                (options.width & 1U) != 0 || (options.height & 1U) != 0)
                throw std::invalid_argument("Invalid H.264 encoder options");

            const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) CheckHr(comResult, "CoInitializeEx");
            ownsCom = SUCCEEDED(comResult);
            try
            {
                CheckHr(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
                mediaFoundationStarted = true;
                auto activated = options.preferHardware
                    ? EnumerateEncoder(true)
                    : EnumerateEncoder(false);
                if (!activated.has_value() && options.preferHardware)
                    activated = EnumerateEncoder(false);
                if (!activated.has_value()) throw std::runtime_error("No Media Foundation H.264 encoder is registered");
                transform = std::move(activated->transform);
                name = std::move(activated->name);
                hardware = activated->hardware;
                if (hardware) InitializeD3dManager();
                Configure();
            }
            catch (...)
            {
                Shutdown();
                throw;
            }
        }

        ~Impl()
        {
            Shutdown();
        }

        void Shutdown() noexcept
        {
            if (transform)
            {
                (void)transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                (void)transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            }
            events.Reset();
            codec.Reset();
            transform.Reset();
            if (mediaFoundationStarted)
            {
                (void)MFShutdown();
                mediaFoundationStarted = false;
            }
            if (ownsCom)
            {
                CoUninitialize();
                ownsCom = false;
            }
        }

        void InitializeD3dManager()
        {
            ComPtr<IMFAttributes> attributes;
            CheckHr(transform->GetAttributes(&attributes), "GetAttributes(D3D)");
            UINT32 async{};
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0)
                CheckHr(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK(D3D)");

            UINT resetToken{};
            CheckHr(MFCreateDXGIDeviceManager(&resetToken, &deviceManager), "MFCreateDXGIDeviceManager");
            D3D_FEATURE_LEVEL featureLevel{};
            CheckHr(D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                &d3dDevice,
                &featureLevel,
                &d3dContext), "D3D11CreateDevice(encoder)");
            ComPtr<ID3D10Multithread> multithread;
            if (SUCCEEDED(d3dDevice.As(&multithread)))
                multithread->SetMultithreadProtected(TRUE);
            CheckHr(deviceManager->ResetDevice(d3dDevice.Get(), resetToken), "IMFDXGIDeviceManager::ResetDevice");
            CheckHr(transform->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(deviceManager.Get())), "MFT_MESSAGE_SET_D3D_MANAGER");

            D3D11_TEXTURE2D_DESC description{};
            description.Width = options.width;
            description.Height = options.height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_NV12;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            CheckHr(d3dDevice->CreateTexture2D(
                &description, nullptr, &hardwareInputTexture),
                "CreateTexture2D(reusable NV12 input)");
            CheckHr(MFCreateDXGISurfaceBuffer(
                __uuidof(ID3D11Texture2D),
                hardwareInputTexture.Get(),
                0,
                FALSE,
                &hardwareInputBuffer), "MFCreateDXGISurfaceBuffer(reusable input)");
        }

        void Configure()
        {
            ComPtr<IMFAttributes> attributes;
            CheckHr(transform->GetAttributes(&attributes), "GetAttributes");
            UINT32 async{};
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0)
            {
                asynchronous = true;
                CheckHr(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK");
                CheckHr(transform.As(&events), "IMFMediaEventGenerator");
            }
            (void)attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

            DWORD inputCount{};
            DWORD outputCount{};
            CheckHr(transform->GetStreamCount(&inputCount, &outputCount), "GetStreamCount");
            if (inputCount != 1 || outputCount != 1)
                throw std::runtime_error("H.264 MFT has an unsupported stream count");
            DWORD inputIds[1]{};
            DWORD outputIds[1]{};
            const auto idsResult = transform->GetStreamIDs(1, inputIds, 1, outputIds);
            if (idsResult != E_NOTIMPL) CheckHr(idsResult, "GetStreamIDs");
            if (SUCCEEDED(idsResult))
            {
                inputId = inputIds[0];
                outputId = outputIds[0];
            }
            (void)transform.As(&codec);

            SetCodecBool(codec.Get(), CODECAPI_AVLowLatencyMode, true);
            SetCodecUint32(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            SetCodecUint32(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, options.bitrate);
            SetCodecUint32(codec.Get(), CODECAPI_AVEncMPVGOPSize, std::max<std::uint32_t>(1, options.fps * 2U));
            SetCodecUint32(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);

            ComPtr<IMFMediaType> outputType;
            CheckHr(MFCreateMediaType(&outputType), "MFCreateMediaType(output)");
            CheckHr(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE(output)");
            CheckHr(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "MF_MT_SUBTYPE(output)");
            CheckHr(outputType->SetUINT32(MF_MT_AVG_BITRATE, options.bitrate), "MF_MT_AVG_BITRATE");
            CheckHr(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "MF_MT_INTERLACE_MODE");
            CheckHr(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, 66), "MF_MT_MPEG2_PROFILE");
            CheckHr(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, options.width, options.height), "MF_MT_FRAME_SIZE(output)");
            CheckHr(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, options.fps, 1), "MF_MT_FRAME_RATE(output)");
            CheckHr(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "MF_MT_PIXEL_ASPECT_RATIO(output)");
            CheckHr(transform->SetOutputType(outputId, outputType.Get(), 0), "SetOutputType");

            ComPtr<IMFMediaType> inputType;
            CheckHr(MFCreateMediaType(&inputType), "MFCreateMediaType(input)");
            CheckHr(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "MF_MT_MAJOR_TYPE(input)");
            CheckHr(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "MF_MT_SUBTYPE(input)");
            CheckHr(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "MF_MT_INTERLACE_MODE(input)");
            CheckHr(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, options.width), "MF_MT_DEFAULT_STRIDE");
            CheckHr(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, options.width, options.height), "MF_MT_FRAME_SIZE(input)");
            CheckHr(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, options.fps, 1), "MF_MT_FRAME_RATE(input)");
            CheckHr(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "MF_MT_PIXEL_ASPECT_RATIO(input)");
            CheckHr(transform->SetInputType(inputId, inputType.Get(), 0), "SetInputType");

            CheckHr(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MFT begin streaming");
            CheckHr(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MFT start stream");
        }

        void WaitForEvent(MediaEventType expected)
        {
            auto& pendingExpected = expected == METransformNeedInput
                ? pendingNeedInput
                : pendingHaveOutput;
            if (pendingExpected != 0)
            {
                --pendingExpected;
                return;
            }

            for (;;)
            {
                ComPtr<IMFMediaEvent> event;
                CheckHr(events->GetEvent(0, &event), "MFT GetEvent");
                HRESULT status{};
                CheckHr(event->GetStatus(&status), "MFT event status");
                CheckHr(status, "MFT asynchronous event");
                MediaEventType type{};
                CheckHr(event->GetType(&type), "MFT event type");
                if (type == expected) return;
                if (type == METransformNeedInput)
                    ++pendingNeedInput;
                else if (type == METransformHaveOutput)
                    ++pendingHaveOutput;
            }
        }

        [[nodiscard]] ComPtr<IMFSample> MakeInputSample(
            std::span<const std::uint8_t> nv12,
            std::uint64_t presentationTimeUs)
        {
            const auto expected = static_cast<std::uint64_t>(options.width) * options.height * 3U / 2U;
            if (nv12.size() != expected || nv12.size() > std::numeric_limits<DWORD>::max())
                throw std::invalid_argument("NV12 frame size does not match encoder configuration");

            ComPtr<IMFMediaBuffer> buffer;
            if (hardware)
            {
                if (!hardwareInputTexture || !hardwareInputBuffer)
                    throw std::runtime_error("Reusable hardware input surface is not initialized");
                d3dContext->UpdateSubresource(
                    hardwareInputTexture.Get(),
                    0,
                    nullptr,
                    nv12.data(),
                    options.width,
                    static_cast<UINT>(nv12.size()));
                buffer = hardwareInputBuffer;
            }
            else
            {
                CheckHr(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer),
                    "MFCreateMemoryBuffer(input)");
                BYTE* destination{};
                DWORD capacity{};
                CheckHr(buffer->Lock(&destination, &capacity, nullptr), "Lock input buffer");
                if (capacity < nv12.size())
                {
                    buffer->Unlock();
                    throw std::runtime_error("Media Foundation input buffer is too small");
                }
                std::copy(nv12.begin(), nv12.end(), destination);
                CheckHr(buffer->Unlock(), "Unlock input buffer");
                CheckHr(buffer->SetCurrentLength(static_cast<DWORD>(nv12.size())),
                    "SetCurrentLength(input)");
            }

            ComPtr<IMFSample> sample;
            CheckHr(MFCreateSample(&sample), "MFCreateSample(input)");
            CheckHr(sample->AddBuffer(buffer.Get()), "AddBuffer(input)");
            CheckHr(sample->SetSampleTime(static_cast<LONGLONG>(presentationTimeUs * 10U)), "SetSampleTime(input)");
            CheckHr(sample->SetSampleDuration(10'000'000LL / options.fps), "SetSampleDuration(input)");
            return sample;
        }

        void RefreshCodecConfig(std::span<const std::uint8_t> accessUnit)
        {
            if (config.has_value()) return;
            ComPtr<IMFMediaType> currentType;
            if (SUCCEEDED(transform->GetOutputCurrentType(outputId, &currentType)))
            {
                UINT8* bytes{};
                UINT32 size{};
                if (SUCCEEDED(currentType->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &bytes, &size)) &&
                    bytes != nullptr && size != 0)
                {
                    try
                    {
                        config = tabletdisplay::video::ParseH264CodecConfig(
                            std::span<const std::uint8_t>(bytes, size));
                    }
                    catch (...)
                    {
                        CoTaskMemFree(bytes);
                        throw;
                    }
                    CoTaskMemFree(bytes);
                }
            }
            if (!config.has_value() &&
                tabletdisplay::video::AnnexBContainsNalType(accessUnit, 7) &&
                tabletdisplay::video::AnnexBContainsNalType(accessUnit, 8))
            {
                config = tabletdisplay::video::ParseH264CodecConfig(accessUnit);
            }
        }

        [[nodiscard]] std::optional<EncodedAccessUnit> TryReadOutput()
        {
            MFT_OUTPUT_STREAM_INFO streamInfo{};
            CheckHr(transform->GetOutputStreamInfo(outputId, &streamInfo), "GetOutputStreamInfo");
            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = outputId;
            ComPtr<IMFSample> allocatedSample;
            if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
            {
                CheckHr(MFCreateSample(&allocatedSample), "MFCreateSample(output)");
                ComPtr<IMFMediaBuffer> buffer;
                const auto bufferSize = std::max<DWORD>(streamInfo.cbSize, options.width * options.height);
                CheckHr(MFCreateMemoryBuffer(bufferSize, &buffer), "MFCreateMemoryBuffer(output)");
                CheckHr(allocatedSample->AddBuffer(buffer.Get()), "AddBuffer(output)");
                output.pSample = allocatedSample.Get();
            }

            DWORD status{};
            auto hr = transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) output.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return std::nullopt;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                ComPtr<IMFMediaType> changedType;
                CheckHr(transform->GetOutputAvailableType(outputId, 0, &changedType), "GetOutputAvailableType");
                CheckHr(transform->SetOutputType(outputId, changedType.Get(), 0), "SetOutputType(stream change)");
                if (asynchronous) return std::nullopt;
                return TryReadOutput();
            }
            CheckHr(hr, "ProcessOutput");

            ComPtr<IMFSample> sample = output.pSample;
            if (!sample) throw std::runtime_error("H.264 MFT returned no output sample");
            ComPtr<IMFMediaBuffer> buffer;
            CheckHr(sample->ConvertToContiguousBuffer(&buffer), "ConvertToContiguousBuffer");
            BYTE* bytes{};
            DWORD length{};
            CheckHr(buffer->Lock(&bytes, nullptr, &length), "Lock output buffer");
            std::vector<std::uint8_t> raw(bytes, bytes + length);
            CheckHr(buffer->Unlock(), "Unlock output buffer");

            RefreshCodecConfig(raw);
            const auto lengthSize = config.has_value() ? config->nalLengthSize : static_cast<std::uint8_t>(4);
            auto annexB = tabletdisplay::video::NormalizeH264AccessUnit(raw, lengthSize);
            RefreshCodecConfig(annexB);

            LONGLONG sampleTime{};
            if (FAILED(sample->GetSampleTime(&sampleTime))) sampleTime = 0;
            UINT32 cleanPoint{};
            const auto keyFrame =
                (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0) ||
                tabletdisplay::video::AnnexBContainsNalType(annexB, 5);
            return EncodedAccessUnit{
                std::move(annexB),
                static_cast<std::uint64_t>(std::max<LONGLONG>(0, sampleTime) / 10),
                keyFrame
            };
        }

        [[nodiscard]] std::vector<EncodedAccessUnit> Encode(
            std::span<const std::uint8_t> nv12,
            std::uint64_t presentationTimeUs,
            bool forceKeyFrame)
        {
            if (forceKeyFrame) SetCodecBool(codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, true);
            auto sample = MakeInputSample(nv12, presentationTimeUs);
            std::vector<EncodedAccessUnit> output;
            if (asynchronous)
            {
                WaitForEvent(METransformNeedInput);
                CheckHr(transform->ProcessInput(inputId, sample.Get(), 0), "ProcessInput(async)");
                for (;;)
                {
                    WaitForEvent(METransformHaveOutput);
                    auto encoded = TryReadOutput();
                    if (encoded.has_value())
                    {
                        output.push_back(std::move(*encoded));
                        break;
                    }
                }
                return output;
            }

            auto hr = transform->ProcessInput(inputId, sample.Get(), 0);
            while (hr == MF_E_NOTACCEPTING)
            {
                auto encoded = TryReadOutput();
                if (!encoded.has_value()) throw std::runtime_error("H.264 MFT rejected input without output");
                output.push_back(std::move(*encoded));
                hr = transform->ProcessInput(inputId, sample.Get(), 0);
            }
            CheckHr(hr, "ProcessInput");
            while (auto encoded = TryReadOutput()) output.push_back(std::move(*encoded));
            return output;
        }

        [[nodiscard]] std::vector<EncodedAccessUnit> Drain()
        {
            CheckHr(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "MFT drain");
            std::vector<EncodedAccessUnit> output;
            if (asynchronous)
            {
                for (;;)
                {
                    ComPtr<IMFMediaEvent> event;
                    CheckHr(events->GetEvent(0, &event), "MFT drain event");
                    HRESULT status{};
                    CheckHr(event->GetStatus(&status), "MFT drain event status");
                    CheckHr(status, "MFT drain asynchronous status");
                    MediaEventType type{};
                    CheckHr(event->GetType(&type), "MFT drain event type");
                    if (type == METransformHaveOutput)
                    {
                        auto encoded = TryReadOutput();
                        if (encoded.has_value()) output.push_back(std::move(*encoded));
                    }
                    else if (type == METransformDrainComplete)
                    {
                        break;
                    }
                }
            }
            else
            {
                while (auto encoded = TryReadOutput()) output.push_back(std::move(*encoded));
            }
            return output;
        }

        EncoderOptions options;
        ComPtr<IMFTransform> transform;
        ComPtr<IMFMediaEventGenerator> events;
        ComPtr<ICodecAPI> codec;
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        ComPtr<IMFDXGIDeviceManager> deviceManager;
        ComPtr<ID3D11Texture2D> hardwareInputTexture;
        ComPtr<IMFMediaBuffer> hardwareInputBuffer;
        std::optional<tabletdisplay::video::H264CodecConfig> config;
        std::string name;
        DWORD inputId{};
        DWORD outputId{};
        bool hardware{};
        bool asynchronous{};
        std::uint32_t pendingNeedInput{};
        std::uint32_t pendingHaveOutput{};
        bool ownsCom{};
        bool mediaFoundationStarted{};
    };

    MediaFoundationH264Encoder::MediaFoundationH264Encoder(EncoderOptions options)
        : m_impl(std::make_unique<Impl>(options))
    {
    }

    MediaFoundationH264Encoder::~MediaFoundationH264Encoder() = default;

    std::vector<EncodedAccessUnit> MediaFoundationH264Encoder::Encode(
        std::span<const std::uint8_t> nv12,
        std::uint64_t presentationTimeUs,
        bool forceKeyFrame)
    {
        return m_impl->Encode(nv12, presentationTimeUs, forceKeyFrame);
    }

    std::vector<EncodedAccessUnit> MediaFoundationH264Encoder::Drain()
    {
        return m_impl->Drain();
    }

    const std::string& MediaFoundationH264Encoder::Name() const noexcept
    {
        return m_impl->name;
    }

    bool MediaFoundationH264Encoder::IsHardware() const noexcept
    {
        return m_impl->hardware;
    }

    const tabletdisplay::video::H264CodecConfig* MediaFoundationH264Encoder::CodecConfig() const noexcept
    {
        return m_impl->config.has_value() ? &*m_impl->config : nullptr;
    }
}
