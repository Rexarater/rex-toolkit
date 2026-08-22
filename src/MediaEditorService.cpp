#include "CacheManager.h"
#include "MediaEditorService.h"

#include <gdiplus.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <mutex>
#include <new>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr size_t kMaximumUndoStates = 80;
constexpr UINT kMaximumImageDimension = 32768;
constexpr unsigned long long kMaximumImageBufferBytes = 256ULL * 1024ULL * 1024ULL;

class ScopedCom
{
public:
    ScopedCom()
    {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    ~ScopedCom()
    {
        if (SUCCEEDED(result_))
        {
            CoUninitialize();
        }
    }

private:
    HRESULT result_ = E_FAIL;
};

std::wstring Lower(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::wstring HResultText(HRESULT result)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (length == 0 || !buffer)
    {
        std::wostringstream text;
        text << L"Error 0x" << std::hex << static_cast<unsigned long>(result);
        return text.str();
    }

    std::wstring text(buffer, length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.'))
    {
        text.pop_back();
    }
    return text;
}

bool CreateWicFactory(ComPtr<IWICImagingFactory>& factory, std::wstring& errorMessage)
{
    const HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        errorMessage = L"The Windows image engine could not start: " + HResultText(result) + L".";
        return false;
    }
    return true;
}

bool SafePixelBufferSize(UINT width, UINT height, UINT& stride, size_t& byteCount)
{
    if (width == 0 || height == 0 || width > kMaximumImageDimension || height > kMaximumImageDimension)
    {
        return false;
    }
    const unsigned long long stride64 = static_cast<unsigned long long>(width) * 4ULL;
    const unsigned long long bytes64 = stride64 * static_cast<unsigned long long>(height);
    if (stride64 > std::numeric_limits<UINT>::max() ||
        bytes64 > std::numeric_limits<UINT>::max() ||
        bytes64 > kMaximumImageBufferBytes)
    {
        return false;
    }
    stride = static_cast<UINT>(stride64);
    byteCount = static_cast<size_t>(bytes64);
    return true;
}

bool DecodeImageFile(
    const std::filesystem::path& path,
    MediaEditorImageBuffer& image,
    std::wstring& errorMessage)
{
    ScopedCom com;
    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, errorMessage))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result = factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(result))
    {
        errorMessage = L"This image could not be opened. It may be damaged or use an unsupported format.";
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result))
    {
        errorMessage = L"This image does not contain a readable frame.";
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    frame->GetSize(&width, &height);
    UINT stride = 0;
    size_t byteCount = 0;
    if (!SafePixelBufferSize(width, height, stride, byteCount))
    {
        errorMessage = L"This image is too large to edit safely.";
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result))
    {
        errorMessage = L"The image could not be prepared for editing.";
        return false;
    }
    result = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result))
    {
        errorMessage = L"This image format cannot be converted for editing.";
        return false;
    }

    image.width = width;
    image.height = height;
    image.stride = stride;
    try
    {
        image.pixels.resize(byteCount);
    }
    catch (const std::bad_alloc&)
    {
        image = {};
        errorMessage = L"There is not enough memory to open this image.";
        return false;
    }

    result = converter->CopyPixels(
        nullptr,
        stride,
        static_cast<UINT>(image.pixels.size()),
        image.pixels.data());
    if (FAILED(result))
    {
        image = {};
        errorMessage = L"The image pixels could not be decoded.";
        return false;
    }
    return true;
}

const GUID& ContainerGuid(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Jpg:
        return GUID_ContainerFormatJpeg;
    case ImageFormat::Webp:
        return GUID_ContainerFormatWebp;
    case ImageFormat::Bmp:
        return GUID_ContainerFormatBmp;
    case ImageFormat::Png:
    default:
        return GUID_ContainerFormatPng;
    }
}

void SetJpegQuality(IPropertyBag2* properties, float quality)
{
    if (!properties)
    {
        return;
    }
    PROPBAG2 option {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value {};
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    properties->Write(1, &option, &value);
    VariantClear(&value);
}

bool EncodeImageFile(
    const MediaEditorImageBuffer& image,
    const std::filesystem::path& path,
    ImageFormat format,
    std::wstring& errorMessage)
{
    if (!image.IsValid())
    {
        errorMessage = L"There is no finished image to save.";
        return false;
    }

    ScopedCom com;
    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, errorMessage))
    {
        return false;
    }

    ComPtr<IWICStream> stream;
    HRESULT result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
    {
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (FAILED(result))
    {
        errorMessage = L"The selected file could not be created. Choose another location.";
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(ContainerGuid(format), nullptr, &encoder);
    if (FAILED(result))
    {
        errorMessage = L"This image format is not available on this Windows installation.";
        return false;
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result))
    {
        errorMessage = L"The image encoder could not start.";
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = encoder->CreateNewFrame(&frame, &properties);
    if (FAILED(result))
    {
        errorMessage = L"The output image frame could not be created.";
        return false;
    }
    if (format == ImageFormat::Jpg)
    {
        SetJpegQuality(properties.Get(), 0.92f);
    }
    result = frame->Initialize(properties.Get());
    if (SUCCEEDED(result)) result = frame->SetSize(image.width, image.height);

    WICPixelFormatGUID pixelFormat = format == ImageFormat::Jpg
        ? GUID_WICPixelFormat24bppBGR
        : GUID_WICPixelFormat32bppPBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(result))
    {
        errorMessage = L"The output image format could not be configured.";
        return false;
    }

    ComPtr<IWICBitmap> sourceBitmap;
    result = factory->CreateBitmapFromMemory(
        image.width,
        image.height,
        GUID_WICPixelFormat32bppPBGRA,
        image.stride,
        static_cast<UINT>(image.pixels.size()),
        const_cast<BYTE*>(image.pixels.data()),
        &sourceBitmap);
    if (FAILED(result))
    {
        errorMessage = L"The finished image could not be prepared for saving.";
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    IWICBitmapSource* source = sourceBitmap.Get();
    if (format == ImageFormat::Jpg)
    {
        result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result))
        {
            result = converter->Initialize(
                sourceBitmap.Get(),
                GUID_WICPixelFormat24bppBGR,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }
        if (FAILED(result))
        {
            errorMessage = L"The image could not be flattened for JPG output.";
            return false;
        }
        source = converter.Get();
    }

    result = frame->WriteSource(source, nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    if (FAILED(result))
    {
        errorMessage = L"The image could not be written to the selected file.";
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

HGLOBAL EncodeClipboardPng(const MediaEditorImageBuffer& image)
{
    if (!image.IsValid())
    {
        return nullptr;
    }

    ScopedCom com;
    std::wstring ignoredError;
    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, ignoredError))
    {
        return nullptr;
    }

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    {
        return nullptr;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    HRESULT result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(result)) result = frame->Initialize(properties.Get());
    if (SUCCEEDED(result)) result = frame->SetSize(image.width, image.height);
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixelFormat);

    ComPtr<IWICBitmap> sourceBitmap;
    if (SUCCEEDED(result))
    {
        result = factory->CreateBitmapFromMemory(
            image.width,
            image.height,
            GUID_WICPixelFormat32bppPBGRA,
            image.stride,
            static_cast<UINT>(image.pixels.size()),
            const_cast<BYTE*>(image.pixels.data()),
            &sourceBitmap);
    }
    if (SUCCEEDED(result)) result = frame->WriteSource(sourceBitmap.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    if (FAILED(result))
    {
        return nullptr;
    }

    STATSTG statistics {};
    HGLOBAL streamMemory = nullptr;
    if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME)) ||
        FAILED(GetHGlobalFromStream(stream.Get(), &streamMemory)) ||
        !streamMemory ||
        statistics.cbSize.QuadPart == 0 ||
        statistics.cbSize.QuadPart > std::numeric_limits<SIZE_T>::max())
    {
        return nullptr;
    }

    const SIZE_T encodedSize = static_cast<SIZE_T>(statistics.cbSize.QuadPart);
    const BYTE* encoded = static_cast<const BYTE*>(GlobalLock(streamMemory));
    if (!encoded)
    {
        return nullptr;
    }

    HGLOBAL clipboardMemory = GlobalAlloc(GMEM_MOVEABLE, encodedSize);
    BYTE* clipboardBytes = clipboardMemory
        ? static_cast<BYTE*>(GlobalLock(clipboardMemory))
        : nullptr;
    if (!clipboardBytes)
    {
        if (clipboardMemory) GlobalFree(clipboardMemory);
        GlobalUnlock(streamMemory);
        return nullptr;
    }
    std::copy_n(encoded, encodedSize, clipboardBytes);
    GlobalUnlock(clipboardMemory);
    GlobalUnlock(streamMemory);
    return clipboardMemory;
}

HGLOBAL CreateClipboardDib(const MediaEditorImageBuffer& image, bool version5)
{
    if (!image.IsValid())
    {
        return nullptr;
    }

    const SIZE_T headerSize = version5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    const SIZE_T pixelBytes = static_cast<SIZE_T>(image.width) * 4ULL * image.height;
    if (pixelBytes > std::numeric_limits<SIZE_T>::max() - headerSize ||
        pixelBytes > std::numeric_limits<DWORD>::max())
    {
        return nullptr;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, headerSize + pixelBytes);
    BYTE* destination = memory ? static_cast<BYTE*>(GlobalLock(memory)) : nullptr;
    if (!destination)
    {
        if (memory) GlobalFree(memory);
        return nullptr;
    }
    ZeroMemory(destination, headerSize);
    if (version5)
    {
        auto* header = reinterpret_cast<BITMAPV5HEADER*>(destination);
        header->bV5Size = sizeof(BITMAPV5HEADER);
        header->bV5Width = static_cast<LONG>(image.width);
        header->bV5Height = -static_cast<LONG>(image.height);
        header->bV5Planes = 1;
        header->bV5BitCount = 32;
        header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
        header->bV5RedMask = 0x00FF0000;
        header->bV5GreenMask = 0x0000FF00;
        header->bV5BlueMask = 0x000000FF;
        header->bV5AlphaMask = 0xFF000000;
        header->bV5CSType = LCS_sRGB;
        header->bV5Intent = LCS_GM_IMAGES;
    }
    else
    {
        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(destination);
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = static_cast<LONG>(image.width);
        header->biHeight = -static_cast<LONG>(image.height);
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(pixelBytes);
    }

    BYTE* pixelDestination = destination + headerSize;
    for (UINT y = 0; y < image.height; ++y)
    {
        std::copy_n(
            image.pixels.data() + static_cast<size_t>(image.stride) * y,
            static_cast<size_t>(image.width) * 4ULL,
            pixelDestination + static_cast<size_t>(image.width) * 4ULL * y);
    }
    GlobalUnlock(memory);
    return memory;
}

void PremultiplyPixels(MediaEditorImageBuffer& image)
{
    bool anyAlpha = false;
    for (size_t index = 3; index < image.pixels.size(); index += 4)
    {
        if (image.pixels[index] != 0)
        {
            anyAlpha = true;
            break;
        }
    }

    for (size_t index = 0; index + 3 < image.pixels.size(); index += 4)
    {
        BYTE& blue = image.pixels[index + 0];
        BYTE& green = image.pixels[index + 1];
        BYTE& red = image.pixels[index + 2];
        BYTE& alpha = image.pixels[index + 3];
        if (!anyAlpha)
        {
            alpha = 255;
        }
        if (alpha < 255)
        {
            blue = static_cast<BYTE>((static_cast<unsigned int>(blue) * alpha + 127) / 255);
            green = static_cast<BYTE>((static_cast<unsigned int>(green) * alpha + 127) / 255);
            red = static_cast<BYTE>((static_cast<unsigned int>(red) * alpha + 127) / 255);
        }
    }
}

std::optional<MediaEditorImageBuffer> ImageFromHBitmap(HBITMAP bitmap, std::wstring& errorMessage)
{
    if (!bitmap)
    {
        errorMessage = L"The clipboard image is unavailable.";
        return std::nullopt;
    }
    BITMAP description {};
    if (GetObjectW(bitmap, sizeof(description), &description) != sizeof(description) ||
        description.bmWidth <= 0 || description.bmHeight == 0)
    {
        errorMessage = L"The clipboard image is invalid.";
        return std::nullopt;
    }

    MediaEditorImageBuffer image;
    image.width = static_cast<UINT>(description.bmWidth);
    image.height = static_cast<UINT>(std::abs(description.bmHeight));
    size_t bytes = 0;
    if (!SafePixelBufferSize(image.width, image.height, image.stride, bytes))
    {
        errorMessage = L"The clipboard image is too large to edit safely.";
        return std::nullopt;
    }
    try
    {
        image.pixels.resize(bytes);
    }
    catch (const std::bad_alloc&)
    {
        errorMessage = L"There is not enough memory to paste this image.";
        return std::nullopt;
    }

    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(image.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(image.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    const int copied = GetDIBits(
        screen,
        bitmap,
        0,
        image.height,
        image.pixels.data(),
        &info,
        DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (copied != static_cast<int>(image.height))
    {
        errorMessage = L"The clipboard image pixels could not be read.";
        return std::nullopt;
    }
    PremultiplyPixels(image);
    return image;
}

std::optional<MediaEditorImageBuffer> ImageFromDib(HGLOBAL memory, std::wstring& errorMessage)
{
    if (!memory)
    {
        return std::nullopt;
    }
    const SIZE_T memorySize = GlobalSize(memory);
    const BYTE* bytes = static_cast<const BYTE*>(GlobalLock(memory));
    if (!bytes || memorySize < sizeof(BITMAPINFOHEADER))
    {
        if (bytes) GlobalUnlock(memory);
        errorMessage = L"The clipboard image data is invalid.";
        return std::nullopt;
    }

    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(bytes);
    const LONG widthValue = header->biWidth;
    const LONG heightValue = header->biHeight;
    if (header->biSize < sizeof(BITMAPINFOHEADER) || widthValue <= 0 || heightValue == 0 ||
        (header->biBitCount != 24 && header->biBitCount != 32) ||
        (header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS))
    {
        GlobalUnlock(memory);
        errorMessage = L"The clipboard image uses an unsupported pixel format.";
        return std::nullopt;
    }

    MediaEditorImageBuffer image;
    image.width = static_cast<UINT>(widthValue);
    image.height = static_cast<UINT>(std::abs(heightValue));
    size_t destinationBytes = 0;
    if (!SafePixelBufferSize(image.width, image.height, image.stride, destinationBytes))
    {
        GlobalUnlock(memory);
        errorMessage = L"The clipboard image is too large to edit safely.";
        return std::nullopt;
    }

    size_t pixelOffset = header->biSize;
    if (header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == BI_BITFIELDS)
    {
        pixelOffset += 3 * sizeof(DWORD);
    }
    const UINT colorCount = header->biClrUsed != 0
        ? header->biClrUsed
        : (header->biBitCount <= 8 ? (1U << header->biBitCount) : 0U);
    pixelOffset += static_cast<size_t>(colorCount) * sizeof(RGBQUAD);
    const size_t sourceStride = ((static_cast<size_t>(image.width) * header->biBitCount + 31U) / 32U) * 4U;
    const size_t required = pixelOffset + sourceStride * image.height;
    if (required > memorySize)
    {
        GlobalUnlock(memory);
        errorMessage = L"The clipboard image data is incomplete.";
        return std::nullopt;
    }

    try
    {
        image.pixels.assign(destinationBytes, 0);
    }
    catch (const std::bad_alloc&)
    {
        GlobalUnlock(memory);
        errorMessage = L"There is not enough memory to paste this image.";
        return std::nullopt;
    }

    const bool topDown = heightValue < 0;
    for (UINT y = 0; y < image.height; ++y)
    {
        const UINT sourceY = topDown ? y : image.height - 1 - y;
        const BYTE* source = bytes + pixelOffset + sourceStride * sourceY;
        BYTE* destination = image.pixels.data() + static_cast<size_t>(image.stride) * y;
        for (UINT x = 0; x < image.width; ++x)
        {
            destination[x * 4 + 0] = source[x * (header->biBitCount / 8) + 0];
            destination[x * 4 + 1] = source[x * (header->biBitCount / 8) + 1];
            destination[x * 4 + 2] = source[x * (header->biBitCount / 8) + 2];
            destination[x * 4 + 3] = header->biBitCount == 32
                ? source[x * 4 + 3]
                : 255;
        }
    }
    GlobalUnlock(memory);
    PremultiplyPixels(image);
    return image;
}

std::vector<std::filesystem::path> ClipboardFilePaths(HDROP drop)
{
    std::vector<std::filesystem::path> paths;
    if (!drop)
    {
        return paths;
    }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index)
    {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, buffer.data(), length + 1) != 0)
        {
            buffer.resize(length);
            paths.emplace_back(buffer);
        }
    }
    return paths;
}

