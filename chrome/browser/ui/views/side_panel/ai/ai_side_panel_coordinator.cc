// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/side_panel/ai/ai_side_panel_coordinator.h"

#include <memory>
#include <string>
#include <utility>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/side_panel/ai/ai_side_panel_view.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/unowned_user_data/user_data_factory.h"
#include "ui/views/view_utils.h"

DEFINE_USER_DATA(AiSidePanelCoordinator);

// static
AiSidePanelCoordinator* AiSidePanelCoordinator::From(tabs::TabInterface* tab) {
  return tab ? AiSidePanelCoordinator::Get(tab->GetUnownedUserDataHost())
             : nullptr;
}

// static
bool AiSidePanelCoordinator::IsSupported(Profile* profile) {
  // The panel sends page context to a model, so it is not offered in
  // incognito, guest or other non-regular profiles.
  return profile->IsRegularProfile() &&
         base::FeatureList::IsEnabled(features::kAiSidePanel);
}

AiSidePanelCoordinator::AiSidePanelCoordinator(
    tabs::TabInterface& tab_interface,
    SidePanelRegistry* registry)
    : tab_interface_(tab_interface),
      scoped_unowned_user_data_(tab_interface.GetUnownedUserDataHost(), *this) {
  if (registry) {
    CreateAndRegisterEntry(registry);
  }
}

AiSidePanelCoordinator::~AiSidePanelCoordinator() {
  // The registry, and therefore the entry, outlives this object.
  if (entry_) {
    entry_->RemoveObserver(this);
  }
}

void AiSidePanelCoordinator::CreateAndRegisterEntry(
    SidePanelRegistry* registry) {
  auto entry = std::make_unique<SidePanelEntry>(
      SidePanelEntryKey(SidePanelEntryId::kAiSidePanel),
      base::BindRepeating(&AiSidePanelCoordinator::CreateAiSidePanelView,
                          base::Unretained(this)),
      /*default_content_width_callback=*/base::NullCallback());
  entry->AddObserver(this);
  SidePanelEntry* const entry_ptr = entry.get();
  // Register() destroys `entry` and returns false if the key is already taken.
  // Only retain the pointer once it is known to be owned by the registry.
  CHECK(registry->Register(std::move(entry)));
  entry_ = entry_ptr;
}

void AiSidePanelCoordinator::Show() {
  if (auto* side_panel_ui =
          SidePanelUI::From(tab_interface_->GetBrowserWindowInterface())) {
    side_panel_ui->Show(SidePanelEntryId::kAiSidePanel);
  }
}

void AiSidePanelCoordinator::OnEntryShown(SidePanelEntry* entry) {
  // The view is cached between shows, so the tab may have navigated since it
  // was built.
  UpdateViewPageContext();
}

void AiSidePanelCoordinator::UpdateViewPageContext() {
  auto* view = views::AsViewClass<AiSidePanelView>(view_tracker_.view());
  if (!view) {
    return;
  }

  content::WebContents* const contents = tab_interface_->GetContents();
  if (!contents) {
    view->SetPageContext(std::u16string(), std::u16string());
    return;
  }

  // TODO: Use url_formatter::FormatUrlForSecurityDisplay() when this stub is
  // replaced. possibly_invalid_spec() renders embedded credentials verbatim
  // and is not how Chrome displays URLs to users.
  view->SetPageContext(
      contents->GetTitle(),
      base::UTF8ToUTF16(
          contents->GetLastCommittedURL().possibly_invalid_spec()));
}

SidePanelNativeView AiSidePanelCoordinator::CreateAiSidePanelView(
    SidePanelEntryScope& scope) {
  auto view = std::make_unique<AiSidePanelView>();
  view_tracker_.SetView(view.get());
  UpdateViewPageContext();
  return view;
}
