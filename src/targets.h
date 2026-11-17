// SPDX-License-Identifier: MIT
// Reconciles the open targets with a *complete* enumeration result. Callers must not call this
// with a partial result: an enumeration error leaves the current targets untouched.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace firstword {

struct ReconcileResult {
    std::vector<std::wstring> remove;    // open targets no longer present
    std::vector<std::wstring> add;       // new targets within the limit
    std::vector<std::wstring> skipped;   // new targets beyond the limit
};

inline ReconcileResult Reconcile(const std::vector<std::wstring>& current, const std::vector<std::wstring>& found, std::size_t limit) {
    ReconcileResult r;
    auto contains = [](const std::vector<std::wstring>& v, const std::wstring& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
    for (const auto& id : current)
        if (!contains(found, id)) r.remove.push_back(id);
    std::size_t kept = current.size() - r.remove.size();
    for (const auto& id : found) {
        if (contains(current, id) || contains(r.add, id) || contains(r.skipped, id)) continue;
        if (kept < limit) { r.add.push_back(id); ++kept; }
        else r.skipped.push_back(id);
    }
    return r;
}

} // namespace firstword