MediaEditorTextLayout MeasureTextLayout(
    Gdiplus::Graphics& graphics,
    const std::wstring& text,
    const std::wstring& fontFamily,
    Gdiplus::REAL width,
    Gdiplus::REAL height)
{
    MediaEditorTextLayout result;
    if (text.empty() || width <= 1.0f || height <= 1.0f)
    {
        return result;
    }
    const Gdiplus::REAL minimumSize = std::max<Gdiplus::REAL>(
        2.0f,
        std::min(width, height) * 0.12f);
    result.fontSize = minimumSize;

    Gdiplus::FontFamily preferredFamily(fontFamily.c_str());
    const Gdiplus::FontFamily* family =
        preferredFamily.GetLastStatus() == Gdiplus::Ok
            ? &preferredFamily
            : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::StringFormat format;
    format.SetFormatFlags(
        Gdiplus::StringFormatFlagsMeasureTrailingSpaces |
        Gdiplus::StringFormatFlagsNoClip);
    format.SetTrimming(Gdiplus::StringTrimmingNone);

    const auto fitsAt = [&](Gdiplus::REAL candidate)
    {
        Gdiplus::Font font(
            family,
            candidate,
            Gdiplus::FontStyleRegular,
            Gdiplus::UnitPixel);
        if (font.GetLastStatus() != Gdiplus::Ok ||
            font.GetHeight(&graphics) > height + 0.25f)
        {
            return false;
        }

        Gdiplus::RectF measured;
        INT fitted = 0;
        INT lines = 0;
        const Gdiplus::Status measuredStatus = graphics.MeasureString(
            text.c_str(),
            static_cast<INT>(text.size()),
            &font,
            Gdiplus::RectF(0.0f, 0.0f, width, height),
            &format,
            &measured,
            &fitted,
            &lines);
        return measuredStatus == Gdiplus::Ok &&
            fitted >= static_cast<INT>(text.size()) &&
            measured.Width <= width + 0.75f &&
            measured.Height <= height + 0.25f;
    };

    if (fitsAt(minimumSize))
    {
        result.fullyFits = true;
    }

    Gdiplus::REAL low = minimumSize;
    Gdiplus::REAL high = std::max(
        low,
        std::min<Gdiplus::REAL>(2048.0f, height));
    for (int pass = 0; pass < 15; ++pass)
    {
        const Gdiplus::REAL candidate = (low + high) * 0.5f;
        if (fitsAt(candidate))
        {
            low = candidate;
            result.fontSize = candidate;
            result.fullyFits = true;
        }
        else
        {
            high = candidate;
        }
    }
    return result;
}

void DrawTextBoxInternal(
    Gdiplus::Graphics& graphics,
    const MediaEditorTextBox& textBox)
{
    if (textBox.text.empty() ||
        textBox.bounds.Width() <= 1.0f ||
        textBox.bounds.Height() <= 1.0f)
    {
        return;
    }

    const int rotation =
        ((textBox.rotationQuarterTurns % 4) + 4) % 4;
    const bool sideways = (rotation % 2) != 0;
    const Gdiplus::REAL physicalWidth = textBox.bounds.Width();
    const Gdiplus::REAL physicalHeight = textBox.bounds.Height();
    const Gdiplus::REAL logicalWidth =
        sideways ? physicalHeight : physicalWidth;
    const Gdiplus::REAL logicalHeight =
        sideways ? physicalWidth : physicalHeight;
    const Gdiplus::REAL padding = std::clamp(
        std::min(logicalWidth, logicalHeight) * 0.035f,
        1.5f,
        18.0f);
    const Gdiplus::REAL contentWidth =
        std::max(1.0f, logicalWidth - padding * 2.0f);
    const Gdiplus::REAL contentHeight =
        std::max(1.0f, logicalHeight - padding * 2.0f);

    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.SetClip(
        Gdiplus::RectF(
            textBox.bounds.left,
            textBox.bounds.top,
            physicalWidth,
            physicalHeight),
        Gdiplus::CombineModeIntersect);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.TranslateTransform(
        (textBox.bounds.left + textBox.bounds.right) * 0.5f,
        (textBox.bounds.top + textBox.bounds.bottom) * 0.5f);
    graphics.RotateTransform(static_cast<Gdiplus::REAL>(rotation * 90));

    Gdiplus::FontFamily preferredFamily(textBox.fontFamily.c_str());
    const Gdiplus::FontFamily* family =
        preferredFamily.GetLastStatus() == Gdiplus::Ok
            ? &preferredFamily
            : Gdiplus::FontFamily::GenericSansSerif();
    const MediaEditorTextLayout textLayout =
        MeasureTextLayout(
            graphics,
            textBox.text,
            textBox.fontFamily,
            contentWidth,
            contentHeight);
    const Gdiplus::REAL fontSize = textLayout.fontSize;
    Gdiplus::Font font(
        family,
        fontSize,
        Gdiplus::FontStyleRegular,
        Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    format.SetTrimming(Gdiplus::StringTrimmingNone);
    const Gdiplus::RectF layoutRect(
        -logicalWidth * 0.5f + padding,
        -logicalHeight * 0.5f + padding,
        contentWidth,
        contentHeight);
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        static_cast<BYTE>(std::clamp(textBox.opacity, 0.0f, 1.0f) * 255.0f),
        GetRValue(textBox.color),
        GetGValue(textBox.color),
        GetBValue(textBox.color)));
    graphics.DrawString(
        textBox.text.c_str(),
        static_cast<INT>(textBox.text.size()),
        &font,
        layoutRect,
        &format,
        &brush);
    graphics.Restore(state);
}

std::wstring FileNameForMessage(const std::filesystem::path& path)
{
    const std::wstring name = path.filename().wstring();
    return name.empty() ? path.wstring() : name;
}
}

MediaEditorTextLayout MeasureMediaEditorTextLayout(
    Gdiplus::Graphics& graphics,
    const std::wstring& text,
    const std::wstring& fontFamily,
    float width,
    float height)
{
    return MeasureTextLayout(graphics, text, fontFamily, width, height);
}

void DrawMediaEditorTextBox(
    Gdiplus::Graphics& graphics,
    const MediaEditorTextBox& textBox)
{
    DrawTextBoxInternal(graphics, textBox);
}

void DrawMediaEditorTextBoxWithCaret(
    Gdiplus::Graphics& graphics,
    const MediaEditorTextBox& textBox,
    size_t caretIndex,
    COLORREF color,
    float thickness)
{
    if (textBox.bounds.Width() <= 1.0f ||
        textBox.bounds.Height() <= 1.0f ||
        textBox.rotationQuarterTurns % 4 != 0)
    {
        return;
    }

    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.SetClip(
        Gdiplus::RectF(
            textBox.bounds.left,
            textBox.bounds.top,
            textBox.bounds.Width(),
            textBox.bounds.Height()),
        Gdiplus::CombineModeIntersect);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    const Gdiplus::REAL width = textBox.bounds.Width();
    const Gdiplus::REAL height = textBox.bounds.Height();
    const Gdiplus::REAL padding = std::clamp(
        std::min(width, height) * 0.035f,
        1.5f,
        18.0f);
    const Gdiplus::REAL contentWidth =
        std::max(1.0f, width - padding * 2.0f);
    const Gdiplus::REAL contentHeight =
        std::max(1.0f, height - padding * 2.0f);
    const std::wstring layoutText = textBox.text.empty() ? L"M" : textBox.text;

    Gdiplus::FontFamily preferredFamily(textBox.fontFamily.c_str());
    const Gdiplus::FontFamily* family =
        preferredFamily.GetLastStatus() == Gdiplus::Ok
            ? &preferredFamily
            : Gdiplus::FontFamily::GenericSansSerif();
    const MediaEditorTextLayout textLayout = MeasureTextLayout(
        graphics,
        layoutText,
        textBox.fontFamily,
        contentWidth,
        contentHeight);
    Gdiplus::Font font(
        family,
        textLayout.fontSize,
        Gdiplus::FontStyleRegular,
        Gdiplus::UnitPixel);
    if (font.GetLastStatus() != Gdiplus::Ok)
    {
        graphics.Restore(state);
        return;
    }

    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    format.SetTrimming(Gdiplus::StringTrimmingNone);
    const Gdiplus::RectF layoutRect(
        textBox.bounds.left + padding,
        textBox.bounds.top + padding,
        contentWidth,
        contentHeight);
    if (!textBox.text.empty())
    {
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(
            static_cast<BYTE>(
                std::clamp(textBox.opacity, 0.0f, 1.0f) * 255.0f),
            GetRValue(textBox.color),
            GetGValue(textBox.color),
            GetBValue(textBox.color)));
        graphics.DrawString(
            textBox.text.c_str(),
            static_cast<INT>(textBox.text.size()),
            &font,
            layoutRect,
            &format,
            &textBrush);
    }
    const size_t boundedIndex = std::min(caretIndex, textBox.text.size());
    const auto isLineBreak = [&](size_t index)
    {
        return index < textBox.text.size() &&
            (textBox.text[index] == L'\r' || textBox.text[index] == L'\n');
    };
    const auto measureCharacter = [&](size_t index, Gdiplus::RectF& bounds)
    {
        if (index >= textBox.text.size() || isLineBreak(index))
        {
            return false;
        }
        Gdiplus::CharacterRange range(static_cast<INT>(index), 1);
        format.SetMeasurableCharacterRanges(1, &range);
        Gdiplus::Region region;
        if (graphics.MeasureCharacterRanges(
                textBox.text.c_str(),
                static_cast<INT>(textBox.text.size()),
                &font,
                layoutRect,
                &format,
                1,
                &region) != Gdiplus::Ok)
        {
            return false;
        }
        return region.GetBounds(&bounds, &graphics) == Gdiplus::Ok;
    };
    const auto countLineBreaks = [&](size_t begin, size_t end)
    {
        int count = 0;
        for (size_t index = begin; index < end; ++index)
        {
            if (textBox.text[index] == L'\r')
            {
                ++count;
                if (index + 1 < end && textBox.text[index + 1] == L'\n')
                {
                    ++index;
                }
            }
            else if (textBox.text[index] == L'\n')
            {
                ++count;
            }
        }
        return count;
    };

    const Gdiplus::REAL lineHeight = std::max<Gdiplus::REAL>(
        2.0f,
        font.GetHeight(&graphics));
    Gdiplus::REAL caretX = layoutRect.X;
    Gdiplus::REAL caretY = layoutRect.Y;
    Gdiplus::REAL caretHeight = lineHeight;
    Gdiplus::RectF characterBounds;
    if (boundedIndex < textBox.text.size() &&
        measureCharacter(boundedIndex, characterBounds))
    {
        caretX = characterBounds.X;
        caretY = characterBounds.Y;
        caretHeight = std::max(2.0f, characterBounds.Height);
    }
    else if (boundedIndex > 0 &&
        !isLineBreak(boundedIndex - 1) &&
        measureCharacter(boundedIndex - 1, characterBounds))
    {
        caretX = characterBounds.GetRight();
        caretY = characterBounds.Y;
        caretHeight = std::max(2.0f, characterBounds.Height);
    }
    else if (boundedIndex > 0)
    {
        size_t previousCharacter = boundedIndex;
        while (previousCharacter > 0)
        {
            --previousCharacter;
            if (!isLineBreak(previousCharacter))
            {
                break;
            }
        }
        const bool hasPreviousCharacter =
            previousCharacter < boundedIndex &&
            !isLineBreak(previousCharacter) &&
            measureCharacter(previousCharacter, characterBounds);
        const size_t breakStart = hasPreviousCharacter
            ? previousCharacter + 1
            : 0;
        const int lineBreaks = countLineBreaks(breakStart, boundedIndex);
        caretY = hasPreviousCharacter
            ? characterBounds.Y + lineHeight * lineBreaks
            : layoutRect.Y + lineHeight * lineBreaks;
    }

    caretX = std::clamp(
        caretX,
        layoutRect.X,
        layoutRect.GetRight() - 1.0f);
    caretY = std::clamp(
        caretY,
        layoutRect.Y,
        layoutRect.GetBottom() - 2.0f);
    caretHeight = std::clamp(
        caretHeight,
        2.0f,
        layoutRect.GetBottom() - caretY);

    Gdiplus::Pen pen(
        Gdiplus::Color(
            255,
            GetRValue(color),
            GetGValue(color),
            GetBValue(color)),
        std::max(1.0f, thickness));
    graphics.DrawLine(
        &pen,
        caretX,
        caretY,
        caretX,
        caretY + caretHeight);
    graphics.Restore(state);
}

MediaEditorMediaKind MediaTypeDetector::KindFromExtension(const std::filesystem::path& path)
{
    const std::wstring extension = Lower(path.extension().wstring());
    if (extension == L".png" || extension == L".jpg" || extension == L".jpeg" ||
        extension == L".webp" || extension == L".bmp")
    {
        return MediaEditorMediaKind::Image;
    }
    if (extension == L".mp4" || extension == L".mov" || extension == L".mkv" ||
        extension == L".webm" || extension == L".avi" || extension == L".m4v" ||
        extension == L".wmv" || extension == L".flv" || extension == L".mpeg" ||
        extension == L".mpg" || extension == L".ts" || extension == L".m2ts" ||
        extension == L".mts" || extension == L".3gp" || extension == L".3g2" ||
        extension == L".ogv" || extension == L".vob" || extension == L".mxf" ||
        extension == L".asf" || extension == L".f4v")
    {
        return MediaEditorMediaKind::Video;
    }
    return MediaEditorMediaKind::Unsupported;
}

bool MediaTypeDetector::IsSupportedImage(const std::filesystem::path& path)
{
    return KindFromExtension(path) == MediaEditorMediaKind::Image;
}

bool MediaTypeDetector::IsSupportedVideo(const std::filesystem::path& path)
{
    return KindFromExtension(path) == MediaEditorMediaKind::Video;
}

bool MediaTypeDetector::ValidateFile(
    const std::filesystem::path& path,
    MediaEditorMediaKind expectedKind,
    std::wstring& errorMessage)
{
    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError) ||
        !std::filesystem::is_regular_file(path, fileError))
    {
        errorMessage = L"The selected file no longer exists.";
        return false;
    }
    if (KindFromExtension(path) != expectedKind)
    {
        errorMessage = L"That file type is not supported by this editor.";
        return false;
    }
    if (expectedKind == MediaEditorMediaKind::Image)
    {
        MediaEditorImageBuffer image;
        return DecodeImageFile(path, image, errorMessage);
    }

    VideoCompressionService analyzer;
    std::atomic_bool cancel { false };
    const VideoAnalysis analysis = analyzer.Analyze(path, cancel, errorMessage);
    return analysis.durationSeconds > 0.0 && analysis.width > 0 && analysis.height > 0;
}

bool MediaEditorImageBuffer::IsValid() const
{
    return width > 0 && height > 0 && stride >= width * 4U &&
        pixels.size() >= static_cast<size_t>(stride) * height;
}

bool ClipboardMediaAvailability::HasCompatibleMedia() const
{
    return hasImagePixels || hasSupportedFiles;
}

std::wstring ClipboardMediaAvailability::Summary() const
{
    if (hasImagePixels || hasImageFile)
    {
        return L"An image is ready to paste.";
    }
    if (hasVideoFiles)
    {
        return videoFileCount > 1
            ? std::to_wstring(videoFileCount) + L" copied videos are ready to open."
            : L"A copied video is ready to open.";
    }
    return L"";
}

