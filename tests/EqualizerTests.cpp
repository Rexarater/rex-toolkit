#include "EqualizerConfigParser.h"
#include "EqualizerAtomicFile.h"
#include "EqualizerService.h"
#include "EqualizerSetupAutomation.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
bool Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

bool Near(double value, double expected, double tolerance)
{
    return std::abs(value - expected) <= tolerance;
}

bool SameFilter(
    const rex::equalizer::EqualizerFilter& left,
    const rex::equalizer::EqualizerFilter& right)
{
    return left.id == right.id &&
        left.enabled == right.enabled &&
        left.type == right.type &&
        Near(left.frequencyHz, right.frequencyHz, 0.001) &&
        Near(left.gainDb, right.gainDb, 0.001) &&
        Near(left.q, right.q, 0.001);
}

std::filesystem::path TestRoot()
{
    return std::filesystem::temp_directory_path() /
        (L"RexToolkitEqualizerTests-" + std::to_wstring(GetCurrentProcessId()));
}

void WriteMeasurement(
    const std::filesystem::path& path,
    bool duplicateFrequency)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "frequency,raw\n";
    constexpr int pointCount = 48;
    for (int index = 0; index < pointCount; ++index)
    {
        const double ratio = static_cast<double>(index) /
            static_cast<double>(pointCount - 1);
        double frequency = 20.0 * std::pow(1000.0, ratio);
        if (duplicateFrequency && index == 8)
        {
            const double previousRatio = static_cast<double>(index - 1) /
                static_cast<double>(pointCount - 1);
            frequency = 20.0 * std::pow(1000.0, previousRatio);
        }
        const double response = 2.0 * std::sin(std::log(frequency / 20.0));
        file << frequency << ',' << response << '\n';
    }
}
}

