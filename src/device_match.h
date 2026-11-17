// SPDX-License-Identifier: MIT
// Hardware identity matching: decides whether a PnP hardware ID belongs to the target USB device.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <cwctype>

namespace firstword {

inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::towupper(a[i]) != std::towupper(b[i])) return false;
    return true;
}

// True only for a USB hardware ID whose whole tokens (split at '\' and '&') contain
// exactly VID_<vid> and PID_<pid>. Other buses (BTHENUM, HID, MMDEVAPI) never match.
inline bool HardwareIdMatches(std::wstring_view id, std::wstring_view vid, std::wstring_view pid) {
    if (vid.empty() || pid.empty()) return false;
    const std::wstring wantVid = L"VID_" + std::wstring(vid);
    const std::wstring wantPid = L"PID_" + std::wstring(pid);
    bool first = true, busOk = false, vidOk = false, pidOk = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= id.size(); ++i) {
        if (i == id.size() || id[i] == L'\\' || id[i] == L'&') {
            const std::wstring_view token = id.substr(start, i - start);
            if (first) { busOk = EqualsNoCase(token, L"USB"); first = false; }
            else if (EqualsNoCase(token, wantVid)) vidOk = true;
            else if (EqualsNoCase(token, wantPid)) pidOk = true;
            start = i + 1;
        }
    }
    return busOk && vidOk && pidOk;
}

inline bool AnyHardwareIdMatches(const std::vector<std::wstring>& ids, std::wstring_view vid, std::wstring_view pid) {
    for (const auto& id : ids)
        if (HardwareIdMatches(id, vid, pid)) return true;
    return false;
}

// PnP instance ID of an audio endpoint's device node. Windows does not expose PKEY_Device_InstanceId
// in the endpoint property store; the endpoint node is then SWD\MMDEVAPI\<IMMDevice::GetId()>.
inline std::wstring EndpointPnpInstanceId(std::wstring_view reported, std::wstring_view endpointId) {
    if (!reported.empty()) return std::wstring(reported);
    if (endpointId.empty()) return {};
    return L"SWD\\MMDEVAPI\\" + std::wstring(endpointId);
}

// Parses a REG_MULTI_SZ / DEVPROP_TYPE_STRING_LIST buffer of `count` wide chars; never reads past it.
// The list must end with an empty entry (double null) inside the buffer; otherwise the whole list is
// rejected (empty result) so that a cut-off ID can never match.
inline std::vector<std::wstring> ParseMultiSz(const wchar_t* data, std::size_t count) {
    std::vector<std::wstring> out;
    if (!data) return out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (data[i] != L'\0') continue;
        if (i == start) return out;      // empty entry: proper end of list
        out.emplace_back(data + start, i - start);
        start = i + 1;
    }
    return {};                           // no list terminator within the buffer
}

} // namespace firstword
