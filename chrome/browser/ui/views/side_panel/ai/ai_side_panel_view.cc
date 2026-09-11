// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/side_panel/ai/ai_side_panel_view.h"

#include <memory>
#include <string>

#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"

namespace {

// Padding around the panel contents, in DIP.
constexpr int kPanelPadding = 16;

// Vertical spacing between the labels, in DIP.
constexpr int kLabelSpacing = 8;

}  // namespace

AiSidePanelView::AiSidePanelView() {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(kPanelPadding),
      kLabelSpacing));

  // No heading here: the side panel frame already renders the entry title.
  auto title_label = std::make_unique<views::Label>(
      std::u16string(), views::style::CONTEXT_LABEL,
      views::style::STYLE_PRIMARY);
  title_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  title_label->SetMultiLine(true);
  title_label_ = AddChildView(std::move(title_label));

  auto url_label = std::make_unique<views::Label>(
      std::u16string(), views::style::CONTEXT_LABEL,
      views::style::STYLE_SECONDARY);
  url_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  url_label->SetMultiLine(true);
  url_label_ = AddChildView(std::move(url_label));
}

AiSidePanelView::~AiSidePanelView() = default;

void AiSidePanelView::SetPageContext(const std::u16string& title,
                                     const std::u16string& url) {
  title_label_->SetText(title);
  url_label_->SetText(url);
}

BEGIN_METADATA(AiSidePanelView)
END_METADATA
