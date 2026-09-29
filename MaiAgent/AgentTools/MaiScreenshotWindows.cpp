#include "MaiScreenshot.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <objidl.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kMaximumWidth = 2560;
constexpr UINT kMaximumHeight = 1600;
constexpr DWORD kCaptureTimeoutMs = 2000;

MaiError hresultError(const char* operation, HRESULT result) {
    char code[16] = {};
    std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
    return MaiError::make(MaiErrorCode::Internal,
                          std::string(operation) + " failed (0x" + code + ")");
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), length);
    return wide;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                          length, nullptr, nullptr);
    return utf8;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return text;
}

struct WindowSearch {
    std::wstring expected;
    std::vector<HWND> exact;
    std::vector<HWND> partial;
};

bool visibleWindowTitle(HWND window, std::wstring& title) {
    if (!::IsWindowVisible(window) || ::IsIconic(window)) return false;
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0) return false;
    title.assign(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) return false;
    title.resize(static_cast<std::size_t>(copied));
    return true;
}

BOOL CALLBACK collectMatchingWindows(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    std::wstring title;
    if (!visibleWindowTitle(window, title)) return TRUE;
    const std::wstring folded = lower(title);
    if (folded == search.expected) {
        search.exact.push_back(window);
    } else if (folded.find(search.expected) != std::wstring::npos) {
        search.partial.push_back(window);
    }
    return TRUE;
}

BOOL CALLBACK collectVisibleWindowTitles(HWND window, LPARAM parameter) {
    auto& titles = *reinterpret_cast<std::vector<std::string>*>(parameter);
    std::wstring title;
    if (visibleWindowTitle(window, title)) {
        std::string utf8 = narrow(title);
        if (!utf8.empty()) titles.push_back(std::move(utf8));
    }
    return TRUE;
}

MaiError findWindow(const std::string& titleUtf8, HWND& window) {
    const std::wstring title = widen(titleUtf8);
    if (title.empty()) {
        return MaiError::make(MaiErrorCode::InvalidInput,
                              "windowTitle is empty or is not valid UTF-8");
    }
    WindowSearch search;
    search.expected = lower(title);
    ::EnumWindows(collectMatchingWindows, reinterpret_cast<LPARAM>(&search));
    const std::vector<HWND>& matches = search.exact.empty() ? search.partial : search.exact;
    if (matches.empty()) {
        return MaiError::make(MaiErrorCode::NotFound,
                              "no visible top-level window matches windowTitle");
    }
    if (matches.size() != 1) {
        return MaiError::make(
            MaiErrorCode::InvalidInput,
            "windowTitle matches more than one visible window; use a more exact title");
    }
    window = matches.front();
    return {};
}

MaiError createDefaultDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
    const HRESULT result = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.GetAddressOf(), &selected,
        context.GetAddressOf());
    if (FAILED(result)) return hresultError("D3D11CreateDevice", result);
    return {};
}

MaiError copyTextureToMemory(ID3D11Device* device, ID3D11DeviceContext* context,
                             ID3D11Texture2D* texture, std::vector<std::uint8_t>& pixels,
                             UINT& width, UINT& height) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return MaiError::make(MaiErrorCode::NotSupported,
                              "DXGI returned unsupported desktop texture format " +
                                  std::to_string(static_cast<int>(description.Format)));
    }

    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&stagingDescription, nullptr, staging.GetAddressOf());
    if (FAILED(result)) return hresultError("ID3D11Device::CreateTexture2D", result);

    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) return hresultError("ID3D11DeviceContext::Map", result);

    width = description.Width;
    height = description.Height;
    const std::size_t destinationRowBytes = static_cast<std::size_t>(width) * 4;
    pixels.resize(destinationRowBytes * height);
    for (UINT row = 0; row < height; ++row) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch * row;
        auto* destination = pixels.data() + destinationRowBytes * row;
        std::memcpy(destination, source, destinationRowBytes);
        for (UINT column = 0; column < width; ++column) destination[column * 4 + 3] = 0xFF;
    }
    context->Unmap(staging.Get(), 0);
    return {};
}

