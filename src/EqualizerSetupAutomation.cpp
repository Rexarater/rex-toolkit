#include "EqualizerSetupAutomation.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <thread>
#include <vector>

namespace rex::equalizer::setup
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr wchar_t kEqualizerApoRegistryPath[] = L"SOFTWARE\\EqualizerAPO";
constexpr wchar_t kRenderEndpointPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
constexpr wchar_t kConnectionNameValue[] =
    L"{a45c254e-df1c-4efd-8020-67d146a850e0},2";
constexpr wchar_t kDeviceNameValue[] =
    L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6";

std::wstring RegistryString(
    HKEY root,
    const std::wstring& path,
    const wchar_t* valueName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
            &key) != ERROR_SUCCESS)
    {
        if (RegOpenKeyExW(
                root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY,
                &key) != ERROR_SUCCESS)
        {
            return {};
        }
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(
            key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key, valueName, nullptr, &type,
            reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS)
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

std::wstring ElementName(IUIAutomationElement* element)
{
    if (!element) return {};
    BSTR raw = nullptr;
    if (FAILED(element->get_CurrentName(&raw)) || !raw) return {};
    std::wstring value(raw, SysStringLen(raw));
    SysFreeString(raw);
    return value;
}

std::wstring CollectElementText(
    IUIAutomation* automation,
    IUIAutomationElement* element)
{
    std::wstring text = ElementName(element);
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> descendants;
    if (!automation ||
        FAILED(automation->CreateTrueCondition(&condition)) ||
        FAILED(element->FindAll(
            TreeScope_Descendants, condition.Get(), &descendants)) ||
        !descendants)
    {
        return text;
    }

    int length = 0;
    descendants->get_Length(&length);
    for (int index = 0; index < length; ++index)
    {
        ComPtr<IUIAutomationElement> child;
        if (FAILED(descendants->GetElement(index, &child)) || !child) continue;
        const std::wstring name = ElementName(child.Get());
        if (name.empty()) continue;
        if (!text.empty()) text.push_back(L' ');
        text += name;
    }
    return text;
}

std::wstring ProcessFileName(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    std::array<wchar_t, 32768> path {};
    DWORD length = static_cast<DWORD>(path.size());
    const bool found = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!found || length == 0) return {};
    return std::filesystem::path(std::wstring(path.data(), length)).filename().wstring();
}

struct WindowQuery
{
    DWORD processId = 0;
    std::wstring titleContains;
    bool findSelector = false;
    HWND result = nullptr;
    DWORD resultProcessId = 0;
};

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter)
{
    auto* query = reinterpret_cast<WindowQuery*>(parameter);
    if (!query || query->result || !IsWindowVisible(window)) return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId || (query->processId && processId != query->processId)) return TRUE;

    std::array<wchar_t, 512> titleBuffer {};
    const int titleLength = GetWindowTextW(
        window, titleBuffer.data(), static_cast<int>(titleBuffer.size()));
    if (titleLength <= 0) return TRUE;
    const std::wstring title = NormalizeSelectorText(
        std::wstring_view(titleBuffer.data(), static_cast<size_t>(titleLength)));

    if (query->findSelector)
    {
        if (title.find(L"device selector") == std::wstring::npos &&
            title.find(L"configurator") == std::wstring::npos)
        {
            return TRUE;
        }
        if (!query->processId)
        {
            const std::wstring executable = NormalizeSelectorText(ProcessFileName(processId));
            if (executable != L"deviceselector exe" &&
                executable != L"configurator exe")
            {
                return TRUE;
            }
        }
    }
    else if (title.find(query->titleContains) == std::wstring::npos)
    {
        return TRUE;
    }

    query->result = window;
    query->resultProcessId = processId;
    return FALSE;
}

HWND FindTopLevelWindow(
    DWORD processId,
    std::wstring titleContains,
    bool selector,
    DWORD* discoveredProcessId = nullptr)
{
    WindowQuery query;
    query.processId = processId;
    query.titleContains = NormalizeSelectorText(titleContains);
    query.findSelector = selector;
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&query));
    if (discoveredProcessId) *discoveredProcessId = query.resultProcessId;
    return query.result;
}

