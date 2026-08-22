#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

namespace rex::equalizer::setup
{
struct SelectorDeviceIdentity
{
    std::wstring endpointGuid;
    std::wstring connectionName;
    std::wstring deviceName;
};

bool IsValidEndpointGuid(std::wstring_view endpointGuid);
std::wstring NormalizeSelectorText(std::wstring_view value);
int ScoreSelectorDeviceText(
    std::wstring_view candidate,
    const SelectorDeviceIdentity& target);

bool LoadSelectorDeviceIdentity(
    std::wstring_view endpointGuid,
    SelectorDeviceIdentity& identity);
bool IsEndpointConfigured(std::wstring_view endpointGuid);
std::filesystem::path FindInstalledDeviceSelector();

void MonitorDeviceSelector(
    const SelectorDeviceIdentity& target,
    DWORD expectedProcessId,
    std::atomic_bool& stopRequested);
}
