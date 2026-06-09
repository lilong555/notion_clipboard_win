#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace ncw
{
struct HotkeySpec
{
    UINT modifiers = 0;
    UINT vk = 0;
    std::string display;
};

HotkeySpec ParseHotkeyOrThrow(const std::string &value);
bool IsModifierVirtualKey(UINT vk);
UINT CurrentHotkeyModifiers();
std::optional<HotkeySpec> HotkeySpecFromRecordedKey(UINT modifiers, UINT vk);
}
