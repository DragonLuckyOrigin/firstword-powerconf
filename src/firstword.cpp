// SPDX-License-Identifier: MIT
// FirstWord: keeps the Anker PowerConf USB microphone streaming in WASAPI shared mode so
// that every application gets speech from the first second. Audio is released unread: nothing is
// stored, analysed or sent. Capture follows Microsoft's CaptureSharedEventDriven pattern.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <algorithm>

#include "backoff.h"
#include "device_match.h"
#include "targets.h"
#include "ancestor_walk.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kMutexName[] = L"Local\\FirstWord";
constexpr wchar_t kStopEventName[] = L"Local\\FirstWord.Stop";
constexpr DWORD kWaitMs = 500;
constexpr std::uint64_t kStaleMs = 3000;
constexpr std::uint64_t kIdleRescanMs = 30000;
constexpr std::size_t kMaxStreams = 8;
constexpr int kMaxParentLevels = 4;
constexpr LONGLONG kMaxLogBytes = 256 * 1024;

// ---------------------------------------------------------------- log (state changes only)

std::wstring g_logPath;

void InitLog() {
    wchar_t base[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir = std::wstring(base) + L"\\FirstWord";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    g_logPath = dir + L"\\firstword.log";
}

void Log(const wchar_t* format, ...) {
    if (g_logPath.empty()) return;
    wchar_t text[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t line[1200];
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s\r\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
    char utf8[3600];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes <= 1) return;

    // Keep the file at or below the limit: rotate before a write would exceed it; if rotation
    // fails, drop the line rather than grow past the limit.
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &info)) {
        const LONGLONG size = (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size + (bytes - 1) > kMaxLogBytes &&
            !MoveFileExW(g_logPath.c_str(), (g_logPath + L".1").c_str(), MOVEFILE_REPLACE_EXISTING))
            return;
    }
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    CloseHandle(file);
}

std::uint64_t Now() { return GetTickCount64(); }

// ---------------------------------------------------------------- device identity

HRESULT FromConfigRet(CONFIGRET cr) {
    return HRESULT_FROM_WIN32(CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE));
}

// Reads a node's hardware IDs. A node without the property, or with malformed data, is a determined
// non-match (empty list, S_OK); any other read failure is an error so the scan counts as incomplete.
HRESULT HardwareIds(DEVINST node, std::vector<std::wstring>& out) {
    out.clear();
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET cr = CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_HardwareIds, &type, nullptr, &size, 0);
    if (cr == CR_NO_SUCH_VALUE) return S_OK;
    if (cr != CR_BUFFER_SMALL) return FromConfigRet(cr);
    if (type != DEVPROP_TYPE_STRING_LIST || size == 0) return S_OK;
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    ULONG bufferBytes = static_cast<ULONG>(buffer.size() * sizeof(wchar_t));
    cr = CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_HardwareIds, &type, reinterpret_cast<PBYTE>(buffer.data()), &bufferBytes, 0);
    if (cr != CR_SUCCESS) return FromConfigRet(cr);
    if (type != DEVPROP_TYPE_STRING_LIST || bufferBytes % sizeof(wchar_t) != 0 || bufferBytes > buffer.size() * sizeof(wchar_t))
        return S_OK;
    // Strict parse within the returned length: an unterminated list yields no IDs (fail-closed).
    out = firstword::ParseMultiSz(buffer.data(), bufferBytes / sizeof(wchar_t));
    return S_OK;
}

std::wstring InstanceId(DEVINST node) {
    wchar_t id[MAX_DEVICE_ID_LEN + 1] = {};
    return CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? std::wstring(id) : std::wstring();
}