ClipboardMediaAvailability ClipboardMediaService::DetectCompatibleMedia() const
{
    ClipboardMediaAvailability availability;
    availability.hasImagePixels =
        IsClipboardFormatAvailable(CF_DIBV5) ||
        IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_BITMAP);
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(nullptr))
    {
        return availability;
    }

    HDROP drop = reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP));
    const std::vector<std::filesystem::path> paths = ClipboardFilePaths(drop);
    CloseClipboard();
    if (paths.size() == 1 && MediaTypeDetector::IsSupportedImage(paths.front()))
    {
        availability.hasSupportedFiles = true;
        availability.hasImageFile = true;
    }
    else if (!paths.empty() && std::all_of(
        paths.begin(), paths.end(),
        [](const std::filesystem::path& path) { return MediaTypeDetector::IsSupportedVideo(path); }))
    {
        availability.hasSupportedFiles = true;
        availability.hasVideoFiles = true;
        availability.videoFileCount = paths.size();
    }
    return availability;
}

std::optional<ClipboardMediaPayload> ClipboardMediaService::ReadCompatibleMedia(std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (!OpenClipboard(nullptr))
    {
        errorMessage = L"The clipboard is busy. Try again in a moment.";
        return std::nullopt;
    }

    ClipboardMediaPayload payload;
    if (IsClipboardFormatAvailable(CF_HDROP))
    {
        payload.files = ClipboardFilePaths(reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP)));
        const bool oneImage = payload.files.size() == 1 && MediaTypeDetector::IsSupportedImage(payload.files.front());
        const bool videos = !payload.files.empty() && std::all_of(
            payload.files.begin(), payload.files.end(),
            [](const std::filesystem::path& path) { return MediaTypeDetector::IsSupportedVideo(path); });
        if (oneImage || videos)
        {
            CloseClipboard();
            return payload;
        }
        payload.files.clear();
    }

    if (IsClipboardFormatAvailable(CF_DIBV5))
    {
        payload.image = ImageFromDib(GetClipboardData(CF_DIBV5), errorMessage);
    }
    if (!payload.image && IsClipboardFormatAvailable(CF_DIB))
    {
        payload.image = ImageFromDib(GetClipboardData(CF_DIB), errorMessage);
    }
    if (!payload.image && IsClipboardFormatAvailable(CF_BITMAP))
    {
        payload.image = ImageFromHBitmap(reinterpret_cast<HBITMAP>(GetClipboardData(CF_BITMAP)), errorMessage);
    }
    CloseClipboard();

    if (payload.image)
    {
        errorMessage.clear();
        return payload;
    }
    if (errorMessage.empty())
    {
        errorMessage = L"The clipboard does not contain a supported image or video.";
    }
    return std::nullopt;
}

bool ClipboardMediaService::CopyImage(
    const MediaEditorImageBuffer& image,
    std::wstring& errorMessage) const
{
    if (!image.IsValid())
    {
        errorMessage = L"There is no finished image to copy.";
        return false;
    }

    HGLOBAL pngMemory = EncodeClipboardPng(image);
    HGLOBAL dibMemory = CreateClipboardDib(image, false);
    HGLOBAL dibV5Memory = CreateClipboardDib(image, true);
    if (!pngMemory && !dibMemory && !dibV5Memory)
    {
        errorMessage = L"There is not enough memory to copy this image.";
        return false;
    }

    if (!OpenClipboard(nullptr))
    {
        if (pngMemory) GlobalFree(pngMemory);
        if (dibMemory) GlobalFree(dibMemory);
        if (dibV5Memory) GlobalFree(dibV5Memory);
        errorMessage = L"The clipboard is busy. Try again in a moment.";
        return false;
    }
    if (!EmptyClipboard())
    {
        CloseClipboard();
        if (pngMemory) GlobalFree(pngMemory);
        if (dibMemory) GlobalFree(dibMemory);
        if (dibV5Memory) GlobalFree(dibV5Memory);
        errorMessage = L"Windows could not clear the clipboard.";
        return false;
    }

    bool published = false;
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (pngMemory && pngFormat != 0 && SetClipboardData(pngFormat, pngMemory))
    {
        pngMemory = nullptr;
        published = true;
    }
    if (dibMemory && SetClipboardData(CF_DIB, dibMemory))
    {
        dibMemory = nullptr;
        published = true;
    }
    if (dibV5Memory && SetClipboardData(CF_DIBV5, dibV5Memory))
    {
        dibV5Memory = nullptr;
        published = true;
    }
    CloseClipboard();
    if (pngMemory) GlobalFree(pngMemory);
    if (dibMemory) GlobalFree(dibMemory);
    if (dibV5Memory) GlobalFree(dibV5Memory);
    if (!published)
    {
        errorMessage = L"Windows could not place a compatible image on the clipboard.";
        return false;
    }
    errorMessage.clear();
    return true;
}

float MediaEditorCropRect::Width() const
{
    return right - left;
}

float MediaEditorCropRect::Height() const
{
    return bottom - top;
}

bool ImageEditingSession::LoadFromFile(
    const std::filesystem::path& path,
    std::wstring& errorMessage)
{
    MediaEditorImageBuffer image;
    if (!DecodeImageFile(path, image, errorMessage))
    {
        return false;
    }
    return LoadFromBuffer(std::move(image), path, errorMessage);
}

bool ImageEditingSession::LoadFromBuffer(
    MediaEditorImageBuffer image,
    const std::filesystem::path& sourcePath,
    std::wstring& errorMessage)
{
    if (!image.IsValid())
    {
        errorMessage = L"The selected image does not contain readable pixels.";
        return false;
    }
    MediaEditorImageBuffer nextOriginal = std::move(image);
    MediaEditorImageBuffer nextOriented;
    std::filesystem::path nextSourcePath;
    try
    {
        nextOriented = nextOriginal;
        nextSourcePath = sourcePath;
    }
    catch (const std::bad_alloc&)
    {
        errorMessage = L"There is not enough memory to open this image.";
        return false;
    }

    originalImage_ = std::move(nextOriginal);
    orientedImage_ = std::move(nextOriented);
    sourcePath_ = std::move(nextSourcePath);
    crop_ = { 0.0f, 0.0f, static_cast<float>(orientedImage_.width), static_cast<float>(orientedImage_.height) };
    strokes_.clear();
    textBoxes_.clear();
    rotationQuarterTurns_ = 0;
    ClearHistory();
    return true;
}

void ImageEditingSession::Reset()
{
    originalImage_ = {};
    orientedImage_ = {};
    sourcePath_.clear();
    crop_ = {};
    strokes_.clear();
    textBoxes_.clear();
    rotationQuarterTurns_ = 0;
    ClearHistory();
}

void ImageEditingSession::ClearHistory()
{
    editStart_ = {};
    editActive_ = false;
    undoStack_.clear();
    redoStack_.clear();
}

bool ImageEditingSession::IsLoaded() const { return orientedImage_.IsValid(); }
UINT ImageEditingSession::Width() const { return orientedImage_.width; }
UINT ImageEditingSession::Height() const { return orientedImage_.height; }
const MediaEditorImageBuffer& ImageEditingSession::Image() const { return orientedImage_; }
const std::filesystem::path& ImageEditingSession::SourcePath() const { return sourcePath_; }
const MediaEditorCropRect& ImageEditingSession::Crop() const { return crop_; }
const std::vector<MediaEditorDrawingStroke>& ImageEditingSession::Strokes() const { return strokes_; }
const std::vector<MediaEditorTextBox>& ImageEditingSession::TextBoxes() const { return textBoxes_; }
int ImageEditingSession::RotationQuarterTurns() const { return rotationQuarterTurns_; }

MediaEditorImageSnapshot ImageEditingSession::CaptureSnapshot() const
{
    MediaEditorImageSnapshot snapshot;
    snapshot.crop = crop_;
    snapshot.strokes = strokes_;
    snapshot.textBoxes = textBoxes_;
    snapshot.rotationQuarterTurns = rotationQuarterTurns_;
    return snapshot;
}

