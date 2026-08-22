#include "EqualizerAtomicFile.h"
#include "EqualizerConfigParser.h"
#include "EqualizerService.h"
#include "EqualizerSetupAutomation.h"

#include <propsys.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shellapi.h>

#include <cwctype>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>
#include <vector>

namespace rex::equalizer
{
namespace
{
constexpr wchar_t kEqualizerApoRegistryPath[] = L"SOFTWARE\\EqualizerAPO";
constexpr wchar_t kManagedFileName[] = L"rexs_toolkit_equalizer.txt";
constexpr wchar_t kMarkerBegin[] = L"# Rex's Toolkit Equalizer begin";
constexpr wchar_t kMarkerEnd[] = L"# Rex's Toolkit Equalizer end";

std::wstring Trim(std::wstring value)
{
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring ErrorFromCode(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (!length || !buffer) return L"Windows error " + std::to_wstring(code);
    std::wstring message(buffer, length);
    LocalFree(buffer);
    return Trim(message);
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::wstring RegistryString(HKEY root, const std::wstring& path, const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) return {};
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (type == REG_EXPAND_SZ)
    {
        const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (required > 1)
        {
            std::wstring expanded(required, L'\0');
            ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
            while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
            value = std::move(expanded);
        }
    }
    return value;
}

bool RegistryKeyExists(HKEY root, const std::wstring& path)
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_SUCCESS) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring ExtractEndpointGuid(const std::wstring& id)
{
    const size_t close = id.find_last_of(L'}');
    if (close == std::wstring::npos) return {};
    const size_t open = id.find_last_of(L'{', close);
    if (open == std::wstring::npos || close <= open) return {};
    return id.substr(open, close - open + 1);
}

bool IsApoConfiguredForEndpoint(const std::wstring& endpointGuid)
{
    return setup::IsEndpointConfigured(endpointGuid);
}

bool ReadTextPreservingEncoding(
    const std::filesystem::path& path,
    std::wstring& text,
    bool& utf16,
    bool& utf8Bom,
    std::wstring& errorMessage)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        errorMessage = L"Could not open Equalizer APO's config.txt.";
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
    utf8Bom = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF;
    if (utf16)
    {
        const size_t characters = (bytes.size() - 2) / sizeof(wchar_t);
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), characters);
    }
    else
    {
        if (utf8Bom) bytes.erase(0, 3);
        text = Utf8ToWide(bytes);
    }
    return true;
}

