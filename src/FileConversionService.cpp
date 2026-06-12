#include "FileConversionService.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace
{
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

std::wstring ToLower(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch)
        {
            return static_cast<wchar_t>(::towlower(ch));
        });
    return value;
}

std::wstring HresultMessage(HRESULT result)
{
    wchar_t* message = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        result,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstring text = message ? message : L"Unknown error.";
    if (message)
    {
        LocalFree(message);
    }

    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L'.'))
    {
        text.pop_back();
    }
    return text;
}

bool Failed(HRESULT result, std::wstring& errorMessage, const wchar_t* context)
{
    if (SUCCEEDED(result))
    {
        return false;
    }

    errorMessage = std::wstring(context) + L": " + HresultMessage(result);
    return true;
}

const GUID& ContainerFormatFor(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Png:
        return GUID_ContainerFormatPng;
    case ImageFormat::Jpg:
        return GUID_ContainerFormatJpeg;
    case ImageFormat::Webp:
        return GUID_ContainerFormatWebp;
    case ImageFormat::Bmp:
        return GUID_ContainerFormatBmp;
    }

    return GUID_ContainerFormatPng;
}

bool IsJpg(ImageFormat format)
{
    return format == ImageFormat::Jpg;
}

bool HasReadableFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

bool CreateWicFactory(ComPtr<IWICImagingFactory>& factory, std::wstring& errorMessage)
{
    const HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));

    return !Failed(result, errorMessage, L"Could not start the image conversion engine");
}

std::wstring BytesLabel(unsigned long long bytes)
{
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    std::wostringstream output;
    if (unitIndex == 0)
    {
        output << static_cast<unsigned long long>(value) << L" " << units[unitIndex];
    }
    else
    {
        output.setf(std::ios::fixed);
        output.precision(1);
        output << value << L" " << units[unitIndex];
    }
    return output.str();
}

void SetEncoderQuality(IPropertyBag2* propertyBag, const wchar_t* propertyName, float quality)
{
    if (!propertyBag)
    {
        return;
    }

    PROPBAG2 option {};
    option.pstrName = const_cast<LPOLESTR>(propertyName);

    VARIANT value {};
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    propertyBag->Write(1, &option, &value);
    VariantClear(&value);
}

void SetEncoderBool(IPropertyBag2* propertyBag, const wchar_t* propertyName, bool enabled)
{
    if (!propertyBag)
    {
        return;
    }

    PROPBAG2 option {};
    option.pstrName = const_cast<LPOLESTR>(propertyName);

    VARIANT value {};
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    propertyBag->Write(1, &option, &value);
    VariantClear(&value);
}

HRESULT CopyConvertedSource(
    IWICImagingFactory* factory,
    IWICBitmapSource* source,
    const WICPixelFormatGUID& pixelFormat,
    IWICBitmapSource** convertedSource)
{
    ComPtr<IWICFormatConverter> converter;
    HRESULT result = factory->CreateFormatConverter(&converter);
    if (FAILED(result))
    {
        return result;
    }

    result = converter->Initialize(
        source,
        pixelFormat,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result))
    {
        return result;
    }

    *convertedSource = converter.Detach();
    return S_OK;
}

HRESULT CreateFlattenedJpegSource(
    IWICImagingFactory* factory,
    IWICBitmapSource* source,
    JpgBackground background,
    IWICBitmapSource** flattenedSource)
{
    ComPtr<IWICBitmapSource> bgraSource;
    HRESULT result = CopyConvertedSource(factory, source, GUID_WICPixelFormat32bppBGRA, &bgraSource);
    if (FAILED(result))
    {
        return result;
    }

    UINT width = 0;
    UINT height = 0;
    result = bgraSource->GetSize(&width, &height);
    if (FAILED(result))
    {
        return result;
    }

    const UINT sourceStride = width * 4;
    const UINT destinationStride = width * 3;
    std::vector<BYTE> sourcePixels(static_cast<size_t>(sourceStride) * height);
    std::vector<BYTE> destinationPixels(static_cast<size_t>(destinationStride) * height);

    result = bgraSource->CopyPixels(nullptr, sourceStride, static_cast<UINT>(sourcePixels.size()), sourcePixels.data());
    if (FAILED(result))
    {
        return result;
    }

    const BYTE backgroundValue = background == JpgBackground::Black ? 0 : 255;
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = sourcePixels.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destinationPixels.data() + (static_cast<size_t>(y) * destinationStride);

        for (UINT x = 0; x < width; ++x)
        {
            const BYTE blue = sourceRow[x * 4 + 0];
            const BYTE green = sourceRow[x * 4 + 1];
            const BYTE red = sourceRow[x * 4 + 2];
            const BYTE alpha = sourceRow[x * 4 + 3];

            const auto blend = [alpha, backgroundValue](BYTE channel)
            {
                return static_cast<BYTE>((static_cast<unsigned int>(channel) * alpha +
                    static_cast<unsigned int>(backgroundValue) * (255 - alpha)) / 255);
            };

            destinationRow[x * 3 + 0] = blend(blue);
            destinationRow[x * 3 + 1] = blend(green);
            destinationRow[x * 3 + 2] = blend(red);
        }
    }

    ComPtr<IWICBitmap> flattenedBitmap;
    result = factory->CreateBitmapFromMemory(
        width,
        height,
        GUID_WICPixelFormat24bppBGR,
        destinationStride,
        static_cast<UINT>(destinationPixels.size()),
        destinationPixels.data(),
        &flattenedBitmap);
    if (FAILED(result))
    {
        return result;
    }

    *flattenedSource = flattenedBitmap.Detach();
    return S_OK;
}
}

