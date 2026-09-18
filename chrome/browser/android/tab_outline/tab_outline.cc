// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/tab_outline/tab_outline.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_role_properties.h"
#include "ui/accessibility/ax_tree_update.h"

namespace tab_outline {

std::vector<std::string> ExtractOutline(const ui::AXTreeUpdate& update,
                                        bool indent_level_2) {
  std::vector<std::string> lines;
  // AXTreeUpdate::nodes is a flat vector in document order, so iterating it is
  // the tree walk -- no recursion, and heading order is preserved for free.
  for (const ui::AXNodeData& node : update.nodes) {
    if (!ui::IsHeading(node.role)) {
      continue;
    }
    const int level =
        node.GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
    if (level != 1 && level != 2) {
      continue;
    }

    // A heading's accessible name is its computed text content, which can carry
    // the source's line breaks and indentation. Collapse it, or a heading
    // wrapped across several lines in the markup arrives as several lines here
    // and breaks the one-heading-per-line layout.
    std::string text = base::CollapseWhitespaceASCII(
        node.GetStringAttribute(ax::mojom::StringAttribute::kName),
        /*trim_sequences_with_line_breaks=*/true);
    if (text.empty()) {
      continue;
    }

    // A page is free to have a pathologically long heading, and all of it would
    // otherwise cross the JNI boundary or go into a prompt.
    // TruncateUTF8ToByteSize() cuts on a character boundary rather than
    // mid-sequence, which substr() would not.
    if (text.size() > kMaxHeadingBytes) {
      text = std::string(base::TruncateUTF8ToByteSize(text, kMaxHeadingBytes));
    }

    lines.push_back((indent_level_2 && level == 2) ? "    " + text : text);
  }
  return lines;
}

}  // namespace tab_outline