bool WriteTextPreservingEncoding(
    const std::filesystem::path& path,
    const std::wstring& text,
    bool utf16,
    bool utf8Bom,
    std::wstring& errorMessage)
{
    const std::filesystem::path temporary = path.wstring() + L".rex.tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        errorMessage = L"Equalizer APO's configuration folder is not writable.";
        return false;
    }
    if (utf16)
    {
        const unsigned char bom[] { 0xFF, 0xFE };
        file.write(reinterpret_cast<const char*>(bom), 2);
        file.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    }
    else
    {
        if (utf8Bom)
        {
            const unsigned char bom[] { 0xEF, 0xBB, 0xBF };
            file.write(reinterpret_cast<const char*>(bom), 3);
        }
        const std::string utf8 = WideToUtf8(text);
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    file.flush();
    if (!file)
    {
        errorMessage = L"Equalizer APO's configuration could not be written completely.";
        return false;
    }
    file.close();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        errorMessage = L"Equalizer APO's configuration could not be updated: " + ErrorFromCode(GetLastError());
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool ManagedIncludePresent(const std::filesystem::path& configPath)
{
    if (configPath.empty()) return false;
    std::wstring text;
    bool utf16 = false;
    bool utf8Bom = false;
    std::wstring ignored;
    if (!ReadTextPreservingEncoding(configPath / L"config.txt", text, utf16, utf8Bom, ignored)) return false;
    return detail::ContainsActiveManagedInclude(text) &&
        std::filesystem::exists(configPath / kManagedFileName);
}

bool EnsureManagedIncludeAtPath(const std::filesystem::path& configPath, std::wstring& errorMessage)
{
    if (configPath.empty() || !std::filesystem::exists(configPath))
    {
        errorMessage = L"Equalizer APO's configuration folder was not found.";
        return false;
    }
    const std::filesystem::path rootConfig = configPath / L"config.txt";
    std::wstring text;
    bool utf16 = false;
    bool utf8Bom = false;
    if (!ReadTextPreservingEncoding(rootConfig, text, utf16, utf8Bom, errorMessage)) return false;
    if (!detail::ContainsActiveManagedInclude(text))
    {
        const std::filesystem::path backup = configPath / L"config.txt.rex-backup";
        std::error_code ec;
        if (!std::filesystem::exists(backup))
        {
            std::filesystem::copy_file(rootConfig, backup, std::filesystem::copy_options::none, ec);
            if (ec)
            {
                errorMessage = L"Could not create a one-time backup of Equalizer APO's config.txt.";
                return false;
            }
        }
        if (!text.empty() && text.back() != L'\n') text += L"\r\n";
        text += std::wstring(kMarkerBegin) + L"\r\n" +
            std::wstring(detail::kManagedIncludeDirective) + L"\r\n" + kMarkerEnd + L"\r\n";
        if (!WriteTextPreservingEncoding(rootConfig, text, utf16, utf8Bom, errorMessage)) return false;
    }
    const std::filesystem::path managed = configPath / kManagedFileName;
    if (!std::filesystem::exists(managed))
    {
        const std::string initial =
            "# Managed by Rex's Toolkit. Existing Equalizer APO configuration is left intact.\r\n"
            "# Open Rex's Toolkit > Equalizer to configure profiles.\r\n";
        std::ofstream file(managed, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            errorMessage = L"Could not create Rex's managed Equalizer APO include file.";
            return false;
        }
        file.write(initial.data(), static_cast<std::streamsize>(initial.size()));
    }
    return true;
}

std::string ApoFilterType(FilterType type)
{
    switch (type)
    {
    case FilterType::Peaking: return "PK";
    case FilterType::LowShelf: return "LSC";
    case FilterType::HighShelf: return "HSC";
    case FilterType::LowPass: return "LPQ";
    case FilterType::HighPass: return "HPQ";
    case FilterType::Notch: return "NO";
    }
    return "PK";
}

std::string BuildManagedConfiguration(
    const std::map<std::wstring, EqualizerProfile>& profiles,
    const std::map<std::wstring, bool>& enabled)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "# Managed by Rex's Toolkit. Manual changes may be overwritten.\r\n";
    stream << "# Applies to supported shared-mode Windows audio through Equalizer APO.\r\n";
    stream << "Device: all\r\n";
    stream << "Stage: post-mix\r\n\r\n";
    for (const auto& [endpointId, profile] : profiles)
    {
        const std::wstring guid = ExtractEndpointGuid(endpointId);
        if (guid.empty()) continue;
        stream << "Device: " << WideToUtf8(guid) << "\r\n";
        stream << "Stage: post-mix\r\n";
        stream << "Channel: all\r\n";
        const auto enabledIterator = enabled.find(endpointId);
        const bool active = enabledIterator != enabled.end() && enabledIterator->second;
        if (!active)
        {
            stream << "# Equalizer bypassed for this endpoint.\r\n\r\n";
            continue;
        }
        stream << std::fixed << std::setprecision(2) << "Preamp: " << profile.preampDb << " dB\r\n";
        int filterNumber = 1;
        for (const auto& filter : profile.filters)
        {
            if (!filter.enabled) continue;
            stream << "Filter " << filterNumber++ << ": ON " << ApoFilterType(filter.type)
                   << " Fc " << std::setprecision(2) << filter.frequencyHz << " Hz";
            if (filter.type != FilterType::LowPass && filter.type != FilterType::HighPass && filter.type != FilterType::Notch)
            {
                stream << " Gain " << filter.gainDb << " dB";
            }
            stream << " Q " << filter.q << "\r\n";
        }
        stream << "\r\n";
    }
    return stream.str();
}

bool WriteManagedConfiguration(const std::filesystem::path& configPath, const std::string& text, std::wstring& errorMessage)
{
    const std::filesystem::path destination = configPath / kManagedFileName;
    const std::filesystem::path temporary = destination.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            detail::RemoveTemporaryFile(temporary);
            errorMessage = L"Could not write Rex's Equalizer APO profile file.";
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file)
        {
            file.close();
            detail::RemoveTemporaryFile(temporary);
            errorMessage = L"Rex's Equalizer APO profile could not be written completely.";
            return false;
        }
    }

    DWORD replaceError = ERROR_SUCCESS;
    if (!detail::CommitTemporaryFile(temporary, destination, replaceError))
    {
        errorMessage = L"Rex's Equalizer APO profile could not be activated: " + ErrorFromCode(replaceError);
        return false;
    }
    return true;
}