bool SupportedFormatRegistry::IsSupportedInputExtension(const std::filesystem::path& path)
{
    return ImageFormatFromExtension(path).has_value();
}

std::wstring SupportedFormatRegistry::ExtensionFor(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Png:
        return L".png";
    case ImageFormat::Jpg:
        return L".jpg";
    case ImageFormat::Webp:
        return L".webp";
    case ImageFormat::Bmp:
        return L".bmp";
    }

    return L".png";
}

std::wstring SupportedFormatRegistry::LabelFor(ImageFormat format)
{
    switch (format)
    {
    case ImageFormat::Png:
        return L"PNG";
    case ImageFormat::Jpg:
        return L"JPG";
    case ImageFormat::Webp:
        return L"WEBP";
    case ImageFormat::Bmp:
        return L"BMP";
    }

    return L"PNG";
}

std::optional<ImageFormat> SupportedFormatRegistry::ImageFormatFromExtension(const std::filesystem::path& path)
{
    const std::wstring extension = ToLower(path.extension().wstring());
    if (extension == L".png")
    {
        return ImageFormat::Png;
    }
    if (extension == L".jpg" || extension == L".jpeg")
    {
        return ImageFormat::Jpg;
    }
    if (extension == L".webp")
    {
        return ImageFormat::Webp;
    }
    if (extension == L".bmp")
    {
        return ImageFormat::Bmp;
    }
    return std::nullopt;
}

bool ImageConverter::ReadImageInfo(ConversionJob& job, std::wstring& errorMessage) const
{
    ScopedCom com;

    if (!std::filesystem::exists(job.inputPath))
    {
        errorMessage = L"File does not exist.";
        return false;
    }

    if (!HasReadableFile(job.inputPath))
    {
        errorMessage = L"File could not be opened for reading.";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, errorMessage))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result = factory->CreateDecoderFromFilename(
        job.inputPath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (Failed(result, errorMessage, L"Could not read image"))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (Failed(result, errorMessage, L"Could not read the first image frame"))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    if (Failed(result, errorMessage, L"Could not read image dimensions"))
    {
        return false;
    }

    job.width = width;
    job.height = height;
    return true;
}

bool ImageConverter::CanEncode(ImageFormat format) const
{
    ScopedCom com;

    std::wstring errorMessage;
    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, errorMessage))
    {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    return SUCCEEDED(factory->CreateEncoder(ContainerFormatFor(format), nullptr, &encoder));
}