bool ImageEditingSession::RestoreSnapshot(const MediaEditorImageSnapshot& snapshot)
{
    MediaEditorImageSnapshot nextSnapshot;
    try
    {
        nextSnapshot = snapshot;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    const int previousRotation = rotationQuarterTurns_;
    rotationQuarterTurns_ = ((nextSnapshot.rotationQuarterTurns % 4) + 4) % 4;
    if (!RebuildOrientedImage())
    {
        rotationQuarterTurns_ = previousRotation;
        return false;
    }
    crop_ = ClampCrop(nextSnapshot.crop);
    strokes_ = std::move(nextSnapshot.strokes);
    textBoxes_ = std::move(nextSnapshot.textBoxes);
    return true;
}

void ImageEditingSession::BeginEdit()
{
    if (!IsLoaded() || editActive_)
    {
        return;
    }
    editStart_ = CaptureSnapshot();
    editActive_ = true;
}

void ImageEditingSession::CommitEdit()
{
    if (!editActive_)
    {
        return;
    }
    undoStack_.push_back(std::move(editStart_));
    if (undoStack_.size() > kMaximumUndoStates)
    {
        undoStack_.erase(undoStack_.begin());
    }
    redoStack_.clear();
    editStart_ = {};
    editActive_ = false;
}

void ImageEditingSession::CancelEdit()
{
    if (!editActive_)
    {
        return;
    }
    if (!RestoreSnapshot(editStart_))
    {
        return;
    }
    editStart_ = {};
    editActive_ = false;
}

MediaEditorCropRect ImageEditingSession::ClampCrop(MediaEditorCropRect crop) const
{
    if (!IsLoaded())
    {
        return {};
    }
    const float width = static_cast<float>(Width());
    const float height = static_cast<float>(Height());
    crop.left = std::clamp(crop.left, 0.0f, std::max(0.0f, width - 2.0f));
    crop.top = std::clamp(crop.top, 0.0f, std::max(0.0f, height - 2.0f));
    crop.right = std::clamp(crop.right, crop.left + 2.0f, width);
    crop.bottom = std::clamp(crop.bottom, crop.top + 2.0f, height);
    return crop;
}

void ImageEditingSession::SetCrop(MediaEditorCropRect crop)
{
    crop_ = ClampCrop(crop);
}

void ImageEditingSession::ResetCrop()
{
    if (!IsLoaded())
    {
        return;
    }
    const bool ownEdit = !editActive_;
    if (ownEdit) BeginEdit();
    crop_ = { 0.0f, 0.0f, static_cast<float>(Width()), static_cast<float>(Height()) };
    if (ownEdit) CommitEdit();
}

bool ImageEditingSession::AddStroke(MediaEditorDrawingStroke stroke)
{
    if (!IsLoaded() || stroke.points.empty())
    {
        return false;
    }
    stroke.thickness = std::clamp(stroke.thickness, 1.0f, 160.0f);
    stroke.opacity = std::clamp(stroke.opacity, 0.05f, 1.0f);
    const bool ownEdit = !editActive_;
    if (ownEdit) BeginEdit();

    if (!stroke.eraser)
    {
        strokes_.push_back(std::move(stroke));
        if (ownEdit) CommitEdit();
        return true;
    }

    const auto pointSegmentDistanceSquared = [](
        const MediaEditorPoint& point,
        const MediaEditorPoint& start,
        const MediaEditorPoint& end)
    {
        const float segmentX = end.x - start.x;
        const float segmentY = end.y - start.y;
        const float lengthSquared = segmentX * segmentX + segmentY * segmentY;
        if (lengthSquared <= 0.0001f)
        {
            const float dx = point.x - start.x;
            const float dy = point.y - start.y;
            return dx * dx + dy * dy;
        }
        const float projection = std::clamp(
            ((point.x - start.x) * segmentX + (point.y - start.y) * segmentY) /
                lengthSquared,
            0.0f,
            1.0f);
        const float nearestX = start.x + segmentX * projection;
        const float nearestY = start.y + segmentY * projection;
        const float dx = point.x - nearestX;
        const float dy = point.y - nearestY;
        return dx * dx + dy * dy;
    };

    float eraserLeft = stroke.points.front().x;
    float eraserTop = stroke.points.front().y;
    float eraserRight = eraserLeft;
    float eraserBottom = eraserTop;
    for (const MediaEditorPoint& point : stroke.points)
    {
        eraserLeft = std::min(eraserLeft, point.x);
        eraserTop = std::min(eraserTop, point.y);
        eraserRight = std::max(eraserRight, point.x);
        eraserBottom = std::max(eraserBottom, point.y);
    }

    bool changed = false;
    std::vector<MediaEditorDrawingStroke> updated;
    updated.reserve(strokes_.size() + 4);
    for (MediaEditorDrawingStroke& existing : strokes_)
    {
        if (existing.points.empty())
        {
            continue;
        }

        const float radius = (stroke.thickness + existing.thickness) * 0.5f;
        float existingLeft = existing.points.front().x;
        float existingTop = existing.points.front().y;
        float existingRight = existingLeft;
        float existingBottom = existingTop;
        for (const MediaEditorPoint& point : existing.points)
        {
            existingLeft = std::min(existingLeft, point.x);
            existingTop = std::min(existingTop, point.y);
            existingRight = std::max(existingRight, point.x);
            existingBottom = std::max(existingBottom, point.y);
        }
        if (existingRight < eraserLeft - radius ||
            existingLeft > eraserRight + radius ||
            existingBottom < eraserTop - radius ||
            existingTop > eraserBottom + radius)
        {
            updated.push_back(std::move(existing));
            continue;
        }

        const float threshold = radius * radius;
        const auto isErased = [&](const MediaEditorPoint& point)
        {
            if (stroke.points.size() == 1)
            {
                return pointSegmentDistanceSquared(
                    point,
                    stroke.points.front(),
                    stroke.points.front()) <= threshold;
            }
            for (size_t index = 1; index < stroke.points.size(); ++index)
            {
                if (pointSegmentDistanceSquared(
                    point,
                    stroke.points[index - 1],
                    stroke.points[index]) <= threshold)
                {
                    return true;
                }
            }
            return false;
        };

        const auto makeFragment = [&]()
        {
            MediaEditorDrawingStroke result;
            result.color = existing.color;
            result.thickness = existing.thickness;
            result.opacity = existing.opacity;
            result.eraser = false;
            return result;
        };
        std::vector<MediaEditorDrawingStroke> kept;
        MediaEditorDrawingStroke fragment = makeFragment();
        bool erasedFromStroke = false;
        const float sampleStep = std::max(0.5f, radius * 0.22f);
        auto appendSample = [&](MediaEditorPoint point)
        {
            if (isErased(point))
            {
                erasedFromStroke = true;
                if (!fragment.points.empty())
                {
                    kept.push_back(std::move(fragment));
                    fragment = makeFragment();
                }
                return;
            }
            if (fragment.points.empty() ||
                std::abs(fragment.points.back().x - point.x) > 0.001f ||
                std::abs(fragment.points.back().y - point.y) > 0.001f)
            {
                fragment.points.push_back(point);
            }
        };

        appendSample(existing.points.front());
        for (size_t index = 1; index < existing.points.size(); ++index)
        {
            const MediaEditorPoint start = existing.points[index - 1];
            const MediaEditorPoint end = existing.points[index];
            const float dx = end.x - start.x;
            const float dy = end.y - start.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const int sampleCount = std::clamp(
                static_cast<int>(std::ceil(distance / sampleStep)),
                1,
                4096);
            for (int sample = 1; sample <= sampleCount; ++sample)
            {
                const float amount = static_cast<float>(sample) /
                    static_cast<float>(sampleCount);
                appendSample({ start.x + dx * amount, start.y + dy * amount });
            }
        }
        if (!fragment.points.empty())
        {
            kept.push_back(std::move(fragment));
        }

        if (!erasedFromStroke)
        {
            updated.push_back(std::move(existing));
            continue;
        }
        changed = true;
        for (MediaEditorDrawingStroke& keptStroke : kept)
        {
            updated.push_back(std::move(keptStroke));
        }
    }
    strokes_ = std::move(updated);
    if (ownEdit)
    {
        if (changed) CommitEdit();
        else CancelEdit();
    }
    return changed;
}

bool ImageEditingSession::AddTextBox(MediaEditorTextBox textBox)
{
    if (!IsLoaded() || textBox.text.empty())
    {
        return false;
    }
    textBox.bounds = ClampCrop(textBox.bounds);
    if (textBox.fontFamily.empty())
    {
        textBox.fontFamily = L"Segoe UI";
    }
    textBox.opacity = std::clamp(textBox.opacity, 0.05f, 1.0f);
    textBox.rotationQuarterTurns =
        ((textBox.rotationQuarterTurns % 4) + 4) % 4;
    const bool ownEdit = !editActive_;
    if (ownEdit) BeginEdit();
    try
    {
        textBoxes_.push_back(std::move(textBox));
    }
    catch (const std::bad_alloc&)
    {
        if (ownEdit) CancelEdit();
        return false;
    }
    if (ownEdit) CommitEdit();
    return true;
}

bool ImageEditingSession::RotateClockwise()
{
    if (!IsLoaded())
    {
        return false;
    }
    const bool ownEdit = !editActive_;
    if (ownEdit) BeginEdit();

    const float oldHeight = static_cast<float>(Height());
    const int previousRotation = rotationQuarterTurns_;
    rotationQuarterTurns_ = (rotationQuarterTurns_ + 1) % 4;
    if (!RebuildOrientedImage())
    {
        rotationQuarterTurns_ = previousRotation;
        if (ownEdit)
        {
            editStart_ = {};
            editActive_ = false;
        }
        return false;
    }

    crop_ = {
        oldHeight - crop_.bottom,
        crop_.left,
        oldHeight - crop_.top,
        crop_.right
    };
    for (MediaEditorDrawingStroke& stroke : strokes_)
    {
        for (MediaEditorPoint& point : stroke.points)
        {
            const float oldX = point.x;
            point.x = oldHeight - point.y;
            point.y = oldX;
        }
    }
    for (MediaEditorTextBox& textBox : textBoxes_)
    {
        const MediaEditorCropRect previous = textBox.bounds;
        textBox.bounds = {
            oldHeight - previous.bottom,
            previous.left,
            oldHeight - previous.top,
            previous.right
        };
        textBox.rotationQuarterTurns =
            (textBox.rotationQuarterTurns + 1) % 4;
    }
    crop_ = ClampCrop(crop_);
    if (ownEdit) CommitEdit();
    return true;
}

bool ImageEditingSession::CanUndo() const { return !undoStack_.empty(); }
bool ImageEditingSession::CanRedo() const { return !redoStack_.empty(); }

bool ImageEditingSession::Undo()
{
    if (editActive_ || undoStack_.empty())
    {
        return false;
    }
    try
    {
        redoStack_.push_back(CaptureSnapshot());
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    const MediaEditorImageSnapshot& snapshot = undoStack_.back();
    if (!RestoreSnapshot(snapshot))
    {
        redoStack_.pop_back();
        return false;
    }
    undoStack_.pop_back();
    return true;
}

bool ImageEditingSession::Redo()
{
    if (editActive_ || redoStack_.empty())
    {
        return false;
    }
    try
    {
        undoStack_.push_back(CaptureSnapshot());
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    const MediaEditorImageSnapshot& snapshot = redoStack_.back();
    if (!RestoreSnapshot(snapshot))
    {
        undoStack_.pop_back();
        return false;
    }
    redoStack_.pop_back();
    return true;
}

bool ImageEditingSession::RebuildOrientedImage()
{
    if (!originalImage_.IsValid())
    {
        orientedImage_ = {};
        return true;
    }
    const int rotation = ((rotationQuarterTurns_ % 4) + 4) % 4;
    MediaEditorImageBuffer rebuilt;
    try
    {
        if (rotation == 0)
        {
            rebuilt = originalImage_;
        }
        else
        {
            const UINT sourceWidth = originalImage_.width;
            const UINT sourceHeight = originalImage_.height;
            rebuilt.width = rotation % 2 == 0 ? sourceWidth : sourceHeight;
            rebuilt.height = rotation % 2 == 0 ? sourceHeight : sourceWidth;
            size_t bytes = 0;
            if (!SafePixelBufferSize(rebuilt.width, rebuilt.height, rebuilt.stride, bytes))
            {
                return false;
            }
            rebuilt.pixels.assign(bytes, 0);

            for (UINT sourceY = 0; sourceY < sourceHeight; ++sourceY)
            {
                for (UINT sourceX = 0; sourceX < sourceWidth; ++sourceX)
                {
                    UINT destinationX = sourceX;
                    UINT destinationY = sourceY;
                    if (rotation == 1)
                    {
                        destinationX = sourceHeight - 1 - sourceY;
                        destinationY = sourceX;
                    }
                    else if (rotation == 2)
                    {
                        destinationX = sourceWidth - 1 - sourceX;
                        destinationY = sourceHeight - 1 - sourceY;
                    }
                    else if (rotation == 3)
                    {
                        destinationX = sourceY;
                        destinationY = sourceWidth - 1 - sourceX;
                    }
                    const BYTE* source = originalImage_.pixels.data() +
                        static_cast<size_t>(originalImage_.stride) * sourceY + sourceX * 4ULL;
                    BYTE* destination = rebuilt.pixels.data() +
                        static_cast<size_t>(rebuilt.stride) * destinationY + destinationX * 4ULL;
                    std::copy_n(source, 4, destination);
                }
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    orientedImage_ = std::move(rebuilt);
    return true;
}

bool ImageEditingSession::Flatten(
    MediaEditorImageBuffer& result,
    std::wstring& errorMessage) const
{
    if (!IsLoaded())
    {
        errorMessage = L"Open or paste an image first.";
        return false;
    }

    const int cropLeft = std::clamp(static_cast<int>(std::floor(crop_.left)), 0, static_cast<int>(Width()) - 1);
    const int cropTop = std::clamp(static_cast<int>(std::floor(crop_.top)), 0, static_cast<int>(Height()) - 1);
    const int cropRight = std::clamp(static_cast<int>(std::ceil(crop_.right)), cropLeft + 1, static_cast<int>(Width()));
    const int cropBottom = std::clamp(static_cast<int>(std::ceil(crop_.bottom)), cropTop + 1, static_cast<int>(Height()));
    const UINT outputWidth = static_cast<UINT>(cropRight - cropLeft);
    const UINT outputHeight = static_cast<UINT>(cropBottom - cropTop);
    UINT outputStride = 0;
    size_t outputBytes = 0;
    if (!SafePixelBufferSize(outputWidth, outputHeight, outputStride, outputBytes))
    {
        errorMessage = L"The crop is too large to export safely.";
        return false;
    }

    try
    {
        Gdiplus::Bitmap source(
            static_cast<INT>(Width()),
            static_cast<INT>(Height()),
            static_cast<INT>(orientedImage_.stride),
            PixelFormat32bppPARGB,
            const_cast<BYTE*>(orientedImage_.pixels.data()));
        Gdiplus::Bitmap drawingLayer(
            static_cast<INT>(Width()),
            static_cast<INT>(Height()),
            PixelFormat32bppPARGB);
        Gdiplus::Graphics drawing(&drawingLayer);
        drawing.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        drawing.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        drawing.Clear(Gdiplus::Color(0, 0, 0, 0));

        for (const MediaEditorDrawingStroke& stroke : strokes_)
        {
            if (stroke.points.empty())
            {
                continue;
            }
            const BYTE alpha = stroke.eraser
                ? 0
                : static_cast<BYTE>(std::clamp(stroke.opacity, 0.0f, 1.0f) * 255.0f);
            Gdiplus::Color color(
                alpha,
                GetRValue(stroke.color),
                GetGValue(stroke.color),
                GetBValue(stroke.color));
            drawing.SetCompositingMode(
                stroke.eraser
                    ? Gdiplus::CompositingModeSourceCopy
                    : Gdiplus::CompositingModeSourceOver);
            Gdiplus::Pen pen(color, stroke.thickness);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            if (stroke.points.size() == 1)
            {
                const float radius = stroke.thickness * 0.5f;
                Gdiplus::SolidBrush brush(color);
                drawing.FillEllipse(
                    &brush,
                    stroke.points.front().x - radius,
                    stroke.points.front().y - radius,
                    radius * 2.0f,
                    radius * 2.0f);
            }
            else
            {
                std::vector<Gdiplus::PointF> points;
                points.reserve(stroke.points.size());
                for (const MediaEditorPoint& point : stroke.points)
                {
                    points.emplace_back(point.x, point.y);
                }
                drawing.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
            }
        }
        drawing.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        for (const MediaEditorTextBox& textBox : textBoxes_)
        {
            DrawMediaEditorTextBox(drawing, textBox);
        }

        Gdiplus::Bitmap output(
            static_cast<INT>(outputWidth),
            static_cast<INT>(outputHeight),
            PixelFormat32bppPARGB);
        Gdiplus::Graphics outputGraphics(&output);
        outputGraphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        outputGraphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        outputGraphics.DrawImage(
            &source,
            Gdiplus::Rect(0, 0, outputWidth, outputHeight),
            cropLeft,
            cropTop,
            outputWidth,
            outputHeight,
            Gdiplus::UnitPixel);
        outputGraphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        outputGraphics.DrawImage(
            &drawingLayer,
            Gdiplus::Rect(0, 0, outputWidth, outputHeight),
            cropLeft,
            cropTop,
            outputWidth,
            outputHeight,
            Gdiplus::UnitPixel);

        result.width = outputWidth;
        result.height = outputHeight;
        result.stride = outputStride;
        result.pixels.assign(outputBytes, 0);
        Gdiplus::BitmapData data {};
        const Gdiplus::Rect lockRect(0, 0, outputWidth, outputHeight);
        if (output.LockBits(
            &lockRect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppPARGB,
            &data) != Gdiplus::Ok)
        {
            result = {};
            errorMessage = L"The finished image could not be read from the drawing canvas.";
            return false;
        }
        for (UINT y = 0; y < outputHeight; ++y)
        {
            const BYTE* sourceRow = data.Stride >= 0
                ? static_cast<const BYTE*>(data.Scan0) + static_cast<size_t>(data.Stride) * y
                : static_cast<const BYTE*>(data.Scan0) + static_cast<size_t>(-data.Stride) * (outputHeight - 1 - y);
            std::copy_n(
                sourceRow,
                static_cast<size_t>(outputWidth) * 4ULL,
                result.pixels.data() + static_cast<size_t>(result.stride) * y);
        }
        output.UnlockBits(&data);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        result = {};
        errorMessage = L"There is not enough memory to finish this image.";
        return false;
    }
}

bool ImageEditingSession::SaveAs(
    const std::filesystem::path& path,
    ImageFormat format,
    std::wstring& errorMessage) const
{
    MediaEditorImageBuffer flattened;
    if (!Flatten(flattened, errorMessage))
    {
        return false;
    }
    return EncodeImageFile(flattened, path, format, errorMessage);
}
double VideoEditorClipModel::Duration() const
{
    return std::max(0.0, trimOutSeconds - trimInSeconds);
}

double VideoEditorClipModel::TimelineStart() const
{
    return timelineOffsetSeconds + trimInSeconds;
}

double VideoEditorClipModel::TimelineEnd() const
{
    return timelineOffsetSeconds + trimOutSeconds;
}

const std::vector<VideoEditorClipModel>& VideoTimelineModel::Clips() const { return clips_; }
int VideoTimelineModel::SelectedIndex() const { return selectedIndex_; }
double VideoTimelineModel::PlayheadSeconds() const { return playheadSeconds_; }

double VideoTimelineModel::DurationSeconds() const
{
    double duration = durationSeconds_;
    for (const VideoEditorClipModel& clip : clips_)
    {
        duration = std::max(duration, clip.TimelineEnd());
    }
    return std::max(0.0, duration);
}

VideoTimelineSnapshot VideoTimelineModel::CaptureSnapshot() const
{
    return { clips_, selectedIndex_, playheadSeconds_, durationSeconds_ };
}

void VideoTimelineModel::RestoreSnapshot(const VideoTimelineSnapshot& snapshot)
{
    clips_ = snapshot.clips;
    selectedIndex_ = snapshot.selectedIndex;
    playheadSeconds_ = snapshot.playheadSeconds;
    durationSeconds_ = snapshot.durationSeconds;
    for (const VideoEditorClipModel& clip : clips_)
    {
        nextClipId_ = std::max(nextClipId_, clip.id + 1);
    }
    ClampState();
}

void VideoTimelineModel::PushCheckpoint()
{
    undoStack_.push_back(CaptureSnapshot());
    if (undoStack_.size() > kMaximumUndoStates)
    {
        undoStack_.erase(undoStack_.begin());
    }
    redoStack_.clear();
}

void VideoTimelineModel::ClampState()
{
    if (clips_.empty())
    {
        selectedIndex_ = -1;
        playheadSeconds_ = 0.0;
        durationSeconds_ = 0.0;
        return;
    }
    selectedIndex_ = std::clamp(selectedIndex_, 0, static_cast<int>(clips_.size()) - 1);
    for (const VideoEditorClipModel& clip : clips_)
    {
        durationSeconds_ = std::max(durationSeconds_, clip.TimelineEnd());
    }
    playheadSeconds_ = std::clamp(playheadSeconds_, 0.0, DurationSeconds());
}

void VideoTimelineModel::Reset()
{
    clips_.clear();
    selectedIndex_ = -1;
    playheadSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    nextClipId_ = 1;
    ClearHistory();
}

void VideoTimelineModel::ClearHistory()
{
    editStart_ = {};
    editActive_ = false;
    editChanged_ = false;
    undoStack_.clear();
    redoStack_.clear();
}

void VideoTimelineModel::AddClip(VideoAnalysis analysis)
{
    if (analysis.durationSeconds <= 0.0)
    {
        return;
    }
    PushCheckpoint();
    VideoEditorClipModel clip;
    clip.id = nextClipId_++;
    clip.sourcePath = analysis.filePath;
    clip.analysis = std::move(analysis);
    clip.trimInSeconds = 0.0;
    clip.trimOutSeconds = clip.analysis.durationSeconds;
    clip.timelineOffsetSeconds = durationSeconds_;
    durationSeconds_ += clip.analysis.durationSeconds;
    clips_.push_back(std::move(clip));
    selectedIndex_ = static_cast<int>(clips_.size()) - 1;
    playheadSeconds_ = clips_[selectedIndex_].TimelineStart();
}

bool VideoTimelineModel::SelectClip(int index)
{
    if (index < 0 || index >= static_cast<int>(clips_.size()))
    {
        return false;
    }
    selectedIndex_ = index;
    const double start = clips_[index].TimelineStart();
    const double end = clips_[index].TimelineEnd();
    if (playheadSeconds_ < start || playheadSeconds_ > end)
    {
        playheadSeconds_ = start;
    }
    return true;
}

void VideoTimelineModel::SetPlayhead(double seconds)
{
    playheadSeconds_ = std::clamp(seconds, 0.0, DurationSeconds());
}

bool VideoTimelineModel::SetTrim(
    int index,
    double trimInSeconds,
    double trimOutSeconds,
    bool checkpoint)
{
    if (index < 0 || index >= static_cast<int>(clips_.size()))
    {
        return false;
    }
    VideoEditorClipModel& clip = clips_[index];
    constexpr double minimumDuration = 0.05;
    const double sourceDuration = std::max(minimumDuration, clip.analysis.durationSeconds);
    const double leftBoundary = index > 0
        ? clips_[index - 1].TimelineEnd()
        : 0.0;
    const double rightBoundary = index + 1 < static_cast<int>(clips_.size())
        ? clips_[index + 1].TimelineStart()
        : std::numeric_limits<double>::infinity();
    const double minimumTrimIn = std::max(0.0, leftBoundary - clip.timelineOffsetSeconds);
    const double maximumTrimOut = std::min(
        sourceDuration,
        rightBoundary - clip.timelineOffsetSeconds);
    if (maximumTrimOut - minimumTrimIn < minimumDuration)
    {
        return false;
    }
    trimInSeconds = std::clamp(
        trimInSeconds,
        minimumTrimIn,
        maximumTrimOut - minimumDuration);
    trimOutSeconds = std::clamp(
        trimOutSeconds,
        trimInSeconds + minimumDuration,
        maximumTrimOut);
    if (std::abs(trimInSeconds - clip.trimInSeconds) < 0.0001 &&
        std::abs(trimOutSeconds - clip.trimOutSeconds) < 0.0001)
    {
        return false;
    }
    if (checkpoint && !editActive_) PushCheckpoint();
    if (editActive_) editChanged_ = true;
    clip.trimInSeconds = trimInSeconds;
    clip.trimOutSeconds = trimOutSeconds;
    durationSeconds_ = std::max(durationSeconds_, clip.TimelineEnd());
    ClampState();
    return true;
}

double VideoTimelineModel::ClipStartTime(int index) const
{
    if (index < 0 || index >= static_cast<int>(clips_.size()))
    {
        return 0.0;
    }
    return clips_[index].TimelineStart();
}

std::optional<VideoTimelineLocation> VideoTimelineModel::Locate(double timelineSeconds) const
{
    if (clips_.empty())
    {
        return std::nullopt;
    }
    const double clamped = std::clamp(timelineSeconds, 0.0, DurationSeconds());
    for (int index = 0; index < static_cast<int>(clips_.size()); ++index)
    {
        const VideoEditorClipModel& clip = clips_[index];
        const double start = clip.TimelineStart();
        const double end = clip.TimelineEnd();
        if (clamped + 0.0001 >= start && clamped <= end + 0.0001)
        {
            VideoTimelineLocation location;
            location.clipIndex = index;
            location.timelineSeconds = clamped;
            location.sourceSeconds = clip.trimInSeconds +
                std::clamp(clamped - start, 0.0, clip.Duration());
            return location;
        }
    }
    return std::nullopt;
}

bool VideoTimelineModel::SplitSelectedAtPlayhead()
{
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(clips_.size()))
    {
        return false;
    }
    const auto location = Locate(playheadSeconds_);
    if (!location || location->clipIndex != selectedIndex_)
    {
        return false;
    }
    VideoEditorClipModel& selected = clips_[selectedIndex_];
    const double splitSource = location->sourceSeconds;
    if (splitSource <= selected.trimInSeconds + 0.04 || splitSource >= selected.trimOutSeconds - 0.04)
    {
        return false;
    }
    PushCheckpoint();
    VideoEditorClipModel right = selected;
    right.id = nextClipId_++;
    right.trimInSeconds = splitSource;
    selected.trimOutSeconds = splitSource;
    clips_.insert(clips_.begin() + selectedIndex_ + 1, std::move(right));
    selectedIndex_ += 1;
    return true;
}

bool VideoTimelineModel::DeleteSelected()
{
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(clips_.size()))
    {
        return false;
    }
    PushCheckpoint();
    clips_.erase(clips_.begin() + selectedIndex_);
    if (clips_.empty())
    {
        selectedIndex_ = -1;
        durationSeconds_ = 0.0;
    }
    else selectedIndex_ = std::min(selectedIndex_, static_cast<int>(clips_.size()) - 1);
    ClampState();
    return true;
}

bool VideoTimelineModel::MoveSelected(int destinationIndex)
{
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(clips_.size()) ||
        destinationIndex < 0 || destinationIndex >= static_cast<int>(clips_.size()) ||
        destinationIndex == selectedIndex_)
    {
        return false;
    }
    PushCheckpoint();
    const unsigned long long selectedId = clips_[selectedIndex_].id;
    const double selectedDuration = clips_[selectedIndex_].Duration();
    if (destinationIndex < selectedIndex_)
    {
        double cursor = clips_[destinationIndex].TimelineStart();
        clips_[selectedIndex_].timelineOffsetSeconds =
            cursor - clips_[selectedIndex_].trimInSeconds;
        cursor += selectedDuration;
        for (int index = destinationIndex; index < selectedIndex_; ++index)
        {
            clips_[index].timelineOffsetSeconds = cursor - clips_[index].trimInSeconds;
            cursor += clips_[index].Duration();
        }
    }
    else
    {
        double cursor = clips_[selectedIndex_].TimelineStart();
        for (int index = selectedIndex_ + 1; index <= destinationIndex; ++index)
        {
            clips_[index].timelineOffsetSeconds = cursor - clips_[index].trimInSeconds;
            cursor += clips_[index].Duration();
        }
        clips_[selectedIndex_].timelineOffsetSeconds =
            cursor - clips_[selectedIndex_].trimInSeconds;
    }
    for (const VideoEditorClipModel& clip : clips_)
    {
        durationSeconds_ = std::max(durationSeconds_, clip.TimelineEnd());
    }
    SortClipsByTimeline(selectedId);
    playheadSeconds_ = ClipStartTime(selectedIndex_);
    return true;
}

void VideoTimelineModel::SortClipsByTimeline(unsigned long long selectedId)
{
    std::stable_sort(
        clips_.begin(),
        clips_.end(),
        [](const VideoEditorClipModel& left, const VideoEditorClipModel& right)
        {
            if (std::abs(left.TimelineStart() - right.TimelineStart()) > 0.0001)
            {
                return left.TimelineStart() < right.TimelineStart();
            }
            return left.id < right.id;
        });
    selectedIndex_ = -1;
    for (int index = 0; index < static_cast<int>(clips_.size()); ++index)
    {
        if (clips_[index].id == selectedId)
        {
            selectedIndex_ = index;
            break;
        }
    }
    ClampState();
}

double VideoTimelineModel::NearestAvailableStart(int index, double desiredStart) const
{
    if (index < 0 || index >= static_cast<int>(clips_.size()))
    {
        return 0.0;
    }
    const double clipDuration = clips_[index].Duration();
    desiredStart = std::max(0.0, desiredStart);
    std::vector<std::pair<double, double>> occupied;
    occupied.reserve(clips_.size() - 1);
    for (int other = 0; other < static_cast<int>(clips_.size()); ++other)
    {
        if (other == index) continue;
        occupied.emplace_back(clips_[other].TimelineStart(), clips_[other].TimelineEnd());
    }
    std::sort(occupied.begin(), occupied.end());

    double bestStart = 0.0;
    double bestDistance = std::numeric_limits<double>::infinity();
    auto consider = [&](double minimum, double maximum)
    {
        if (maximum + 0.0001 < minimum) return;
        const double candidate = std::clamp(desiredStart, minimum, maximum);
        const double distance = std::abs(candidate - desiredStart);
        if (distance < bestDistance)
        {
            bestStart = candidate;
            bestDistance = distance;
        }
    };

    double cursor = 0.0;
    for (const auto& interval : occupied)
    {
        if (interval.first - cursor >= clipDuration - 0.0001)
        {
            consider(cursor, std::max(cursor, interval.first - clipDuration));
        }
        cursor = std::max(cursor, interval.second);
    }
    consider(cursor, std::max(cursor, desiredStart));
    return bestStart;
}

bool VideoTimelineModel::MoveSelectedTo(double timelineStartSeconds, bool checkpoint)
{
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(clips_.size()))
    {
        return false;
    }
    const int movingIndex = selectedIndex_;
    const double candidate = NearestAvailableStart(movingIndex, timelineStartSeconds);
    if (std::abs(candidate - clips_[movingIndex].TimelineStart()) < 0.0001)
    {
        return false;
    }
    if (checkpoint && !editActive_) PushCheckpoint();
    if (editActive_) editChanged_ = true;
    const unsigned long long selectedId = clips_[movingIndex].id;
    const double duration = clips_[movingIndex].Duration();
    clips_[movingIndex].timelineOffsetSeconds = candidate - clips_[movingIndex].trimInSeconds;
    durationSeconds_ = std::max(durationSeconds_, candidate + duration);
    SortClipsByTimeline(selectedId);
    playheadSeconds_ = ClipStartTime(selectedIndex_);
    return true;
}

void VideoTimelineModel::BeginEdit()
{
    if (editActive_)
    {
        return;
    }
    editStart_ = CaptureSnapshot();
    editActive_ = true;
    editChanged_ = false;
}

void VideoTimelineModel::CommitEdit()
{
    if (!editActive_)
    {
        return;
    }
    if (editChanged_)
    {
        undoStack_.push_back(std::move(editStart_));
        if (undoStack_.size() > kMaximumUndoStates)
        {
            undoStack_.erase(undoStack_.begin());
        }
        redoStack_.clear();
    }
    editStart_ = {};
    editActive_ = false;
    editChanged_ = false;
}

void VideoTimelineModel::CancelEdit()
{
    if (!editActive_)
    {
        return;
    }
    RestoreSnapshot(editStart_);
    editStart_ = {};
    editActive_ = false;
    editChanged_ = false;
}

bool VideoTimelineModel::CanUndo() const { return !undoStack_.empty(); }
bool VideoTimelineModel::CanRedo() const { return !redoStack_.empty(); }

bool VideoTimelineModel::Undo()
{
    if (editActive_ || undoStack_.empty())
    {
        return false;
    }
    redoStack_.push_back(CaptureSnapshot());
    VideoTimelineSnapshot snapshot = std::move(undoStack_.back());
    undoStack_.pop_back();
    RestoreSnapshot(snapshot);
    return true;
}

bool VideoTimelineModel::Redo()
{
    if (editActive_ || redoStack_.empty())
    {
        return false;
    }
    undoStack_.push_back(CaptureSnapshot());
    VideoTimelineSnapshot snapshot = std::move(redoStack_.back());
    redoStack_.pop_back();
    RestoreSnapshot(snapshot);
    return true;
}
namespace
{
std::wstring DecimalSeconds(double seconds)
{
    std::wostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(6) << std::max(0.0, seconds);
    return output.str();
}

std::wstring SanitizeMediaStem(std::wstring stem)
{
    for (wchar_t& character : stem)
    {
        if (character < 32 || character == L'<' || character == L'>' || character == L':' ||
            character == L'"' || character == L'/' || character == L'\\' || character == L'|' ||
            character == L'?' || character == L'*')
        {
            character = L'_';
        }
    }
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.'))
    {
        stem.pop_back();
    }
    return stem.empty() ? L"video" : stem;
}

std::filesystem::path ResolveMediaOutputConflict(std::filesystem::path requested)
{
    std::error_code error;
    if (!std::filesystem::exists(requested, error))
    {
        return requested;
    }
    const std::filesystem::path folder = requested.parent_path();
    const std::wstring stem = requested.stem().wstring();
    const std::wstring extension = requested.extension().wstring();
    for (int index = 1; index < 10000; ++index)
    {
        std::filesystem::path candidate = folder / (stem + L" (" + std::to_wstring(index) + L")" + extension);
        error.clear();
        if (!std::filesystem::exists(candidate, error))
        {
            return candidate;
        }
    }
    return folder / (stem + L"_" + std::to_wstring(GetTickCount64()) + extension);
}

std::filesystem::path CreateMediaEditorTempDirectory()
{
    const std::filesystem::path root = CacheManager::MediaEditorTemporaryRoot();
    if (root.empty()) return {};
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error)
    {
        return {};
    }
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const std::filesystem::path candidate = root /
            (L"job-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        error.clear();
        if (std::filesystem::create_directory(candidate, error))
        {
            return candidate;
        }
    }
    return {};
}

void CleanupMediaPath(const std::filesystem::path& path, bool recursive = false)
{
    if (path.empty())
    {
        return;
    }
    std::error_code error;
    if (recursive) std::filesystem::remove_all(path, error);
    else std::filesystem::remove(path, error);
}

struct EditorOutputGeometry
{
    int width = 1280;
    int height = 720;
    double fps = 30.0;
};

EditorOutputGeometry ChooseOutputGeometry(
    const std::vector<VideoEditorClipModel>& clips,
    bool smaller)
{
    EditorOutputGeometry geometry;
    if (clips.empty())
    {
        return geometry;
    }

    geometry.width = std::max(2, clips.front().analysis.width);
    geometry.height = std::max(2, clips.front().analysis.height);
    geometry.fps = clips.front().analysis.fps > 0.0 ? clips.front().analysis.fps : 30.0;
    for (const VideoEditorClipModel& clip : clips)
    {
        geometry.width = std::min(geometry.width, std::max(2, clip.analysis.width));
        geometry.height = std::min(geometry.height, std::max(2, clip.analysis.height));
        if (clip.analysis.fps > 0.0)
        {
            geometry.fps = std::min(geometry.fps, clip.analysis.fps);
        }
    }

    if (smaller)
    {
        const int largest = std::max(geometry.width, geometry.height);
        if (largest > 1920)
        {
            const double scale = 1920.0 / static_cast<double>(largest);
            geometry.width = static_cast<int>(std::floor(geometry.width * scale));
            geometry.height = static_cast<int>(std::floor(geometry.height * scale));
        }
        geometry.fps = std::min(geometry.fps, 60.0);
    }
    geometry.width = std::max(2, geometry.width - (geometry.width % 2));
    geometry.height = std::max(2, geometry.height - (geometry.height % 2));
    geometry.fps = std::clamp(geometry.fps, 1.0, 120.0);
    return geometry;
}

std::wstring SecondsArgument(double seconds)
{
    std::wostringstream value;
    value.imbue(std::locale::classic());
    value << std::fixed << std::setprecision(6) << std::max(0.0, seconds);
    return value.str();
}

bool IsPackedTimeline(
    const std::vector<VideoEditorClipModel>& clips,
    double timelineDuration)
{
    double cursor = 0.0;
    for (const VideoEditorClipModel& clip : clips)
    {
        if (std::abs(clip.TimelineStart() - cursor) > 0.002)
        {
            return false;
        }
        cursor = clip.TimelineEnd();
    }
    return std::abs(cursor - timelineDuration) <= 0.02;
}

void AppendEditorEncoderArguments(
    std::vector<std::wstring>& arguments,
    const std::wstring& encoder,
    bool smaller)
{
    const std::wstring quality = smaller ? L"25" : L"18";
    arguments.insert(arguments.end(), { L"-c:v", encoder });
    if (encoder == L"h264_nvenc")
    {
        arguments.insert(arguments.end(), {
            L"-preset", L"p4",
            L"-rc", L"vbr",
            L"-cq", quality,
            L"-b:v", L"0"
        });
    }
    else if (encoder == L"h264_qsv")
    {
        arguments.insert(arguments.end(), {
            L"-preset", L"medium",
            L"-global_quality", quality
        });
    }
    else if (encoder == L"h264_amf")
    {
        const int baseQuality = smaller ? 25 : 18;
        arguments.insert(arguments.end(), {
            L"-quality", L"balanced",
            L"-rc", L"cqp",
            L"-qp_i", std::to_wstring(baseQuality),
            L"-qp_p", std::to_wstring(baseQuality + 2)
        });
    }
    else
    {
        arguments.insert(arguments.end(), {
            L"-preset", L"medium",
            L"-crf", smaller ? L"24" : L"18"
        });
    }
}

std::wstring EditorEncoderLabel(const std::wstring& encoder)
{
    if (encoder == L"h264_nvenc") return L"NVIDIA GPU";
    if (encoder == L"h264_qsv") return L"Intel GPU";
    if (encoder == L"h264_amf") return L"AMD GPU";
    return L"CPU";
}

std::wstring BuildTimelineFilter(
    const std::vector<VideoEditorClipModel>& clips,
    const EditorOutputGeometry& geometry,
    double timelineDuration)
{
    std::wostringstream filter;
    filter.imbue(std::locale::classic());
    filter << std::fixed << std::setprecision(6);
    timelineDuration = std::max(0.05, timelineDuration);
    const bool packed = IsPackedTimeline(clips, timelineDuration);
    if (!packed)
    {
        filter << L"color=c=black:s=" << geometry.width << L"x" << geometry.height
            << L":r=" << geometry.fps << L":d=" << timelineDuration
            << L",format=yuv420p[vbase];"
            << L"anullsrc=channel_layout=stereo:sample_rate=48000"
            << L",atrim=duration=" << timelineDuration
            << L",asetpts=PTS-STARTPTS[abase];";
    }
    for (size_t index = 0; index < clips.size(); ++index)
    {
        const VideoEditorClipModel& clip = clips[index];
        filter << L"[" << index << L":v:0]"
            << L"trim=start=0:end=" << clip.Duration()
            << L",setpts=PTS-STARTPTS";
        if (!packed) filter << L"+" << clip.TimelineStart() << L"/TB";
        filter
            << L",scale=" << geometry.width << L":" << geometry.height
            << L":force_original_aspect_ratio=decrease"
            << L",pad=" << geometry.width << L":" << geometry.height << L":(ow-iw)/2:(oh-ih)/2:black"
            << L",setsar=1,fps=" << geometry.fps << L",format=yuv420p[v" << index << L"];";

        if (clip.analysis.hasAudio)
        {
            const long long delayMilliseconds = std::max<long long>(
                0, static_cast<long long>(std::llround(clip.TimelineStart() * 1000.0)));
            filter << L"[" << index << L":a:0]"
                << L"atrim=start=0:end=" << clip.Duration()
                << L",asetpts=PTS-STARTPTS,aresample=48000"
                << L",aformat=sample_fmts=fltp:channel_layouts=stereo";
            if (!packed) filter << L",adelay=delays=" << delayMilliseconds << L":all=1";
            filter << L"[a" << index << L"];";
        }
        else
        {
            const long long delayMilliseconds = std::max<long long>(
                0, static_cast<long long>(std::llround(clip.TimelineStart() * 1000.0)));
            filter << L"anullsrc=channel_layout=stereo:sample_rate=48000"
                << L",atrim=duration=" << clip.Duration()
                << L",asetpts=PTS-STARTPTS";
            if (!packed) filter << L",adelay=delays=" << delayMilliseconds << L":all=1";
            filter << L"[a" << index << L"];";
        }
    }

    if (packed)
    {
        if (clips.size() == 1)
        {
            filter << L"[v0]null[vout];[a0]anull[aout]";
        }
        else
        {
            for (size_t index = 0; index < clips.size(); ++index)
            {
                filter << L"[v" << index << L"][a" << index << L"]";
            }
            filter << L"concat=n=" << clips.size() << L":v=1:a=1[vout][aout]";
        }
        return filter.str();
    }

    std::wstring previousVideo = L"vbase";
    for (size_t index = 0; index < clips.size(); ++index)
    {
        const std::wstring output = index + 1 == clips.size()
            ? L"vout"
            : L"vstage" + std::to_wstring(index);
        filter << L"[" << previousVideo << L"][v" << index << L"]"
            << L"overlay=eof_action=pass:shortest=0:repeatlast=0[" << output << L"];";
        previousVideo = output;
    }

    filter << L"[abase]";
    for (size_t index = 0; index < clips.size(); ++index)
    {
        filter << L"[a" << index << L"]";
    }
    filter << L"amix=inputs=" << (clips.size() + 1)
        << L":duration=first:dropout_transition=0:normalize=0"
        << L",atrim=duration=" << timelineDuration
        << L",asetpts=PTS-STARTPTS[aout]";
    return filter.str();
}

bool CanStreamCopySingleClip(const VideoEditorClipModel& clip, double timelineDuration)
{
    const std::wstring extension = Lower(clip.sourcePath.extension().wstring());
    const std::wstring videoCodec = Lower(clip.analysis.videoCodec);
    const std::wstring audioCodec = Lower(clip.analysis.audioCodec);
    const bool untrimmed =
        clip.trimInSeconds <= 0.001 &&
        std::abs(clip.trimOutSeconds - clip.analysis.durationSeconds) <= 0.02;
    const bool fillsTimeline =
        clip.TimelineStart() <= 0.001 &&
        std::abs(clip.TimelineEnd() - timelineDuration) <= 0.02;
    return extension == L".mp4" && untrimmed && fillsTimeline &&
        (videoCodec == L"h264" || videoCodec == L"avc") &&
        (!clip.analysis.hasAudio || audioCodec == L"aac");
}

class EditorFfmpegProgressParser
{
public:
    explicit EditorFfmpegProgressParser(double durationSeconds)
        : durationSeconds_(std::max(0.001, durationSeconds))
    {
    }

    std::optional<double> Consume(const std::wstring& chunk)
    {
        pending_ += chunk;
        size_t lineEnd = std::wstring::npos;
        std::optional<double> latest;
        while ((lineEnd = pending_.find(L'\n')) != std::wstring::npos)
        {
            std::wstring line = pending_.substr(0, lineEnd);
            pending_.erase(0, lineEnd + 1);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            const size_t separator = line.find(L'=');
            if (separator == std::wstring::npos) continue;
            const std::wstring key = line.substr(0, separator);
            const std::wstring value = line.substr(separator + 1);
            try
            {
                double seconds = 0.0;
                if (key == L"out_time_us" || key == L"out_time_ms")
                {
                    seconds = std::stod(value) / 1000000.0;
                }
                else if (key == L"out_time")
                {
                    int hours = 0;
                    int minutes = 0;
                    double remaining = 0.0;
                    if (swscanf_s(value.c_str(), L"%d:%d:%lf", &hours, &minutes, &remaining) == 3)
                    {
                        seconds = hours * 3600.0 + minutes * 60.0 + remaining;
                    }
                }
                else
                {
                    continue;
                }
                latest = std::clamp(seconds / durationSeconds_, 0.0, 1.0);
            }
            catch (...)
            {
            }
        }
        return latest;
    }

private:
    double durationSeconds_ = 1.0;
    std::wstring pending_;
};

std::wstring WorkingOutputName(const std::filesystem::path& finalPath)
{
    return L"." + finalPath.stem().wstring() + L".rex-working-" +
        std::to_wstring(GetCurrentProcessId()) + L".mp4";
}
}

std::filesystem::path VideoEditorExportService::SuggestedOutputPath(
    const std::vector<VideoEditorClipModel>& clips,
    VideoEditorExportMode mode,
    unsigned long long targetSizeBytes,
    const std::filesystem::path& folder)
{
    std::wstring stem = clips.empty()
        ? L"video"
        : SanitizeMediaStem(clips.front().sourcePath.stem().wstring());
    stem += L"_edited";
    if (mode == VideoEditorExportMode::FitUnderSizeLimit)
    {
        const unsigned long long megabytes = std::max<unsigned long long>(
            1,
            targetSizeBytes / (1024ULL * 1024ULL));
        stem += L"_" + std::to_wstring(megabytes) + L"mb";
    }
    return folder / (stem + L".mp4");
}

std::wstring VideoEditorExportService::ModeLabel(VideoEditorExportMode mode)
{
    switch (mode)
    {
    case VideoEditorExportMode::MakeFileSmaller:
        return L"Make file smaller";
    case VideoEditorExportMode::FitUnderSizeLimit:
        return L"Fit under a size limit";
    case VideoEditorExportMode::KeepOriginalQuality:
    default:
        return L"Keep original quality";
    }
}

std::wstring VideoEditorExportService::PhaseLabel(VideoEditorExportPhase phase)
{
    switch (phase)
    {
    case VideoEditorExportPhase::Preparing: return L"Preparing";
    case VideoEditorExportPhase::Exporting: return L"Exporting";
    case VideoEditorExportPhase::Compressing: return L"Compressing";
    case VideoEditorExportPhase::Verifying: return L"Verifying";
    case VideoEditorExportPhase::Complete: return L"Complete";
    case VideoEditorExportPhase::Failed: return L"Failed";
    case VideoEditorExportPhase::Cancelled: return L"Cancelled";
    case VideoEditorExportPhase::Idle:
    default:
        return L"Ready";
    }
}
VideoEditorExportResult VideoEditorExportService::Export(
    const std::vector<VideoEditorClipModel>& clips,
    const VideoEditorExportOptions& options,
    const std::atomic_bool& cancelRequested,
    const ProgressCallback& progressCallback) const
{
    VideoEditorExportResult result;
    if (clips.empty())
    {
        result.errorMessage = L"Add at least one video clip before exporting.";
        return result;
    }
    if (options.outputPath.empty())
    {
        result.errorMessage = L"Choose where to save the edited video.";
        return result;
    }
    if (options.mode == VideoEditorExportMode::FitUnderSizeLimit &&
        options.targetSizeBytes < 1024ULL * 1024ULL)
    {
        result.errorMessage = L"Enter a size limit of at least 1 MB.";
        return result;
    }

    const ExternalToolStatus tools = externalToolService_.CheckTools();
    if (!tools.ffmpegFound)
    {
        result.errorMessage = L"FFmpeg is missing. Restore the bundled tools folder or add FFmpeg to PATH.";
        return result;
    }
    if (options.mode == VideoEditorExportMode::FitUnderSizeLimit && !tools.ffprobeFound)
    {
        result.errorMessage = L"FFprobe is missing. Restore the bundled tools folder or add FFprobe to PATH.";
        return result;
    }

    for (const VideoEditorClipModel& clip : clips)
    {
        std::error_code fileError;
        if (!std::filesystem::exists(clip.sourcePath, fileError) ||
            !std::filesystem::is_regular_file(clip.sourcePath, fileError))
        {
            result.errorMessage = L"The source file \"" + FileNameForMessage(clip.sourcePath) +
                L"\" is no longer available.";
            return result;
        }
        if (clip.Duration() < 0.04)
        {
            result.errorMessage = L"One of the timeline clips is too short to export.";
            return result;
        }
    }

    std::filesystem::path outputFolder = options.outputPath.parent_path();
    if (outputFolder.empty())
    {
        outputFolder = std::filesystem::current_path();
    }
    std::error_code folderError;
    if (!std::filesystem::exists(outputFolder, folderError) ||
        !std::filesystem::is_directory(outputFolder, folderError))
    {
        result.errorMessage = L"The selected save folder is no longer available.";
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    auto report = [&started, &progressCallback](
        VideoEditorExportPhase phase,
        double progress,
        const std::wstring& message,
        double estimatedRemainingSeconds = 0.0)
    {
        if (!progressCallback)
        {
            return;
        }
        VideoEditorExportProgress update;
        update.phase = phase;
        update.progress = std::clamp(progress, 0.0, 1.0);
        update.message = message;
        update.elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        update.estimatedRemainingSeconds = std::max(0.0, estimatedRemainingSeconds);
        progressCallback(update);
    };

    const double timelineDuration = std::accumulate(
        clips.begin(),
        clips.end(),
        0.05,
        [](double duration, const VideoEditorClipModel& clip)
        {
            return std::max(duration, clip.TimelineEnd());
        });
    const bool streamCopyEligible =
        options.mode == VideoEditorExportMode::KeepOriginalQuality &&
        clips.size() == 1 &&
        CanStreamCopySingleClip(clips.front(), timelineDuration);
    std::wstring selectedEncoder = L"libx264";
    if (options.encoderMode == VideoEncoderMode::AutomaticGpu && !streamCopyEligible)
    {
        report(VideoEditorExportPhase::Preparing, 0.0, L"Checking available GPU acceleration...");
        selectedEncoder = compressionService_.SelectAvailableEncoder(
            options.encoderMode,
            cancelRequested);
    }
    report(
        VideoEditorExportPhase::Preparing,
        0.0,
        streamCopyEligible
            ? L"Preparing a fast original-quality export..."
            : L"Opening the selected timeline clips...");

    const std::filesystem::path finalPath = ResolveMediaOutputConflict(options.outputPath);
    const std::filesystem::path workingPath = outputFolder / WorkingOutputName(finalPath);
    CleanupMediaPath(workingPath);

    auto runAssembly = [&](
        const std::filesystem::path& destination,
        bool smaller,
        bool allowStreamCopy,
        double progressStart,
        double progressSpan,
        const std::wstring& encoder) -> ProcessResult
    {
        std::vector<std::wstring> arguments {
            L"-hide_banner",
            L"-loglevel", L"error",
            L"-y"
        };

        const bool streamCopy =
            allowStreamCopy &&
            clips.size() == 1 &&
            CanStreamCopySingleClip(clips.front(), timelineDuration);
        if (streamCopy)
        {
            arguments.insert(arguments.end(), {
                L"-i", clips.front().sourcePath.wstring(),
                L"-map", L"0:v:0",
                L"-map", L"0:a:0?",
                L"-c", L"copy",
                L"-sn",
                L"-movflags", L"+faststart",
                L"-progress", L"pipe:1",
                L"-nostats",
                destination.wstring()
            });
        }
        else
        {
            for (const VideoEditorClipModel& clip : clips)
            {
                if (clip.trimInSeconds > 0.0005)
                {
                    arguments.push_back(L"-ss");
                    arguments.push_back(SecondsArgument(clip.trimInSeconds));
                }
                arguments.push_back(L"-t");
                arguments.push_back(SecondsArgument(clip.Duration()));
                arguments.push_back(L"-i");
                arguments.push_back(clip.sourcePath.wstring());
            }
            const EditorOutputGeometry geometry = ChooseOutputGeometry(clips, smaller);
            arguments.insert(arguments.end(), {
                L"-filter_complex", BuildTimelineFilter(clips, geometry, timelineDuration),
                L"-map", L"[vout]",
                L"-map", L"[aout]"
            });
            AppendEditorEncoderArguments(arguments, encoder, smaller);
            arguments.insert(arguments.end(), {
                L"-c:a", L"aac",
                L"-b:a", smaller ? L"128k" : L"192k",
                L"-sn",
                L"-map_metadata", L"-1",
                L"-movflags", L"+faststart",
                L"-t", SecondsArgument(timelineDuration),
                L"-progress", L"pipe:1",
                L"-nostats",
                destination.wstring()
            });
        }

        EditorFfmpegProgressParser parser(timelineDuration);
        double progressAnchor = -1.0;
        auto progressAnchorTime = std::chrono::steady_clock::now();
        double smoothedRemaining = 0.0;
        bool estimateReady = false;
        return processRunner_.Run(
            tools.ffmpegPath,
            arguments,
            cancelRequested,
            [&](const std::wstring& chunk)
            {
                const std::optional<double> parsed = parser.Consume(chunk);
                if (parsed)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (progressAnchor < 0.0 && *parsed > 0.001)
                    {
                        progressAnchor = *parsed;
                        progressAnchorTime = now;
                    }
                    else if (progressAnchor >= 0.0 && *parsed > progressAnchor)
                    {
                        const double sampleSeconds = std::chrono::duration<double>(
                            now - progressAnchorTime).count();
                        const double sampleProgress = *parsed - progressAnchor;
                        if (sampleSeconds >= 2.0 && sampleProgress >= 0.025)
                        {
                            const double rate = sampleProgress / sampleSeconds;
                            const double rawRemaining = rate > 0.000001
                                ? (1.0 - *parsed) / rate
                                : 0.0;
                            if (std::isfinite(rawRemaining) && rawRemaining > 0.0 && rawRemaining < 86400.0)
                            {
                                smoothedRemaining = estimateReady
                                    ? smoothedRemaining * 0.72 + rawRemaining * 0.28
                                    : rawRemaining;
                                estimateReady = true;
                            }
                        }
                    }
                    report(
                        VideoEditorExportPhase::Exporting,
                        progressStart + *parsed * progressSpan,
                        smaller
                            ? L"Creating a smaller video with " + EditorEncoderLabel(encoder) + L"..."
                            : L"Exporting with " + EditorEncoderLabel(encoder) + L"...",
                        estimateReady ? smoothedRemaining : 0.0);
                }
            });
    };

    auto runAssemblyWithFallback = [&] (
        const std::filesystem::path& destination,
        bool smaller,
        bool allowStreamCopy,
        double progressStart,
        double progressSpan) -> ProcessResult
    {
        ProcessResult process = runAssembly(
            destination,
            smaller,
            allowStreamCopy,
            progressStart,
            progressSpan,
            selectedEncoder);
        const bool usedHardware = selectedEncoder != L"libx264" &&
            !(allowStreamCopy && streamCopyEligible);
        if (!process.cancelled && process.exitCode != 0 &&
            usedHardware && !cancelRequested.load())
        {
            CleanupMediaPath(destination);
            report(
                VideoEditorExportPhase::Preparing,
                progressStart,
                L"GPU export could not start. Retrying with the CPU...");
            selectedEncoder = L"libx264";
            process = runAssembly(
                destination,
                smaller,
                allowStreamCopy,
                progressStart,
                progressSpan,
                selectedEncoder);
        }
        return process;
    };

    if (cancelRequested.load())
    {
        result.cancelled = true;
        result.errorMessage = L"Export cancelled.";
        report(VideoEditorExportPhase::Cancelled, 0.0, result.errorMessage);
        return result;
    }

    if (options.mode == VideoEditorExportMode::FitUnderSizeLimit)
    {
        const std::filesystem::path tempDirectory = CreateMediaEditorTempDirectory();
        if (tempDirectory.empty())
        {
            result.errorMessage = L"Temporary files could not be created for this export.";
            report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
            return result;
        }
        const std::filesystem::path assembledPath = tempDirectory / L"timeline.mp4";
        ProcessResult assembly = runAssemblyWithFallback(assembledPath, false, false, 0.02, 0.38);
        if (assembly.cancelled || cancelRequested.load())
        {
            CleanupMediaPath(tempDirectory, true);
            result.cancelled = true;
            result.errorMessage = L"Export cancelled.";
            report(VideoEditorExportPhase::Cancelled, 0.0, result.errorMessage);
            return result;
        }
        std::error_code assembledError;
        if (assembly.exitCode != 0 ||
            !std::filesystem::exists(assembledPath, assembledError) ||
            std::filesystem::file_size(assembledPath, assembledError) == 0)
        {
            CleanupMediaPath(tempDirectory, true);
            result.errorMessage = L"FFmpeg could not assemble these clips. A source video may be damaged or locked.";
            report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
            return result;
        }

        report(VideoEditorExportPhase::Compressing, 0.42, L"Fitting the video under the size limit...");
        std::wstring analysisError;
        const VideoAnalysis assembledAnalysis = compressionService_.Analyze(
            assembledPath,
            cancelRequested,
            analysisError);
        if (cancelRequested.load())
        {
            CleanupMediaPath(tempDirectory, true);
            result.cancelled = true;
            result.errorMessage = L"Export cancelled.";
            report(VideoEditorExportPhase::Cancelled, 0.0, result.errorMessage);
            return result;
        }
        if (assembledAnalysis.durationSeconds <= 0.0)
        {
            CleanupMediaPath(tempDirectory, true);
            result.errorMessage = analysisError.empty()
                ? L"The assembled video could not be analyzed."
                : analysisError;
            report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
            return result;
        }

        VideoCompressionOptions compressionOptions;
        compressionOptions.targetSizeBytes = options.targetSizeBytes;
        compressionOptions.mode = VideoCompressionMode::Accurate;
        compressionOptions.outputFolder = outputFolder;
        compressionOptions.resolutionMode = VideoResolutionMode::Auto;
        compressionOptions.fpsMode = VideoFpsMode::Auto;
        compressionOptions.audioMode = VideoAudioMode::Auto;
        compressionOptions.preset = VideoEncodingPreset::Slow;
        compressionOptions.encoderMode = selectedEncoder == L"libx264"
            ? VideoEncoderMode::Cpu
            : options.encoderMode;
        compressionOptions.conflictBehavior = VideoConflictBehavior::AutoRename;
        compressionOptions.verifyFinalSize = true;
        compressionOptions.retryIfTooLarge = true;
        compressionOptions.maxRetries = 2;

        const VideoCompressionResult compressed = compressionService_.Compress(
            assembledAnalysis,
            compressionOptions,
            finalPath,
            cancelRequested,
            [&](const VideoCompressionProgress& progress)
            {
                report(
                    progress.phase == VideoCompressionPhase::Verifying
                        ? VideoEditorExportPhase::Verifying
                        : VideoEditorExportPhase::Compressing,
                    0.42 + std::clamp(progress.progress, 0.0, 1.0) * 0.56,
                    progress.message.empty()
                        ? L"Fitting the video under the size limit..."
                        : progress.message,
                    progress.elapsedSeconds >= 4.0 && progress.progress >= 0.05
                        ? progress.estimatedRemainingSeconds
                        : 0.0);
            });
        CleanupMediaPath(tempDirectory, true);
        if (compressed.cancelled || cancelRequested.load())
        {
            result.cancelled = true;
            result.errorMessage = L"Export cancelled.";
            report(VideoEditorExportPhase::Cancelled, 0.0, result.errorMessage);
            return result;
        }
        if (!compressed.success)
        {
            result.errorMessage = compressed.errorMessage.empty()
                ? L"The video could not be kept under the requested size."
                : compressed.errorMessage;
            report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
            return result;
        }

        result.success = true;
        result.outputPath = compressed.outputPath;
        result.outputSizeBytes = compressed.finalSizeBytes;
        result.details = L"H.264 / AAC MP4, verified under the requested size. " +
            std::wstring(compressed.plan.hardwareEncoding ? L"GPU accelerated." : L"CPU encoded.");
        report(VideoEditorExportPhase::Complete, 1.0, L"Export complete.");
        return result;
    }

    const bool smaller = options.mode == VideoEditorExportMode::MakeFileSmaller;
    ProcessResult exportProcess = runAssemblyWithFallback(
        workingPath,
        smaller,
        options.mode == VideoEditorExportMode::KeepOriginalQuality,
        0.02,
        0.88);
    if (exportProcess.cancelled || cancelRequested.load())
    {
        CleanupMediaPath(workingPath);
        result.cancelled = true;
        result.errorMessage = L"Export cancelled.";
        report(VideoEditorExportPhase::Cancelled, 0.0, result.errorMessage);
        return result;
    }

    std::error_code outputError;
    if (exportProcess.exitCode != 0 ||
        !std::filesystem::exists(workingPath, outputError) ||
        std::filesystem::file_size(workingPath, outputError) == 0)
    {
        CleanupMediaPath(workingPath);
        result.errorMessage = L"FFmpeg could not export this timeline. A source video may be damaged, locked, or incompatible.";
        report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
        return result;
    }

    report(VideoEditorExportPhase::Verifying, 0.94, L"Verifying the finished video...");
    const unsigned long long outputSize = std::filesystem::file_size(workingPath, outputError);
    if (outputError || outputSize == 0)
    {
        CleanupMediaPath(workingPath);
        result.errorMessage = L"The exported video was not created correctly.";
        report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
        return result;
    }

    std::filesystem::path resolvedFinalPath = ResolveMediaOutputConflict(finalPath);
    if (!MoveFileExW(workingPath.c_str(), resolvedFinalPath.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        CleanupMediaPath(workingPath);
        result.errorMessage = L"The video was exported, but it could not be moved to the selected save location.";
        report(VideoEditorExportPhase::Failed, 0.0, result.errorMessage);
        return result;
    }

    result.success = true;
    result.outputPath = resolvedFinalPath;
    result.outputSizeBytes = outputSize;
    const bool copied = clips.size() == 1 &&
        options.mode == VideoEditorExportMode::KeepOriginalQuality &&
        CanStreamCopySingleClip(clips.front(), timelineDuration);
    result.details = copied
        ? L"Original H.264/AAC streams were preserved without re-encoding."
        : smaller
            ? L"H.264 / AAC MP4 with automatic lightweight compression using " + EditorEncoderLabel(selectedEncoder) + L"."
            : L"High-quality H.264 / AAC MP4 export using " + EditorEncoderLabel(selectedEncoder) + L".";
    report(VideoEditorExportPhase::Complete, 1.0, L"Export complete.");
    return result;
}
class VideoPreviewService::Impl
{
public:
    class Callback final : public IMFMediaEngineNotify
    {
    public:
        explicit Callback(Impl* owner)
            : owner_(owner)
        {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override
        {
            if (!object)
            {
                return E_POINTER;
            }
            if (interfaceId == IID_IUnknown || interfaceId == IID_IMFMediaEngineNotify)
            {
                *object = static_cast<IMFMediaEngineNotify*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&references_));
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG references = static_cast<ULONG>(InterlockedDecrement(&references_));
            if (references == 0)
            {
                delete this;
            }
            return references;
        }

        HRESULT STDMETHODCALLTYPE EventNotify(
            DWORD mediaEvent,
            DWORD_PTR parameter1,
            DWORD parameter2) override
        {
            if (owner_)
            {
                owner_->OnEvent(mediaEvent, parameter1, parameter2);
            }
            return S_OK;
        }

        void Detach()
        {
            owner_ = nullptr;
        }

    private:
        ~Callback() = default;
        volatile LONG references_ = 1;
        Impl* owner_ = nullptr;
    };

    bool Initialize(HWND videoHost, HWND notificationWindow, UINT readyMessage, UINT errorMessage)
    {
        host_ = videoHost;
        notificationWindow_ = notificationWindow;
        readyMessage_ = readyMessage;
        errorMessageId_ = errorMessage;
        if (!mfStarted_)
        {
            const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
            if (FAILED(result))
            {
                SetError(L"Windows Media Foundation could not start: " + HResultText(result) + L".");
                return false;
            }
            mfStarted_ = true;
        }
        if (!callback_)
        {
            callback_ = new Callback(this);
        }
        if (!factory_)
        {
            const HRESULT result = CoCreateInstance(
                CLSID_MFMediaEngineClassFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
            if (FAILED(result))
            {
                SetError(L"Windows Media Engine could not start: " + HResultText(result) + L".");
                return false;
            }
        }
        return true;
    }

    void ClearFrameSteps()
    {
        frameStepRequested_ = false;
    }

    bool HasFrameSteps() const
    {
        return frameStepRequested_.load();
    }

    void Close()
    {
        ready_ = false;
        playing_ = false;
        actualPlaying_ = false;
        playbackCommandPending_ = false;
        playbackEnded_ = false;
        frameStepPending_ = false;
        frameStepAbandoned_ = false;
        frameStepStartedAt_ = 0;
        seekPending_ = false;
        seekRequested_ = false;
        seekStartedAt_ = 0;
        seekTimeoutCount_ = 0;
        ClearFrameSteps();
        if (engine_)
        {
            engine_->Shutdown();
        }
        engineEx_.Reset();
        engine_.Reset();
        if (host_)
        {
            InvalidateRect(host_, nullptr, TRUE);
        }
    }

    void Shutdown()
    {
        Close();
        factory_.Reset();
        if (callback_)
        {
            callback_->Detach();
            callback_->Release();
            callback_ = nullptr;
        }
        if (mfStarted_)
        {
            MFShutdown();
            mfStarted_ = false;
        }
    }

    std::wstring SourceUrl(const std::filesystem::path& path) const
    {
        std::array<wchar_t, 4096> url {};
        DWORD length = static_cast<DWORD>(url.size());
        const HRESULT result = UrlCreateFromPathW(path.c_str(), url.data(), &length, 0);
        return SUCCEEDED(result) ? std::wstring(url.data()) : path.wstring();
    }

    bool Open(const std::filesystem::path& path, double trimInSeconds, double trimOutSeconds)
    {
        if (!mfStarted_ || !callback_ || !factory_ || !host_)
        {
            SetError(L"Video preview is not available.");
            return false;
        }
        Close();
        trimInSeconds_ = std::max(0.0, trimInSeconds);
        trimOutSeconds_ = std::max(trimInSeconds_ + 0.01, trimOutSeconds);
        pendingSeekSeconds_ = trimInSeconds_;
        seekRequested_ = true;
        playbackEnded_ = false;
        durationSeconds_ = trimOutSeconds_;
        errorText_.clear();

        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(attributes.ReleaseAndGetAddressOf(), 2);
        if (SUCCEEDED(result))
        {
            result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, callback_);
        }
        if (SUCCEEDED(result))
        {
            result = attributes->SetUINT64(
                MF_MEDIA_ENGINE_PLAYBACK_HWND,
                static_cast<UINT64>(reinterpret_cast<UINT_PTR>(host_)));
        }

        ComPtr<IMFMediaEngine> engine;
        if (SUCCEEDED(result))
        {
            result = factory_->CreateInstance(0, attributes.Get(), engine.ReleaseAndGetAddressOf());
        }
        if (SUCCEEDED(result))
        {
            result = engine.As(&engineEx_);
        }
        if (FAILED(result) || !engine || !engineEx_)
        {
            engineEx_.Reset();
            SetError(L"Windows could not create the video preview engine.");
            return false;
        }
        engine_ = std::move(engine);
        engine_->SetAutoPlay(FALSE);
        engine_->SetLoop(FALSE);
        engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        engine_->SetVolume(volume_);
        engine_->SetMuted(muted_ ? TRUE : FALSE);

        const std::wstring sourceUrl = SourceUrl(path);
        BSTR source = SysAllocString(sourceUrl.c_str());
        if (!source)
        {
            Close();
            SetError(L"The video path could not be prepared for preview.");
            return false;
        }
        result = engine_->SetSource(source);
        SysFreeString(source);
        if (SUCCEEDED(result))
        {
            result = engine_->Load();
        }
        if (FAILED(result))
        {
            Close();
            SetError(L"Windows could not preview this video format.");
            return false;
        }
        return true;
    }

    void PostReady()
    {
        if (notificationWindow_ && readyMessage_ != 0)
        {
            PostMessageW(notificationWindow_, readyMessage_, 0, 0);
        }
    }

    void ReadDurationAndClampRange()
    {
        if (!engine_)
        {
            return;
        }
        const double duration = engine_->GetDuration();
        if (std::isfinite(duration) && duration > 0.0)
        {
            durationSeconds_ = duration;
        }
        durationSeconds_ = std::max(durationSeconds_, trimInSeconds_ + 0.01);
        trimOutSeconds_ = std::max(
            trimInSeconds_ + 0.01,
            std::min(trimOutSeconds_, durationSeconds_));
    }

    void MarkReady()
    {
        if (!engine_ || engine_->GetReadyState() < MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA)
        {
            return;
        }
        ReadDurationAndClampRange();
        const bool wasReady = ready_.exchange(true);
        actualPlaying_ = engine_->IsPaused() == FALSE && engine_->IsEnded() == FALSE;
        playbackCommandPending_ = false;
        UpdateVideo();
        if (!wasReady && !seekRequested_)
        {
            PostReady();
        }
    }

    void OnEvent(DWORD mediaEvent, DWORD_PTR, DWORD)
    {
        if (!engine_)
        {
            return;
        }
        switch (mediaEvent)
        {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
            ReadDurationAndClampRange();
            break;

        case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:
        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
            MarkReady();
            break;

        case MF_MEDIA_ENGINE_EVENT_SEEKING:
            break;

        case MF_MEDIA_ENGINE_EVENT_SEEKED:
            seekPending_ = false;
            seekStartedAt_ = 0;
            seekTimeoutCount_ = 0;
            if (std::abs(pendingSeekSeconds_.load() - issuedSeekSeconds_.load()) <= 0.00005)
            {
                seekRequested_ = false;
                UpdateVideo();
                PostReady();
            }
            else
            {
                seekRequested_ = true;
            }
            break;

        case MF_MEDIA_ENGINE_EVENT_PLAY:
        case MF_MEDIA_ENGINE_EVENT_PLAYING:
            actualPlaying_ = true;
            playbackCommandPending_ = false;
            break;

        case MF_MEDIA_ENGINE_EVENT_PAUSE:
            actualPlaying_ = false;
            playbackCommandPending_ = false;
            break;

        case MF_MEDIA_ENGINE_EVENT_FRAMESTEPCOMPLETED:
        {
            if (!frameStepPending_.exchange(false))
            {
                break;
            }
            const bool abandoned = frameStepAbandoned_.exchange(false);
            if (!abandoned)
            {
                playing_ = false;
            }
            actualPlaying_ = false;
            frameStepStartedAt_ = 0;
            if (!abandoned)
            {
                UpdateVideo();
                PostReady();
            }
            break;
        }

        case MF_MEDIA_ENGINE_EVENT_ENDED:
            playing_ = false;
            actualPlaying_ = false;
            playbackCommandPending_ = false;
            playbackEnded_ = true;
            ClearFrameSteps();
            break;

        case MF_MEDIA_ENGINE_EVENT_ERROR:
        case MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR:
            SetError(L"Windows could not preview this clip. Export is still available.");
            break;

        default:
            break;
        }
    }

    void SetError(std::wstring message)
    {
        errorText_ = std::move(message);
        ready_ = false;
        playing_ = false;
        actualPlaying_ = false;
        playbackCommandPending_ = false;
        playbackEnded_ = false;
        frameStepPending_ = false;
        frameStepAbandoned_ = false;
        frameStepStartedAt_ = 0;
        seekPending_ = false;
        seekRequested_ = false;
        seekStartedAt_ = 0;
        seekTimeoutCount_ = 0;
        ClearFrameSteps();
        if (notificationWindow_ && errorMessageId_ != 0)
        {
            PostMessageW(notificationWindow_, errorMessageId_, 0, 0);
        }
    }

    void RequestPlaybackState()
    {
        if (!engine_ || !ready_ || frameStepPending_ || seekPending_ || seekRequested_ ||
            playing_.load() == actualPlaying_.load())
        {
            return;
        }
        bool expected = false;
        if (!playbackCommandPending_.compare_exchange_strong(expected, true))
        {
            return;
        }
        const bool requestedPlaying = playing_.load();
        const HRESULT result = requestedPlaying ? engine_->Play() : engine_->Pause();
        if (FAILED(result))
        {
            playbackCommandPending_ = false;
            playing_ = actualPlaying_.load();
            PostReady();
        }
    }

    void RequestFrameStep()
    {
        if (!engine_ || !engineEx_ || !ready_ || !frameStepRequested_ ||
            actualPlaying_ ||
            playbackCommandPending_ || seekPending_ || seekRequested_ || frameStepPending_)
        {
            return;
        }

        const double position = PositionSeconds();
        if (position >= trimOutSeconds_ - 0.0005)
        {
            ClearFrameSteps();
            PostReady();
            return;
        }

        bool expected = false;
        if (!frameStepPending_.compare_exchange_strong(expected, true))
        {
            return;
        }
        frameStepRequested_ = false;
        frameStepAbandoned_ = false;
        frameStepStartedAt_ = GetTickCount64();
        const HRESULT result = engineEx_->FrameStep(TRUE);
        if (SUCCEEDED(result))
        {
            return;
        }

        frameStepPending_ = false;
        frameStepAbandoned_ = false;
        frameStepStartedAt_ = 0;
        PostReady();
    }

    bool StepFrame(int direction)
    {
        if (!engine_ || !engineEx_ || !ready_ || direction == 0)
        {
            return false;
        }
        direction = direction > 0 ? 1 : -1;
        if (direction < 0) return false;
        const bool stepAlreadyPending = frameStepPending_ || frameStepRequested_;
        const double position = PositionSeconds();
        if (!stepAlreadyPending &&
            position >= trimOutSeconds_ - 0.0005)
        {
            return false;
        }

        playbackEnded_ = false;
        playing_ = false;
        if (stepAlreadyPending)
        {
            return true;
        }
        frameStepRequested_ = true;
        RequestPlaybackState();
        RequestFrameStep();
        return true;
    }

    bool CancelPendingFrameStep()
    {
        ClearFrameSteps();
        if (!frameStepPending_)
        {
            return false;
        }
        frameStepAbandoned_ = true;
        return true;
    }

    void Play()
    {
        if (!engine_ || !ready_)
        {
            return;
        }
        ClearFrameSteps();
        const double position = PositionSeconds();
        const bool cancelledStep = CancelPendingFrameStep();
        if (cancelledStep) QueueSeek(position);
        playbackEnded_ = false;
        if (position < trimInSeconds_ || position >= trimOutSeconds_ - 0.01)
        {
            QueueSeek(trimInSeconds_);
        }
        playing_ = true;
        RequestSeek(cancelledStep);
        RequestPlaybackState();
    }

    void Pause()
    {
        ClearFrameSteps();
        if (engine_)
        {
            const double position = PositionSeconds();
            const bool cancelledStep = CancelPendingFrameStep();
            if (cancelledStep) QueueSeek(position);
            playing_ = false;
            RequestSeek(cancelledStep);
            RequestPlaybackState();
        }
    }

    void TogglePlayback()
    {
        if (playing_)
        {
            Pause();
        }
        else
        {
            Play();
        }
    }

    void QueueSeek(double sourceSeconds)
    {
        const bool newRequest = !seekPending_ && !seekRequested_;
        pendingSeekSeconds_.store(std::clamp(
            sourceSeconds,
            trimInSeconds_,
            std::max(trimInSeconds_, trimOutSeconds_)));
        seekRequested_ = true;
        if (newRequest) seekTimeoutCount_ = 0;
    }

    bool RequestSeek(bool force = false)
    {
        if (!engine_ || !ready_ || !seekRequested_ || seekPending_ ||
            frameStepPending_)
        {
            return false;
        }
        if (engine_->GetReadyState() < MF_MEDIA_ENGINE_READY_HAVE_METADATA)
        {
            return false;
        }

        const double target = pendingSeekSeconds_.load();
        const double current = engine_->GetCurrentTime();
        if (!force && std::isfinite(current) && std::abs(current - target) <= 0.00005)
        {
            seekRequested_ = false;
            seekStartedAt_ = 0;
            UpdateVideo();
            PostReady();
            return true;
        }

        issuedSeekSeconds_ = target;
        seekPending_ = true;
        seekStartedAt_ = GetTickCount64();
        HRESULT result = engineEx_
            ? engineEx_->SetCurrentTimeEx(target, MF_MEDIA_ENGINE_SEEK_MODE_NORMAL)
            : engine_->SetCurrentTime(target);
        if (FAILED(result))
        {
            seekPending_ = false;
            seekRequested_ = false;
            seekStartedAt_ = 0;
            return false;
        }
        return true;
    }

    void Seek(double sourceSeconds)
    {
        ClearFrameSteps();
        const bool cancelledStep = CancelPendingFrameStep();
        QueueSeek(sourceSeconds);
        RequestSeek(cancelledStep);
    }

    void SetPlaybackRange(double trimInSeconds, double trimOutSeconds)
    {
        ClearFrameSteps();
        const bool cancelledStep = CancelPendingFrameStep();
        trimInSeconds_ = std::max(0.0, trimInSeconds);
        const double availableEnd = durationSeconds_ > trimInSeconds_
            ? durationSeconds_
            : trimOutSeconds;
        trimOutSeconds_ = std::max(
            trimInSeconds_ + 0.01,
            std::min(trimOutSeconds, availableEnd));
        const double position = std::clamp(
            PositionSeconds(),
            trimInSeconds_,
            trimOutSeconds_);
        pendingSeekSeconds_.store(position);
        playbackEnded_ = false;
        if (cancelledStep)
        {
            seekRequested_ = true;
            RequestSeek(true);
        }
    }

    double PositionSeconds() const
    {
        if (!engine_)
        {
            return pendingSeekSeconds_.load();
        }
        const double position = engine_->GetCurrentTime();
        return std::isfinite(position) ? position : pendingSeekSeconds_.load();
    }

    bool Tick()
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG frameStepStarted = frameStepStartedAt_.load();
        if (frameStepPending_ && frameStepStarted != 0 && now - frameStepStarted >= 900)
        {
            const double recoveryPosition = PositionSeconds();
            ClearFrameSteps();
            if (frameStepPending_.exchange(false))
            {
                frameStepAbandoned_ = false;
                frameStepStartedAt_ = 0;
                QueueSeek(recoveryPosition);
                RequestSeek(true);
            }
        }

        const ULONGLONG seekStarted = seekStartedAt_.load();
        if (seekPending_ && seekStarted != 0 && now - seekStarted >= 1800)
        {
            seekPending_ = false;
            seekStartedAt_ = 0;
            seekRequested_ = true;
            if (++seekTimeoutCount_ <= 1)
            {
                RequestSeek(true);
            }
            else
            {
                seekRequested_ = false;
                seekTimeoutCount_ = 0;
                playbackCommandPending_ = false;
                UpdateVideo();
                PostReady();
            }
        }

        RequestSeek();
        RequestPlaybackState();
        RequestFrameStep();
        if (playbackEnded_.exchange(false))
        {
            return true;
        }
        if (seekPending_ || seekRequested_ || frameStepPending_ ||
            frameStepRequested_ || playbackCommandPending_)
        {
            return false;
        }
        if (!actualPlaying_ || !ready_)
        {
            return false;
        }
        return PositionSeconds() >= trimOutSeconds_ - 0.01;
    }

    void UpdateVideo()
    {
        if (!engineEx_ || !ready_ || !host_)
        {
            return;
        }
        RECT destination {};
        GetClientRect(host_, &destination);
        if (destination.right <= destination.left || destination.bottom <= destination.top)
        {
            return;
        }
        const MFARGB border { 14, 10, 8, 255 };
        engineEx_->UpdateVideoStream(nullptr, &destination, &border);
    }

    ComPtr<IMFMediaEngineClassFactory> factory_;
    ComPtr<IMFMediaEngine> engine_;
    ComPtr<IMFMediaEngineEx> engineEx_;
    Callback* callback_ = nullptr;
    HWND host_ = nullptr;
    HWND notificationWindow_ = nullptr;
    UINT readyMessage_ = 0;
    UINT errorMessageId_ = 0;
    bool mfStarted_ = false;
    std::atomic_bool ready_ { false };
    std::atomic_bool playing_ { false };
    std::atomic_bool actualPlaying_ { false };
    std::atomic_bool playbackCommandPending_ { false };
    std::atomic_bool playbackEnded_ { false };
    std::atomic_bool frameStepRequested_ { false };
    std::atomic_bool frameStepPending_ { false };
    std::atomic_bool frameStepAbandoned_ { false };
    std::atomic<ULONGLONG> frameStepStartedAt_ { 0 };
    std::atomic_bool seekPending_ { false };
    std::atomic_bool seekRequested_ { false };
    std::atomic<ULONGLONG> seekStartedAt_ { 0 };
    std::atomic_int seekTimeoutCount_ { 0 };
    std::atomic<double> pendingSeekSeconds_ { 0.0 };
    std::atomic<double> issuedSeekSeconds_ { 0.0 };
    bool muted_ = false;
    double volume_ = 0.8;
    double trimInSeconds_ = 0.0;
    double trimOutSeconds_ = 0.0;
    double durationSeconds_ = 0.0;
    std::wstring errorText_;
};

VideoPreviewService::VideoPreviewService()
    : impl_(std::make_unique<Impl>())
{
}

VideoPreviewService::~VideoPreviewService()
{
    Shutdown();
}

bool VideoPreviewService::Initialize(
    HWND videoHost,
    HWND notificationWindow,
    UINT readyMessage,
    UINT errorMessage)
{
    return impl_->Initialize(videoHost, notificationWindow, readyMessage, errorMessage);
}

void VideoPreviewService::Shutdown()
{
    if (impl_)
    {
        impl_->Shutdown();
    }
}

void VideoPreviewService::Close()
{
    if (impl_) impl_->Close();
}

bool VideoPreviewService::Open(
    const std::filesystem::path& path,
    double trimInSeconds,
    double trimOutSeconds)
{
    return impl_->Open(path, trimInSeconds, trimOutSeconds);
}

void VideoPreviewService::Play() { impl_->Play(); }
void VideoPreviewService::Pause() { impl_->Pause(); }
void VideoPreviewService::TogglePlayback() { impl_->TogglePlayback(); }
void VideoPreviewService::Seek(double sourceSeconds) { impl_->Seek(sourceSeconds); }
void VideoPreviewService::SetPlaybackRange(double trimInSeconds, double trimOutSeconds) { impl_->SetPlaybackRange(trimInSeconds, trimOutSeconds); }

void VideoPreviewService::SetVolume(float volume)
{
    impl_->volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (impl_->engine_) impl_->engine_->SetVolume(impl_->volume_);
}

bool VideoPreviewService::StepFrame(int direction) { return impl_->StepFrame(direction); }
void VideoPreviewService::SetMuted(bool muted)
{
    impl_->muted_ = muted;
    if (impl_->engine_) impl_->engine_->SetMuted(muted ? TRUE : FALSE);
}

bool VideoPreviewService::Tick() { return impl_->Tick(); }
void VideoPreviewService::UpdateVideo() { impl_->UpdateVideo(); }
bool VideoPreviewService::IsReady() const { return impl_->ready_; }
bool VideoPreviewService::IsPlaying() const { return impl_->playing_; }
bool VideoPreviewService::IsNavigationPending() const
{
    return impl_->seekPending_ || impl_->seekRequested_ ||
        impl_->frameStepPending_ || impl_->frameStepRequested_ ||
        impl_->playbackCommandPending_;
}
bool VideoPreviewService::IsMuted() const { return impl_->muted_; }
double VideoPreviewService::PositionSeconds() const { return impl_->PositionSeconds(); }
double VideoPreviewService::DurationSeconds() const { return impl_->durationSeconds_; }
const std::wstring& VideoPreviewService::ErrorMessage() const { return impl_->errorText_; }

namespace
{
constexpr std::array<const wchar_t*, 25> kMediaEditorExtensions {
    L".mp4", L".mov", L".mkv", L".webm", L".avi", L".m4v",
    L".wmv", L".flv", L".mpeg", L".mpg", L".ts", L".m2ts",
    L".mts", L".3gp", L".3g2", L".ogv", L".vob", L".mxf",
    L".asf", L".f4v",
    L".png", L".jpg", L".jpeg", L".webp", L".bmp"
};

std::wstring MediaEditorShellKey(const wchar_t* extension)
{
    return L"Software\\Classes\\SystemFileAssociations\\" +
        std::wstring(extension) +
        L"\\shell\\RexToolkit.Edit";
}

bool WriteRegistryString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
}

bool ShellIntegrationService::SetMediaEditorContextMenuEnabled(
    bool enabled,
    const std::filesystem::path& executablePath,
    std::wstring& errorMessage) const
{
    errorMessage.clear();
    if (enabled && executablePath.empty())
    {
        errorMessage = L"The Rex's Toolkit executable path could not be found.";
        return false;
    }

    bool success = true;
    for (const wchar_t* extension : kMediaEditorExtensions)
    {
        const std::wstring shellKey = MediaEditorShellKey(extension);
        if (!enabled)
        {
            const LSTATUS deleted = RegDeleteTreeW(HKEY_CURRENT_USER, shellKey.c_str());
            if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND)
            {
                success = false;
            }
            continue;
        }

        HKEY key = nullptr;
        if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            shellKey.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) != ERROR_SUCCESS)
        {
            success = false;
            continue;
        }
        const std::wstring icon = L"\"" + executablePath.wstring() + L"\",0";
        success = WriteRegistryString(key, nullptr, L"Edit in Rex's Toolkit") &&
            WriteRegistryString(key, L"Icon", icon) &&
            success;
        RegCloseKey(key);

        HKEY commandKey = nullptr;
        const std::wstring commandPath = shellKey + L"\\command";
        if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            commandPath.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &commandKey,
            nullptr) != ERROR_SUCCESS)
        {
            success = false;
            continue;
        }
        const std::wstring command =
            L"\"" + executablePath.wstring() + L"\" --edit \"%1\"";
        success = WriteRegistryString(commandKey, nullptr, command) && success;
        RegCloseKey(commandKey);
    }

    if (!success)
    {
        errorMessage = enabled
            ? L"Windows could not add the editor to every supported file menu."
            : L"Windows could not remove every editor file-menu entry.";
        if (enabled)
        {
            std::wstring ignored;
            SetMediaEditorContextMenuEnabled(false, executablePath, ignored);
        }
        return false;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool ShellIntegrationService::IsMediaEditorContextMenuEnabled() const
{
    HKEY key = nullptr;
    const std::wstring commandKey = MediaEditorShellKey(L".png") + L"\\command";
    const LSTATUS result = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        commandKey.c_str(),
        0,
        KEY_QUERY_VALUE,
        &key);
    if (result == ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return true;
    }
    return false;
}
