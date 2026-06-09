#include "hotkey.h"

#include "util.h"

#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ncw
{
namespace
{
std::vector<std::string> SplitHotkeyTokens(const std::string &value)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : value)
    {
        if (ch == '+')
        {
            std::string token = Trim(current);
            if (!token.empty())
            {
                tokens.push_back(token);
            }
            current.clear();
            continue;
        }
        if (ch != '<' && ch != '>')
        {
            current.push_back(ch);
        }
    }

    std::string token = Trim(current);
    if (!token.empty())
    {
        tokens.push_back(token);
    }
    return tokens;
}

std::optional<std::pair<UINT, std::string>> ParseHotkeyKey(const std::string &token)
{
    const std::string key = ToLowerAscii(token);
    if (key.size() == 1)
    {
        const unsigned char ch = static_cast<unsigned char>(key[0]);
        if (std::isalpha(ch))
        {
            return std::make_pair(static_cast<UINT>(std::toupper(ch)),
                                  std::string(1, static_cast<char>(std::toupper(ch))));
        }
        if (std::isdigit(ch))
        {
            return std::make_pair(static_cast<UINT>(ch), std::string(1, static_cast<char>(ch)));
        }
    }

    if (key.size() >= 2 && key[0] == 'f')
    {
        const int index = ParseIntOrDefault(key.substr(1), 0);
        if (index >= 1 && index <= 24)
        {
            return std::make_pair(static_cast<UINT>(VK_F1 + index - 1), "F" + std::to_string(index));
        }
    }

    const std::map<std::string, std::pair<UINT, std::string>> names = {
        {"backspace", {VK_BACK, "Backspace"}},
        {"del", {VK_DELETE, "Delete"}},
        {"delete", {VK_DELETE, "Delete"}},
        {"down", {VK_DOWN, "Down"}},
        {"end", {VK_END, "End"}},
        {"enter", {VK_RETURN, "Enter"}},
        {"esc", {VK_ESCAPE, "Esc"}},
        {"escape", {VK_ESCAPE, "Esc"}},
        {"home", {VK_HOME, "Home"}},
        {"ins", {VK_INSERT, "Insert"}},
        {"insert", {VK_INSERT, "Insert"}},
        {"left", {VK_LEFT, "Left"}},
        {"pagedown", {VK_NEXT, "PageDown"}},
        {"pageup", {VK_PRIOR, "PageUp"}},
        {"pause", {VK_PAUSE, "Pause"}},
        {"pgdn", {VK_NEXT, "PageDown"}},
        {"pgup", {VK_PRIOR, "PageUp"}},
        {"printscreen", {VK_SNAPSHOT, "PrintScreen"}},
        {"prtsc", {VK_SNAPSHOT, "PrintScreen"}},
        {"return", {VK_RETURN, "Enter"}},
        {"right", {VK_RIGHT, "Right"}},
        {"space", {VK_SPACE, "Space"}},
        {"tab", {VK_TAB, "Tab"}},
        {"up", {VK_UP, "Up"}},
    };
    const auto it = names.find(key);
    if (it != names.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> HotkeyKeyLabelFromVk(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= '0' && vk <= '9')
    {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        return "F" + std::to_string(static_cast<int>(vk - VK_F1 + 1));
    }

    const std::map<UINT, std::string> names = {
        {VK_BACK, "Backspace"},
        {VK_DELETE, "Delete"},
        {VK_DOWN, "Down"},
        {VK_END, "End"},
        {VK_RETURN, "Enter"},
        {VK_HOME, "Home"},
        {VK_INSERT, "Insert"},
        {VK_LEFT, "Left"},
        {VK_NEXT, "PageDown"},
        {VK_PRIOR, "PageUp"},
        {VK_PAUSE, "Pause"},
        {VK_SNAPSHOT, "PrintScreen"},
        {VK_RIGHT, "Right"},
        {VK_SPACE, "Space"},
        {VK_TAB, "Tab"},
        {VK_UP, "Up"},
    };
    const auto it = names.find(vk);
    if (it != names.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::string FormatHotkeyDisplay(UINT modifiers, const std::string &key_label)
{
    std::vector<std::string> parts;
    if ((modifiers & MOD_CONTROL) != 0)
    {
        parts.push_back("Ctrl");
    }
    if ((modifiers & MOD_ALT) != 0)
    {
        parts.push_back("Alt");
    }
    if ((modifiers & MOD_SHIFT) != 0)
    {
        parts.push_back("Shift");
    }
    if ((modifiers & MOD_WIN) != 0)
    {
        parts.push_back("Win");
    }
    parts.push_back(key_label);

    std::ostringstream display;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            display << "+";
        }
        display << parts[i];
    }
    return display.str();
}

bool IsVirtualKeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
}

