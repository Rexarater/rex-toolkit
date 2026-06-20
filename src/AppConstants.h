#pragma once

#define REX_TOOLKIT_WIDEN2(value) L##value
#define REX_TOOLKIT_WIDEN(value) REX_TOOLKIT_WIDEN2(value)

inline constexpr wchar_t APP_VERSION[] = L"0.5.0";
inline constexpr wchar_t APP_BUILD_DATE[] = REX_TOOLKIT_WIDEN(__DATE__);
inline constexpr wchar_t UPDATE_MANIFEST_URL[] =
    L"https://raw.githubusercontent.com/Rexarater/rex-toolkit/main/latest.json";