// Walks from the endpoint's PnP node up to kMaxParentLevels parents. S_OK with `usb` set = match;
// S_OK with `usb` empty = determined non-match (fail-closed); a failure HRESULT = could not decide,
// which makes the whole scan incomplete so existing targets are kept.
HRESULT FindUsbAncestor(const std::wstring& endpointInstanceId, const std::wstring& vid, const std::wstring& pid, bool derived, std::wstring& usb) {
    usb.clear();
    DEVINST node = 0;
    const CONFIGRET cr = CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(endpointInstanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
    // A derived (not reported) instance ID that has no device node is a determined non-match; any
    // other locate failure leaves the scan incomplete.
    if (cr == CR_NO_SUCH_DEVNODE && derived) return S_OK;
    if (cr != CR_SUCCESS) return FromConfigRet(cr);
    HRESULT error = S_OK;
    auto readIds = [&error](DEVINST n, std::vector<std::wstring>& ids) {
        const HRESULT hr = HardwareIds(n, ids);
        if (FAILED(hr)) { error = hr; return false; }
        return true;
    };
    // Only CR_NO_SUCH_DEVNODE means "no parent" (top of the tree); every other failure is an error.
    auto parentOf = [&error](DEVINST n, DEVINST& parent) {
        const CONFIGRET r = CM_Get_Parent(&parent, n, 0);
        if (r == CR_SUCCESS) return firstword::ParentStep::Found;
        if (r == CR_NO_SUCH_DEVNODE) return firstword::ParentStep::TopOfTree;
        error = FromConfigRet(r);
        return firstword::ParentStep::Error;
    };
    DEVINST matched = 0;
    switch (firstword::WalkAncestors(node, kMaxParentLevels, vid, pid, readIds, parentOf, matched)) {
    case firstword::WalkResult::Match:
        usb = InstanceId(matched);
        if (usb.empty()) usb = L"(USB instance id unavailable)";
        return S_OK;
    case firstword::WalkResult::NoMatch:
        return S_OK;
    case firstword::WalkResult::Error:
        break;
    }
    return FAILED(error) ? error : E_FAIL;
}

struct Match {
    std::wstring endpointId;
    std::wstring usbInstance;
};

HRESULT FindTargets(IMMDeviceEnumerator* enumerator, const std::wstring& vid, const std::wstring& pid, std::vector<Match>& out) {
    out.clear();
    ComPtr<IMMDeviceCollection> devices;
    HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr)) return hr;
    UINT count = 0;
    hr = devices->GetCount(&count);
    if (FAILED(hr)) return hr;
    // Any API failure makes the whole result incomplete: report it instead of a shorter list, so
    // the caller never closes a target just because it could not be read this time.
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        hr = devices->Item(i, &device);
        if (FAILED(hr)) return hr;
        LPWSTR rawId = nullptr;
        hr = device->GetId(&rawId);
        if (FAILED(hr)) return hr;
        if (!rawId) return E_POINTER;
        std::wstring endpointId(rawId);
        CoTaskMemFree(rawId);
        ComPtr<IPropertyStore> store;
        hr = device->OpenPropertyStore(STGM_READ, &store);
        if (FAILED(hr)) return hr;
        PROPVARIANT value;
        PropVariantInit(&value);
        hr = store->GetValue(PKEY_Device_InstanceId, &value);
        if (FAILED(hr)) { PropVariantClear(&value); return hr; }
        // Endpoints normally do not report the property; then use the endpoint's SWD\MMDEVAPI node.
        const std::wstring reported = (value.vt == VT_LPWSTR && value.pwszVal) ? std::wstring(value.pwszVal) : std::wstring();
        PropVariantClear(&value);
        const std::wstring instance = firstword::EndpointPnpInstanceId(reported, endpointId);
        std::wstring usb;
        hr = FindUsbAncestor(instance, vid, pid, /*derived=*/reported.empty(), usb);
        if (FAILED(hr)) return hr;
        if (!usb.empty()) out.push_back({ endpointId, usb });
    }
    return S_OK;
}

// ---------------------------------------------------------------- device notifications

class Notifications final : public IMMNotificationClient {
public:
    explicit Notifications(HANDLE rescan) : rescan_(rescan) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // Only signal here: no waiting, no COM release, no rescanning on the notification thread.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { SetEvent(rescan_); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { SetEvent(rescan_); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { SetEvent(rescan_); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~Notifications() = default;
    HANDLE rescan_;
    LONG refs_ = 1;
};

// ---------------------------------------------------------------- streams

struct Stream {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    std::uint64_t lastPacket = 0;
    bool delivered = false;

    ~Stream() {
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
        if (event) CloseHandle(event);
    }
};

struct Target {
    std::wstring usbInstance;
    firstword::Backoff backoff;
    std::unique_ptr<Stream> stream;
};

HRESULT Open(IMMDeviceEnumerator* enumerator, const std::wstring& endpointId, std::unique_ptr<Stream>& out) {
    auto stream = std::make_unique<Stream>();
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDevice(endpointId.c_str(), &device);
    if (FAILED(hr)) return hr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(stream->client.GetAddressOf()));
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* mix = nullptr;
    hr = stream->client->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    hr = stream->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return hr;
    stream->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stream->event) return HRESULT_FROM_WIN32(GetLastError());
    hr = stream->client->SetEventHandle(stream->event);
    if (FAILED(hr)) return hr;
    hr = stream->client->GetService(IID_PPV_ARGS(&stream->capture));
    if (FAILED(hr)) return hr;
    hr = stream->client->Start();
    if (FAILED(hr)) return hr;
    stream->lastPacket = Now();
    out = std::move(stream);
    return S_OK;
}

