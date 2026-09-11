// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_AI_AI_SIDE_PANEL_COORDINATOR_H_
#define CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_AI_AI_SIDE_PANEL_COORDINATOR_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_observer.h"
#include "chrome/browser/ui/side_panel/side_panel_native_view.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/views/view_tracker.h"

class Profile;
class SidePanelEntry;
class SidePanelEntryScope;
class SidePanelRegistry;

namespace tabs {
class TabInterface;
}  // namespace tabs

// Creates and registers the AI side panel entry for a single tab.
//
// This is a tab-scoped coordinator: it is owned by TabFeatures and registers
// into the tab's SidePanelRegistry, so the panel opens next to the tab it
// belongs to and follows tab switches. See docs-ob/ai_side_panel_design.md.
class AiSidePanelCoordinator : public SidePanelEntryObserver {
 public:
  AiSidePanelCoordinator(tabs::TabInterface& tab_interface,
                         SidePanelRegistry* registry);
  AiSidePanelCoordinator(const AiSidePanelCoordinator&) = delete;
  AiSidePanelCoordinator& operator=(const AiSidePanelCoordinator&) = delete;
  ~AiSidePanelCoordinator() override;

  DECLARE_USER_DATA(AiSidePanelCoordinator);
  static AiSidePanelCoordinator* From(tabs::TabInterface* tab);

  // Returns whether the AI side panel is available for `profile`. When this
  // returns false no entry is registered and the menu item is not added.
  static bool IsSupported(Profile* profile);

  // Shows the AI side panel for this tab.
  void Show();

  // SidePanelEntryObserver:
  void OnEntryShown(SidePanelEntry* entry) override;

 private:
  void CreateAndRegisterEntry(SidePanelRegistry* registry);

  // Pushes the tab's current title and URL into the panel view, if one exists.
  void UpdateViewPageContext();

  // Builds the panel contents. Invoked lazily by SidePanelEntry the first time
  // the panel is shown, not at registration time.
  SidePanelNativeView CreateAiSidePanelView(SidePanelEntryScope& scope);

  const raw_ref<tabs::TabInterface> tab_interface_;

  // Owned by the tab's SidePanelRegistry, which outlives this object. Held so
  // that the observer registration can be undone on destruction.
  raw_ptr<SidePanelEntry> entry_ = nullptr;

  // Tracks the view handed to the side panel so that it can be refreshed when
  // the entry is shown again. The view is owned by the side panel, and may be
  // destroyed independently of this object.
  views::ViewTracker view_tracker_;

  ui::ScopedUnownedUserData<AiSidePanelCoordinator> scoped_unowned_user_data_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_AI_AI_SIDE_PANEL_COORDINATOR_H_
