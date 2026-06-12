#include "MediaDownloadService.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <sstream>

namespace
{
class ScopedHandle
{
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle()
    {
        Reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const
    {
        return handle_;
    }

    HANDLE* Put()
    {
        Reset();
        return &handle_;
    }

    HANDLE Release()
    {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void Reset(HANDLE handle = nullptr)
    {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring ToLower(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });
    return value;
}

bool StartsWithHttp(const std::wstring& url)
{
    const std::wstring lower = ToLower(url);
    return lower.rfind(L"https://", 0) == 0 || lower.rfind(L"http://", 0) == 0;
}

std::optional<std::filesystem::path> FindOnPath(const std::wstring& executableName)
{
    std::array<wchar_t, 32768> buffer {};
    const DWORD length = SearchPathW(
        nullptr,
        executableName.c_str(),
        nullptr,
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);

    if (length == 0 || length >= buffer.size())
    {
        return std::nullopt;
    }

    return std::filesystem::path(buffer.data());
}

std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, MAX_PATH> buffer {};
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return std::filesystem::current_path();
    }

    return std::filesystem::path(buffer.data()).parent_path();
}

std::optional<std::filesystem::path> FindBesideExecutable(const std::vector<std::filesystem::path>& relativePaths)
{
    const std::filesystem::path executableDirectory = ExecutableDirectory();
    for (const std::filesystem::path& relativePath : relativePaths)
    {
        std::filesystem::path candidate = executableDirectory / relativePath;
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    return std::nullopt;
}

std::wstring QuoteProcessArgument(const std::wstring& argument)
{
    if (argument.empty())
    {
        return L"\"\"";
    }

    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needsQuotes)
    {
        return argument;
    }

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
            continue;
        }

        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }

    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine = QuoteProcessArgument(executable.wstring());
    for (const std::wstring& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += QuoteProcessArgument(argument);
    }
    return commandLine;
}

std::wstring BytesToWide(const std::string& bytes)
{
    if (bytes.empty())
    {
        return {};
    }

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length == 0)
    {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }

    if (length == 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), result.data(), length);
    return result;
}

std::optional<std::wstring> JsonStringValue(const std::wstring& json, const std::wstring& key)
{
    const std::wstring token = L"\"" + key + L"\"";
    size_t position = json.find(token);
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    position = json.find(L':', position + token.size());
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    ++position;
    while (position < json.size() && std::iswspace(json[position]))
    {
        ++position;
    }

    if (position >= json.size() || json[position] != L'"')
    {
        return std::nullopt;
    }

    ++position;
    std::wstring value;
    while (position < json.size())
    {
        const wchar_t ch = json[position++];
        if (ch == L'"')
        {
            return value;
        }
        if (ch == L'\\' && position < json.size())
        {
            const wchar_t escaped = json[position++];
            switch (escaped)
            {
            case L'"':
            case L'\\':
            case L'/':
                value.push_back(escaped);
                break;
            case L'n':
                value.push_back(L'\n');
                break;
            case L'r':
                value.push_back(L'\r');
                break;
            case L't':
                value.push_back(L'\t');
                break;
            default:
                value.push_back(escaped);
                break;
            }
            continue;
        }
        value.push_back(ch);
    }

    return std::nullopt;
}