class ComApartment {
public:
    ComApartment() : mResult(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(mResult)) ::CoUninitialize();
    }
    HRESULT result() const {
        return mResult;
    }

private:
    HRESULT mResult;
};

MaiError encodePng(const std::vector<std::uint8_t>& pixels, UINT sourceWidth, UINT sourceHeight,
                   MaiScreenshot& screenshot) {
    ComApartment apartment;
    if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
        return hresultError("CoInitializeEx", apartment.result());
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) return hresultError("CoCreateInstance(WICImagingFactory)", result);

    if (pixels.size() > static_cast<std::size_t>(UINT_MAX)) {
        return MaiError::make(MaiErrorCode::Internal, "captured screen is too large to encode");
    }
    ComPtr<IWICBitmap> bitmap;
    result = factory->CreateBitmapFromMemory(
        sourceWidth, sourceHeight, GUID_WICPixelFormat32bppBGRA, sourceWidth * 4,
        static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()), bitmap.GetAddressOf());
    if (FAILED(result)) return hresultError("IWICImagingFactory::CreateBitmapFromMemory", result);

    ComPtr<IWICBitmapSource> source;
    result = bitmap.As(&source);
    if (FAILED(result)) return hresultError("IWICBitmap::QueryInterface", result);

    UINT width = 0;
    UINT height = 0;
    result = source->GetSize(&width, &height);
    if (FAILED(result)) return hresultError("IWICBitmapSource::GetSize", result);
    if (width > kMaximumWidth || height > kMaximumHeight) {
        const double scale = std::min(static_cast<double>(kMaximumWidth) / width,
                                      static_cast<double>(kMaximumHeight) / height);
        const UINT scaledWidth = std::max(1u, static_cast<UINT>(width * scale));
        const UINT scaledHeight = std::max(1u, static_cast<UINT>(height * scale));
        ComPtr<IWICBitmapScaler> scaler;
        result = factory->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(result)) return hresultError("IWICImagingFactory::CreateBitmapScaler", result);
        result = scaler->Initialize(source.Get(), scaledWidth, scaledHeight,
                                    WICBitmapInterpolationModeFant);
        if (FAILED(result)) return hresultError("IWICBitmapScaler::Initialize", result);
        result = scaler.As(&source);
        if (FAILED(result)) return hresultError("IWICBitmapScaler::QueryInterface", result);
        width = scaledWidth;
        height = scaledHeight;
    }

    ComPtr<IStream> stream;
    result = ::CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(result)) return hresultError("CreateStreamOnHGlobal", result);
    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(result)) return hresultError("IWICImagingFactory::CreateEncoder", result);
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) return hresultError("IWICBitmapEncoder::Initialize", result);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (FAILED(result)) return hresultError("IWICBitmapEncoder::CreateNewFrame", result);
    result = frame->Initialize(properties.Get());
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::Initialize", result);
    result = frame->SetSize(width, height);
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::SetSize", result);
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    result = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::SetPixelFormat", result);
    result = frame->WriteSource(source.Get(), nullptr);
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::WriteSource", result);
    result = frame->Commit();
    if (FAILED(result)) return hresultError("IWICBitmapFrameEncode::Commit", result);
    result = encoder->Commit();
    if (FAILED(result)) return hresultError("IWICBitmapEncoder::Commit", result);

    HGLOBAL storage = nullptr;
    result = ::GetHGlobalFromStream(stream.Get(), &storage);
    if (FAILED(result)) return hresultError("GetHGlobalFromStream", result);
    const SIZE_T size = ::GlobalSize(storage);
    const void* data = ::GlobalLock(storage);
    if (data == nullptr || size == 0) {
        if (data != nullptr) ::GlobalUnlock(storage);
        return MaiError::make(MaiErrorCode::Internal, "WIC produced an empty PNG");
    }
    const auto* first = static_cast<const std::uint8_t*>(data);
    screenshot.pngBytes.assign(first, first + size);
    ::GlobalUnlock(storage);
    screenshot.width = static_cast<int>(width);
    screenshot.height = static_cast<int>(height);
    return {};
}