// Releases every queued packet unread. Returns a failure HRESULT when the stream is broken.
HRESULT Drain(Stream& stream) {
    for (;;) {
        UINT32 next = 0;
        HRESULT hr = stream.capture->GetNextPacketSize(&next);
        if (FAILED(hr)) return hr;
        if (next == 0) return S_OK;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) return hr;
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
        hr = stream.capture->ReleaseBuffer(frames);
        if (FAILED(hr)) return hr;
        stream.lastPacket = Now();
        stream.delivered = true;
    }
}

// ---------------------------------------------------------------- holder

class Holder {
public:
    Holder(HANDLE stop, std::wstring vid, std::wstring pid) : stop_(stop), vid_(std::move(vid)), pid_(std::move(pid)) {
        rescan_ = CreateEventW(nullptr, FALSE, TRUE, nullptr);   // first pass scans immediately
    }
    ~Holder() {
        targets_.clear();
        DropEnumerator();
        if (rescan_) CloseHandle(rescan_);
    }

    int Run() {
        if (!rescan_) { Log(L"error: rescan event %lu", GetLastError()); return 4; }
        Log(L"start: target USB VID_%s PID_%s", vid_.c_str(), pid_.c_str());
        for (;;) {
            const std::uint64_t now = Now();
            if (!enumerator_ && enumeratorRecovery_.Due(now)) MakeEnumerator(now);
            if (enumerator_ && (rescanPending_ || now - lastScan_ >= kIdleRescanMs)) Rescan(now);
            OpenDueTargets(now);

            std::vector<HANDLE> handles = { stop_, rescan_ };
            for (auto& entry : targets_)
                if (entry.second.stream) handles.push_back(entry.second.stream->event);

            const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, kWaitMs);
            if (result == WAIT_OBJECT_0) { Log(L"stop requested"); return 0; }
            if (result == WAIT_OBJECT_0 + 1) rescanPending_ = true;
            if (result == WAIT_FAILED) { Log(L"error: wait failed %lu", GetLastError()); return 4; }

            // After every return (event, rescan or timeout): drain all streams and run the watchdog.
            ServiceStreams(Now());
        }
    }