bool InvokeNamedButton(
    IUIAutomation* automation,
    IUIAutomationElement* root,
    std::initializer_list<std::wstring_view> names)
{
    if (!automation || !root) return false;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> elements;
    if (FAILED(automation->CreateTrueCondition(&condition)) ||
        FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &elements)) ||
        !elements)
    {
        return false;
    }

    int length = 0;
    elements->get_Length(&length);
    for (int index = 0; index < length; ++index)
    {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(index, &element)) || !element) continue;
        const std::wstring elementName = NormalizeSelectorText(ElementName(element.Get()));
        const bool nameMatches = std::any_of(names.begin(), names.end(), [&](std::wstring_view name) {
            return elementName == NormalizeSelectorText(name);
        });
        if (!nameMatches) continue;

        BOOL enabled = FALSE;
        if (FAILED(element->get_CurrentIsEnabled(&enabled)) || !enabled) continue;
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (FAILED(element->GetCurrentPatternAs(
                UIA_InvokePatternId,
                __uuidof(IUIAutomationInvokePattern),
                reinterpret_cast<void**>(invoke.GetAddressOf()))) ||
            !invoke)
        {
            continue;
        }
        return SUCCEEDED(invoke->Invoke());
    }
    return false;
}

enum class ConfigureResult
{
    NotReady,
    UnsafeMatch,
    Applied
};

ConfigureResult ConfigureTarget(
    IUIAutomation* automation,
    HWND selectorWindow,
    const SelectorDeviceIdentity& target)
{
    ComPtr<IUIAutomationElement> window;
    if (!automation ||
        FAILED(automation->ElementFromHandle(selectorWindow, &window)) ||
        !window)
    {
        return ConfigureResult::NotReady;
    }

    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> allElements;
    if (FAILED(automation->CreateTrueCondition(&condition)) ||
        FAILED(window->FindAll(TreeScope_Descendants, condition.Get(), &allElements)) ||
        !allElements)
    {
        return ConfigureResult::NotReady;
    }

    ComPtr<IUIAutomationElement> playbackRoot;
    int allLength = 0;
    allElements->get_Length(&allLength);
    for (int index = 0; index < allLength; ++index)
    {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(allElements->GetElement(index, &element)) || !element) continue;
        if (NormalizeSelectorText(ElementName(element.Get())) == L"playback devices")
        {
            playbackRoot = element;
            break;
        }
    }

    struct Match
    {
        int score = 0;
        ComPtr<IUIAutomationElement> element;
        ComPtr<IUIAutomationTogglePattern> toggle;
    };
    std::vector<Match> matches;
    const auto collectMatches = [&](IUIAutomationElement* searchRoot) {
        ComPtr<IUIAutomationElementArray> candidates;
        if (!searchRoot || FAILED(searchRoot->FindAll(
                TreeScope_Descendants, condition.Get(), &candidates)) ||
            !candidates)
        {
            return;
        }

        int candidateLength = 0;
        candidates->get_Length(&candidateLength);
        for (int index = 0; index < candidateLength; ++index)
        {
            ComPtr<IUIAutomationElement> element;
            if (FAILED(candidates->GetElement(index, &element)) || !element) continue;
            ComPtr<IUIAutomationTogglePattern> toggle;
            if (FAILED(element->GetCurrentPatternAs(
                    UIA_TogglePatternId,
                    __uuidof(IUIAutomationTogglePattern),
                    reinterpret_cast<void**>(toggle.GetAddressOf()))) ||
                !toggle)
            {
                continue;
            }
            const int score = ScoreSelectorDeviceText(
                CollectElementText(automation, element.Get()), target);
            if (score > 0) matches.push_back({ score, element, toggle });
        }
    };

    collectMatches(playbackRoot ? playbackRoot.Get() : window.Get());
    if (matches.empty() && playbackRoot) collectMatches(window.Get());

    if (matches.empty()) return ConfigureResult::UnsafeMatch;
    std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
        return left.score > right.score;
    });
    constexpr int kMinimumSafeScore = 220;
    if (matches.front().score < kMinimumSafeScore ||
        (matches.size() > 1 && matches.front().score == matches[1].score))
    {
        return ConfigureResult::UnsafeMatch;
    }

    ToggleState state = ToggleState_Indeterminate;
    if (FAILED(matches.front().toggle->get_CurrentToggleState(&state)))
    {
        return ConfigureResult::NotReady;
    }
    if (state == ToggleState_Off)
    {
        if (FAILED(matches.front().toggle->Toggle())) return ConfigureResult::NotReady;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    else if (state != ToggleState_On)
    {
        return ConfigureResult::UnsafeMatch;
    }

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (InvokeNamedButton(automation, window.Get(), { L"OK" }))
        {
            return ConfigureResult::Applied;
        }
        if (state == ToggleState_On &&
            InvokeNamedButton(automation, window.Get(), { L"Close" }))
        {
            return ConfigureResult::Applied;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return ConfigureResult::NotReady;
}

bool InvokeDialogButton(
    IUIAutomation* automation,
    HWND window,
    std::initializer_list<std::wstring_view> names)
{
    ComPtr<IUIAutomationElement> root;
    return automation && window &&
        SUCCEEDED(automation->ElementFromHandle(window, &root)) && root &&
        InvokeNamedButton(automation, root.Get(), names);
}

bool IsExpectedInfoDialog(IUIAutomation* automation, HWND window)
{
    ComPtr<IUIAutomationElement> root;
    if (!automation || !window ||
        FAILED(automation->ElementFromHandle(window, &root)) || !root)
    {
        return false;
    }
    const std::wstring text = NormalizeSelectorText(
        CollectElementText(automation, root.Get()));
    return text.find(L"dialog can be reopened anytime") != std::wstring::npos;
}
}

