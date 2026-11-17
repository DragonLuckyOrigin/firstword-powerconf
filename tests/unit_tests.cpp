// SPDX-License-Identifier: MIT
// Unit tests for the hardware-free parts of FirstWord.
#include "../src/device_match.h"
#include "../src/backoff.h"
#include "../src/targets.h"
#include "../src/ancestor_walk.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void HardwareIdMatching() {
    using firstword::HardwareIdMatches;
    CHECK(HardwareIdMatches(L"USB\\VID_291A&PID_3301&REV_0000&MI_01", L"291A", L"3301"));
    CHECK(HardwareIdMatches(L"USB\\VID_291A&PID_3301&MI_01", L"291A", L"3301"));
    CHECK(HardwareIdMatches(L"usb\\vid_291a&pid_3301", L"291A", L"3301"));        // case-insensitive
    CHECK(!HardwareIdMatches(L"USB\\VID_291A&PID_3302&MI_01", L"291A", L"3301"));  // other product
    CHECK(!HardwareIdMatches(L"USB\\VID_291AB&PID_3301", L"291A", L"3301"));      // no substring match
    CHECK(!HardwareIdMatches(L"USB\\VID_1291A&PID_3301", L"291A", L"3301"));
    CHECK(!HardwareIdMatches(L"BTHENUM\\VID_291A&PID_3301", L"291A", L"3301"));   // Bluetooth never
    CHECK(!HardwareIdMatches(L"HID\\VID_291A&PID_3301&MI_00", L"291A", L"3301"));  // only USB bus
    CHECK(!HardwareIdMatches(L"MMDEVAPI\\AudioEndpoints", L"291A", L"3301"));
    CHECK(!HardwareIdMatches(L"USB\\VID_291A", L"291A", L"3301"));                // PID missing
    CHECK(!HardwareIdMatches(L"", L"291A", L"3301"));
    CHECK(!HardwareIdMatches(L"USB\\VID_291A&PID_3301", L"", L"3301"));           // empty filter never matches
    CHECK(!HardwareIdMatches(L"USB\\VID_291A&PID_33011", L"291A", L"3301"));      // PID substring
    CHECK(!HardwareIdMatches(L"USB\\VID_291A&PID_330", L"291A", L"3301"));        // truncated PID
    // VID and PID spread over two list entries must not combine into a match.
    CHECK(!firstword::AnyHardwareIdMatches({ L"USB\\VID_291A&PID_0001", L"USB\\VID_0001&PID_3301" }, L"291A", L"3301"));
}

static void EndpointInstance() {
    using firstword::EndpointPnpInstanceId;
    // Windows does not expose PKEY_Device_InstanceId on audio endpoints :
    // the endpoint's PnP node is SWD\MMDEVAPI\<endpoint id>.
    CHECK(EndpointPnpInstanceId(L"", L"{0.0.1.00000000}.{00000000-1111-2222-3333-444444444444}")
          == L"SWD\\MMDEVAPI\\{0.0.1.00000000}.{00000000-1111-2222-3333-444444444444}");
    CHECK(EndpointPnpInstanceId(L"SWD\\MMDEVAPI\\X", L"{0.0.1.00000000}.{y}") == L"SWD\\MMDEVAPI\\X");   // reported value wins
    CHECK(EndpointPnpInstanceId(L"", L"").empty());
}

static void MultiSzParsing() {
    using firstword::ParseMultiSz;
    const wchar_t data[] = L"USB\\VID_291A&PID_3301&REV_0000&MI_01\0USB\\VID_291A&PID_3301&MI_01\0\0";
    auto ids = ParseMultiSz(data, sizeof(data) / sizeof(wchar_t));
    CHECK(ids.size() == 2);
    CHECK(ids.size() == 2 && ids[1] == L"USB\\VID_291A&PID_3301&MI_01");
    CHECK(firstword::AnyHardwareIdMatches(ids, L"291A", L"3301"));
    // Invalid lists are rejected completely (fail-closed), never partially accepted.
    const wchar_t unterminated[] = { L'A', L'B' };
    CHECK(ParseMultiSz(unterminated, 2).empty());
    const wchar_t truncated[] = L"USB\\VID_291A&PID_3301";                     // one terminator, no list end
    CHECK(ParseMultiSz(truncated, sizeof(truncated) / sizeof(wchar_t)).empty());
    const wchar_t cut[] = { L'U', L'S', L'B' };                                 // cut inside an entry
    CHECK(ParseMultiSz(cut, 3).empty());
    const wchar_t emptyList[] = { L'\0', L'\0' };
    CHECK(ParseMultiSz(emptyList, 2).empty());
    CHECK(ParseMultiSz(nullptr, 0).empty());
}