int main()
{
    using namespace rex::equalizer;

    bool ok = true;
    std::wstring error;

    ok &= Require(
        !detail::ContainsActiveManagedInclude(
            L"# Include: rexs_toolkit_equalizer.txt\r\n") &&
        !detail::ContainsActiveManagedInclude(
            L"; Include: rexs_toolkit_equalizer.txt\r\n"),
        "Commented Equalizer APO includes should not be treated as active.");
    ok &= Require(
        detail::ContainsActiveManagedInclude(
            L"# disabled\r\n  include: REXS_TOOLKIT_EQUALIZER.TXT  \r\n"),
        "An uncommented managed include should be detected case-insensitively.");

    const setup::SelectorDeviceIdentity selectorTarget {
        L"{01234567-89ab-cdef-0123-456789abcdef}",
        L"Speakers",
        L"Realtek(R) Audio"
    };
    ok &= Require(
        setup::IsValidEndpointGuid(selectorTarget.endpointGuid) &&
            !setup::IsValidEndpointGuid(L"not-an-endpoint"),
        "Endpoint setup should accept only canonical Windows endpoint GUIDs.");
    ok &= Require(
        !setup::IsEndpointConfigured(L"not-an-endpoint"),
        "Endpoint verification should reject malformed output identifiers.");
    ok &= Require(
        setup::ScoreSelectorDeviceText(
            L"Speakers Realtek(R) Audio Not installed", selectorTarget) >= 600,
        "Selector matching should strongly prefer a row containing both device names.");
    ok &= Require(
        setup::ScoreSelectorDeviceText(L"Speakers", selectorTarget) >= 220,
        "Selector matching should recognize the exact playback connector.");
    ok &= Require(
        setup::ScoreSelectorDeviceText(
            L"Microphone Realtek(R) Audio", selectorTarget) < 220,
        "Selector matching should not automatically choose an ambiguous partial match.");
    ok &= Require(
        setup::NormalizeSelectorText(L"  Realtek(R)  Audio  ") == L"realtek r audio",
        "Selector matching should normalize punctuation and repeated whitespace.");

    const DeviceEqualizerSettings defaultDeviceSettings;
    ok &= Require(!defaultDeviceSettings.preventClipping,
        "Clipping protection should be opt-in for new output profiles.");

    EqualizerFilter flat;
    flat.gainDb = 0.0;
    ok &= Require(
        Near(AutoEqService::FilterResponseDb(flat, 1000.0), 0.0, 0.001),
        "A zero-gain peaking filter should be flat.");

    EqualizerFilter peak;
    peak.type = FilterType::Peaking;
    peak.frequencyHz = 1000.0;
    peak.gainDb = 6.0;
    peak.q = 1.0;
    ok &= Require(
        Near(AutoEqService::FilterResponseDb(peak, 1000.0), 6.0, 0.05),
        "A peaking filter should reach its requested center gain.");
    ok &= Require(
        AutoEqService::FilterResponseDb(peak, 50.0) <
            AutoEqService::FilterResponseDb(peak, 1000.0),
        "A peaking filter should attenuate away from its center.");

    EqualizerFilter lowShelf;
    lowShelf.type = FilterType::LowShelf;
    lowShelf.frequencyHz = 100.0;
    lowShelf.gainDb = 6.0;
    lowShelf.q = 0.7;
    const double lowShelfBass =
        AutoEqService::FilterResponseDb(lowShelf, 30.0);
    const double lowShelfTreble =
        AutoEqService::FilterResponseDb(lowShelf, 5000.0);
    ok &= Require(
        lowShelfBass > 4.5 && std::abs(lowShelfTreble) < 0.5,
        "A low shelf should boost low frequencies without boosting treble.");

    EqualizerFilter highShelf;
    highShelf.type = FilterType::HighShelf;
    highShelf.frequencyHz = 5000.0;
    highShelf.gainDb = 5.0;
    highShelf.q = 0.7;
    const double highShelfBass =
        AutoEqService::FilterResponseDb(highShelf, 100.0);
    const double highShelfTreble =
        AutoEqService::FilterResponseDb(highShelf, 12000.0);
    ok &= Require(
        highShelfTreble > 3.5 && std::abs(highShelfBass) < 0.5,
        "A high shelf should boost treble without boosting bass.");

    const double automaticPreamp =
        AutoEqService::CalculateAutomaticPreamp({ peak });
    ok &= Require(
        automaticPreamp <= -6.0 && automaticPreamp >= -6.12,
        "Automatic preamp should reserve only a small safety margin beyond the positive peak.");

    EqualizerFilter cut = peak;
    cut.gainDb = -6.0;
    ok &= Require(
        Near(AutoEqService::CalculateAutomaticPreamp({ cut }), 0.0, 0.001),
        "A profile containing only cuts should not reduce overall volume.");

    EqualizerFilter narrowPeak = peak;
    narrowPeak.frequencyHz = 1234.5;
    narrowPeak.gainDb = 5.7;
    narrowPeak.q = 20.0;
    EqualizerFilter adjacentPeak = peak;
    adjacentPeak.frequencyHz = 1271.25;
    adjacentPeak.gainDb = 3.1;
    adjacentPeak.q = 14.0;
    const std::vector<EqualizerFilter> narrowFilters { narrowPeak, adjacentPeak };
    double denseMaximum = 0.0;
    constexpr int denseSampleCount = 100000;
    for (int index = 0; index < denseSampleCount; ++index)
    {
        const double ratio = static_cast<double>(index) /
            static_cast<double>(denseSampleCount - 1);
        const double frequency = 20.0 * std::pow(1000.0, ratio);
        double response = 0.0;
        for (const auto& filter : narrowFilters)
        {
            response += AutoEqService::FilterResponseDb(filter, frequency);
        }
        denseMaximum = std::max(denseMaximum, response);
    }
    const double narrowHeadroom =
        AutoEqService::CalculateAutomaticPreamp(narrowFilters);
    ok &= Require(
        -narrowHeadroom >= denseMaximum && -narrowHeadroom <= denseMaximum + 0.08,
        "Automatic preamp should find narrow combined peaks without reserving excessive headroom.");

    struct ExpectedFilter
    {
        FilterType type;
        double frequencyHz;
        double gainDb;
        double q;
    };
    struct ExpectedPreset
    {
        const wchar_t* id;
        const wchar_t* displayName;
        std::vector<ExpectedFilter> filters;
    };
    const std::array<ExpectedPreset, 12> expectedPresets {{
        { L"balanced", L"Balanced", {} },
        { L"bass_plus", L"Bass+", {
            { FilterType::LowShelf, 95.0, 5.0, 0.70 } } },
        { L"bass_reduce", L"Bass Reduce", {
            { FilterType::LowShelf, 105.0, -3.0, 0.70 } } },
        { L"warm", L"Warm", {
            { FilterType::LowShelf, 180.0, 2.0, 0.65 },
            { FilterType::HighShelf, 5500.0, -0.8, 0.70 } } },
        { L"bright", L"Bright", {
            { FilterType::HighShelf, 4200.0, 2.0, 0.70 } } },
        { L"music", L"Music", {
            { FilterType::LowShelf, 90.0, 1.2, 0.70 },
            { FilterType::HighShelf, 6000.0, 0.8, 0.70 } } },
        { L"movies", L"Movies", {
            { FilterType::LowShelf, 90.0, 1.8, 0.70 },
            { FilterType::Peaking, 2600.0, 1.0, 0.85 } } },
        { L"voice", L"Voice", {
            { FilterType::LowShelf, 160.0, -2.0, 0.70 },
            { FilterType::Peaking, 2300.0, 2.0, 0.90 } } },
        { L"gaming", L"Gaming", {
            { FilterType::LowShelf, 105.0, -1.0, 0.70 },
            { FilterType::Peaking, 2800.0, 1.4, 0.85 } } },
        { L"footsteps", L"Footsteps", {
            { FilterType::LowShelf, 120.0, -2.5, 0.70 },
            { FilterType::Peaking, 3200.0, 2.0, 0.85 } } },
        { L"late_night", L"Late Night", {
            { FilterType::LowShelf, 120.0, -2.0, 0.70 },
            { FilterType::HighShelf, 6500.0, -1.0, 0.70 } } },
        { L"custom", L"Custom", {} }
    }};

    AutoEqService presetOptimizer;
    EqualizerProfile bassProfile;
    for (const auto& expected : expectedPresets)
    {
        DeviceEqualizerSettings presetSettings;
        presetSettings.soundPreset = expected.id;
        presetSettings.preventClipping = true;
        presetSettings.automaticPreamp = true;
        const EqualizerProfile profile =
            presetOptimizer.Compose(EqualizerProfile {}, presetSettings);
        ok &= Require(
            profile.preferenceProfileId == expected.id &&
                profile.name == expected.displayName,
            "Composed sound profiles should report the selected preset.");
        ok &= Require(
            profile.filters.size() == expected.filters.size(),
            "Each sound preset should compose exactly its intended filters.");

        const size_t filterCount = std::min(profile.filters.size(), expected.filters.size());
        for (size_t filterIndex = 0; filterIndex < filterCount; ++filterIndex)
        {
            const auto& actual = profile.filters[filterIndex];
            const auto& intended = expected.filters[filterIndex];
            ok &= Require(
                actual.type == intended.type &&
                    Near(actual.frequencyHz, intended.frequencyHz, 0.001) &&
                    Near(actual.gainDb, intended.gainDb, 0.001) &&
                    Near(actual.q, intended.q, 0.001),
                "A sound preset filter differs from its intended DSP definition.");
            error.clear();
            ok &= Require(
                AutoEqService::ValidateFilter(actual, error),
                "Every built-in sound preset filter should be valid.");
        }
        ok &= Require(
            Near(profile.preampDb,
                AutoEqService::CalculateAutomaticPreamp(profile.filters), 0.001),
            "Every sound preset should apply automatic clipping headroom.");
        double maximumResponse = -1000.0;
        for (int responseIndex = 0; responseIndex < 512; ++responseIndex)
        {
            const double ratio = static_cast<double>(responseIndex) / 511.0;
            const double frequency = 20.0 * std::pow(1000.0, ratio);
            maximumResponse = std::max(
                maximumResponse,
                AutoEqService::ProfileResponseDb(profile, frequency));
        }
        ok &= Require(
            maximumResponse <= 0.001,
            "A clipping-protected sound preset should not exceed digital full scale.");
        if (std::wstring(expected.id) == L"bass_plus") bassProfile = profile;
    }

    const double bassLow = AutoEqService::ProfileResponseDb(bassProfile, 35.0);
    const double bassMid = AutoEqService::ProfileResponseDb(bassProfile, 1000.0);
    const double bassHigh = AutoEqService::ProfileResponseDb(bassProfile, 10000.0);
    ok &= Require(
        bassProfile.filters.size() == 1 &&
            bassLow - bassMid > 4.2 &&
            std::abs(bassHigh - bassMid) < 0.25,
        "Bass+ should boost bass by about 5 dB without raising mids or treble.");
    ok &= Require(
        bassProfile.preampDb < -4.8 && bassProfile.preampDb > -5.2,
        "Bass+ should reserve about 5 dB of clipping headroom.");

    EqualizerProfile correction;
    correction.name = L"Measured correction";
    correction.headphoneProfileId = L"autoeq/test-headphones";
    correction.targetCurveId = L"harman-over-ear-2018";
    correction.source = L"AutoEq / Test measurement rig";
    correction.sourceUrl = L"https://example.test/autoeq/test-headphones";
    correction.profileVersion = L"2026-08-14";
    correction.preampDb = -6.25;
    correction.filters.push_back(peak);
    EqualizerFilter correctionShelf;
    correctionShelf.id = 2;
    correctionShelf.type = FilterType::LowShelf;
    correctionShelf.frequencyHz = 180.0;
    correctionShelf.gainDb = -2.5;
    correctionShelf.q = 0.75;
    correction.filters.push_back(correctionShelf);
    DeviceEqualizerSettings preference;
    preference.soundPreset = L"bass_plus";
    preference.bassDb = 1.5;
    preference.trebleDb = -1.0;
    preference.preventClipping = true;
    preference.automaticPreamp = true;
    AutoEqService optimizer;
    const EqualizerProfile composed =
        optimizer.Compose(correction, preference);
    ok &= Require(
        composed.filters.size() == correction.filters.size() + 3,
        "Composition should retain correction and layer preset/user preferences.");
    ok &= Require(
        composed.headphoneProfileId == correction.headphoneProfileId &&
            composed.targetCurveId == correction.targetCurveId &&
            composed.source == correction.source &&
            composed.sourceUrl == correction.sourceUrl &&
            composed.profileVersion == correction.profileVersion &&
            composed.preferenceProfileId == L"bass_plus",
        "Composition should preserve headphone-profile identity and attribution while reporting the selected preset.");
    bool correctionFiltersPreserved =
        composed.filters.size() >= correction.filters.size();
    for (size_t index = 0;
         correctionFiltersPreserved && index < correction.filters.size();
         ++index)
    {
        correctionFiltersPreserved =
            SameFilter(composed.filters[index], correction.filters[index]);
    }
    ok &= Require(
        correctionFiltersPreserved,
        "Composition should preserve every headphone-correction filter in its original order.");
    const size_t preferenceOffset = correction.filters.size();
    ok &= Require(
        composed.filters.size() >= preferenceOffset + 3 &&
            composed.filters[preferenceOffset].type == FilterType::LowShelf &&
            Near(composed.filters[preferenceOffset].frequencyHz, 95.0, 0.001) &&
            Near(composed.filters[preferenceOffset].gainDb, 5.0, 0.001) &&
            composed.filters[preferenceOffset + 1].type == FilterType::LowShelf &&
            Near(composed.filters[preferenceOffset + 1].frequencyHz, 95.0, 0.001) &&
            Near(composed.filters[preferenceOffset + 1].gainDb, 1.5, 0.001) &&
            composed.filters[preferenceOffset + 2].type == FilterType::HighShelf &&
            Near(composed.filters[preferenceOffset + 2].frequencyHz, 6000.0, 0.001) &&
            Near(composed.filters[preferenceOffset + 2].gainDb, -1.0, 0.001),
        "Bass+, Bass fine-tuning, and Treble fine-tuning should be appended after the headphone correction.");

    DeviceEqualizerSettings correctionOnlySettings;
    correctionOnlySettings.preventClipping = true;
    correctionOnlySettings.automaticPreamp = true;
    const EqualizerProfile correctionOnly =
        optimizer.Compose(correction, correctionOnlySettings);
    const EqualizerProfile preferenceOnly =
        optimizer.Compose(std::nullopt, preference);
    bool responseLayersAdd = true;
    for (const double frequency :
         std::array<double, 7> { 30.0, 95.0, 180.0, 1000.0, 3000.0, 6000.0, 16000.0 })
    {
        const double combinedWithoutPreamp =
            AutoEqService::ProfileResponseDb(composed, frequency) - composed.preampDb;
        const double correctionWithoutPreamp =
            AutoEqService::ProfileResponseDb(correctionOnly, frequency) - correctionOnly.preampDb;
        const double preferenceWithoutPreamp =
            AutoEqService::ProfileResponseDb(preferenceOnly, frequency) - preferenceOnly.preampDb;
        responseLayersAdd = responseLayersAdd && Near(
            combinedWithoutPreamp,
            correctionWithoutPreamp + preferenceWithoutPreamp,
            0.001);
    }
    ok &= Require(
        responseLayersAdd,
        "The final frequency response should be the sum of headphone correction and the selected sound preset.");
    ok &= Require(
        Near(
            composed.preampDb,
            AutoEqService::CalculateAutomaticPreamp(composed.filters),
            0.001),
        "Clipping headroom should be recalculated from the complete combined profile.");
    double composedMaximumResponse = -1000.0;
    for (int responseIndex = 0; responseIndex < 512; ++responseIndex)
    {
        const double ratio = static_cast<double>(responseIndex) / 511.0;
        const double frequency = 20.0 * std::pow(1000.0, ratio);
        composedMaximumResponse = std::max(
            composedMaximumResponse,
            AutoEqService::ProfileResponseDb(composed, frequency));
    }
    ok &= Require(
        composedMaximumResponse <= 0.001,
        "The combined headphone correction and preset should remain below digital full scale.");
    ok &= Require(
        composed.preampDb < -5.9,
        "Composed positive boosts should receive automatic headroom.");

    EqualizerProfile rateSensitiveCorrection;
    EqualizerFilter highFrequencyShelf;
    highFrequencyShelf.id = 1;
    highFrequencyShelf.type = FilterType::HighShelf;
    highFrequencyShelf.frequencyHz = 18000.0;
    highFrequencyShelf.gainDb = 12.0;
    highFrequencyShelf.q = 0.70;
    rateSensitiveCorrection.filters.push_back(highFrequencyShelf);
    DeviceEqualizerSettings rateSensitiveSettings;
    rateSensitiveSettings.preventClipping = true;
    rateSensitiveSettings.automaticPreamp = true;
    const EqualizerProfile composedAt44100 =
        optimizer.Compose(rateSensitiveCorrection, rateSensitiveSettings, 44100.0);
    const EqualizerProfile composedAt96000 =
        optimizer.Compose(rateSensitiveCorrection, rateSensitiveSettings, 96000.0);
    ok &= Require(
        Near(composedAt44100.preampDb,
            AutoEqService::CalculateAutomaticPreamp(composedAt44100.filters, 44100.0), 0.001) &&
        Near(composedAt96000.preampDb,
            AutoEqService::CalculateAutomaticPreamp(composedAt96000.filters, 96000.0), 0.001),
        "Automatic headroom should use the selected output sample rate.");
    ok &= Require(
        std::abs(composedAt44100.preampDb - composedAt96000.preampDb) > 0.10,
        "The sample-rate regression fixture should produce distinct headroom values.");

    DeviceEqualizerSettings parametric;
    parametric.editorMode = EditorMode::Parametric;
    parametric.parametricOverrideActive = true;
    parametric.customFilters = correction.filters;
    const EqualizerProfile editableCorrection =
        optimizer.Compose(correction, parametric);
    ok &= Require(
        editableCorrection.filters.size() == correction.filters.size(),
        "Parametric editing should replace, not duplicate, the measured correction.");
    ok &= Require(
        Near(editableCorrection.filters.front().gainDb, peak.gainDb, 0.001),
        "The editable Parametric copy should preserve the generated filter values.");
    parametric.soundPreset = L"bass_plus";
    const EqualizerProfile editableWithPreference =
        optimizer.Compose(correction, parametric);
    ok &= Require(
        editableWithPreference.filters.size() == correction.filters.size() + 1,
        "Sound preferences should continue to layer over an editable Parametric correction.");
    parametric.soundPreset = L"balanced";
    parametric.customFilters.clear();
    const EqualizerProfile intentionallyEmpty =
        optimizer.Compose(correction, parametric);
    ok &= Require(
        intentionallyEmpty.filters.empty(),
        "An intentionally empty Parametric override should not restore the measured correction.");

    EqualizerFilter invalid = peak;
    invalid.frequencyHz = 5.0;
    error.clear();
    ok &= Require(
        !AutoEqService::ValidateFilter(invalid, error) && !error.empty(),
        "Out-of-range filter frequencies should be rejected.");
    invalid = peak;
    invalid.gainDb = 25.0;
    error.clear();
    ok &= Require(
        !AutoEqService::ValidateFilter(invalid, error),
        "Out-of-range filter gain should be rejected.");
    invalid = peak;
    invalid.q = 0.01;
    error.clear();
    ok &= Require(
        !AutoEqService::ValidateFilter(invalid, error),
        "Out-of-range filter Q should be rejected.");

    const std::filesystem::path root = TestRoot();
    std::error_code fileError;
    std::filesystem::remove_all(root, fileError);
    std::filesystem::create_directories(root, fileError);
    ok &= Require(!fileError, "The Equalizer test folder should be created.");

    std::filesystem::create_directories(root / L"profiles", fileError);
    ok &= Require(!fileError, "The AutoEq test profile folder should be created.");
    {
        std::ofstream index(root / L"profiles" / L"autoeq_index.md", std::ios::binary);
        index << "# Headphone Results\n"
              << "- [1MORE Aero (ANC Off)](./HypetheSonics/in-ear/1MORE%20Aero%20(ANC%20Off)) by HypetheSonics on B&K 5128\n";
    }
    HeadphoneProfileService indexService;
    error.clear();
    ok &= Require(
        indexService.Initialize(root, error),
        "A local AutoEq index should load.");
    ok &= Require(
        indexService.Profiles().size() == 1 &&
            indexService.Profiles().front().id ==
                L"HypetheSonics/in-ear/1MORE%20Aero%20(ANC%20Off)",
        "AutoEq index paths containing literal parentheses should remain intact.");
    {
        std::ofstream invalidIndex(
            root / L"profiles" / L"autoeq_index.md",
            std::ios::binary | std::ios::trunc);
        invalidIndex << "# Index\nThis changed by upstream, but contains no profile entries by anyone.\n";
    }
    error.clear();
    ok &= Require(
        !indexService.Initialize(root, error) &&
            !error.empty() &&
            indexService.Profiles().size() == 1 &&
            indexService.Profiles().front().id ==
                L"HypetheSonics/in-ear/1MORE%20Aero%20(ANC%20Off)",
        "An invalid AutoEq index should be rejected without clearing the known-good directory.");
    {
        std::ofstream index(root / L"profiles" / L"autoeq_index.md", std::ios::binary | std::ios::trunc);
        index << "# Headphone Results\n"
              << "- [1MORE Aero (ANC Off)](./HypetheSonics/in-ear/1MORE%20Aero%20(ANC%20Off)) by HypetheSonics on B&K 5128\n";
    }
    EqualizerProfile uncachedProfile;
    error.clear();
    ok &= Require(
        !indexService.ResolveCachedProfile(
            L"HypetheSonics/in-ear/1MORE%20Aero%20(ANC%20Off)",
            uncachedProfile,
            error) &&
            !error.empty(),
        "Synchronous profile resolution should fail locally when a profile has not been downloaded.");

    const std::filesystem::path measurementPath =
        root / L"measurement.csv";
    WriteMeasurement(measurementPath, false);
    std::vector<MeasurementPoint> points;
    error.clear();
    ok &= Require(
        AutoEqService::ParseMeasurementFile(
            measurementPath, points, error),
        "A valid frequency-response CSV should parse.");
    ok &= Require(
        points.size() == 48,
        "A valid measurement should preserve all in-range points.");

    error.clear();
    const EqualizerProfile optimized =
        optimizer.OptimizeMeasurement(L"Test headphones", points, error);
    ok &= Require(
        error.empty() && optimized.filters.size() == 10,
        "Dynamic optimization should produce a bounded ten-filter profile.");
    ok &= Require(
        optimized.preampDb <= 0.0,
        "Dynamic optimization should not create positive preamp headroom.");

    std::filesystem::create_directories(root / L"custom", fileError);
    ok &= Require(!fileError, "The custom profile test folder should be created.");
    EqualizerProfile downloadedProfile = optimized;
    downloadedProfile.name = L"1MORE Aero Downloaded";
    downloadedProfile.headphoneProfileId = L"custom:downloaded-1more-aero";
    HeadphoneProfileService profileWriter;
    error.clear();
    ok &= Require(
        profileWriter.ExportProfile(
            root / L"custom" / L"downloaded_1more_aero.rexeq",
            downloadedProfile,
            error),
        "A downloaded-profile fixture should be written.");

    HeadphoneProfileService rankedIndexService;
    error.clear();
    ok &= Require(
        rankedIndexService.Initialize(root, error),
        "The profile directory should reload with cached entries.");
    const auto rankedResults = rankedIndexService.Search(L"1MORE Aero", 8);
    ok &= Require(
        rankedResults.size() >= 2 &&
            rankedResults.front().cached &&
            rankedResults.front().id == downloadedProfile.headphoneProfileId,
        "Downloaded headphone profiles should rank ahead of matching network-only entries.");
    EqualizerProfile cachedProfile;
    error.clear();
    ok &= Require(
        rankedIndexService.ResolveCachedProfile(
            downloadedProfile.headphoneProfileId,
            cachedProfile,
            error) &&
            cachedProfile.headphoneProfileId == downloadedProfile.headphoneProfileId &&
            !cachedProfile.filters.empty(),
        "Cache-only resolution should load an already downloaded headphone profile.");

    const std::filesystem::path duplicatePath =
        root / L"duplicate.csv";
    WriteMeasurement(duplicatePath, true);
    points.clear();
    error.clear();
    ok &= Require(
        !AutoEqService::ParseMeasurementFile(
            duplicatePath, points, error),
        "Duplicate/non-increasing measurement frequencies should be rejected.");

    HeadphoneProfileSummary autoEqSummary;
    autoEqSummary.id = L"oratory1990/over-ear/Test%20Headphones";
    autoEqSummary.manufacturer = L"Test";
    autoEqSummary.model = L"Headphones";
    autoEqSummary.measurementSource = L"oratory1990";
    autoEqSummary.sourceUrl = L"https://example.invalid/profile";
    const std::string currentAutoEqDocument = R"md(# Test Headphones
### Parametric EQs
Apply preamp of -6.2 dB.
| # | Type | Fc (Hz) | Q | Gain (dB) |
|---|---|---|---|---|
| 1 | LowShelf | 105 | 0.70 | 6.4 |
| 2 | Peaking | 1928 | 1.28 | 3.5 |
| 3 | HighShelf | 10000 | 0.70 | -4.2 |
### Fixed Band EQs
| 1 | Peaking | 31 | 1.41 | 6.6 |
)md";
    EqualizerProfile parsedAutoEq;
    error.clear();
    ok &= Require(
        HeadphoneProfileService::ParseAutoEqResult(
            currentAutoEqDocument, autoEqSummary, L"2026-08-12",
            parsedAutoEq, error),
        "Current AutoEq parametric tables should parse.");
    ok &= Require(
        parsedAutoEq.filters.size() == 3 &&
            parsedAutoEq.filters[0].type == FilterType::LowShelf &&
            parsedAutoEq.filters[2].type == FilterType::HighShelf,
        "AutoEq parsing should stop before fixed-band filters and retain filter types.");
    ok &= Require(
        parsedAutoEq.preampDb <= -6.2 &&
            parsedAutoEq.source.find(L"oratory1990") != std::wstring::npos,
        "AutoEq parsing should retain conservative headroom and attribution.");

    DeviceEqualizerSettings parsedProfilePreference;
    parsedProfilePreference.soundPreset = L"bass_plus";
    parsedProfilePreference.preventClipping = true;
    parsedProfilePreference.automaticPreamp = true;
    const EqualizerProfile parsedProfileComposed =
        optimizer.Compose(parsedAutoEq, parsedProfilePreference);
    bool parsedFiltersPreserved =
        parsedProfileComposed.filters.size() == parsedAutoEq.filters.size() + 1;
    for (size_t index = 0;
         parsedFiltersPreserved && index < parsedAutoEq.filters.size();
         ++index)
    {
        parsedFiltersPreserved =
            SameFilter(parsedProfileComposed.filters[index], parsedAutoEq.filters[index]);
    }
    ok &= Require(
        parsedFiltersPreserved &&
            parsedProfileComposed.source == parsedAutoEq.source &&
            parsedProfileComposed.sourceUrl == parsedAutoEq.sourceUrl &&
            parsedProfileComposed.preferenceProfileId == L"bass_plus" &&
            parsedProfileComposed.filters.back().type == FilterType::LowShelf &&
            Near(parsedProfileComposed.filters.back().frequencyHz, 95.0, 0.001) &&
            Near(parsedProfileComposed.filters.back().gainDb, 5.0, 0.001),
        "A downloaded AutoEq profile should retain all correction filters and append the selected sound preset.");

    const std::string compatibleFourColumnDocument = R"md(### Parametric EQs