bool ImageConverter::ConvertImage(ConversionJob& job, const ConversionOptions& options, std::wstring& errorMessage) const
{
    ScopedCom com;

    job.outputFormat = options.outputFormat;
    job.outputPath = BuildOutputPath(job, options, errorMessage);
    if (job.outputPath.empty())
    {
        return false;
    }

    bool skipped = false;
    job.outputPath = ResolveConflict(job.outputPath, options.conflictBehavior, skipped);
    if (skipped)
    {
        job.status = ConversionStatus::Skipped;
        job.errorMessage = L"Skipped because the output file already exists.";
        return true;
    }

    std::filesystem::create_directories(job.outputPath.parent_path());

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory, errorMessage))
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result = factory->CreateDecoderFromFilename(
        job.inputPath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (Failed(result, errorMessage, L"Could not decode image"))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> sourceFrame;
    result = decoder->GetFrame(0, &sourceFrame);
    if (Failed(result, errorMessage, L"Could not read image frame"))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    result = sourceFrame->GetSize(&width, &height);
    if (Failed(result, errorMessage, L"Could not read image dimensions"))
    {
        return false;
    }

    ComPtr<IWICBitmapSource> bitmapSource;
    WICPixelFormatGUID targetPixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (IsJpg(options.outputFormat))
    {
        result = CreateFlattenedJpegSource(factory.Get(), sourceFrame.Get(), options.jpgBackground, &bitmapSource);
        targetPixelFormat = GUID_WICPixelFormat24bppBGR;
    }
    else if (options.outputFormat == ImageFormat::Bmp)
    {
        result = CopyConvertedSource(factory.Get(), sourceFrame.Get(), GUID_WICPixelFormat32bppBGRA, &bitmapSource);
        targetPixelFormat = GUID_WICPixelFormat32bppBGRA;
    }
    else
    {
        result = CopyConvertedSource(factory.Get(), sourceFrame.Get(), GUID_WICPixelFormat32bppBGRA, &bitmapSource);
        targetPixelFormat = GUID_WICPixelFormat32bppBGRA;
    }

    if (Failed(result, errorMessage, L"Could not prepare image pixels"))
    {
        return false;
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (Failed(result, errorMessage, L"Could not create output stream"))
    {
        return false;
    }

    result = stream->InitializeFromFilename(job.outputPath.c_str(), GENERIC_WRITE);
    if (Failed(result, errorMessage, L"Could not open output file"))
    {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(ContainerFormatFor(options.outputFormat), nullptr, &encoder);
    if (Failed(result, errorMessage, L"Could not create encoder for the selected format"))
    {
        return false;
    }

    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (Failed(result, errorMessage, L"Could not initialize encoder"))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    ComPtr<IPropertyBag2> propertyBag;
    result = encoder->CreateNewFrame(&frameEncode, &propertyBag);
    if (Failed(result, errorMessage, L"Could not create output frame"))
    {
        return false;
    }

    if (options.outputFormat == ImageFormat::Jpg)
    {
        SetEncoderQuality(propertyBag.Get(), L"ImageQuality", static_cast<float>(options.jpgQuality) / 100.0f);
    }
    else if (options.outputFormat == ImageFormat::Webp)
    {
        SetEncoderQuality(propertyBag.Get(), L"ImageQuality", static_cast<float>(options.webpQuality) / 100.0f);
        SetEncoderBool(propertyBag.Get(), L"Lossless", options.webpLossless);
    }

    result = frameEncode->Initialize(propertyBag.Get());
    if (Failed(result, errorMessage, L"Could not initialize output frame"))
    {
        return false;
    }

    result = frameEncode->SetSize(width, height);
    if (Failed(result, errorMessage, L"Could not set output size"))
    {
        return false;
    }

    WICPixelFormatGUID framePixelFormat = targetPixelFormat;
    result = frameEncode->SetPixelFormat(&framePixelFormat);
    if (Failed(result, errorMessage, L"Could not set output pixel format"))
    {
        return false;
    }

    result = frameEncode->WriteSource(bitmapSource.Get(), nullptr);
    if (Failed(result, errorMessage, L"Could not write converted pixels"))
    {
        return false;
    }

    result = frameEncode->Commit();
    if (Failed(result, errorMessage, L"Could not finish output frame"))
    {
        return false;
    }

    result = encoder->Commit();
    if (Failed(result, errorMessage, L"Could not finish output image"))
    {
        return false;
    }

    job.width = width;
    job.height = height;
    return true;
}

std::filesystem::path ImageConverter::BuildOutputPath(
    const ConversionJob& job,
    const ConversionOptions& options,
    std::wstring& errorMessage) const
{
    if (!job.outputPath.empty())
    {
        if (job.outputPath.has_parent_path())
        {
            std::error_code errorCode;
            std::filesystem::create_directories(job.outputPath.parent_path(), errorCode);
            if (errorCode)
            {
                errorMessage = L"Could not create output folder.";
                return {};
            }
        }

        return job.outputPath;
    }

    std::filesystem::path outputDirectory;
    switch (options.outputDirectoryMode)
    {
    case OutputDirectoryMode::ConvertedSubfolder:
        outputDirectory = job.inputPath.parent_path() / L"Converted";
        break;
    case OutputDirectoryMode::SameFolder:
        outputDirectory = job.inputPath.parent_path();
        break;
    case OutputDirectoryMode::ChosenFolder:
        if (options.outputDirectory.empty())
        {
            errorMessage = L"Choose an output folder first.";
            return {};
        }
        outputDirectory = options.outputDirectory;
        break;
    }

    std::error_code errorCode;
    if (!std::filesystem::exists(outputDirectory, errorCode))
    {
        std::filesystem::create_directories(outputDirectory, errorCode);
    }

    if (errorCode)
    {
        errorMessage = L"Could not create output folder.";
        return {};
    }

    std::filesystem::path fileName = job.inputPath.stem();
    fileName += SupportedFormatRegistry::ExtensionFor(options.outputFormat);
    return outputDirectory / fileName;
}

std::filesystem::path ImageConverter::ResolveConflict(
    const std::filesystem::path& requestedPath,
    ConflictBehavior behavior,
    bool& skipped) const
{
    skipped = false;
    if (!std::filesystem::exists(requestedPath))
    {
        return requestedPath;
    }

    if (behavior == ConflictBehavior::Overwrite)
    {
        return requestedPath;
    }

    if (behavior == ConflictBehavior::Skip)
    {
        skipped = true;
        return requestedPath;
    }

    const std::filesystem::path directory = requestedPath.parent_path();
    const std::wstring stem = requestedPath.stem().wstring();
    const std::wstring extension = requestedPath.extension().wstring();

    for (int suffix = 1; suffix < 10000; ++suffix)
    {
        std::filesystem::path candidate = directory / (stem + L"_" + std::to_wstring(suffix) + extension);
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    return requestedPath;
}

std::optional<ConversionJob> FileConversionService::CreateJob(
    const std::filesystem::path& path,
    std::wstring& warning) const
{
    if (std::filesystem::is_directory(path))
    {
        warning = L"Folder conversion is not supported yet.";
        return std::nullopt;
    }

    const auto inputFormat = SupportedFormatRegistry::ImageFormatFromExtension(path);
    if (!inputFormat)
    {
        warning = L"Unsupported file type.";
        return std::nullopt;
    }

    ConversionJob job {};
    job.inputPath = path;
    job.fileName = path.filename().wstring();
    job.inputFormat = SupportedFormatRegistry::LabelFor(*inputFormat);
    job.outputFormat = ImageFormat::Png;

    std::error_code errorCode;
    job.fileSize = std::filesystem::file_size(path, errorCode);
    if (errorCode)
    {
        job.fileSize = 0;
    }

    std::wstring errorMessage;
    if (!imageConverter_.ReadImageInfo(job, errorMessage))
    {
        job.status = ConversionStatus::Failed;
        job.errorMessage = errorMessage;
    }

    return job;
}

std::vector<ImageFormat> FileConversionService::SupportedOutputFormats() const
{
    std::vector<ImageFormat> formats;
    for (ImageFormat format : { ImageFormat::Png, ImageFormat::Jpg, ImageFormat::Webp, ImageFormat::Bmp })
    {
        if (imageConverter_.CanEncode(format))
        {
            formats.push_back(format);
        }
    }

    if (formats.empty())
    {
        formats.push_back(ImageFormat::Png);
    }

    return formats;
}

bool FileConversionService::CanConvertAny(const std::vector<ConversionJob>& jobs) const
{
    return std::any_of(
        jobs.begin(),
        jobs.end(),
        [](const ConversionJob& job)
        {
            return job.status == ConversionStatus::Pending ||
                job.status == ConversionStatus::Failed ||
                job.status == ConversionStatus::Skipped;
        });
}

void FileConversionService::ConvertBatch(
    std::vector<ConversionJob> jobs,
    ConversionOptions options,
    const std::atomic_bool& cancelRequested,
    const ProgressCallback& callback) const
{
    for (size_t index = 0; index < jobs.size(); ++index)
    {
        ConversionJob job = jobs[index];
        job.outputFormat = options.outputFormat;

        if (cancelRequested.load())
        {
            job.status = ConversionStatus::Skipped;
            job.errorMessage = L"Cancelled.";
            callback({ job, index });
            continue;
        }

        if (job.status == ConversionStatus::Failed && !job.errorMessage.empty())
        {
            callback({ job, index });
            continue;
        }

        job.status = ConversionStatus::Converting;
        job.errorMessage.clear();
        callback({ job, index });

        std::wstring errorMessage;
        if (imageConverter_.ConvertImage(job, options, errorMessage))
        {
            if (job.status != ConversionStatus::Skipped)
            {
                job.status = ConversionStatus::Complete;
                job.errorMessage.clear();
            }
        }
        else
        {
            job.status = ConversionStatus::Failed;
            job.errorMessage = errorMessage.empty() ? L"Conversion failed." : errorMessage;
        }

        callback({ job, index });
    }
}
