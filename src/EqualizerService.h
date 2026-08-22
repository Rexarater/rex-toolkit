#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rex::equalizer
{
inline constexpr wchar_t kFollowDefaultOutputId[] = L"__windows_default__";

enum class FilterType
{
    Peaking,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    Notch
};

enum class EditorMode
{
    Simple,
    Graphic,
    Parametric
};

struct EqualizerFilter
{
    int id = 0;
    bool enabled = true;
    FilterType type = FilterType::Peaking;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
};

struct EqualizerProfile
{
    int schemaVersion = 1;
    std::wstring name = L"Balanced";
    std::wstring headphoneProfileId;
    std::wstring targetCurveId = L"balanced";
    std::wstring preferenceProfileId = L"balanced";
    std::wstring source;
    std::wstring sourceUrl;
    std::wstring profileVersion;
    double preampDb = 0.0;
    bool automaticPreamp = true;
    std::vector<EqualizerFilter> filters;
};

struct AudioOutputDevice
{
    std::wstring id;
    std::wstring endpointGuid;
    std::wstring name;
    bool isDefault = false;
    bool connected = false;
    bool apoConfigured = false;
    unsigned int sampleRate = 0;
    unsigned int channelCount = 0;
};

enum class BackendAvailability
{
    NotInstalled,
    NoOutput,
    DeviceNotConfigured,
    ToolkitIncludeMissing,
    Ready,
    Error
};

struct BackendStatus
{
    BackendAvailability availability = BackendAvailability::NotInstalled;
    std::wstring backendName = L"Equalizer APO";
    std::wstring backendVersion;
    std::wstring installPath;
    std::wstring configPath;
    std::wstring endpointName;
    std::wstring endpointId;
    bool connected = false;
    bool attached = false;
    bool managedIncludeInstalled = false;
    unsigned int sampleRate = 0;
    unsigned int channelCount = 0;
    int activeFilterCount = 0;
    double preampDb = 0.0;
    std::wstring latency = L"Managed by Equalizer APO";
    std::wstring lastError;
};

struct DeviceEqualizerSettings
{
    bool enabled = false;
    std::wstring headphoneProfileId;
    std::wstring headphoneDisplayName = L"No headphone correction";
    std::wstring soundPreset = L"balanced";
    double bassDb = 0.0;
    double warmthDb = 0.0;
    double presenceDb = 0.0;
    double trebleDb = 0.0;
    bool preventClipping = false;
    bool automaticPreamp = true;
    double manualPreampDb = 0.0;
    EditorMode editorMode = EditorMode::Simple;
    std::array<double, 10> graphicGains {};
    bool parametricOverrideActive = false;
    std::wstring customProfileName;
    std::wstring customProfileSource;
    std::wstring customProfileSourceUrl;
    std::wstring customProfileVersion;
    std::wstring customTargetCurveId;
    std::vector<EqualizerFilter> customFilters;
};

struct EqualizerSettings
{
    int schemaVersion = 1;
    bool followWindowsDefault = true;
    std::wstring selectedOutputId = kFollowDefaultOutputId;
    bool rememberPerDevice = true;
    bool automaticallyApplyDeviceProfile = true;
    bool enableOnStartup = false;
    bool trayControlsEnabled = true;
    bool globalHotkeysEnabled = false;
    bool showTechnicalControls = false;
    bool advancedVisible = false;
    int maximumPeqFilters = 10;
    std::wstring defaultSoundPreset = L"balanced";
    std::map<std::wstring, DeviceEqualizerSettings> deviceProfiles;
};

struct HeadphoneProfileSummary
{
    std::wstring id;
    std::wstring manufacturer;
    std::wstring model;
    std::wstring variant;
    std::wstring measurementSource;
    std::wstring sourceUrl;
    bool cached = false;
    bool recommendedEqAvailable = false;

    std::wstring DisplayName() const;
};

struct MeasurementPoint
{
    double frequencyHz = 0.0;
    double rawDb = 0.0;
};

struct ApplyResult
{
    bool success = false;
    bool settingsSaved = false;
    EqualizerProfile profile;
    BackendStatus backendStatus;
    std::wstring message;
};

class IEqualizerBackend
{
public:
    virtual ~IEqualizerBackend() = default;

    virtual bool Initialize(std::wstring& errorMessage) = 0;
    virtual void Shutdown() = 0;
    virtual std::vector<AudioOutputDevice> EnumerateOutputDevices(std::wstring& errorMessage) = 0;
    virtual BackendStatus GetStatus(const std::optional<AudioOutputDevice>& device) const = 0;
    virtual bool ApplyDeviceProfiles(
        const std::map<std::wstring, EqualizerProfile>& profiles,
        const std::map<std::wstring, bool>& enabled,
        std::wstring& errorMessage) = 0;
    virtual bool EnsureManagedInclude(std::wstring& errorMessage) = 0;
    virtual std::filesystem::path ConfiguratorPath() const = 0;
    virtual std::uint64_t DeviceChangeGeneration() const = 0;
};

class EqualizerApoBackend final : public IEqualizerBackend
{
public:
    EqualizerApoBackend();
    ~EqualizerApoBackend() override;

    bool Initialize(std::wstring& errorMessage) override;
    void Shutdown() override;
    std::vector<AudioOutputDevice> EnumerateOutputDevices(std::wstring& errorMessage) override;
    BackendStatus GetStatus(const std::optional<AudioOutputDevice>& device) const override;
    bool ApplyDeviceProfiles(
        const std::map<std::wstring, EqualizerProfile>& profiles,
        const std::map<std::wstring, bool>& enabled,
        std::wstring& errorMessage) override;
    bool EnsureManagedInclude(std::wstring& errorMessage) override;
    std::filesystem::path ConfiguratorPath() const override;
    std::uint64_t DeviceChangeGeneration() const override;

    static bool InstallManagedIncludeForDetectedInstallation(std::wstring& errorMessage);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class AutoEqService
{
public:
    EqualizerProfile Compose(
        const std::optional<EqualizerProfile>& headphoneCorrection,
        const DeviceEqualizerSettings& settings,
        double sampleRate = 48000.0) const;
    EqualizerProfile OptimizeMeasurement(
        const std::wstring& name,
        const std::vector<MeasurementPoint>& measurement,
        std::wstring& errorMessage) const;

    static double FilterResponseDb(const EqualizerFilter& filter, double frequencyHz, double sampleRate = 48000.0);
    static double ProfileResponseDb(const EqualizerProfile& profile, double frequencyHz, double sampleRate = 48000.0);
    static double CalculateAutomaticPreamp(const std::vector<EqualizerFilter>& filters, double sampleRate = 48000.0);
    static bool ValidateFilter(const EqualizerFilter& filter, std::wstring& errorMessage);
    static bool ParseMeasurementFile(
        const std::filesystem::path& path,
        std::vector<MeasurementPoint>& points,
        std::wstring& errorMessage);
};

class HeadphoneProfileService
{
public:
    bool Initialize(const std::filesystem::path& root, std::wstring& errorMessage);
    const std::vector<HeadphoneProfileSummary>& Profiles() const;
    std::vector<HeadphoneProfileSummary> Search(const std::wstring& query, size_t limit = 12) const;
    bool UpdateAutoEqIndex(std::wstring& errorMessage);
    bool ResolveCachedProfile(const std::wstring& id, EqualizerProfile& profile, std::wstring& errorMessage) const;
    bool ResolveProfile(const std::wstring& id, EqualizerProfile& profile, std::wstring& errorMessage);
    static bool ParseAutoEqResult(
        const std::string& document,
        const HeadphoneProfileSummary& summary,
        const std::wstring& databaseVersion,
        EqualizerProfile& profile,
        std::wstring& errorMessage);
    bool ImportProfile(const std::filesystem::path& path, EqualizerProfile& profile, std::wstring& errorMessage) const;
    bool ExportProfile(const std::filesystem::path& path, const EqualizerProfile& profile, std::wstring& errorMessage) const;
    std::wstring DatabaseVersion() const;
    std::filesystem::path ProfileDirectory() const;

private:
    bool LoadIndex(std::wstring& errorMessage);
    bool LoadCachedProfile(const HeadphoneProfileSummary& summary, EqualizerProfile& profile, std::wstring& errorMessage) const;
    bool SaveCachedProfile(const HeadphoneProfileSummary& summary, const EqualizerProfile& profile, std::wstring& errorMessage) const;

    std::filesystem::path root_;
    std::filesystem::path profilesDirectory_;
    std::filesystem::path cacheDirectory_;
    std::filesystem::path indexPath_;
    std::vector<HeadphoneProfileSummary> profiles_;
    std::wstring databaseVersion_ = L"Not downloaded";
};

class EqualizerSettingsRepository
{
public:
    explicit EqualizerSettingsRepository(std::filesystem::path path);
    bool Load(EqualizerSettings& settings, std::wstring& errorMessage) const;
    bool Save(const EqualizerSettings& settings, std::wstring& errorMessage) const;

private:
    std::filesystem::path path_;
};

class EqualizerService
{
public:
    EqualizerService();
    ~EqualizerService();

    EqualizerService(const EqualizerService&) = delete;
    EqualizerService& operator=(const EqualizerService&) = delete;

    bool Initialize(std::wstring& errorMessage);
    void Shutdown();
    bool ReinitializeBackend(std::wstring& errorMessage);
    bool RefreshDevices(std::wstring& errorMessage);
    ApplyResult Apply(bool persistSettings = true);
    bool InstallManagedInclude(std::wstring& errorMessage);

    const EqualizerSettings& Settings() const;
    const std::vector<AudioOutputDevice>& OutputDevices() const;
    std::optional<AudioOutputDevice> CurrentOutputDevice() const;
    DeviceEqualizerSettings& CurrentDeviceSettings();
    const DeviceEqualizerSettings& CurrentDeviceSettings() const;
    const EqualizerProfile& CurrentProfile() const;
    BackendStatus CurrentBackendStatus() const;
    std::filesystem::path BackendConfiguratorPath() const;
    std::filesystem::path DataDirectory() const;
    std::uint64_t DeviceChangeGeneration() const;

    void SelectOutput(const std::wstring& id);
    void SetEnabled(bool enabled);

    void SetSoundPreset(const std::wstring& presetId);
    void SetPreferenceValue(const std::wstring& controlId, double valueDb);
    void SetAdvancedVisible(bool visible);
    void SetEditorMode(EditorMode mode);
    void SetPreventClipping(bool enabled);
    void SetAutomaticPreamp(bool enabled);
    void SetManualPreamp(double valueDb);
    void ConvertCurrentProfileToCustom();
    void SetRememberPerDevice(bool enabled);
    void SetAutomaticallyApplyDeviceProfile(bool enabled);
    void SetEnableOnStartup(bool enabled);
    void SetTrayControlsEnabled(bool enabled);
    void SetShowTechnicalControls(bool enabled);
    void SetGraphicBand(size_t index, double gainDb);
    void SetCustomFilter(size_t index, const EqualizerFilter& filter);
    void AddCustomFilter();
    void RemoveCustomFilter(size_t index);
    void ResetCustomEq();
    void ResetDeviceProfile();

    std::vector<HeadphoneProfileSummary> SearchHeadphones(const std::wstring& query, size_t limit = 12) const;
    bool UpdateHeadphoneIndex(std::wstring& errorMessage);
    bool SelectHeadphone(const std::wstring& profileId, std::wstring& errorMessage);
    void SelectResolvedHeadphone(
        const std::wstring& profileId,
        const std::wstring& displayName,
        EqualizerProfile profile);
    bool ReloadHeadphoneIndex(std::wstring& errorMessage);
    bool ImportProfile(const std::filesystem::path& path, std::wstring& errorMessage);
    bool ExportCurrentProfile(const std::filesystem::path& path, std::wstring& errorMessage) const;
    bool ImportMeasurement(const std::filesystem::path& path, const std::wstring& profileName, std::wstring& errorMessage);
    std::wstring HeadphoneDatabaseVersion() const;

    bool Save(std::wstring& errorMessage) const;
    static std::filesystem::path DefaultDataDirectory();
    static std::wstring PresetDisplayName(const std::wstring& presetId);
    static std::vector<std::wstring> PresetIds();
    static std::wstring BackendAvailabilityText(BackendAvailability availability);

private:
    std::wstring EffectiveOutputId() const;
    std::optional<EqualizerProfile> SelectedHeadphoneCorrection(
        const DeviceEqualizerSettings& deviceSettings,
        std::wstring& errorMessage);
    double SampleRateForOutput(const std::wstring& deviceId) const;
    double CurrentSampleRate() const;
    bool TryComposeProfile(
        const DeviceEqualizerSettings& deviceSettings,
        double sampleRate,
        EqualizerProfile& profile,
        std::wstring& errorMessage);
    bool RebuildCurrentProfile(std::wstring* errorMessage = nullptr);
    void EnsureDeviceSettings(const std::wstring& deviceId);

    std::filesystem::path dataDirectory_;
    EqualizerSettings settings_;
    std::unique_ptr<EqualizerSettingsRepository> repository_;
    std::unique_ptr<IEqualizerBackend> backend_;
    HeadphoneProfileService headphoneProfiles_;
    AutoEqService autoEq_;
    std::vector<AudioOutputDevice> devices_;
    std::map<std::wstring, EqualizerProfile> resolvedHeadphoneProfiles_;
    EqualizerProfile currentProfile_;
    DeviceEqualizerSettings fallbackDeviceSettings_;
    std::wstring lastError_;
    bool initialized_ = false;
};
}