class ScopedCom
{
public:
    ScopedCom()
    {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    ~ScopedCom()
    {
        if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
    }
    bool Available() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT result_ = E_FAIL;
};
}
class EndpointNotificationClient final : public IMMNotificationClient
{
public:
    explicit EndpointNotificationClient(std::atomic<std::uint64_t>& generation)
        : generation_(generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override
    {
        if (!object) return E_POINTER;
        if (interfaceId == __uuidof(IUnknown) || interfaceId == __uuidof(IMMNotificationClient))
        {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --references_;
        if (references == 0) delete this;
        return references;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override
    {
        if (flow == eRender || flow == eAll) Changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        Changed();
        return S_OK;
    }

private:
    void Changed()
    {
        generation_.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<ULONG> references_ { 1 };
    std::atomic<std::uint64_t>& generation_;
};


class EqualizerApoBackend::Impl
{
public:
    bool Discover(std::wstring& errorMessage)
    {
        installPath = RegistryString(HKEY_LOCAL_MACHINE, kEqualizerApoRegistryPath, L"InstallPath");
        configPath = RegistryString(HKEY_LOCAL_MACHINE, kEqualizerApoRegistryPath, L"ConfigPath");
        version = RegistryString(HKEY_LOCAL_MACHINE, kEqualizerApoRegistryPath, L"Version");
        if (installPath.empty())
        {
            errorMessage = L"Equalizer APO is not installed.";
            return false;
        }
        if (configPath.empty()) configPath = std::filesystem::path(installPath) / L"config";
        return true;
    }

    std::wstring installPath;
    std::filesystem::path configPath;
    std::wstring version;
    std::wstring lastError;
    HRESULT comInitialization = E_FAIL;
    IMMDeviceEnumerator* notificationEnumerator = nullptr;
    EndpointNotificationClient* notificationClient = nullptr;
    std::atomic<std::uint64_t> deviceGeneration { 1 };
};

EqualizerApoBackend::EqualizerApoBackend() : impl_(std::make_unique<Impl>()) {}
EqualizerApoBackend::~EqualizerApoBackend() = default;

bool EqualizerApoBackend::Initialize(std::wstring& errorMessage)
{
    const bool result = impl_->Discover(errorMessage);
    impl_->comInitialization = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(impl_->comInitialization) || impl_->comInitialization == RPC_E_CHANGED_MODE)
    {
        if (SUCCEEDED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&impl_->notificationEnumerator))) &&
            impl_->notificationEnumerator)
        {
            impl_->notificationClient = new EndpointNotificationClient(impl_->deviceGeneration);
            if (FAILED(impl_->notificationEnumerator->RegisterEndpointNotificationCallback(
                    impl_->notificationClient)))
            {
                impl_->notificationClient->Release();
                impl_->notificationClient = nullptr;
                impl_->notificationEnumerator->Release();
                impl_->notificationEnumerator = nullptr;
            }
        }
    }
    impl_->lastError = result ? L"" : errorMessage;
    return result;
}

void EqualizerApoBackend::Shutdown()
{
    if (impl_->notificationEnumerator && impl_->notificationClient)
    {
        impl_->notificationEnumerator->UnregisterEndpointNotificationCallback(impl_->notificationClient);
    }
    if (impl_->notificationClient) impl_->notificationClient->Release();
    if (impl_->notificationEnumerator) impl_->notificationEnumerator->Release();
    impl_->notificationClient = nullptr;
    impl_->notificationEnumerator = nullptr;
    if (impl_->comInitialization == S_OK || impl_->comInitialization == S_FALSE) CoUninitialize();
    impl_->comInitialization = E_FAIL;
}

std::vector<AudioOutputDevice> EqualizerApoBackend::EnumerateOutputDevices(std::wstring& errorMessage)
{
    std::vector<AudioOutputDevice> devices;
    ScopedCom com;
    if (!com.Available())
    {
        errorMessage = L"Windows audio device discovery could not initialize COM.";
        return devices;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result) || !enumerator)
    {
        errorMessage = L"Windows audio output devices could not be enumerated.";
        return devices;
    }
    LPWSTR defaultIdRaw = nullptr;
    IMMDevice* defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice)) && defaultDevice)
    {
        defaultDevice->GetId(&defaultIdRaw);
        defaultDevice->Release();
    }
    const std::wstring defaultId = defaultIdRaw ? defaultIdRaw : L"";
    if (defaultIdRaw) CoTaskMemFree(defaultIdRaw);

    IMMDeviceCollection* collection = nullptr;
    result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &collection);
    if (FAILED(result) || !collection)
    {
        enumerator->Release();
        errorMessage = L"Windows did not return any audio output devices.";
        return devices;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index)
    {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(index, &device)) || !device) continue;
        AudioOutputDevice output;
        LPWSTR idRaw = nullptr;
        if (SUCCEEDED(device->GetId(&idRaw)) && idRaw)
        {
            output.id = idRaw;
            CoTaskMemFree(idRaw);
        }
        DWORD state = 0;
        device->GetState(&state);
        output.connected = (state & DEVICE_STATE_ACTIVE) != 0;
        output.isDefault = output.id == defaultId;
        output.endpointGuid = ExtractEndpointGuid(output.id);
        output.apoConfigured = IsApoConfiguredForEndpoint(output.endpointGuid);

        IPropertyStore* properties = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) && properties)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            {
                output.name = value.pwszVal;
            }
            PropVariantClear(&value);
            properties->Release();
        }
        IAudioClient* audioClient = nullptr;
        if (output.connected && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient))) && audioClient)
        {
            WAVEFORMATEX* format = nullptr;
            if (SUCCEEDED(audioClient->GetMixFormat(&format)) && format)
            {
                output.sampleRate = format->nSamplesPerSec;
                output.channelCount = format->nChannels;
                CoTaskMemFree(format);
            }
            audioClient->Release();
        }
        if (output.name.empty()) output.name = L"Windows audio output";
        if (!output.id.empty()) devices.push_back(std::move(output));
        device->Release();
    }
    collection->Release();
    enumerator->Release();
    std::stable_sort(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
        if (left.isDefault != right.isDefault) return left.isDefault;
        if (left.connected != right.connected) return left.connected;
        return Lower(left.name) < Lower(right.name);
    });
    if (devices.empty()) errorMessage = L"No Windows audio output devices are available.";
    return devices;
}