std::optional<double> JsonNumberValue(const std::wstring& json, const std::wstring& key)
{
    const std::wstring token = L"\"" + key + L"\"";
    size_t position = json.find(token);
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    position = json.find(L':', position + token.size());
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }

    ++position;
    while (position < json.size() && std::iswspace(json[position]))
    {
        ++position;
    }

    size_t end = position;
    while (end < json.size() && (std::iswdigit(json[end]) || json[end] == L'.'))
    {
        ++end;
    }

    if (end == position)
    {
        return std::nullopt;
    }

    try
    {
        return std::stod(json.substr(position, end - position));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool JsonHasVideoFormat(const std::wstring& json)
{
    const std::wstring token = L"\"vcodec\"";
    size_t position = 0;
    while ((position = json.find(token, position)) != std::wstring::npos)
    {
        position = json.find(L':', position + token.size());
        if (position == std::wstring::npos)
        {
            return false;
        }

        ++position;
        while (position < json.size() && std::iswspace(json[position]))
        {
            ++position;
        }

        if (position >= json.size())
        {
            return false;
        }

        if (json.compare(position, 4, L"null") == 0)
        {
            position += 4;
            continue;
        }

        if (json[position] != L'"')
        {
            ++position;
            continue;
        }

        ++position;
        std::wstring value;
        while (position < json.size())
        {
            const wchar_t ch = json[position++];
            if (ch == L'"')
            {
                break;
            }
            if (ch == L'\\' && position < json.size())
            {
                value.push_back(json[position++]);
            }
            else
            {
                value.push_back(ch);
            }
        }

        value = ToLower(value);
        if (!value.empty() && value != L"none")
        {
            return true;
        }
    }

    return false;
}

std::wstring DurationLabel(double secondsValue)
{
    const int totalSeconds = std::max(0, static_cast<int>(secondsValue + 0.5));
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;

    wchar_t buffer[32] {};
    if (hours > 0)
    {
        swprintf_s(buffer, L"%d:%02d:%02d", hours, minutes, seconds);
    }
    else
    {
        swprintf_s(buffer, L"%d:%02d", minutes, seconds);
    }
    return buffer;
}

std::wstring ExtensionFor(MediaOutputFormat format)
{
    switch (format)
    {
    case MediaOutputFormat::Mp4:
        return L".mp4";
    case MediaOutputFormat::Mp3:
        return L".mp3";
    case MediaOutputFormat::Wav:
        return L".wav";
    }
    return L".mp4";
}

std::wstring Mp4FormatSelector(Mp4Quality quality)
{
    switch (quality)
    {
    case Mp4Quality::Best:
        return L"bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]/best";
    case Mp4Quality::P1080:
        return L"bv*[height<=1080][ext=mp4]+ba[ext=m4a]/b[height<=1080][ext=mp4]/best[height<=1080]";
    case Mp4Quality::P720:
        return L"bv*[height<=720][ext=mp4]+ba[ext=m4a]/b[height<=720][ext=mp4]/best[height<=720]";
    case Mp4Quality::P480:
        return L"bv*[height<=480][ext=mp4]+ba[ext=m4a]/b[height<=480][ext=mp4]/best[height<=480]";
    }
    return L"bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]/best";
}

std::wstring BitrateValue(Mp3Bitrate bitrate)
{
    switch (bitrate)
    {
    case Mp3Bitrate::K320:
        return L"320K";
    case Mp3Bitrate::K256:
        return L"256K";
    case Mp3Bitrate::K192:
        return L"192K";
    case Mp3Bitrate::K128:
        return L"128K";
    }
    return L"320K";
}

std::filesystem::path ResolveConflict(const std::filesystem::path& requestedPath)
{
    if (!std::filesystem::exists(requestedPath))
    {
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

void ParseProgressText(const std::wstring& text, MediaDownloadJob& job)
{
    if (text.find(L"[ExtractAudio]") != std::wstring::npos ||
        text.find(L"[Merger]") != std::wstring::npos ||
        text.find(L"ffmpeg") != std::wstring::npos)
    {
        job.status = MediaDownloadStatus::Converting;
    }
    else if (text.find(L"[download]") != std::wstring::npos)
    {
        job.status = MediaDownloadStatus::Downloading;
    }

    const size_t percentPosition = text.find(L'%');
    if (percentPosition != std::wstring::npos)
    {
        size_t start = percentPosition;
        while (start > 0 && (std::iswdigit(text[start - 1]) || text[start - 1] == L'.' || text[start - 1] == L' '))
        {
            --start;
        }

        try
        {
            const double percent = std::stod(text.substr(start, percentPosition - start));
            job.progress = std::clamp(percent / 100.0, 0.0, 1.0);
        }
        catch (...)
        {
        }
    }

    const size_t speedMarker = text.find(L" at ");
    if (speedMarker != std::wstring::npos)
    {
        size_t speedStart = speedMarker + 4;
        size_t speedEnd = text.find(L" ETA ", speedStart);
        if (speedEnd == std::wstring::npos)
        {
            speedEnd = text.find(L'\n', speedStart);
        }
        if (speedEnd != std::wstring::npos && speedEnd > speedStart)
        {
            job.speed = text.substr(speedStart, speedEnd - speedStart);
        }
    }

    const size_t etaMarker = text.find(L" ETA ");
    if (etaMarker != std::wstring::npos)
    {
        size_t etaStart = etaMarker + 5;
        size_t etaEnd = text.find_first_of(L"\r\n", etaStart);
        if (etaEnd == std::wstring::npos)
        {
            etaEnd = text.size();
        }
        if (etaEnd > etaStart)
        {
            job.eta = text.substr(etaStart, etaEnd - etaStart);
        }
    }
}

std::wstring JoinMissingToolsMessage(const ExternalToolStatus& tools)
{
    std::wstring message;
    if (!tools.ytDlpFound)
    {
        message += L"yt-dlp";
    }
    if (!tools.ffmpegFound)
    {
        if (!message.empty())
        {
            message += L" and ";
        }
        message += L"FFmpeg";
    }
    if (!message.empty())
    {
        message += L" must be installed and available on PATH.";
    }
    return message;
}
}

MediaPlatform SupportedPlatformRegistry::DetectPlatform(const std::wstring& url)
{
    if (!StartsWithHttp(url))
    {
        return MediaPlatform::Unknown;
    }

    const std::wstring lower = ToLower(url);
    if (lower.find(L"youtube.com/") != std::wstring::npos ||
        lower.find(L"youtu.be/") != std::wstring::npos ||
        lower.find(L"music.youtube.com/") != std::wstring::npos)
    {
        return MediaPlatform::YouTube;
    }

    if (lower.find(L"soundcloud.com/") != std::wstring::npos)
    {
        return MediaPlatform::SoundCloud;
    }

    return MediaPlatform::Unknown;
}

bool SupportedPlatformRegistry::IsSupportedUrl(const std::wstring& url)
{
    return DetectPlatform(url) != MediaPlatform::Unknown;
}

std::wstring SupportedPlatformRegistry::PlatformLabel(MediaPlatform platform)
{
    switch (platform)
    {
    case MediaPlatform::YouTube:
        return L"YouTube";
    case MediaPlatform::SoundCloud:
        return L"SoundCloud";
    case MediaPlatform::Unknown:
        break;
    }
    return L"Unknown";
}

ExternalToolStatus ExternalToolService::CheckTools() const
{
    ExternalToolStatus status;
    if (const auto ytDlp = FindBesideExecutable({ L"tools\\yt-dlp.exe", L"yt-dlp.exe" }))
    {
        status.ytDlpFound = true;
        status.ytDlpPath = *ytDlp;
    }
    else if (const auto ytDlpOnPath = FindOnPath(L"yt-dlp.exe"))
    {
        status.ytDlpFound = true;
        status.ytDlpPath = *ytDlpOnPath;
    }
    else if (const auto ytDlpNoExtension = FindOnPath(L"yt-dlp"))
    {
        status.ytDlpFound = true;
        status.ytDlpPath = *ytDlpNoExtension;
    }

    if (const auto ffmpeg = FindBesideExecutable({ L"tools\\ffmpeg.exe", L"ffmpeg.exe" }))
    {
        status.ffmpegFound = true;
        status.ffmpegPath = *ffmpeg;
    }
    else if (const auto ffmpegOnPath = FindOnPath(L"ffmpeg.exe"))
    {
        status.ffmpegFound = true;
        status.ffmpegPath = *ffmpegOnPath;
    }
    else if (const auto ffmpegNoExtension = FindOnPath(L"ffmpeg"))
    {
        status.ffmpegFound = true;
        status.ffmpegPath = *ffmpegNoExtension;
    }

    return status;
}

std::filesystem::path ExternalToolService::DefaultDownloadsFolder()
{
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path)
    {
        std::filesystem::path downloads = path;
        CoTaskMemFree(path);
        return downloads;
    }

    wchar_t userProfile[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", userProfile, static_cast<DWORD>(std::size(userProfile)));
    if (length > 0 && length < std::size(userProfile))
    {
        return std::filesystem::path(userProfile) / L"Downloads";
    }

    return std::filesystem::current_path();
}

ProcessResult ProcessRunner::Run(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::atomic_bool& cancelRequested,
    const OutputCallback& outputCallback) const
{
    ProcessResult result;

    SECURITY_ATTRIBUTES securityAttributes {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    ScopedHandle readPipe;
    ScopedHandle writePipe;
    if (!CreatePipe(readPipe.Put(), writePipe.Put(), &securityAttributes, 0))
    {
        result.output = L"Could not create process pipe.";
        return result;
    }
    SetHandleInformation(readPipe.Get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdOutput = writePipe.Get();
    startupInfo.hStdError = writePipe.Get();
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION processInformation {};
    std::wstring commandLine = BuildCommandLine(executable, arguments);

    const BOOL created = CreateProcessW(
        executable.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInformation);

    writePipe.Reset();

    if (!created)
    {
        result.output = L"Could not start external tool.";
        return result;
    }

    ScopedHandle process(processInformation.hProcess);
    ScopedHandle thread(processInformation.hThread);

    std::string pendingBytes;
    std::array<char, 4096> buffer {};
    bool processRunning = true;
    while (processRunning)
    {
        if (cancelRequested.load())
        {
            TerminateProcess(process.Get(), 1);
            result.cancelled = true;
        }

        DWORD available = 0;
        while (PeekNamedPipe(readPipe.Get(), nullptr, 0, nullptr, &available, nullptr) && available > 0)
        {
            DWORD bytesRead = 0;
            const DWORD bytesToRead = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(readPipe.Get(), buffer.data(), bytesToRead, &bytesRead, nullptr) || bytesRead == 0)
            {
                break;
            }

            std::string chunk(buffer.data(), buffer.data() + bytesRead);
            pendingBytes += chunk;
            const std::wstring wideChunk = BytesToWide(chunk);
            result.output += wideChunk;
            if (outputCallback)
            {
                outputCallback(wideChunk);
            }
            available = 0;
        }

        const DWORD waitResult = WaitForSingleObject(process.Get(), 50);
        processRunning = waitResult == WAIT_TIMEOUT;
    }

    DWORD bytesRead = 0;
    while (ReadFile(readPipe.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
    {
        std::string chunk(buffer.data(), buffer.data() + bytesRead);
        const std::wstring wideChunk = BytesToWide(chunk);
        result.output += wideChunk;
        if (outputCallback)
        {
            outputCallback(wideChunk);
        }
    }

    GetExitCodeProcess(process.Get(), &result.exitCode);
    if (result.cancelled)
    {
        result.exitCode = 1;
    }
    return result;
}

std::optional<MediaDownloadJob> MediaMetadataService::Analyze(
    const std::wstring& url,
    const ExternalToolStatus& tools,
    const std::atomic_bool& cancelRequested,
    std::wstring& errorMessage) const
{
    if (!SupportedPlatformRegistry::IsSupportedUrl(url))
    {
        errorMessage = L"Unsupported URL. Please enter a YouTube or SoundCloud link.";
        return std::nullopt;
    }

    if (!tools.ytDlpFound)
    {
        errorMessage = L"yt-dlp must be installed and available on PATH.";
        return std::nullopt;
    }

    std::vector<std::wstring> arguments {
        L"--dump-single-json",
        L"--skip-download",
        L"--no-warnings",
        L"--no-playlist",
        url
    };

    const ProcessResult result = processRunner_.Run(tools.ytDlpPath, arguments, cancelRequested, nullptr);
    if (result.cancelled)
    {
        errorMessage = L"Analysis cancelled.";
        return std::nullopt;
    }

    if (result.exitCode != 0)
    {
        errorMessage = result.output.empty() ? L"Could not analyze this URL." : result.output;
        return std::nullopt;
    }

    MediaDownloadJob job;
    job.url = url;
    job.platform = SupportedPlatformRegistry::DetectPlatform(url);
    job.title = JsonStringValue(result.output, L"title").value_or(L"Untitled media");
    job.uploader = JsonStringValue(result.output, L"uploader").value_or(JsonStringValue(result.output, L"channel").value_or(L"Unknown"));
    job.thumbnailUrl = JsonStringValue(result.output, L"thumbnail").value_or(L"");
    if (const auto duration = JsonNumberValue(result.output, L"duration"))
    {
        job.duration = DurationLabel(*duration);
    }
    else
    {
        job.duration = L"Unknown";
    }

    const std::wstring vcodec = ToLower(JsonStringValue(result.output, L"vcodec").value_or(L""));
    const bool hasVideoFormat = JsonHasVideoFormat(result.output);
    if (job.platform == MediaPlatform::SoundCloud)
    {
        job.mediaType = MediaType::Audio;
    }
    else if (hasVideoFormat || (!vcodec.empty() && vcodec != L"none"))
    {
        job.mediaType = MediaType::Video;
    }
    else if (vcodec == L"none")
    {
        job.mediaType = MediaType::Audio;
    }
    else
    {
        job.mediaType = job.platform == MediaPlatform::YouTube ? MediaType::Video : MediaType::Unknown;
    }

    job.status = MediaDownloadStatus::Ready;
    return job;
}

ExternalToolStatus MediaDownloadService::CheckExternalTools() const
{
    return externalToolService_.CheckTools();
}

std::optional<MediaDownloadJob> MediaDownloadService::Analyze(
    const std::wstring& url,
    const std::atomic_bool& cancelRequested,
    std::wstring& errorMessage) const
{
    const ExternalToolStatus tools = CheckExternalTools();
    return metadataService_.Analyze(url, tools, cancelRequested, errorMessage);
}

MediaDownloadJob MediaDownloadService::Download(
    MediaDownloadJob job,
    const MediaDownloadOptions& options,
    const std::atomic_bool& cancelRequested,
    const ProgressCallback& progressCallback) const
{
    const ExternalToolStatus tools = CheckExternalTools();
    const std::wstring missingTools = JoinMissingToolsMessage(tools);
    if (!missingTools.empty())
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = missingTools;
        return job;
    }

    if (!SupportedPlatformRegistry::IsSupportedUrl(job.url))
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"Unsupported URL. Please enter a YouTube or SoundCloud link.";
        return job;
    }

    if (options.outputFolder.empty() || !std::filesystem::exists(options.outputFolder))
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"Output folder does not exist.";
        return job;
    }

    if (options.outputFormat == MediaOutputFormat::Mp4 &&
        job.platform == MediaPlatform::SoundCloud &&
        job.mediaType == MediaType::Audio)
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = L"This source appears to be audio-only. Choose MP3 or WAV instead.";
        return job;
    }

    job.outputFormat = options.outputFormat;
    job.mp4Quality = options.mp4Quality;
    job.mp3Bitrate = options.mp3Bitrate;
    job.outputFolder = options.outputFolder;
    job.progress = 0.0;
    job.speed.clear();
    job.eta.clear();
    job.errorMessage.clear();

    std::wstring baseName = options.customFileName.empty() ? job.title : options.customFileName;
    baseName = SanitizeFileName(baseName);
    const std::filesystem::path requestedPath = options.outputFolder / (baseName + ExtensionFor(options.outputFormat));
    job.outputFilePath = ResolveConflict(requestedPath);

    std::filesystem::path outputTemplate = job.outputFilePath.parent_path() / (job.outputFilePath.stem().wstring() + L".%(ext)s");

    std::vector<std::wstring> arguments {
        L"--no-playlist",
        L"--newline",
        L"--ffmpeg-location",
        tools.ffmpegPath.parent_path().wstring(),
        L"-o",
        outputTemplate.wstring()
    };

    if (options.outputFormat == MediaOutputFormat::Mp4)
    {
        arguments.push_back(L"-f");
        arguments.push_back(Mp4FormatSelector(options.mp4Quality));
        arguments.push_back(L"--merge-output-format");
        arguments.push_back(L"mp4");
    }
    else if (options.outputFormat == MediaOutputFormat::Mp3)
    {
        arguments.push_back(L"-x");
        arguments.push_back(L"--audio-format");
        arguments.push_back(L"mp3");
        arguments.push_back(L"--audio-quality");
        arguments.push_back(BitrateValue(options.mp3Bitrate));
    }
    else
    {
        arguments.push_back(L"-x");
        arguments.push_back(L"--audio-format");
        arguments.push_back(L"wav");
    }

    arguments.push_back(job.url);

    job.status = MediaDownloadStatus::Downloading;
    if (progressCallback)
    {
        progressCallback(job);
    }

    ProcessResult result = processRunner_.Run(
        tools.ytDlpPath,
        arguments,
        cancelRequested,
        [&](const std::wstring& output)
        {
            ParseProgressText(output, job);
            if (progressCallback)
            {
                progressCallback(job);
            }
        });

    if (result.cancelled)
    {
        job.status = MediaDownloadStatus::Cancelled;
        job.errorMessage = L"Download cancelled.";
        return job;
    }

    if (result.exitCode != 0)
    {
        job.status = MediaDownloadStatus::Failed;
        job.errorMessage = result.output.empty() ? L"Download failed." : result.output;
        return job;
    }

    if (!std::filesystem::exists(job.outputFilePath))
    {
        const std::filesystem::path fallbackPath = job.outputFilePath.parent_path() /
            (job.outputFilePath.stem().wstring() + ExtensionFor(options.outputFormat));
        if (std::filesystem::exists(fallbackPath))
        {
            job.outputFilePath = fallbackPath;
        }
    }

    job.progress = 1.0;
    job.status = MediaDownloadStatus::Complete;
    job.errorMessage.clear();
    return job;
}

std::wstring MediaDownloadService::FormatLabel(MediaOutputFormat format)
{
    switch (format)
    {
    case MediaOutputFormat::Mp4:
        return L"MP4";
    case MediaOutputFormat::Mp3:
        return L"MP3";
    case MediaOutputFormat::Wav:
        return L"WAV";
    }
    return L"MP4";
}

std::wstring MediaDownloadService::Mp4QualityLabel(Mp4Quality quality)
{
    switch (quality)
    {
    case Mp4Quality::Best:
        return L"Best available";
    case Mp4Quality::P1080:
        return L"1080p";
    case Mp4Quality::P720:
        return L"720p";
    case Mp4Quality::P480:
        return L"480p";
    }
    return L"Best available";
}

std::wstring MediaDownloadService::Mp3BitrateLabel(Mp3Bitrate bitrate)
{
    switch (bitrate)
    {
    case Mp3Bitrate::K320:
        return L"320 kbps";
    case Mp3Bitrate::K256:
        return L"256 kbps";
    case Mp3Bitrate::K192:
        return L"192 kbps";
    case Mp3Bitrate::K128:
        return L"128 kbps";
    }
    return L"320 kbps";
}

std::wstring MediaDownloadService::MediaTypeLabel(MediaType mediaType)
{
    switch (mediaType)
    {
    case MediaType::Video:
        return L"Video";
    case MediaType::Audio:
        return L"Audio";
    case MediaType::Unknown:
        break;
    }
    return L"Unknown";
}

std::wstring MediaDownloadService::StatusLabel(MediaDownloadStatus status)
{
    switch (status)
    {
    case MediaDownloadStatus::Idle:
        return L"Idle";
    case MediaDownloadStatus::Analyzing:
        return L"Analyzing";
    case MediaDownloadStatus::Ready:
        return L"Ready";
    case MediaDownloadStatus::Downloading:
        return L"Downloading";
    case MediaDownloadStatus::Converting:
        return L"Converting";
    case MediaDownloadStatus::Complete:
        return L"Complete";
    case MediaDownloadStatus::Failed:
        return L"Failed";
    case MediaDownloadStatus::Cancelled:
        return L"Cancelled";
    }
    return L"Idle";
}

std::wstring MediaDownloadService::SanitizeFileName(const std::wstring& value)
{
    std::wstring sanitized;
    for (wchar_t ch : value)
    {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch))
        {
            sanitized.push_back(L'_');
        }
        else
        {
            sanitized.push_back(ch);
        }
    }

    while (!sanitized.empty() && (sanitized.back() == L'.' || sanitized.back() == L' '))
    {
        sanitized.pop_back();
    }

    while (!sanitized.empty() && sanitized.front() == L' ')
    {
        sanitized.erase(sanitized.begin());
    }

    if (sanitized.empty())
    {
        sanitized = L"media";
    }

    if (sanitized.size() > 160)
    {
        sanitized.resize(160);
    }

    return sanitized;
}
