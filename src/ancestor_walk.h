// SPDX-License-Identifier: MIT
// Walks a device node and its parents looking for the target hardware IDs. The PnP calls are passed
// in, so the decision logic is testable without hardware.
#pragma once
#include <string>
#include <vector>
#include "device_match.h"

namespace firstword {

enum class ParentStep { Found, TopOfTree, Error };
enum class WalkResult { Match, NoMatch, Error };

// readIds(node, ids) -> bool (false = read error); parentOf(node, parent) -> ParentStep.
// Only the top of the tree or the level limit end a walk as NoMatch; any read error is Error,
// so a caller never mistakes "could not read" for "not the device".
template <typename Node, typename ReadIds, typename ParentOf>
WalkResult WalkAncestors(Node start, int maxParentLevels, const std::wstring& vid, const std::wstring& pid,
                         ReadIds readIds, ParentOf parentOf, Node& matched) {
    Node node = start;
    for (int level = 0; level <= maxParentLevels; ++level) {
        std::vector<std::wstring> ids;
        if (!readIds(node, ids)) return WalkResult::Error;
        if (AnyHardwareIdMatches(ids, vid, pid)) { matched = node; return WalkResult::Match; }
        if (level == maxParentLevels) break;
        Node parent{};
        switch (parentOf(node, parent)) {
        case ParentStep::Found: node = parent; break;
        case ParentStep::TopOfTree: return WalkResult::NoMatch;
        case ParentStep::Error: return WalkResult::Error;
        }
    }
    return WalkResult::NoMatch;
}

} // namespace firstword
