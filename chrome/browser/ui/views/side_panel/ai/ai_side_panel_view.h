// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_AI_AI_SIDE_PANEL_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_AI_AI_SIDE_PANEL_VIEW_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace views {
class Label;
}  // namespace views

// The contents of the AI side panel for a single tab.
//
// This is a placeholder surface: it renders the context the panel will
// eventually operate on so that the tab-scoped plumbing is observable. It is
// replaced by a WebUI host in phase 2, see docs-ob/ai_side_panel_design.md.
//
// The view deliberately holds no reference to its tab. The side panel outlives
// the tab strip during browser teardown, so anything the view pointed at would
// dangle; AiSidePanelCoordinator pushes context in instead.
class AiSidePanelView : public views::View {
  METADATA_HEADER(AiSidePanelView, views::View)

 public:
  AiSidePanelView();
  AiSidePanelView(const AiSidePanelView&) = delete;
  AiSidePanelView& operator=(const AiSidePanelView&) = delete;
  ~AiSidePanelView() override;

  // Sets the page context shown in the panel. Called by the coordinator when
  // the view is created and whenever the entry is shown again, because the view
  // is cached between shows and the tab may have navigated in the meantime.
  void SetPageContext(const std::u16string& title, const std::u16string& url);

 private:
  raw_ptr<views::Label> title_label_ = nullptr;
  raw_ptr<views::Label> url_label_ = nullptr;
};

#endif  // CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_AI_AI_SIDE_PANEL_VIEW_H_
