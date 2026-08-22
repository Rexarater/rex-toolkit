#include "EqualizerService.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <system_error>

namespace rex::equalizer
{
namespace
{
constexpr wchar_t kFallbackOutputId[] = L"__no_output__";
constexpr wchar_t kSharedOutputProfileId[] = L"__shared_output_profile__";

std::wstring SafeFileName(std::wstring value)
{
    for (wchar_t& character : value)
    {
        if (!(std::iswalnum(character) || character == L'-' || character == L'_')) character = L'_';
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' ' || value.back() == L'_')) value.pop_back();
    if (value.empty()) value = L"custom_headphones";
    if (value.size() > 80) value.resize(80);
    return value;
}

bool IsKnownPreset(const std::wstring& id)
{
    const auto ids = EqualizerService::PresetIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

double NormalizeSampleRate(unsigned int sampleRate)
{
    constexpr unsigned int minimumSampleRate = 8000;
    constexpr unsigned int maximumSampleRate = 768000;
    return sampleRate >= minimumSampleRate && sampleRate <= maximumSampleRate
        ? static_cast<double>(sampleRate) : 48000.0;
}

void ClearCustomProfileMetadata(DeviceEqualizerSettings& settings)
{
    settings.customProfileName.clear();
    settings.customProfileSource.clear();
    settings.customProfileSourceUrl.clear();
    settings.customProfileVersion.clear();
    settings.customTargetCurveId.clear();
}

void CaptureCustomProfileMetadata(
    DeviceEqualizerSettings& settings,
    const EqualizerProfile& profile)
{
    settings.customProfileName = profile.name;
    settings.customProfileSource = profile.source;
    settings.customProfileSourceUrl = profile.sourceUrl;
    settings.customProfileVersion = profile.profileVersion;
    settings.customTargetCurveId = profile.targetCurveId;
}
}

EqualizerService::EqualizerService()
    : dataDirectory_(DefaultDataDirectory()),
      backend_(std::make_unique<EqualizerApoBackend>())
{
}

EqualizerService::~EqualizerService()
{
    Shutdown();
}

bool EqualizerService::Initialize(std::wstring& errorMessage)
{
    if (initialized_) return true;

    std::error_code fileError;
    std::filesystem::create_directories(dataDirectory_, fileError);
    if (fileError)
    {
        errorMessage = L"Rex's Toolkit could not create the Equalizer data folder.";
        return false;
    }

    repository_ = std::make_unique<EqualizerSettingsRepository>(dataDirectory_ / L"settings.json");
    if (!repository_->Load(settings_, errorMessage)) return false;
    if (settings_.selectedOutputId.empty()) settings_.selectedOutputId = kFollowDefaultOutputId;

    if (!headphoneProfiles_.Initialize(dataDirectory_, errorMessage)) return false;

    std::wstring backendError;
    backend_->Initialize(backendError);
    std::wstring deviceError;
    RefreshDevices(deviceError);
    EnsureDeviceSettings(EffectiveOutputId());
    std::wstring profileError;
    RebuildCurrentProfile(&profileError);
    initialized_ = true;

    lastError_ = !backendError.empty()
        ? backendError
        : !deviceError.empty() ? deviceError : profileError;
    errorMessage.clear();
    return true;
}

void EqualizerService::Shutdown()
{
    if (!initialized_) return;
    std::wstring ignored;
    Save(ignored);
    if (backend_) backend_->Shutdown();
    initialized_ = false;
}

bool EqualizerService::ReinitializeBackend(std::wstring& errorMessage)
{
    if (!backend_)
    {
        errorMessage = L"The system equalizer backend is unavailable.";
        lastError_ = errorMessage;
        return false;
    }

    backend_->Shutdown();
    devices_.clear();
    std::wstring backendError;
    const bool backendAvailable = backend_->Initialize(backendError);
    std::wstring deviceError;
    RefreshDevices(deviceError);
    EnsureDeviceSettings(EffectiveOutputId());
    std::wstring profileError;
    RebuildCurrentProfile(&profileError);

    errorMessage = !backendError.empty()
        ? backendError
        : !deviceError.empty() ? deviceError : profileError;
    lastError_ = errorMessage;
    return backendAvailable;
}

bool EqualizerService::RefreshDevices(std::wstring& errorMessage)
{
    const std::wstring previousId = EffectiveOutputId();
    devices_ = backend_->EnumerateOutputDevices(errorMessage);
    const std::wstring currentId = EffectiveOutputId();
    EnsureDeviceSettings(currentId);
    if (previousId != currentId) RebuildCurrentProfile();
    return !devices_.empty();
}

ApplyResult EqualizerService::Apply(bool persistSettings)
{
    ApplyResult result;
    std::wstring currentProfileError;
    const bool currentProfileReady = RebuildCurrentProfile(&currentProfileError);
    result.profile = currentProfile_;

    if (persistSettings)
    {
        std::wstring saveError;
        result.settingsSaved = Save(saveError);
        if (!result.settingsSaved)
        {
            result.message = saveError;
            result.backendStatus = CurrentBackendStatus();
            return result;
        }
    }

    std::map<std::wstring, EqualizerProfile> profiles;
    std::map<std::wstring, bool> enabled;
    std::wstring profileError;
    const auto currentOutput = CurrentOutputDevice();
    const std::wstring currentOutputId = currentOutput ? currentOutput->id : std::wstring {};
    const auto composeEnabledProfile = [&](const DeviceEqualizerSettings& deviceSettings,
                                           double sampleRate,
                                           EqualizerProfile& profile,
                                           bool isCurrentOutput) {
        profile = {};
        if (!deviceSettings.enabled) return true;
        if (isCurrentOutput)
        {
            if (currentProfileReady)
            {
                profile = currentProfile_;
                return true;
            }
            if (profileError.empty())
            {
                profileError = currentProfileError.empty()
                    ? L"Rex's Toolkit could not build the selected headphone correction."
                    : currentProfileError;
            }
            return false;
        }

        std::wstring composeError;
        if (TryComposeProfile(deviceSettings, sampleRate, profile, composeError)) return true;
        if (profileError.empty()) profileError = std::move(composeError);
        return false;
    };
    if (!settings_.rememberPerDevice)
    {
        const auto found = settings_.deviceProfiles.find(kSharedOutputProfileId);
        const DeviceEqualizerSettings& shared = found == settings_.deviceProfiles.end()
            ? fallbackDeviceSettings_
            : found->second;
        for (const auto& device : devices_)
        {
            if (!device.id.empty())
            {
                EqualizerProfile profile;
                const bool profileReady = composeEnabledProfile(
                    shared,
                    NormalizeSampleRate(device.sampleRate),
                    profile,
                    device.id == currentOutputId);
                profiles[device.id] = std::move(profile);
                enabled[device.id] = shared.enabled && profileReady;
            }
        }
    }
    else
    {
        for (const auto& [deviceId, settings] : settings_.deviceProfiles)
        {
            if (deviceId == kFallbackOutputId ||
                deviceId == kFollowDefaultOutputId ||
                deviceId == kSharedOutputProfileId)
            {
                continue;
            }
            EqualizerProfile profile;
            const bool profileReady = composeEnabledProfile(
                settings,
                SampleRateForOutput(deviceId),
                profile,
                deviceId == currentOutputId);
            profiles[deviceId] = std::move(profile);
            enabled[deviceId] = settings.enabled && profileReady;
        }
    }

    std::wstring backendError;
    const bool wroteConfiguration = backend_->ApplyDeviceProfiles(profiles, enabled, backendError);
    result.backendStatus = CurrentBackendStatus();
    result.backendStatus.activeFilterCount = static_cast<int>(currentProfile_.filters.size());
    result.backendStatus.preampDb = currentProfile_.preampDb;

    if (!wroteConfiguration)
    {
        result.message = backendError.empty() ? BackendAvailabilityText(result.backendStatus.availability) : backendError;
        lastError_ = result.message;
        return result;
    }

    if (!profileError.empty())
    {
        result.message = profileError +
            L" Rex's Toolkit safely bypassed audio processing for the affected output instead of applying partial tuning.";
        lastError_ = result.message;
        return result;
    }

    if (CurrentDeviceSettings().enabled && result.backendStatus.availability != BackendAvailability::Ready)
    {
        result.message = BackendAvailabilityText(result.backendStatus.availability);
        lastError_ = result.message;
        return result;
    }

    result.success = true;
    result.message = CurrentDeviceSettings().enabled
        ? L"Equalizer active for the selected Windows output."
        : L"Equalizer bypassed for the selected Windows output.";
    lastError_.clear();
    return result;
}

bool EqualizerService::InstallManagedInclude(std::wstring& errorMessage)
{
    if (!backend_->EnsureManagedInclude(errorMessage))
    {
        lastError_ = errorMessage;
        return false;
    }
    std::wstring ignored;
    RefreshDevices(ignored);
    lastError_.clear();
    return true;
}

const EqualizerSettings& EqualizerService::Settings() const { return settings_; }
const std::vector<AudioOutputDevice>& EqualizerService::OutputDevices() const { return devices_; }

std::optional<AudioOutputDevice> EqualizerService::CurrentOutputDevice() const
{
    if (devices_.empty()) return std::nullopt;
    if (settings_.followWindowsDefault || settings_.selectedOutputId == kFollowDefaultOutputId)
    {
        const auto found = std::find_if(devices_.begin(), devices_.end(), [](const auto& device) { return device.isDefault; });
        return found == devices_.end() ? std::optional<AudioOutputDevice>(devices_.front()) : std::optional<AudioOutputDevice>(*found);
    }
    const auto found = std::find_if(devices_.begin(), devices_.end(), [&](const auto& device) {
        return device.id == settings_.selectedOutputId;
    });
    return found == devices_.end() ? std::nullopt : std::optional<AudioOutputDevice>(*found);
}

DeviceEqualizerSettings& EqualizerService::CurrentDeviceSettings()
{
    const std::wstring id = EffectiveOutputId();
    if (id == kFallbackOutputId) return fallbackDeviceSettings_;
    EnsureDeviceSettings(id);
    return settings_.deviceProfiles[id];
}

const DeviceEqualizerSettings& EqualizerService::CurrentDeviceSettings() const
{
    const std::wstring id = EffectiveOutputId();
    const auto found = settings_.deviceProfiles.find(id);
    return found == settings_.deviceProfiles.end() ? fallbackDeviceSettings_ : found->second;
}

const EqualizerProfile& EqualizerService::CurrentProfile() const { return currentProfile_; }

BackendStatus EqualizerService::CurrentBackendStatus() const
{
    BackendStatus status = backend_->GetStatus(CurrentOutputDevice());
    status.activeFilterCount = static_cast<int>(currentProfile_.filters.size());
    status.preampDb = currentProfile_.preampDb;
    if (!lastError_.empty() && status.lastError.empty()) status.lastError = lastError_;
    return status;
}

std::uint64_t EqualizerService::DeviceChangeGeneration() const
{
    return backend_ ? backend_->DeviceChangeGeneration() : 0;
}

std::filesystem::path EqualizerService::BackendConfiguratorPath() const { return backend_->ConfiguratorPath(); }
std::filesystem::path EqualizerService::DataDirectory() const { return dataDirectory_; }

void EqualizerService::SelectOutput(const std::wstring& id)
{
    settings_.followWindowsDefault = id.empty() || id == kFollowDefaultOutputId;
    settings_.selectedOutputId = settings_.followWindowsDefault ? kFollowDefaultOutputId : id;
    EnsureDeviceSettings(EffectiveOutputId());
    RebuildCurrentProfile();
}

void EqualizerService::SetEnabled(bool enabled)
{
    CurrentDeviceSettings().enabled = enabled;
    RebuildCurrentProfile();
}


void EqualizerService::SetSoundPreset(const std::wstring& presetId)
{
    CurrentDeviceSettings().soundPreset = IsKnownPreset(presetId) ? presetId : L"balanced";
    RebuildCurrentProfile();
}

void EqualizerService::SetPreferenceValue(const std::wstring& controlId, double valueDb)
{
    const double value = std::clamp(std::isfinite(valueDb) ? valueDb : 0.0, -6.0, 6.0);
    auto& settings = CurrentDeviceSettings();
    if (controlId == L"bass") settings.bassDb = value;
    else if (controlId == L"warmth") settings.warmthDb = value;
    else if (controlId == L"presence") settings.presenceDb = value;
    else if (controlId == L"treble") settings.trebleDb = value;
    RebuildCurrentProfile();
}

void EqualizerService::SetAdvancedVisible(bool visible) { settings_.advancedVisible = visible; }

void EqualizerService::SetEditorMode(EditorMode mode)
{
    CurrentDeviceSettings().editorMode = mode;
    RebuildCurrentProfile();
}

void EqualizerService::SetPreventClipping(bool enabled)
{
    CurrentDeviceSettings().preventClipping = enabled;
    RebuildCurrentProfile();
}

void EqualizerService::SetAutomaticPreamp(bool enabled)
{
    CurrentDeviceSettings().automaticPreamp = enabled;
    RebuildCurrentProfile();
}

void EqualizerService::SetManualPreamp(double valueDb)
{
    CurrentDeviceSettings().manualPreampDb = std::clamp(std::isfinite(valueDb) ? valueDb : 0.0, -30.0, 6.0);
    RebuildCurrentProfile();
}

void EqualizerService::ConvertCurrentProfileToCustom()
{
    auto& settings = CurrentDeviceSettings();
    settings.customFilters = currentProfile_.filters;
    CaptureCustomProfileMetadata(settings, currentProfile_);
    settings.parametricOverrideActive = true;
    settings.soundPreset = L"balanced";
    settings.bassDb = settings.warmthDb = settings.presenceDb = settings.trebleDb = 0.0;
    settings.graphicGains.fill(0.0);
    settings.editorMode = EditorMode::Parametric;
    RebuildCurrentProfile();
}

void EqualizerService::SetRememberPerDevice(bool enabled)
{
    if (settings_.rememberPerDevice == enabled) return;
    const DeviceEqualizerSettings previous = CurrentDeviceSettings();
    settings_.rememberPerDevice = enabled;
    EnsureDeviceSettings(EffectiveOutputId());
    CurrentDeviceSettings() = previous;
    RebuildCurrentProfile();
}

void EqualizerService::SetAutomaticallyApplyDeviceProfile(bool enabled)
{
    settings_.automaticallyApplyDeviceProfile = enabled;
}

void EqualizerService::SetEnableOnStartup(bool enabled)
{
    settings_.enableOnStartup = enabled;
}

void EqualizerService::SetTrayControlsEnabled(bool enabled)
{
    settings_.trayControlsEnabled = enabled;
}

void EqualizerService::SetShowTechnicalControls(bool enabled)
{
    settings_.showTechnicalControls = enabled;
}
void EqualizerService::SetGraphicBand(size_t index, double gainDb)
{
    if (index >= CurrentDeviceSettings().graphicGains.size()) return;
    CurrentDeviceSettings().graphicGains[index] = std::clamp(std::isfinite(gainDb) ? gainDb : 0.0, -12.0, 12.0);
    RebuildCurrentProfile();
}

void EqualizerService::SetCustomFilter(size_t index, const EqualizerFilter& filter)
{
    auto& settings = CurrentDeviceSettings();
    auto& filters = settings.customFilters;
    if (index >= filters.size()) return;
    std::wstring ignored;
    if (!AutoEqService::ValidateFilter(filter, ignored)) return;
    settings.parametricOverrideActive = true;
    filters[index] = filter;
    RebuildCurrentProfile();
}

void EqualizerService::AddCustomFilter()
{
    auto& settings = CurrentDeviceSettings();
    settings.parametricOverrideActive = true;
    auto& filters = settings.customFilters;
    if (filters.size() >= static_cast<size_t>(settings_.maximumPeqFilters)) return;
    EqualizerFilter filter;
    filter.id = static_cast<int>(filters.size()) + 1;
    filters.push_back(filter);
    RebuildCurrentProfile();
}

void EqualizerService::RemoveCustomFilter(size_t index)
{
    auto& settings = CurrentDeviceSettings();
    settings.parametricOverrideActive = true;
    auto& filters = settings.customFilters;
    if (index >= filters.size()) return;
    filters.erase(filters.begin() + static_cast<std::ptrdiff_t>(index));
    for (size_t item = 0; item < filters.size(); ++item) filters[item].id = static_cast<int>(item) + 1;
    RebuildCurrentProfile();
}

void EqualizerService::ResetCustomEq()
{
    auto& settings = CurrentDeviceSettings();
    settings.graphicGains.fill(0.0);
    settings.customFilters.clear();
    ClearCustomProfileMetadata(settings);
    settings.parametricOverrideActive = false;
    settings.bassDb = settings.warmthDb = settings.presenceDb = settings.trebleDb = 0.0;
    settings.soundPreset = L"balanced";
    settings.editorMode = EditorMode::Simple;
    settings.preventClipping = false;
    settings.automaticPreamp = true;
    RebuildCurrentProfile();
}

void EqualizerService::ResetDeviceProfile()
{
    const bool wasEnabled = CurrentDeviceSettings().enabled;
    DeviceEqualizerSettings reset;
    reset.enabled = wasEnabled;
    CurrentDeviceSettings() = std::move(reset);
    RebuildCurrentProfile();
}

std::vector<HeadphoneProfileSummary> EqualizerService::SearchHeadphones(const std::wstring& query, size_t limit) const
{
    return headphoneProfiles_.Search(query, limit);
}

bool EqualizerService::UpdateHeadphoneIndex(std::wstring& errorMessage)
{
    if (!headphoneProfiles_.UpdateAutoEqIndex(errorMessage))
    {
        lastError_ = errorMessage;
        return false;
    }
    lastError_.clear();
    return true;
}

bool EqualizerService::ReloadHeadphoneIndex(std::wstring& errorMessage)
{
    return headphoneProfiles_.Initialize(dataDirectory_, errorMessage);
}

bool EqualizerService::SelectHeadphone(const std::wstring& profileId, std::wstring& errorMessage)
{
    auto& settings = CurrentDeviceSettings();
    if (profileId.empty())
    {
        settings.headphoneProfileId.clear();
        settings.headphoneDisplayName = L"No headphone correction";
        settings.customFilters.clear();
        ClearCustomProfileMetadata(settings);
        settings.parametricOverrideActive = false;
        RebuildCurrentProfile();
        return true;
    }

    EqualizerProfile profile;
    if (!headphoneProfiles_.ResolveCachedProfile(profileId, profile, errorMessage))
    {
        lastError_ = errorMessage;
        return false;
    }
    const auto& summaries = headphoneProfiles_.Profiles();
    const auto found = std::find_if(summaries.begin(), summaries.end(), [&](const auto& summary) { return summary.id == profileId; });
    settings.headphoneProfileId = profileId;
    settings.headphoneDisplayName = found == summaries.end() ? profile.name : found->DisplayName();
    settings.customFilters.clear();
    ClearCustomProfileMetadata(settings);
    settings.parametricOverrideActive = false;
    resolvedHeadphoneProfiles_[profileId] = std::move(profile);
    RebuildCurrentProfile();
    lastError_.clear();
    return true;
}
void EqualizerService::SelectResolvedHeadphone(
    const std::wstring& profileId,
    const std::wstring& displayName,
    EqualizerProfile profile)
{
    if (profileId.empty() || profile.filters.empty()) return;
    auto& settings = CurrentDeviceSettings();
    settings.headphoneProfileId = profileId;
    settings.headphoneDisplayName = displayName.empty() ? profile.name : displayName;
    settings.customFilters.clear();
    ClearCustomProfileMetadata(settings);
    settings.parametricOverrideActive = false;
    resolvedHeadphoneProfiles_[profileId] = std::move(profile);
    RebuildCurrentProfile();
    lastError_.clear();
}


bool EqualizerService::ImportProfile(const std::filesystem::path& path, std::wstring& errorMessage)
{
    EqualizerProfile profile;
    if (!headphoneProfiles_.ImportProfile(path, profile, errorMessage)) return false;
    auto& settings = CurrentDeviceSettings();
    settings.headphoneProfileId.clear();
    settings.headphoneDisplayName = L"Custom EQ profile";
    settings.soundPreset = L"balanced";
    settings.bassDb = settings.warmthDb = settings.presenceDb = settings.trebleDb = 0.0;
    settings.editorMode = EditorMode::Parametric;
    settings.customFilters = profile.filters;
    CaptureCustomProfileMetadata(settings, profile);
    settings.parametricOverrideActive = true;
    settings.automaticPreamp = profile.automaticPreamp;
    settings.manualPreampDb = profile.preampDb;
    RebuildCurrentProfile();
    return true;
}

bool EqualizerService::ExportCurrentProfile(const std::filesystem::path& path, std::wstring& errorMessage) const
{
    return headphoneProfiles_.ExportProfile(path, currentProfile_, errorMessage);
}

bool EqualizerService::ImportMeasurement(
    const std::filesystem::path& path,
    const std::wstring& profileName,
    std::wstring& errorMessage)
{
    std::vector<MeasurementPoint> points;
    if (!AutoEqService::ParseMeasurementFile(path, points, errorMessage)) return false;
    EqualizerProfile profile = autoEq_.OptimizeMeasurement(profileName, points, errorMessage);
    if (profile.filters.empty()) return false;

    const std::wstring id = L"custom:" + SafeFileName(profileName);
    profile.headphoneProfileId = id;
    const std::filesystem::path destination = dataDirectory_ / L"custom" / (SafeFileName(profileName) + L".rexeq");
    if (!headphoneProfiles_.ExportProfile(destination, profile, errorMessage)) return false;

    if (!headphoneProfiles_.Initialize(dataDirectory_, errorMessage))
    {
        const std::wstring detail = errorMessage;
        errorMessage = L"The measurement profile was saved, but the headphone directory could not be refreshed.";
        if (!detail.empty())
        {
            errorMessage += L" " + detail;
        }
        return false;
    }

    resolvedHeadphoneProfiles_[id] = profile;
    auto& settings = CurrentDeviceSettings();
    settings.headphoneProfileId = id;
    settings.headphoneDisplayName = profileName.empty() ? L"Custom headphones" : profileName;
    settings.customFilters.clear();
    ClearCustomProfileMetadata(settings);
    settings.parametricOverrideActive = false;
    RebuildCurrentProfile();
    return true;
}

std::wstring EqualizerService::HeadphoneDatabaseVersion() const { return headphoneProfiles_.DatabaseVersion(); }

bool EqualizerService::Save(std::wstring& errorMessage) const
{
    return repository_ && repository_->Save(settings_, errorMessage);
}

std::filesystem::path EqualizerService::DefaultDataDirectory()
{
    wchar_t appData[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
    if (length == 0 || length >= std::size(appData)) return std::filesystem::path(L".") / L"equalizer";
    return std::filesystem::path(appData) / L"RexToolkit" / L"equalizer";
}

std::wstring EqualizerService::PresetDisplayName(const std::wstring& presetId)
{
    if (presetId == L"bass_plus") return L"Bass+";
    if (presetId == L"bass_reduce") return L"Bass Reduce";
    if (presetId == L"warm") return L"Warm";
    if (presetId == L"bright") return L"Bright";
    if (presetId == L"music") return L"Music";
    if (presetId == L"movies") return L"Movies";
    if (presetId == L"voice") return L"Voice";
    if (presetId == L"gaming") return L"Gaming";
    if (presetId == L"footsteps") return L"Footsteps";
    if (presetId == L"late_night") return L"Late Night";
    if (presetId == L"custom") return L"Custom";
    return L"Balanced";
}

std::vector<std::wstring> EqualizerService::PresetIds()
{
    return { L"balanced", L"bass_plus", L"bass_reduce", L"warm", L"bright", L"music",
        L"movies", L"voice", L"gaming", L"footsteps", L"late_night", L"custom" };
}

std::wstring EqualizerService::BackendAvailabilityText(BackendAvailability availability)
{
    switch (availability)
    {
    case BackendAvailability::NotInstalled:
        return L"The bundled system audio engine is ready to install.";
    case BackendAvailability::NoOutput:
        return L"No compatible Windows output is currently available.";
    case BackendAvailability::DeviceNotConfigured:
        return L"This output is not configured in Equalizer APO yet.";
    case BackendAvailability::ToolkitIncludeMissing:
        return L"Rex's managed Equalizer APO configuration has not been enabled yet.";
    case BackendAvailability::Ready:
        return L"Ready for supported shared-mode Windows audio.";
    case BackendAvailability::Error:
        return L"The system equalizer backend reported an error.";
    }
    return L"Equalizer status is unavailable.";
}

std::wstring EqualizerService::EffectiveOutputId() const
{
    if (!settings_.rememberPerDevice) return kSharedOutputProfileId;
    const auto device = CurrentOutputDevice();
    return device ? device->id : kFallbackOutputId;
}

double EqualizerService::SampleRateForOutput(const std::wstring& deviceId) const
{
    const auto found = std::find_if(
        devices_.begin(), devices_.end(),
        [&](const AudioOutputDevice& device) { return device.id == deviceId; });
    return found == devices_.end()
        ? 48000.0
        : NormalizeSampleRate(found->sampleRate);
}

double EqualizerService::CurrentSampleRate() const
{
    const auto device = CurrentOutputDevice();
    return device
        ? NormalizeSampleRate(device->sampleRate)
        : 48000.0;
}

std::optional<EqualizerProfile> EqualizerService::SelectedHeadphoneCorrection(
    const DeviceEqualizerSettings& deviceSettings,
    std::wstring& errorMessage)
{
    errorMessage.clear();
    if (deviceSettings.headphoneProfileId.empty()) return std::nullopt;
    const auto cached = resolvedHeadphoneProfiles_.find(deviceSettings.headphoneProfileId);
    if (cached != resolvedHeadphoneProfiles_.end()) return cached->second;

    EqualizerProfile profile;
    std::wstring resolutionError;
    if (!headphoneProfiles_.ResolveCachedProfile(deviceSettings.headphoneProfileId, profile, resolutionError))
    {
        const std::wstring displayName = deviceSettings.headphoneDisplayName.empty()
            ? L"the selected headphones"
            : L"\"" + deviceSettings.headphoneDisplayName + L"\"";
        errorMessage = L"Rex's Toolkit could not load the correction profile for " + displayName + L".";
        if (!resolutionError.empty()) errorMessage += L" " + resolutionError;
        return std::nullopt;
    }
    resolvedHeadphoneProfiles_[deviceSettings.headphoneProfileId] = profile;
    return profile;
}

bool EqualizerService::TryComposeProfile(
    const DeviceEqualizerSettings& deviceSettings,
    double sampleRate,
    EqualizerProfile& profile,
    std::wstring& errorMessage)
{
    std::optional<EqualizerProfile> correction;
    if (!deviceSettings.headphoneProfileId.empty())
    {
        correction = SelectedHeadphoneCorrection(deviceSettings, errorMessage);
        if (!correction)
        {
            profile = {};
            return false;
        }
    }

    profile = autoEq_.Compose(correction, deviceSettings, sampleRate);
    errorMessage.clear();
    return true;
}

bool EqualizerService::RebuildCurrentProfile(std::wstring* errorMessage)
{
    EqualizerProfile profile;
    std::wstring rebuildError;
    if (!TryComposeProfile(CurrentDeviceSettings(), CurrentSampleRate(), profile, rebuildError))
    {
        currentProfile_ = {};
        lastError_ = rebuildError;
        if (errorMessage) *errorMessage = rebuildError;
        return false;
    }

    currentProfile_ = std::move(profile);
    if (errorMessage) errorMessage->clear();
    return true;
}

void EqualizerService::EnsureDeviceSettings(const std::wstring& deviceId)
{
    if (deviceId.empty() || deviceId == kFallbackOutputId) return;
    if (settings_.deviceProfiles.find(deviceId) != settings_.deviceProfiles.end()) return;
    DeviceEqualizerSettings defaults;
    defaults.soundPreset = IsKnownPreset(settings_.defaultSoundPreset) ? settings_.defaultSoundPreset : L"balanced";
    settings_.deviceProfiles.emplace(deviceId, std::move(defaults));
}
}