bool IsValidEndpointGuid(std::wstring_view endpointGuid)
{
    if (endpointGuid.size() != 38 || endpointGuid.front() != L'{' ||
        endpointGuid.back() != L'}')
    {
        return false;
    }
    const std::wstring value(endpointGuid);
    GUID parsed {};
    return SUCCEEDED(CLSIDFromString(value.c_str(), &parsed));
}

std::wstring NormalizeSelectorText(std::wstring_view value)
{
    std::wstring normalized;
    normalized.reserve(value.size());
    bool pendingSpace = false;
    for (const wchar_t character : value)
    {
        if (std::iswalnum(character))
        {
            if (pendingSpace && !normalized.empty()) normalized.push_back(L' ');
            normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
            pendingSpace = false;
        }
        else
        {
            pendingSpace = true;
        }
    }
    return normalized;
}

int ScoreSelectorDeviceText(
    std::wstring_view candidate,
    const SelectorDeviceIdentity& target)
{
    const std::wstring text = NormalizeSelectorText(candidate);
    const std::wstring connection = NormalizeSelectorText(target.connectionName);
    const std::wstring device = NormalizeSelectorText(target.deviceName);
    if (text.empty() || (connection.empty() && device.empty())) return 0;

    const bool hasConnection = !connection.empty() &&
        text.find(connection) != std::wstring::npos;
    const bool hasDevice = !device.empty() && text.find(device) != std::wstring::npos;
    if (hasConnection && hasDevice) return 600;
    if (!connection.empty() && text == connection) return 260;
    if (!device.empty() && text == device) return 240;
    if (hasConnection) return 180;
    if (hasDevice) return 160;
    return 0;
}

bool LoadSelectorDeviceIdentity(
    std::wstring_view endpointGuid,
    SelectorDeviceIdentity& identity)
{
    identity = {};
    if (!IsValidEndpointGuid(endpointGuid)) return false;
    identity.endpointGuid.assign(endpointGuid.begin(), endpointGuid.end());
    const std::wstring properties =
        std::wstring(kRenderEndpointPath) + identity.endpointGuid + L"\\Properties";
    identity.connectionName = RegistryString(
        HKEY_LOCAL_MACHINE, properties, kConnectionNameValue);
    identity.deviceName = RegistryString(
        HKEY_LOCAL_MACHINE, properties, kDeviceNameValue);
    return !identity.connectionName.empty() || !identity.deviceName.empty();
}