HotkeySpec ParseHotkeyOrThrow(const std::string &value)
{
    HotkeySpec spec;
    std::string key_label;
    for (const std::string &raw_token : SplitHotkeyTokens(value))
    {
        const std::string token = ToLowerAscii(Trim(raw_token));
        if (token == "ctrl" || token == "control")
        {
            spec.modifiers |= MOD_CONTROL;
        }
        else if (token == "alt")
        {
            spec.modifiers |= MOD_ALT;
        }
        else if (token == "shift")
        {
            spec.modifiers |= MOD_SHIFT;
        }
        else if (token == "win" || token == "windows" || token == "super" || token == "meta")
        {
            spec.modifiers |= MOD_WIN;
        }
        else
        {
            const auto key = ParseHotkeyKey(token);
            if (!key.has_value())
            {
                throw std::runtime_error("无法解析热键: " + value);
            }
            if (spec.vk != 0)
            {
                throw std::runtime_error("热键只能包含一个主按键: " + value);
            }
            spec.vk = key->first;
            key_label = key->second;
        }
    }

    if (spec.vk == 0 || spec.modifiers == 0)
    {
        throw std::runtime_error("热键必须包含至少一个修饰键和一个主按键: " + value);
    }
    spec.display = FormatHotkeyDisplay(spec.modifiers, key_label);
    return spec;
}

bool IsModifierVirtualKey(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
           vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LWIN ||
           vk == VK_RWIN;
}

UINT CurrentHotkeyModifiers()
{
    UINT modifiers = 0;
    if (IsVirtualKeyDown(VK_CONTROL) || IsVirtualKeyDown(VK_LCONTROL) || IsVirtualKeyDown(VK_RCONTROL))
    {
        modifiers |= MOD_CONTROL;
    }
    if (IsVirtualKeyDown(VK_MENU) || IsVirtualKeyDown(VK_LMENU) || IsVirtualKeyDown(VK_RMENU))
    {
        modifiers |= MOD_ALT;
    }
    if (IsVirtualKeyDown(VK_SHIFT) || IsVirtualKeyDown(VK_LSHIFT) || IsVirtualKeyDown(VK_RSHIFT))
    {
        modifiers |= MOD_SHIFT;
    }
    if (IsVirtualKeyDown(VK_LWIN) || IsVirtualKeyDown(VK_RWIN))
    {
        modifiers |= MOD_WIN;
    }
    return modifiers;
}

std::optional<HotkeySpec> HotkeySpecFromRecordedKey(UINT modifiers, UINT vk)
{
    if (modifiers == 0 || IsModifierVirtualKey(vk) || vk == VK_ESCAPE)
    {
        return std::nullopt;
    }
    const auto key_label = HotkeyKeyLabelFromVk(vk);
    if (!key_label.has_value())
    {
        return std::nullopt;
    }

    HotkeySpec spec;
    spec.modifiers = modifiers;
    spec.vk = vk;
    spec.display = FormatHotkeyDisplay(modifiers, *key_label);
    return spec;
}
}
