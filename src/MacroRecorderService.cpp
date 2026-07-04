#include "MacroRecorderService.h"

#include <objbase.h>
#include <psapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>

namespace
{
constexpr ULONG_PTR kMacroPlaybackExtraInfo = 0x52584D43;
constexpr wchar_t kInterceptionDllRelativePath[] = L"tools\\interception\\x64\\interception.dll";

using InterceptionContext = void*;
using InterceptionDevice = int;
using InterceptionStroke = char[sizeof(int) * 5];

constexpr int kInterceptionMaxKeyboard = 10;
constexpr int kInterceptionMaxMouse = 10;
constexpr int kInterceptionKeyboardBase = 1;
constexpr int kInterceptionMouseBase = kInterceptionMaxKeyboard + 1;
constexpr unsigned short kInterceptionFilterAll = 0xFFFF;
constexpr unsigned short kInterceptionKeyUp = 0x01;
constexpr unsigned short kInterceptionKeyE0 = 0x02;
constexpr unsigned short kInterceptionKeyE1 = 0x04;
constexpr unsigned short kInterceptionMouseLeftDown = 0x001;
constexpr unsigned short kInterceptionMouseLeftUp = 0x002;
constexpr unsigned short kInterceptionMouseRightDown = 0x004;
constexpr unsigned short kInterceptionMouseRightUp = 0x008;
constexpr unsigned short kInterceptionMouseMiddleDown = 0x010;
constexpr unsigned short kInterceptionMouseMiddleUp = 0x020;
constexpr unsigned short kInterceptionMouseButton4Down = 0x040;
constexpr unsigned short kInterceptionMouseButton4Up = 0x080;
constexpr unsigned short kInterceptionMouseButton5Down = 0x100;
constexpr unsigned short kInterceptionMouseButton5Up = 0x200;
constexpr unsigned short kInterceptionMouseWheel = 0x400;
constexpr unsigned short kInterceptionMouseMoveRelative = 0x000;
constexpr unsigned short kInterceptionMouseMoveAbsolute = 0x001;
constexpr unsigned short kInterceptionMouseVirtualDesktop = 0x002;

struct InterceptionMouseStroke
{
    unsigned short state;
    unsigned short flags;
    short rolling;
    int x;
    int y;
    unsigned int information;
};

struct InterceptionKeyStroke
{
    unsigned short code;
    unsigned short state;
    unsigned int information;
};

MacroRecorderService* g_macroRecorder = nullptr;

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring Trim(std::wstring value)
{
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    }).base();
    if (first >= last)
    {
        return {};
    }
    return std::wstring(first, last);
}

std::string JsonEscape(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::string output;
    output.reserve(utf8.size() + 8);
    output.push_back('"');
    for (unsigned char ch : utf8)
    {
        switch (ch)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                output += stream.str();
            }
            else
            {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::wstring UnescapeJsonString(const std::string& text)
{
    std::string output;
    output.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (ch != '\\' || index + 1 >= text.size())
        {
            output.push_back(ch);
            continue;
        }

        const char escaped = text[++index];
        switch (escaped)
        {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        default:
            break;
        }
    }
    return Utf8ToWide(output);
}

size_t FindField(const std::string& object, const char* key)
{
    const std::string quotedKey = std::string("\"") + key + "\"";
    const size_t keyPosition = object.find(quotedKey);
    if (keyPosition == std::string::npos)
    {
        return std::string::npos;
    }
    const size_t colon = object.find(':', keyPosition + quotedKey.size());
    return colon == std::string::npos ? std::string::npos : colon + 1;
}

size_t SkipSpaces(const std::string& text, size_t position)
{
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0)
    {
        ++position;
    }
    return position;
}

std::wstring ExtractString(const std::string& object, const char* key)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return {};
    }
    if (object.compare(position, 4, "null") == 0)
    {
        return {};
    }
    if (object[position] != '"')
    {
        return {};
    }

    ++position;
    std::string value;
    bool escaped = false;
    for (; position < object.size(); ++position)
    {
        const char ch = object[position];
        if (escaped)
        {
            value.push_back('\\');
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
        {
            break;
        }
        value.push_back(ch);
    }
    return UnescapeJsonString(value);
}

long long ExtractInt64(const std::string& object, const char* key, long long fallback = 0)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    return std::strtoll(object.c_str() + position, nullptr, 10);
}

int ExtractInt(const std::string& object, const char* key, int fallback = 0)
{
    return static_cast<int>(ExtractInt64(object, key, fallback));
}

double ExtractDouble(const std::string& object, const char* key, double fallback = 0.0)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(object.c_str() + position, &end);
    return end == object.c_str() + position ? fallback : value;
}

bool ExtractBool(const std::string& object, const char* key, bool fallback = false)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    if (object.compare(position, 4, "true") == 0)
    {
        return true;
    }
    if (object.compare(position, 5, "false") == 0)
    {
        return false;
    }
    return fallback;
}