static void TargetReconcile() {
    using firstword::Reconcile;
    // Healthy targets stay, missing ones go, new ones are added up to the limit.
    auto r = Reconcile({ L"a", L"b" }, { L"b", L"c" }, 8);
    CHECK(r.remove.size() == 1 && r.remove[0] == L"a");
    CHECK(r.add.size() == 1 && r.add[0] == L"c");
    CHECK(r.skipped.empty());
    auto full = Reconcile({ L"a" }, { L"a", L"b", L"c" }, 2);
    CHECK(full.add.size() == 1 && full.add[0] == L"b");
    CHECK(full.skipped.size() == 1 && full.skipped[0] == L"c");
    auto same = Reconcile({ L"a" }, { L"a" }, 8);
    CHECK(same.remove.empty() && same.add.empty());
}

static void EnumeratorRecoveryBackoff() {
    // Creating the enumerator is not success: only a completed enumeration resets the backoff.
    firstword::EnumeratorRecovery e;
    e.Failed(0);            CHECK(!e.Due(999));  CHECK(e.Due(1000));    // 1 s
    e.Created();
    e.Failed(1000);         CHECK(!e.Due(2999)); CHECK(e.Due(3000));    // 2 s, not back to 1 s
    e.Created();
    e.Failed(3000);         CHECK(!e.Due(7999)); CHECK(e.Due(8000));    // 5 s
    e.EnumerationSucceeded();
    e.Failed(9000);         CHECK(e.Due(10000));                        // reset after real success
}

static void BackoffSchedule() {
    firstword::Backoff b;
    CHECK(b.Due(0));                       // a new target is due immediately
    b.Fail(1000);  CHECK(!b.Due(1999)); CHECK(b.Due(2000));     // 1 s
    b.Fail(2000);  CHECK(!b.Due(3999)); CHECK(b.Due(4000));     // 2 s
    b.Fail(4000);  CHECK(!b.Due(8999)); CHECK(b.Due(9000));     // 5 s
    b.Fail(9000);  CHECK(!b.Due(18999)); CHECK(b.Due(19000));   // 10 s
    b.Fail(19000); CHECK(!b.Due(28999)); CHECK(b.Due(29000));   // stays at 10 s
    CHECK(b.MillisUntilDue(20000) == 9000);
    CHECK(b.MillisUntilDue(30000) == 0);
    b.Reset();     CHECK(b.Due(0)); CHECK(b.Attempts() == 0);
}

static void Watchdog() {
    using firstword::IsStale;
    CHECK(!IsStale(1000, 3999, 3000));
    CHECK(!IsStale(1000, 4000, 3000));
    CHECK(IsStale(1000, 4001, 3000));
    CHECK(!IsStale(5000, 4000, 3000));     // clock before last packet: never stale
}

static void AncestorWalk() {
    using namespace firstword;
    // Tree: 0 = endpoint (MMDEVAPI), 1 = USB interface (target), 2 = USB hub, 3 = root.
    const std::vector<std::vector<std::wstring>> ids = {
        { L"MMDEVAPI\\AudioEndpoints" }, { L"USB\\VID_291A&PID_3301&MI_01" }, { L"USB\\ROOT_HUB30" }, { L"ROOT" } };
    auto read = [&](int n, std::vector<std::wstring>& out) { out = ids[n]; return true; };
    auto parent = [](int n, int& p) { if (n >= 3) return ParentStep::TopOfTree; p = n + 1; return ParentStep::Found; };
    int matched = -1;
    CHECK(WalkAncestors(0, 4, L"291A", L"3301", read, parent, matched) == WalkResult::Match && matched == 1);
    CHECK(WalkAncestors(0, 4, L"291A", L"9999", read, parent, matched) == WalkResult::NoMatch);   // reaches the top
    CHECK(WalkAncestors(0, 0, L"291A", L"3301", read, parent, matched) == WalkResult::NoMatch);   // level limit
    // A parent query that fails before the match is an error, never a non-match.
    auto brokenParent = [](int, int&) { return ParentStep::Error; };
    CHECK(WalkAncestors(0, 4, L"291A", L"3301", read, brokenParent, matched) == WalkResult::Error);
    // A hardware-ID read that fails is an error as well.
    auto brokenRead = [](int, std::vector<std::wstring>&) { return false; };
    CHECK(WalkAncestors(0, 4, L"291A", L"3301", brokenRead, parent, matched) == WalkResult::Error);
}

int main() {
    EndpointInstance();
    AncestorWalk();
    HardwareIdMatching();
    MultiSzParsing();
    BackoffSchedule();
    Watchdog();
    TargetReconcile();
    EnumeratorRecoveryBackoff();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