MaiError captureGraphicsItem(const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item,
                             MaiScreenshot& screenshot) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    MaiError error = createDefaultDevice(device, context);
    if (error.hasError()) return error;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT result = device.As(&dxgiDevice);
    if (FAILED(result)) return hresultError("ID3D11Device::QueryInterface", result);
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice captureDevice{nullptr};
    result = ::CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(captureDevice)));
    if (FAILED(result)) return hresultError("CreateDirect3D11DeviceFromDXGIDevice", result);

    using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
    using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
    const auto size = item.Size();
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
    auto session = pool.CreateCaptureSession(item);
    winrt::handle arrived(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!arrived) {
        return MaiError::make(MaiErrorCode::Internal,
                              "CreateEvent failed while starting native capture");
    }
    const auto token = pool.FrameArrived(
        [&arrived](const Direct3D11CaptureFramePool&,
                   const winrt::Windows::Foundation::IInspectable&) { ::SetEvent(arrived.get()); });
    session.StartCapture();
    const DWORD wait = ::WaitForSingleObject(arrived.get(), kCaptureTimeoutMs);
    pool.FrameArrived(token);
    if (wait != WAIT_OBJECT_0) {
        session.Close();
        pool.Close();
        return MaiError::make(MaiErrorCode::Internal,
                              "Windows Graphics Capture timed out waiting for a frame");
    }

    auto frame = pool.TryGetNextFrame();
    if (!frame) {
        session.Close();
        pool.Close();
        return MaiError::make(MaiErrorCode::Internal, "Windows Graphics Capture returned no frame");
    }
    using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
    const auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())));
    std::vector<std::uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    error = copyTextureToMemory(device.Get(), context.Get(), texture.Get(), pixels, width, height);
    frame.Close();
    session.Close();
    pool.Close();
    if (error.hasError()) return error;
    return encodePng(pixels, width, height, screenshot);
}

MaiError captureDisplay(MaiScreenshot& screenshot) {
    POINT pointer{};
    if (!::GetCursorPos(&pointer)) {
        return MaiError::make(MaiErrorCode::Internal, "GetCursorPos failed");
    }
    const HMONITOR monitor = ::MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
        if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            return MaiError::make(MaiErrorCode::NotSupported,
                                  "Windows Graphics Capture is not supported on this system");
        }
        const auto interop =
            winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForMonitor(
            monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        return captureGraphicsItem(item, screenshot);
    } catch (const winrt::hresult_error& exception) {
        return hresultError("Windows Graphics Capture", exception.code());
    } catch (...) {
        return MaiError::make(MaiErrorCode::Internal,
                              "Windows Graphics Capture failed unexpectedly");
    }
}

MaiError captureWindow(const std::string& title, MaiScreenshot& screenshot) {
    HWND window = nullptr;
    MaiError error = findWindow(title, window);
    if (error.hasError()) return error;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
        if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            return MaiError::make(MaiErrorCode::NotSupported,
                                  "Windows Graphics Capture is not supported on this system");
        }
        const auto interop =
            winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(),
                                                      winrt::put_abi(item)));
        return captureGraphicsItem(item, screenshot);
    } catch (const winrt::hresult_error& exception) {
        return hresultError("Windows Graphics Capture", exception.code());
    } catch (...) {
        return MaiError::make(MaiErrorCode::Internal,
                              "Windows Graphics Capture failed unexpectedly");
    }
}

}  // namespace

bool maiIsScreenshotSupported() {
    return true;
}

MaiError maiCaptureScreenshot(const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
    maiAssertBlockingAllowed("maiCaptureScreenshot");
    screenshot = MaiScreenshot{};
    if (request.target == MaiScreenshotTarget::WindowByTitle) {
        return captureWindow(request.windowTitle, screenshot);
    }
    return captureDisplay(screenshot);
}

MaiError maiListCaptureWindows(std::vector<std::string>& titles) {
    titles.clear();
    if (!::EnumWindows(collectVisibleWindowTitles, reinterpret_cast<LPARAM>(&titles))) {
        return MaiError::make(MaiErrorCode::Internal, "EnumWindows failed");
    }
    std::sort(titles.begin(), titles.end());
    return {};
}