std::string ExtractObject(const std::string& object, const char* key)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size() || object[position] != '{')
    {
        return {};
    }

    const size_t objectStart = position;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; position < object.size(); ++position)
    {
        const char ch = object[position];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == '{')
        {
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0)
            {
                return object.substr(objectStart, position - objectStart + 1);
            }
        }
    }

    return {};
}

std::vector<std::string> ExtractObjectsInArray(const std::string& json, const char* key)
{
    std::vector<std::string> objects;
    size_t position = SkipSpaces(json, FindField(json, key));
    if (position == std::string::npos || position >= json.size() || json[position] != '[')
    {
        return objects;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;
    for (++position; position < json.size(); ++position)
    {
        const char ch = json[position];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == '{')
        {
            if (depth == 0)
            {
                objectStart = position;
            }
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                objects.push_back(json.substr(objectStart, position - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
        else if (ch == ']' && depth == 0)
        {
            break;
        }
    }
    return objects;
}

std::wstring EventTypeName(MacroEventType type)
{
    switch (type)
    {
    case MacroEventType::KeyDown:
        return L"keyDown";
    case MacroEventType::KeyUp:
        return L"keyUp";
    case MacroEventType::MouseDown:
        return L"mouseDown";
    case MacroEventType::MouseUp:
        return L"mouseUp";
    case MacroEventType::MouseMove:
        return L"mouseMove";
    case MacroEventType::MouseWheel:
        return L"mouseWheel";
    }
    return L"unknown";
}

MacroEventType EventTypeFromName(const std::wstring& name)
{
    if (name == L"keyUp") return MacroEventType::KeyUp;
    if (name == L"mouseDown") return MacroEventType::MouseDown;
    if (name == L"mouseUp") return MacroEventType::MouseUp;
    if (name == L"mouseMove") return MacroEventType::MouseMove;
    if (name == L"mouseWheel") return MacroEventType::MouseWheel;
    return MacroEventType::KeyDown;
}

std::wstring MouseButtonName(MacroMouseButton button)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return L"left";
    case MacroMouseButton::Right:
        return L"right";
    case MacroMouseButton::Middle:
        return L"middle";
    case MacroMouseButton::X1:
        return L"x1";
    case MacroMouseButton::X2:
        return L"x2";
    case MacroMouseButton::None:
        break;
    }
    return L"none";
}

MacroMouseButton MouseButtonFromName(const std::wstring& name)
{
    if (name == L"left") return MacroMouseButton::Left;
    if (name == L"right") return MacroMouseButton::Right;
    if (name == L"middle") return MacroMouseButton::Middle;
    if (name == L"x1") return MacroMouseButton::X1;
    if (name == L"x2") return MacroMouseButton::X2;
    return MacroMouseButton::None;
}

std::wstring MouseModeName(MacroMouseMode mode)
{
    switch (mode)
    {
    case MacroMouseMode::Absolute:
        return L"Absolute";
    case MacroMouseMode::WindowRelative:
        return L"WindowRelative";
    case MacroMouseMode::Relative:
        break;
    }
    return L"Relative";
}

MacroMouseMode MouseModeFromName(const std::wstring& name)
{
    if (name == L"Absolute") return MacroMouseMode::Absolute;
    if (name == L"WindowRelative") return MacroMouseMode::WindowRelative;
    return MacroMouseMode::Relative;
}

DWORD MouseDownFlag(MacroMouseButton button)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return MOUSEEVENTF_LEFTDOWN;
    case MacroMouseButton::Right:
        return MOUSEEVENTF_RIGHTDOWN;
    case MacroMouseButton::Middle:
        return MOUSEEVENTF_MIDDLEDOWN;
    case MacroMouseButton::X1:
    case MacroMouseButton::X2:
        return MOUSEEVENTF_XDOWN;
    case MacroMouseButton::None:
        break;
    }
    return 0;
}

DWORD MouseUpFlag(MacroMouseButton button)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return MOUSEEVENTF_LEFTUP;
    case MacroMouseButton::Right:
        return MOUSEEVENTF_RIGHTUP;
    case MacroMouseButton::Middle:
        return MOUSEEVENTF_MIDDLEUP;
    case MacroMouseButton::X1:
    case MacroMouseButton::X2:
        return MOUSEEVENTF_XUP;
    case MacroMouseButton::None:
        break;
    }
    return 0;
}

DWORD XButtonData(MacroMouseButton button)
{
    if (button == MacroMouseButton::X1)
    {
        return XBUTTON1;
    }
    if (button == MacroMouseButton::X2)
    {
        return XBUTTON2;
    }
    return 0;
}