BackendStatus EqualizerApoBackend::GetStatus(const std::optional<AudioOutputDevice>& device) const
{
    BackendStatus status;
    status.backendVersion = impl_->version.empty() ? L"Installed" : impl_->version;
    status.installPath = impl_->installPath;
    status.configPath = impl_->configPath.wstring();
    status.lastError = impl_->lastError;
    if (impl_->installPath.empty())
    {
        status.availability = BackendAvailability::NotInstalled;
        return status;
    }
    if (!device)
    {
        status.availability = BackendAvailability::NoOutput;
        return status;
    }
    status.endpointName = device->name;
    status.endpointId = device->id;
    status.connected = device->connected;
    status.sampleRate = device->sampleRate;
    status.channelCount = device->channelCount;
    status.attached = device->apoConfigured;
    status.managedIncludeInstalled = ManagedIncludePresent(impl_->configPath);
    if (!device->connected)
    {
        status.availability = BackendAvailability::NoOutput;
        return status;
    }
    if (!device->apoConfigured)
    {
        status.availability = BackendAvailability::DeviceNotConfigured;
    }
    else if (!status.managedIncludeInstalled)
    {
        status.availability = BackendAvailability::ToolkitIncludeMissing;
    }
    else
    {
        status.availability = BackendAvailability::Ready;
    }
    return status;
}

bool EqualizerApoBackend::ApplyDeviceProfiles(
    const std::map<std::wstring, EqualizerProfile>& profiles,
    const std::map<std::wstring, bool>& enabled,
    std::wstring& errorMessage)
{
    if (impl_->installPath.empty() && !impl_->Discover(errorMessage))
    {
        impl_->lastError = errorMessage;
        return false;
    }
    if (!ManagedIncludePresent(impl_->configPath))
    {
        errorMessage = L"Finish Equalizer setup before enabling audio processing.";
        impl_->lastError = errorMessage;
        return false;
    }
    if (!WriteManagedConfiguration(impl_->configPath, BuildManagedConfiguration(profiles, enabled), errorMessage))
    {
        impl_->lastError = errorMessage;
        return false;
    }
    impl_->lastError.clear();
    return true;
}

bool EqualizerApoBackend::EnsureManagedInclude(std::wstring& errorMessage)
{
    if (impl_->installPath.empty() && !impl_->Discover(errorMessage)) return false;
    const bool result = EnsureManagedIncludeAtPath(impl_->configPath, errorMessage);
    impl_->lastError = result ? L"" : errorMessage;
    return result;
}

std::filesystem::path EqualizerApoBackend::ConfiguratorPath() const
{
    if (impl_->installPath.empty()) return {};
    const std::filesystem::path root = impl_->installPath;
    const std::filesystem::path deviceSelector = root / L"DeviceSelector.exe";
    if (std::filesystem::exists(deviceSelector)) return deviceSelector;
    return root / L"Configurator.exe";
}

std::uint64_t EqualizerApoBackend::DeviceChangeGeneration() const
{
    return impl_->deviceGeneration.load(std::memory_order_relaxed);
}

bool EqualizerApoBackend::InstallManagedIncludeForDetectedInstallation(std::wstring& errorMessage)
{
    const std::wstring installPath = RegistryString(HKEY_LOCAL_MACHINE, kEqualizerApoRegistryPath, L"InstallPath");
    if (installPath.empty())
    {
        errorMessage = L"Equalizer APO is not installed.";
        return false;
    }
    std::wstring configPath = RegistryString(HKEY_LOCAL_MACHINE, kEqualizerApoRegistryPath, L"ConfigPath");
    if (configPath.empty()) configPath = (std::filesystem::path(installPath) / L"config").wstring();
    return EnsureManagedIncludeAtPath(configPath, errorMessage);
}
}