private:
    void MakeEnumerator(std::uint64_t now) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
        if (SUCCEEDED(hr)) {
            notifications_.Attach(new Notifications(rescan_));
            hr = enumerator_->RegisterEndpointNotificationCallback(notifications_.Get());
        }
        if (FAILED(hr)) {
            Log(L"error: device enumerator 0x%08lX", static_cast<unsigned long>(hr));
            DropEnumerator();
            enumeratorRecovery_.Failed(now);
            return;
        }
        enumeratorRecovery_.Created();   // not yet healthy: only a completed enumeration resets the backoff
        rescanPending_ = true;
    }

    void DropEnumerator() {
        if (enumerator_ && notifications_) enumerator_->UnregisterEndpointNotificationCallback(notifications_.Get());
        notifications_.Reset();
        enumerator_.Reset();
    }

    // Audio service gone: close everything and rebuild the enumerator with backoff.
    void ResetAudio(std::uint64_t now, HRESULT hr) {
        Log(L"audio service unavailable 0x%08lX: closing all streams", static_cast<unsigned long>(hr));
        for (auto& entry : targets_) entry.second.stream.reset();
        DropEnumerator();
        enumeratorRecovery_.Failed(now);
    }

    void Rescan(std::uint64_t now) {
        rescanPending_ = false;
        lastScan_ = now;
        std::vector<Match> found;
        const HRESULT hr = FindTargets(enumerator_.Get(), vid_, pid_, found);
        if (FAILED(hr)) {
            // Incomplete result: leave every target and stream as it is; rebuild the enumerator with
            // backoff. Open streams fail on their own if the audio service is really gone.
            Log(L"enumeration failed 0x%08lX: targets unchanged, enumerator rebuilt", static_cast<unsigned long>(hr));
            DropEnumerator();
            enumeratorRecovery_.Failed(now);
            return;
        }
        enumeratorRecovery_.EnumerationSucceeded();

        std::vector<std::wstring> current, ids;
        for (const auto& entry : targets_) current.push_back(entry.first);
        for (const auto& m : found) ids.push_back(m.endpointId);
        const firstword::ReconcileResult r = firstword::Reconcile(current, ids, kMaxStreams);
        for (const auto& id : r.remove) { Log(L"device gone: %s", id.c_str()); targets_.erase(id); }
        for (const auto& m : found) {
            if (std::find(r.add.begin(), r.add.end(), m.endpointId) == r.add.end()) continue;
            Log(L"device found: %s on %s", m.endpointId.c_str(), m.usbInstance.c_str());
            targets_[m.endpointId].usbInstance = m.usbInstance;
        }
        // Log only state changes: the skipped set (compared as a set) and the "no device" state.
        std::vector<std::wstring> skipped = r.skipped;
        std::sort(skipped.begin(), skipped.end());
        if (skipped != skipped_) {
            for (const auto& id : skipped) Log(L"skipped (limit %zu): %s", kMaxStreams, id.c_str());
            skipped_ = std::move(skipped);
        }
        const bool none = found.empty();
        if (none && !noTargetLogged_) Log(L"no target device present");
        noTargetLogged_ = none;
    }

    void OpenDueTargets(std::uint64_t now) {
        if (!enumerator_) return;
        for (auto& [id, target] : targets_) {
            if (target.stream || !target.backoff.Due(now)) continue;
            const HRESULT hr = Open(enumerator_.Get(), id, target.stream);
            if (SUCCEEDED(hr)) { Log(L"stream open: %s", id.c_str()); continue; }
            target.stream.reset();
            target.backoff.Fail(now);
            Log(L"open failed 0x%08lX (attempt %u): %s", static_cast<unsigned long>(hr), target.backoff.Attempts(), id.c_str());
            if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { ResetAudio(now, hr); return; }
            rescanPending_ = true;
        }
    }

    void ServiceStreams(std::uint64_t now) {
        for (auto& [id, target] : targets_) {
            if (!target.stream) continue;
            const HRESULT hr = Drain(*target.stream);
            if (FAILED(hr)) {
                Log(L"stream lost 0x%08lX: %s", static_cast<unsigned long>(hr), id.c_str());
                target.stream.reset();
                target.backoff.Fail(now);
                rescanPending_ = true;
                if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { ResetAudio(now, hr); return; }
                continue;
            }
            if (target.stream->delivered && target.backoff.Attempts() != 0) target.backoff.Reset();
            if (firstword::IsStale(target.stream->lastPacket, Now(), kStaleMs)) {
                Log(L"watchdog: no packets for %llu ms: %s", static_cast<unsigned long long>(kStaleMs), id.c_str());
                target.stream.reset();
                target.backoff.Fail(now);
                rescanPending_ = true;
            }
        }
    }

    HANDLE stop_;
    HANDLE rescan_ = nullptr;
    std::wstring vid_, pid_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<Notifications> notifications_;
    firstword::EnumeratorRecovery enumeratorRecovery_;
    std::map<std::wstring, Target> targets_;
    std::vector<std::wstring> skipped_;
    bool noTargetLogged_ = false;
    bool rescanPending_ = true;
    std::uint64_t lastScan_ = 0;
};

bool IsHex4(const std::wstring& s) {
    if (s.size() != 4) return false;
    for (wchar_t c : s) if (!iswxdigit(c)) return false;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    bool stop = false;
    std::wstring vid = L"291A", pid = L"3301";
    bool argsOk = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--stop") stop = true;
        else if (arg == L"--vid" && i + 1 < argc) vid = argv[++i];
        else if (arg == L"--pid" && i + 1 < argc) pid = argv[++i];
        else argsOk = false;
    }
    LocalFree(argv);
    if (!argsOk || !IsHex4(vid) || !IsHex4(pid)) return 2;

    // --stop is handled before the single-instance check.
    if (stop) {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
        if (!event) return GetLastError() == ERROR_FILE_NOT_FOUND ? 3 : 4;   // 3 only when no instance exists
        const BOOL set = SetEvent(event);
        CloseHandle(event);
        return set ? 0 : 4;
    }

    InitLog();
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) { Log(L"error: mutex %lu", GetLastError()); return 4; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    if (!stopEvent) { Log(L"error: stop event %lu", GetLastError()); CloseHandle(mutex); return 4; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"warning: stop event pre-existed; resetting");
        if (!ResetEvent(stopEvent)) {
            Log(L"error: reset of pre-existing stop event %lu", GetLastError());
            CloseHandle(stopEvent);
            CloseHandle(mutex);
            return 4;
        }
    }

    int code = 4;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Log(L"error: COM 0x%08lX", static_cast<unsigned long>(hr));
    } else {
        {
            Holder holder(stopEvent, vid, pid);
            code = holder.Run();
        }
        CoUninitialize();
    }
    Log(L"exit %d", code);
    CloseHandle(stopEvent);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