MacroMouseButton MouseButtonFromMessage(WPARAM message, const MSLLHOOKSTRUCT& mouse)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return MacroMouseButton::Left;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return MacroMouseButton::Right;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return MacroMouseButton::Middle;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return HIWORD(mouse.mouseData) == XBUTTON2 ? MacroMouseButton::X2 : MacroMouseButton::XÛMzæÚ$z{-®éÜj×‚ÒVæF–ætÖ÷W6TG…ó°¢WfVçBæG’ÒVæF–ætÖ÷W6TG•ó°¢VæF–ætÖ÷W6TG…òÒ°¢VæF–ætÖ÷W6TG•òÒ°¢VæF–ætÖ÷W6U7F'EW5òÒ°¢Æ7DÖ÷W6TÖ÷fUW5òÒF–ÖUW3°¢FDWfVçB†WfVçB“°§Ð ¤Ö7&õÆ–&6µ6W'f–6S£¤Ö7&õÆ–&6µ6W'f–6R‚’ÒFVfVÇC° ¤Ö7&õÆ–&6µ6W'f–6S£§äÖ7&õÆ–&6µ6W'f–6R‚§°¢7F÷‚“°§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¥7F'B†6öç7BÖ7&ôFVf–æ—F–öâbÖ7&òÂ6öç7BÖ7&õÆ–&6´÷F–öç2b÷F–öç2Â7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢–b†Ö7&òæWfVçG2æV×G’‚’¢°¢W'&÷$ÖW76vRÒÂ%F†—2Ö7&ò†2æòWfVçG2FòÆ’â#°¢&WGW&âfÇ6S°¢Ð¢–b„—5Æ––ær‚’¢°¢W'&÷$ÖW76vRÒÂ%Æ–&6²—2Ç&VG’'Vææ–ærâ#°¢&WGW&âfÇ6S°¢Ð¢–b†÷F–öç2ç&WV—&UF&vWDfö7W6VBbbÖ7&õF&vWEv–æF÷u6W'f–6S£¤—5F&vWDfö7W6VB†Ö7&ò’¢°¢W'&÷$ÖW76vRÒÂ%F&vWBv–æF÷r—2æ÷Bfö7W6VBâ#°¢&WGW&âfÇ6S°¢Ð ¢7F÷‚“°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢6æ6†÷EòÒ·Ó°¢6æ6†÷Eòç7FGW2Ò÷F–öç2ç7F'DFVÆ•6V6öæG2âòÖ7&õÆ–&6µ7FGW3£¥7F'F–ær¢Ö7&õÆ–&6µ7FGW3£¥Æ––æs°¢6æ6†÷EòæÖW76vRÒ÷F–öç2ç7F'DFVÆ•6V6öæG2â ¢òÂ%Æ–&6²6÷VçFF÷vâ7F'FVBâfö7W2F†RF&vWBv–æF÷ræ÷râ ¢¢Â%Æ––ærÖ7&òâ#°¢6æ6†÷Eòç&WVW7FVDÆö÷2Ò÷F–öç2æÆö÷VçF–Å7F÷VBò¢7FC£¦Ö‚ƒÂ÷F–öç2æÆö÷6÷VçB“°¢6æ6†÷EòçF÷FÅW2ÒÖ7&òæWfVçG2æV×G’‚’ò¢Ö7&òæWfVçG2æ&6²‚’çF–ÖUW3°¢Ð ¢7F÷&WVW7FVEòç7F÷&R†fÇ6R“°¢Æ–&6µF‡&VEòÒ7FC£§F‡&VB‚dÖ7&õÆ–&6µ6W'f–6S£¥Æ–&6´Æö÷ÂF†—2ÂÖ7&òÂ÷F–öç2“°¢&WGW&âG'VS°§Ð §fö–BÖ7&õÆ–&6µ6W'f–6S£¥7F÷‚§°¢7F÷&WVW7FVEòç7F÷&R‡G'VR“°¢–b‡Æ–&6µF‡&VEòæ¦ö–æ&ÆR‚’¢°¢Æ–&6µF‡&VEòæ¦ö–â‚“°¢Ð§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¤—5Æ––ær‚’6öç7@§°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢&WGW&â6æ6†÷Eòç7FGW2ÓÒÖ7&õÆ–&6µ7FGW3£¥7F'F–ærÇÂ6æ6†÷Eòç7FGW2ÓÒÖ7&õÆ–&6µ7FGW3£¥Æ––æs°§Ð ¤Ö7&õÆ–&6µ6æ6†÷BÖ7&õÆ–&6µ6W'f–6S£¥6æ6†÷B‚’6öç7@§°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢&WGW&â6æ6†÷Eó°§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¤—4–çFW&6WF–öäf–Æ&ÆR‡7FC£§w7G&–ærbW'&÷$ÖW76vR’6öç7@§°¢–çFW&6WF–öåÆ–&6´&6¶VæB&6¶VæC°¢&WGW&â&6¶VæBä–æ—F–Æ—¦R†W'&÷$ÖW76vR“°§Ð §fö–BÖ7&õÆ–&6µ6W'f–6S£¥Æ–&6´Æö÷„Ö7&ôFVf–æ—F–öâÖ7&òÂÖ7&õÆ–&6´÷F–öç2÷F–öç2§°¢÷F–öç2ç7VVBÒ7FC£¦6Æ×†÷F–öç2ç7VVBÂãÂã“°¢÷F–öç2æÆö÷6÷VçBÒ7FC£¦6Æ×†÷F–öç2æÆö÷6÷VçBÂÂ““’“°¢÷F–öç2ç7F'DFVÆ•6V6öæG2Ò7FC£¦6Æ×†÷F–öç2ç7F'DFVÆ•6V6öæG2ÂÂc“° ¢6öç7B&ööÂF–ÖW%&W6öÇWF–öå&—6VBÒF–ÖT&Vv–åW&–öBƒ’ÓÒD”ÔU%%ôäôU%$õ#°¢6öç7B„äDÄRÆ–&6µF‡&VBÒvWD7W'&VçEF‡&VB‚“°¢6öç7B–çB&Wf–÷W5F‡&VE&–÷&—G’ÒvWEF‡&VE&–÷&—G’‡Æ–&6µF‡&VB“°¢–b‡&Wf–÷W5F‡&VE&–÷&—G’ÒD…$TEõ$”õ$•E•ôU%$õ%õ$UEU$â¢°¢6WEF‡&VE&–÷&—G’‡Æ–&6µF‡&VBÂD…$TEõ$”õ$•E•ô„”t„U5B“°¢Ð¢WFò&W7F÷&UÆ–&6µF–Ö–ærÒ²eÒ‚¢°¢–b‡&Wf–÷W5F‡&VE&–÷&—G’ÒD…$TEõ$”õ$•E•ôU%$õ%õ$UEU$â¢°¢6WEF‡&VE&–÷&—G’‡Æ–&6µF‡&VBÂ&Wf–÷W5F‡&VE&–÷&—G’“°¢Ð¢–b‡F–ÖW%&W6öÇWF–öå&—6VB¢°¢F–ÖTVæEW&–öBƒ“°¢Ð¢Ó° ¢–b†÷F–öç2ç7F'DFVÆ•6V6öæG2â¢°¢–b…v—D–çFW''WF–&ÆR‡7FC£¦6‡&öæó£§6V6öæG2†÷F–öç2ç7F'DFVÆ•6V6öæG2’’¢°¢&W7F÷&UÆ–&6µF–Ö–ær‚“°¢&WGW&ã°¢Ð¢Ð ¢6öç7B–çBÖ„Æö÷2Ò÷F–öç2æÆö÷VçF–Å7F÷VBò7FC£¦çVÖW&–5öÆ–Ö—G3Æ–çCã£¦Ö‚‚’¢÷F–öç2æÆö÷6÷VçC°¢–çFW&6WF–öåÆ–&6´&6¶VæB–çFW&6WF–öä&6¶VæC°¢–çFW&6WF–öåÆ–&6´&6¶VæB¢G&—fW$&6¶VæBÒçVÆÇG#°¢–b†÷F–öç2æ&6¶VæBÓÒÖ7&õÆ–&6´&6¶VæC£¤–çFW&6WF–öâ¢°¢7FC£§w7G&–ær&6¶VæDW'&÷#°¢–b‚–çFW&6WF–öä&6¶VæBä–æ—F–Æ—¦R†&6¶VæDW'&÷"’¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢6æ6†÷Eòç7FGW2ÒÖ7&õÆ–&6µ7FGW3£¤f–ÆVC°¢6æ6†÷EòæÖW76vRÒ&6¶VæDW'&÷"æV×G’‚¢òÂ$–çFW&6WF–öâÆ–&6²—2Væf–Æ&ÆRâ ¢¢&6¶VæDW'&÷#°¢7F÷&WVW7FVEòç7F÷&R‡G'VR“°¢&W7F÷&UÆ–&6µF–Ö–ær‚“°¢&WGW&ã°¢Ð¢G&—fW$&6¶VæBÒf–çFW&6WF–öä&6¶VæC°¢Ð ¢7FC£¦ÖÄEtõ$BÂÖ7&ôWfVçCâ†VÆD¶W—3°¢7FC£§fV7F÷#ÄÖ7&ôÖ÷W6T'WGFöãâ†VÆDÖ÷W6T'WGFöç3°¢WFò&VÖVÖ&W$†VÆD–çWBÒ²eÒ†6öç7BÖ7&ôWfVçBbWfVçB¢°¢–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤¶W”F÷vâ¢°¢†VÆD¶W—5¶WfVçBçf—'GVÄ¶W•ÒÒWfVçC°¢Ð¢VÇ6R–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤¶W•W¢°¢†VÆD¶W—2æW&6R†WfVçBçf—'GVÄ¶W’“°¢Ð¢VÇ6R–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6TF÷vâbbWfVçBæÖ÷W6T'WGFöâÒÖ7&ôÖ÷W6T'WGFöã£¤æöæR¢°¢–b‡7FC£¦f–æB††VÆDÖ÷W6T'WGFöç2æ&Vv–â‚’Â†VÆDÖ÷W6T'WGFöç2æVæB‚’ÂWfVçBæÖ÷W6T'WGFöâ’ÓÒ†VÆDÖ÷W6T'WGFöç2æVæB‚’¢°¢†VÆDÖ÷W6T'WGFöç2çW6…ö&6²†WfVçBæÖ÷W6T'WGFöâ“°¢Ð¢Ð¢VÇ6R–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6UW¢°¢†VÆDÖ÷W6T'WGFöç2æW&6R€¢7FC£§&VÖ÷fR††VÆDÖ÷W6T'WGFöç2æ&Vv–â‚’Â†VÆDÖ÷W6T'WGFöç2æVæB‚’ÂWfVçBæÖ÷W6T'WGFöâ’À¢†VÆDÖ÷W6T'WGFöç2æVæB‚’“°¢Ð¢Ó° ¢f÷"†–çBÆö÷Ò²Æö÷ÃÒÖ„Æö÷2bb7F÷&WVW7FVEòæÆöB‚“²²¶Æö÷¢°¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢6æ6†÷Eòç7FGW2ÒÖ7&õÆ–&6µ7FGW3£¥Æ––æs°¢6æ6†÷EòæÖW76vRÒ÷F–öç2æÆö÷VçF–Å7F÷VBòÂ%Æ––ærÖ7&òVçF–Â7F÷VBâ"¢Â%Æ––ærÖ7&òâ#°¢6æ6†÷Eòæ7W'&VçDÆö÷ÒÆö÷°¢Ð ¢6öç7BWFòÆö÷7F'BÒ7FC£¦6‡&öæó£§7FVG•ö6Æö6³£¦æ÷r‚“°¢f÷"‡6—¦U÷BWfVçD–æFW‚Ò²WfVçD–æFW‚ÂÖ7&òæWfVçG2ç6—¦R‚“²¢°¢6öç7BÖ7&ôWfVçBbWfVçBÒÖ7&òæWfVçG5¶WfVçD–æFW…Ó°¢6öç7BWFòF&vWDöfg6WBÒ7FC£¦6‡&öæó£¦GW&F–öåö67CÇ7FC£¦6‡&öæó£§7FVG•ö6Æö6³£¦GW&F–öãâ€¢7FC£¦6‡&öæó£¦GW&F–öãÆF÷V&ÆSâ‡7FF–5ö67CÆF÷V&ÆSâ‡7FC£¦ÖƒÆÆöærÆöæsâƒÂWfVçBçF–ÖUW2’’òãò÷F–öç2ç7VVB’“°¢–b…v—EVçF–Ä–çFW''WF–&ÆR†Æö÷7F'B²F&vWDöfg6WB’¢°¢'&V³°¢Ð¢–b‡7F÷&WVW7FVEòæÆöB‚’¢°¢'&V³°¢Ð¢–b†÷F–öç2ç&WV—&UF&vWDfö7W6VBbbÖ7&õF&vWEv–æF÷u6W'f–6S£¤—5F&vWDfö7W6VB†Ö7&ò’¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢6æ6†÷Eòç7FGW2ÒÖ7&õÆ–&6µ7FGW3£¤f–ÆVC°¢6æ6†÷EòæÖW76vRÒÂ%F&vWBv–æF÷rÆ÷7Bfö7W2âÆ–&6²7F÷VBâ#°¢7F÷&WVW7FVEòç7F÷&R‡G'VR“°¢'&V³°¢Ð ¢6öç7B&ööÂ6VçBÒ6VæDÖ7&ôWfVçB†WfVçBÂÖ7&òç&V6÷&F–ætÖöFRæÖ÷W6TÖöFRÂG&—fW$&6¶VæB“°¢–b‡6VçB¢°¢&VÖVÖ&W$†VÆD–çWB†WfVçB“°¢²¶WfVçD–æFWƒ°¢Ð ¢–b‚6VçB¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢6æ6†÷Eòç7FGW2ÒÖ7&õÆ–&6µ7FGW3£¤f–ÆVC°¢6æ6†÷EòæÖW76vRÒÂ%v–æF÷w2F–Bæ÷B66WBöæRöbF†R6–×VÆFVB–çWG2â#°¢7F÷&WVW7FVEòç7F÷&R‡G'VR“°¢'&V³°¢Ð ¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢6æ6†÷EòæVÆ6VEW2Ò7FC£¦6‡&öæó£¦GW&F–öåö67CÇ7FC£¦6‡&öæó£¦Ö–7&÷6V6öæG3â€¢7FC£¦6‡&öæó£§7FVG•ö6Æö6³£¦æ÷r‚’ÒÆö÷7F'B’æ6÷VçB‚“°¢Ð¢Ð¢Ð ¢&VÆV6T†VÆD–çWG2††VÆD¶W—2Â†VÆDÖ÷W6T'WGFöç2“° ¢°¢7FC£¦Æö6µöwV&CÇ7FC£¦×WFWƒâÆö6²†×WFW…ò“°¢–b‡6æ6†÷Eòç7FGW2ÒÖ7&õÆ–&6µ7FGW3£¤f–ÆVB¢°¢6æ6†÷Eòç7FGW2Ò7F÷&WVW7FVEòæÆöB‚’òÖ7&õÆ–&6µ7FGW3£¥7F÷VB¢Ö7&õÆ–&6µ7FGW3£¤6ö×ÆWFS°¢6æ6†÷EòæÖW76vRÒ7F÷&WVW7FVEòæÆöB‚’òÂ%Æ–&6²7F÷VBâ"¢Â%Æ–&6²6ö×ÆWFRâ#°¢Ð¢Ð ¢7F÷&WVW7FVEòç7F÷&R‡G'VR“°¢&W7F÷&UÆ–&6µF–Ö–ær‚“°§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¤'V–ÆDÖ7&ô–çWB†6öç7BÖ7&ôWfVçBbWfVçBÂÖ7&ôÖ÷W6TÖöFRÖöFRÂ”åUBb–çWB§°¢–çWBÒ·Ó°¢–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤¶W”F÷vâÇÂWfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤¶W•W¢°¢–çWBçG—RÒ”åUEô´U”$ô$C°¢–b†WfVçBç66ä6öFRÒ¢°¢–çWBæ¶’çu66âÒ7FF–5ö67CÅtõ$Câ†WfVçBç66ä6öFR“°¢–çWBæ¶’æGtfÆw2Ò´U”UdTåDeõ44ä4ôDS°¢Ð¢VÇ6P¢°¢–çWBæ¶’çuf²Ò7FF–5ö67CÅtõ$Câ†WfVçBçf—'GVÄ¶W’“°¢Ð¢–b‚†WfVçBæ¶W”fÆw2bÄÄ´„eôU…DTäDTB’Ò¢°¢–çWBæ¶’æGtfÆw2ÃÒ´U”UdTåDeôU…DTäDTD´U“°¢Ð¢–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤¶W•W¢°¢–çWBæ¶’æGtfÆw2ÃÒ´U”UdTåDeô´U•U°¢Ð¢–çWBæ¶’æGtW‡G&–æfòÒ´Ö7&õÆ–&6´W‡G&–æfó°¢&WGW&âG'VS°¢Ð ¢–çWBçG—RÒ”åUEôÔõU4S°¢–çWBæÖ’æGtW‡G&–æfòÒ´Ö7&õÆ–&6´W‡G&–æfó°¢–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6TÖ÷fR¢°¢–b†ÖöFRÓÒÖ7&ôÖ÷W6TÖöFS£¥&VÆF—fRbbWfVçBæ'6öÇWFTÖ÷fR¢°¢–çWBæÖ’æGtfÆw2ÒÔõU4TUdTåDeôÔõdS°¢–çWBæÖ’æG‚ÒWfVçBæGƒ°¢–çWBæÖ’æG’ÒWfVçBæG“°¢Ð¢VÇ6P¢°¢Äôär‚ÒWfVçBçƒ°¢Äôär’ÒWfVçBç“°¢–b‚WfVçBæ'6öÇWFTÖ÷fRbbÖöFRÓÒÖ7&ôÖ÷W6TÖöFS£¥v–æF÷u&VÆF—fR¢°¢…täBf÷&Vw&÷VæBÒvWDf÷&Vw&÷VæEv–æF÷r‚“°¢$T5B&V7B·Ó°¢–b†f÷&Vw&÷VæBbbvWEv–æF÷u&V7B†f÷&Vw&÷VæBÂg&V7B’¢°¢‚³Ò&V7BæÆVgC°¢’³Ò&V7BçF÷°¢Ð¢Ð ¢6öç7B–çB÷&–v–å‚ÒvWE7—7FVÔÖWG&–72…4Õõ…d•%ETÅ45$TTâ“°¢6öç7B–çB÷&–v–å’ÒvWE7—7FVÔÖWG&–72…4Õõ•d•%ETÅ45$TTâ“°¢6öç7B–çBv–GF‚ÒvWE7—7FVÔÖWG&–72…4Õô5…d•%ETÅ45$TTâ“°¢6öç7B–çB†V–v‡BÒvWE7—7FVÔÖWG&–72…4Õô5•d•%ETÅ45$TTâ“°¢–çWBæÖ’æGtfÆw2ÒÔõU4TUdTåDeôÔõdRÂÔõU4TUdTåDeô%4ôÅUDRÂÔõU4TUdTåDeõd•%ETÄDU4³°¢–çWBæÖ’æG‚Òæ÷&ÖÆ—¦T'6öÇWFT6ö÷&F–æFR‡‚Â÷&–v–å‚Âv–GF‚“°¢–çWBæÖ’æG’Òæ÷&ÖÆ—¦T'6öÇWFT6ö÷&F–æFR‡’Â÷&–v–å’Â†V–v‡B“°¢Ð¢Ð¢VÇ6R–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6TF÷vâÇÂWfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6UW¢°¢–çWBæÖ’æGtfÆw2ÒWfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6TF÷và¢òÖ÷W6TF÷väfÆr†WfVçBæÖ÷W6T'WGFöâ¢¢Ö÷W6UWfÆr†WfVçBæÖ÷W6T'WGFöâ“°¢–çWBæÖ’æÖ÷W6TFFÒ„'WGFöäFF†WfVçBæÖ÷W6T'WGFöâ“°¢Ð¢VÇ6R–b†WfVçBçG—RÓÒÖ7&ôWfVçEG—S£¤Ö÷W6Uv†VVÂ¢°¢–çWBæÖ’æGtfÆw2ÒÔõU4TUdTåDeõt„TTÃ°¢–çWBæÖ’æÖ÷W6TFFÒ7FF–5ö67CÄEtõ$Câ†WfVçBçv†VVÄFVÇF“°¢Ð ¢&WGW&â–çWBæÖ’æGtfÆw2Ò°§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¥6VæDÖ7&ôWfVçB†6öç7BÖ7&ôWfVçBbWfVçBÂÖ7&ôÖ÷W6TÖöFRÖöFRÂfö–B¢G&—fW$&6¶VæB§°¢–b†G&—fW$&6¶VæB¢°¢&WGW&â7FF–5ö67CÄ–çFW&6WF–öåÆ–&6´&6¶VæB£â†G&—fW$&6¶VæB’Óå6VæDWfVçB†WfVçBÂÖöFR“°¢Ð ¢”åUB–çWB·Ó°¢&WGW&â'V–ÆDÖ7&ô–çWB†WfVçBÂÖöFRÂ–çWB’bb6VæD–çWBƒÂf–çWBÂ6—¦Vöb„”åUB’’ÓÒ°§Ð §fö–BÖ7&õÆ–&6µ6W'f–6S£¥&VÆV6T†VÆD–çWG2€¢6öç7B7FC£¦ÖÄEtõ$BÂÖ7&ôWfVçCâb†VÆD¶W—2À¢6öç7B7FC£§fV7F÷#ÄÖ7&ôÖ÷W6T'WGFöãâb†VÆDÖ÷W6T'WGFöç2§°¢7FC£§fV7F÷#Ä”åUCâ–çWG3°¢–çWG2ç&W6W'fR††VÆD¶W—2ç6—¦R‚’²†VÆDÖ÷W6T'WGFöç2ç6—¦R‚’“° ¢f÷"†WFò—BÒ†VÆD¶W—2ç&&Vv–â‚“²—BÒ†VÆD¶W—2ç&VæB‚“²²¶—B¢°¢6öç7BÖ7&ôWfVçBbWfVçBÒ—BÓç6V6öæC°¢”åUB–çWB·Ó°¢–çWBçG—RÒ”åUEô´U”$ô$C°¢–b†WfVçBç66ä6öFRÒ¢°¢–çWBæ¶’çu66âÒ7FF–5ö67CÅtõ$Câ†WfVçBç66ä6öFR“°¢–çWBæ¶’æGtfÆw2Ò´U”UdTåDeõ44ä4ôDS°¢Ð¢VÇ6P¢°¢–çWBæ¶’çuf²Ò7FF–5ö67CÅtõ$Câ†WfVçBçf—'GVÄ¶W’“°¢Ð¢–b‚†WfVçBæ¶W”fÆw2bÄÄ´„eôU…DTäDTB’Ò¢°¢–çWBæ¶’æGtfÆw2ÃÒ´U”UdTåDeôU…DTäDTD´U“°¢Ð¢–çWBæ¶’æGtfÆw2ÃÒ´U”UdTåDeô´U•U°¢–çWBæ¶’æGtW‡G&–æfòÒ´Ö7&õÆ–&6´W‡G&–æfó°¢–çWG2çW6…ö&6²†–çWB“°¢Ð ¢f÷"†WFò—BÒ†VÆDÖ÷W6T'WGFöç2ç&&Vv–â‚“²—BÒ†VÆDÖ÷W6T'WGFöç2ç&VæB‚“²²¶—B¢°¢6öç7BEtõ$BfÆrÒÖ÷W6UWfÆr‚¦—B“°¢–b†fÆrÓÒ¢°¢6öçF–çVS°¢Ð ¢”åUB–çWB·Ó°¢–çWBçG—RÒ”åUEôÔõU4S°¢–çWBæÖ’æGtfÆw2ÒfÆs°¢–çWBæÖ’æÖ÷W6TFFÒ„'WGFöäFF‚¦—B“°¢–çWBæÖ’æGtW‡G&–æfòÒ´Ö7&õÆ–&6´W‡G&–æfó°¢–çWG2çW6…ö&6²†–çWB“°¢Ð ¢–b‚–çWG2æV×G’‚’¢°¢6VæD–çWB‡7FF–5ö67CÅT”åCâ†–çWG2ç6—¦R‚’’Â–çWG2æFF‚’Â6—¦Vöb„”åUB’“°¢Ð§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¥v—D–çFW''WF–&ÆR‡7FC£¦6‡&öæó£§7FVG•ö6Æö6³£¦GW&F–öâGW&F–öâ§°¢6öç7BWFòVæBÒ7FC£¦6‡&öæó£§7FVG•ö6Æö6³£¦æ÷r‚’²GW&F–öã°¢&WGW&âv—EVçF–Ä–çFW''WF–&ÆR†VæB“°§Ð ¦&ööÂÖ7&õÆ–&6µ6W'f–6S£¥v—EVçF–Ä–çFW''WF–&ÆR‡7FC£¦6‡&öæó£§7FVG•ö6Æö6³£§F–ÖU÷ö–çBF&vWEF–ÖR§°¢v†–ÆR‚7F÷&WVW7FVEòæÆöB‚’¢°¢6öç7BWFòæ÷rÒ7FC£¦6‡&öæó£§7FVG•ö6Æö6³£¦æ÷r‚“°¢–b†æ÷rãÒF&vWEF–ÖR¢°¢&WGW&âfÇ6S°¢Ð¢6öç7BWFò&VÖ–æ–ærÒF&vWEF–ÖRÒæ÷s°¢–b‡&VÖ–æ–ærâ7FC£¦6‡&öæó£¦Ö–ÆÆ—6V6öæG2ƒ2’¢°¢7FC£§F†—5÷F‡&VC£§6ÆVWöf÷"‡7FC£¦6‡&öæó£¦Ö–ÆÆ—6V6öæG2ƒ’“°¢Ð¢VÇ6R–b‡&VÖ–æ–ærâ7FC£¦6‡&öæó£¦Ö–7&÷6V6öæG2ƒ“’¢°¢7v—F6…FõF‡&VB‚“°¢Ð¢VÇ6P¢°¢––VÆE&ö6W76÷"‚“°¢Ð¢Ð¢&WGW&âG'VS°§Ð ¤Ö7&ô†÷F¶W•6W'f–6S£§äÖ7&ô†÷F¶W•6W'f–6R‚’ÒFVfVÇC° ¦&ööÂÖ7&ô†÷F¶W•6W'f–6S£¥&Vv—7FW"„…täB‡væBÂ–çB–BÂ6öç7BÖ7&ô†÷F¶W’b†÷F¶W’Â7FC£§w7G&–ærbW'&÷$ÖW76vR§°¢–b‚‡væBÇÂ—4V×G’††÷F¶W’’¢°¢&WGW&âG'VS°¢Ð ¢Vç&Vv—7FW"†‡væBÂ–B“°¢–b‚&Vv—7FW$†÷D¶W’†‡væBÂ–BÂ†÷F¶W’æÖöF–f–W'2ÂÔôEôäõ$UTBÂ†÷F¶W’çf—'GVÄ¶W’’¢°¢W'&÷$ÖW76vRÒÂ$6÷VÆBæ÷B&Vv—7FW""²†÷F¶W”Æ&VÂ††÷F¶W’’²Â"âæ÷F†W"Ö’Ç&VG’&RW6–ær—Bâ#°¢&WGW&âfÇ6S°¢Ð ¢–b‡7FC£¦f–æB‡&Vv—7FW&VD–G5òæ&Vv–â‚’Â&Vv—7FW&VD–G5òæVæB‚’Â–B’ÓÒ&Vv—7FW&VD–G5òæVæB‚’¢°¢&Vv—7FW&VD–G5òçW6…ö&6²†–B“°¢Ð¢&WGW&âG'VS°§Ð §fö–BÖ7&ô†÷F¶W•6W'f–6S£¥Vç&Vv—7FW"„…täB‡væBÂ–çB–B§°¢–b†‡væB¢°¢Vç&Vv—7FW$†÷D¶W’†‡væBÂ–B“°¢Ð¢&Vv—7FW&VD–G5òæW&6R‡7FC£§&VÖ÷fR‡&Vv—7FW&VD–G5òæ&Vv–â‚’Â&Vv—7FW&VD–G5òæVæB‚’Â–B’Â&Vv—7FW&VD–G5òæVæB‚’“°§Ð §fö–BÖ7&ô†÷F¶W•6W'f–6S£¥Vç&Vv—7FW$ÆÂ„…täB‡væB§°¢f÷"†–çB–B¢&Vv—7FW&VD–G5ò¢°¢–b†‡væB¢°¢Vç&Vv—7FW$†÷D¶W’†‡væBÂ–B“°¢Ð¢Ð¢&Vv—7FW&VD–G5òæ6ÆV"‚“°§Ð §7FC£§w7G&–ærÖ7&ô†÷F¶W•6W'f–6S£¤†÷F¶W”Æ&VÂ†6öç7BÖ7&ô†÷F¶W’b†÷F¶W’§°¢–b„—4V×G’††÷F¶W’’¢°¢&WGW&âÂ%Væ&÷VæB#°¢Ð ¢7FC£§w7G&–ærÆ&VÃ°¢–b††÷F¶W’æÖöF–f–W'2bÔôEô4ôåE$ôÂ’Æ&VÂ³ÒÂ$7G&Â²#°¢–b††÷F¶W’æÖöF–f–W'2bÔôEõ4„”eB’Æ&VÂ³ÒÂ%6†–gB²#°¢–b††÷F¶W’æÖöF–f–W'2bÔôEôÅB’Æ&VÂ³ÒÂ$ÇB²#°¢Æ&VÂ³Ò¶W”æÖR††÷F¶W’çf—'GVÄ¶W’“°¢&WGW&âÆ&VÃ°§Ð ¦&ööÂÖ7&ô†÷F¶W•6W'f–6S£¤—4V×G’†6öç7BÖ7&ô†÷F¶W’b†÷F¶W’§°¢&WGW&â†÷F¶W’çf—'GVÄ¶W’ÓÒ°§Ð