| Type | Fc | Q | Gain |
|---|---|---|---|
| Peaking | 1000 Hz | 1.00 | -2.0 dB |
)md";
    error.clear();
    ok &= Require(
        HeadphoneProfileService::ParseAutoEqResult(
            compatibleFourColumnDocument, autoEqSummary, L"test",
            parsedAutoEq, error) &&
            parsedAutoEq.filters.size() == 1,
        "Compatible four-column parametric tables should parse.");

    const std::string oversizedNumericDocument =
        "### Parametric EQs\n"
        "| Type | Fc | Q | Gain |\n"
        "|---|---|---|---|\n"
        "| Peaking | " + std::string(400, '9') + " | 1.00 | -2.0 |\n";
    error.clear();
    bool oversizedNumericThrew = false;
    bool oversizedNumericParsed = false;
    try
    {
        oversizedNumericParsed = HeadphoneProfileService::ParseAutoEqResult(
            oversizedNumericDocument, autoEqSummary, L"test", parsedAutoEq, error);
    }
    catch (...)
    {
        oversizedNumericThrew = true;
    }
    ok &= Require(
        !oversizedNumericThrew && !oversizedNumericParsed && !error.empty(),
        "Out-of-range AutoEq numbers should be rejected without escaping the worker boundary.");

    HeadphoneProfileService profileService;
    const std::filesystem::path profilePath =
        root / L"roundtrip.rexeq";
    error.clear();
    ok &= Require(
        profileService.ExportProfile(profilePath, optimized, error),
        "A validated profile should export.");
    EqualizerProfile imported;
    error.clear();
    ok &= Require(
        profileService.ImportProfile(profilePath, imported, error),
        "An exported profile should import.");
    ok &= Require(
        imported.filters.size() == optimized.filters.size() &&
            imported.name == optimized.name,
        "Profile import/export should preserve core data.");

    HANDLE profileLock = CreateFileW(
        profilePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    ok &= Require(
        profileLock != INVALID_HANDLE_VALUE,
        "The profile fixture should open without delete sharing.");
    EqualizerProfile replacementProfile = optimized;
    replacementProfile.name = L"Updated while monitored";
    error.clear();
    const bool lockedReplacementSucceeded =
        profileLock != INVALID_HANDLE_VALUE &&
        profileService.ExportProfile(profilePath, replacementProfile, error);
    ok &= Require(
        !lockedReplacementSucceeded && !error.empty(),
        "A profile update should fail safely when Windows blocks atomic replacement.");
    if (profileLock != INVALID_HANDLE_VALUE) CloseHandle(profileLock);
    imported = {};
    error.clear();
    ok &= Require(
        profileService.ImportProfile(profilePath, imported, error) &&
            imported.name == optimized.name &&
            imported.filters.size() == optimized.filters.size(),
        "A failed atomic profile update should preserve the previous known-good file.");
    ok &= Require(
        !std::filesystem::exists(profilePath.wstring() + L".tmp"),
        "A failed atomic profile update should clean up its temporary file.");

    const std::filesystem::path retryDestination = root / L"atomic-retry.txt";
    const std::filesystem::path retryTemporary = root / L"atomic-retry.txt.tmp";
    {
        std::ofstream destination(retryDestination, std::ios::binary);
        destination << "old";
        std::ofstream temporary(retryTemporary, std::ios::binary);
        temporary << "new";
    }
    HANDLE transientLock = CreateFileW(
        retryDestination.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    ok &= Require(
        transientLock != INVALID_HANDLE_VALUE,
        "The transient atomic-replacement fixture should open without delete sharing.");
    std::thread releaseTransientLock;
    if (transientLock != INVALID_HANDLE_VALUE)
    {
        releaseTransientLock = std::thread([transientLock]() {
            Sleep(25);
            CloseHandle(transientLock);
        });
    }
    DWORD commitError = ERROR_SUCCESS;
    const bool retriedCommit = transientLock != INVALID_HANDLE_VALUE &&
        rex::equalizer::detail::CommitTemporaryFile(
            retryTemporary, retryDestination, commitError);
    if (releaseTransientLock.joinable()) releaseTransientLock.join();
    std::ifstream committedDestination(retryDestination, std::ios::binary);
    std::string committedText;
    std::getline(committedDestination, committedText);
    ok &= Require(
        retriedCommit && commitError == ERROR_SUCCESS && committedText == "new",
        "Atomic replacement should retry when a transient reader releases its lock.");
    error.clear();
    ok &= Require(
        profileService.ExportProfile(profilePath, replacementProfile, error),
        "A profile update should succeed after the destination lock is released.");

    const std::filesystem::path malformedPath =
        root / L"malformed.rexeq";
    {
        std::ofstream malformed(malformedPath, std::ios::binary);
        malformed <<
            "{\"format\":\"rex-equalizer-profile\","
            "\"schemaVersion\":1,\"filters\":[{"
            "\"type\":\"peaking\",\"frequencyHz\":1,"
            "\"gainDb\":0,\"q\":1}]}";
    }
    error.clear();
    ok &= Require(
        !profileService.ImportProfile(
            malformedPath, imported, error),
        "Imported profiles with unsafe filter values should be rejected.");

    EqualizerSettings saved;
    saved.followWindowsDefault = false;
    saved.selectedOutputId = L"endpoint-test";
    saved.rememberPerDevice = true;
    saved.deviceProfiles[L"endpoint-test"].enabled = true;
    saved.deviceProfiles[L"endpoint-test"].soundPreset = L"warm";
    saved.deviceProfiles[L"endpoint-test"].editorMode = EditorMode::Parametric;
    saved.deviceProfiles[L"endpoint-test"].parametricOverrideActive = true;
    saved.deviceProfiles[L"endpoint-test"].customProfileName = L"Imported reference";
    saved.deviceProfiles[L"endpoint-test"].customProfileSource = L"Profile author";
    saved.deviceProfiles[L"endpoint-test"].customProfileSourceUrl = L"https://example.test/profile";
    saved.deviceProfiles[L"endpoint-test"].customProfileVersion = L"2026-08-12";
    saved.deviceProfiles[L"endpoint-test"].customTargetCurveId = L"custom-target";
    saved.deviceProfiles[L"endpoint-test"].headphoneProfileId = L"autoeq/test-headphones";
    saved.deviceProfiles[L"endpoint-test"].headphoneDisplayName = L"Test Headphones";
    saved.deviceProfiles[L"endpoint-test"].preventClipping = true;
    EqualizerSettingsRepository repository(root / L"settings.json");
    error.clear();
    ok &= Require(
        repository.Save(saved, error),
        "Equalizer settings should save atomically.");
    EqualizerSettings loaded;
    error.clear();
    ok &= Require(
        repository.Load(loaded, error),
        "Equalizer settings should reload.");
    ok &= Require(
        loaded.selectedOutputId == L"endpoint-test" &&
            loaded.deviceProfiles[L"endpoint-test"].enabled &&
            loaded.deviceProfiles[L"endpoint-test"].soundPreset == L"warm" &&
            loaded.deviceProfiles[L"endpoint-test"].parametricOverrideActive &&
            loaded.deviceProfiles[L"endpoint-test"].customProfileName == L"Imported reference" &&
            loaded.deviceProfiles[L"endpoint-test"].customProfileSource == L"Profile author" &&
            loaded.deviceProfiles[L"endpoint-test"].customProfileSourceUrl == L"https://example.test/profile" &&
            loaded.deviceProfiles[L"endpoint-test"].customProfileVersion == L"2026-08-12" &&
            loaded.deviceProfiles[L"endpoint-test"].customTargetCurveId == L"custom-target" &&
            loaded.deviceProfiles[L"endpoint-test"].headphoneProfileId == L"autoeq/test-headphones" &&
            loaded.deviceProfiles[L"endpoint-test"].headphoneDisplayName == L"Test Headphones" &&
            loaded.deviceProfiles[L"endpoint-test"].preventClipping,
        "Per-output Equalizer state should survive a round trip.");

    std::filesystem::remove_all(root, fileError);
    return ok ? 0 : 1;
}