bool IsEndpointConfigured(std::wstring_view endpointGuid)
{
    if (!IsValidEndpointGuid(endpointGuid)) return false;
    const std::wstring path = std::wstring(kEqualizerApoRegistryPath) +
        L"\\Child APOs\\" + std::wstring(endpointGuid);

    const auto configuredInView = [&](REGSAM registryView)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE, path.c_str(), 0,
                KEY_QUERY_VALUE | registryView, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD type = 0;
        DWORD bytes = 0;
        bool configured = false;
        if (RegQueryValueExW(
                key, L"Version", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS)
        {
            if (type == REG_DWORD && bytes == sizeof(DWORD))
            {
                DWORD version = 0;
                DWORD valueBytes = sizeof(version);
                DWORD valueType = 0;
                configured = RegQueryValueExW(
                    key, L"Version", nullptr, &valueType,
                    reinterpret_cast<BYTE*>(&version), &valueBytes) == ERROR_SUCCESS &&
                    valueType == REG_DWORD && valueBytes == sizeof(version) && version > 0;
            }
            else if ((type == REG_SZ || type == REG_EXPAND_SZ) &&
                     bytes >= sizeof(wchar_t))
            {
                std::vector<wchar_t> version(
                    bytes / sizeof(wchar_t) + 1, L'\0');
                DWORD valueBytes = bytes;
                DWORD valueType = 0;
                if (RegQueryValueExW(
                        key, L"Version", nullptr, &valueType,
                        reinterpret_cast<BYTE*>(version.data()), &valueBytes) == ERROR_SUCCESS &&
                    (valueType == REG_SZ || valueType == REG_EXPAND_SZ))
                {
                    size_t length = valueBytes / sizeof(wchar_t);
                    while (length > 0 && version[length - 1] == L'\0') --length;
                    configured = length > 0 &&
                        std::all_of(version.begin(), version.begin() + length,
                            [](wchar_t character) { return std::iswdigit(character) != 0; }) &&
                        std::any_of(version.begin(), version.begin() + length,
                            [](wchar_t character) { return character != L'0'; });
                }
            }
        }
        RegCloseKey(key);
        return configured;
    };

    return configuredInView(KEY_WOW64_64KEY) ||
        configuredInView(KEY_WOW64_32KEY);
}

std::filesystem::path FindInstalledDeviceSelector()
{
    const std::wstring installPath = RegistryString(
        HKEY_LOCAL_MACHINE, kEqualizerApoRegistryPath, L"InstallPath");
    if (installPath.empty()) return {};
    const std::filesystem::path root(installPath);
    const std::filesystem::path selector = root / L"DeviceSelector.exe";
    if (std::filesystem::is_regular_file(selector)) return selector;
    const std::filesystem::path configurator = root / L"Configurator.exe";
    return std::filesystem::is_regular_file(configurator)
        ? configurator
        : std::filesystem::path {};
}

void MonitorDeviceSelector(
    const SelectorDeviceIdentity& target,
    DWORD expectedProcessId,
    std::atomic_bool& stopRequested)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) return;

    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation))) || !automation)
    {
        if (uninitialize) CoUninitialize();
        return;
    }

    DWORD selectorProcessId = expectedProcessId;
    bool selectorHandled = false;
    while (!stopRequested.load(std::memory_order_relaxed))
    {
        DWORD discoveredProcessId = 0;
        const HWND selector = FindTopLevelWindow(
            selectorProcessId, {}, true, &discoveredProcessId);
        if (selector)
        {
            if (!selectorProcessId) selectorProcessId = discoveredProcessId;
            if (!selectorHandled &&
                ConfigureTarget(automation.Get(), selector, target) == ConfigureResult::Applied)
            {
                selectorHandled = true;
            }
        }

        if (selectorProcessId)
        {
            const HWND testWindow = FindTopLevelWindow(
                selectorProcessId, L"testing apo installation", false);
            if (testWindow)
            {
                InvokeDialogButton(automation.Get(), testWindow, { L"OK" });
            }

            const HWND infoWindow = FindTopLevelWindow(
                selectorProcessId, L"info", false);
            if (infoWindow && IsExpectedInfoDialog(automation.Get(), infoWindow))
            {
                InvokeDialogButton(automation.Get(), infoWindow, { L"OK" });
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    automation.Reset();
    if (uninitialize) CoUninitialize();
}
}
