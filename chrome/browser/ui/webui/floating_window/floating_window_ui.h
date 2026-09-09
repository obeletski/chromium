// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_FLOATING_WINDOW_FLOATING_WINDOW_UI_H_
#define CHROME_BROWSER_UI_WEBUI_FLOATING_WINDOW_FLOATING_WINDOW_UI_H_

#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"

class FloatingWindowUI;

// Every chrome:// page is described by a WebUIConfig, which is the registry
// entry that maps a (scheme, host) pair to the C++ object that backs it.
//
// The lifecycle is:
//   1. RegisterChromeWebUIConfigs() adds one instance of this class to the
//      global WebUIConfigMap at startup (see chrome_web_ui_configs.cc).
//   2. When something navigates to chrome://floating-window, content looks the
//      host up in that map.
//   3. The map calls CreateWebUIController(), which builds a FloatingWindowUI
//      for the WebContents doing the navigating.
//
// content::DefaultWebUIConfig<T> supplies step 3 for the common case: it just
// does `std::make_unique<T>(web_ui)`. Deriving from the bare WebUIConfig is
// only necessary when construction needs more than the WebUI*, or when the
// page must be conditionally disabled (override IsWebUIEnabled()).
class FloatingWindowUIConfig
    : public content::DefaultWebUIConfig<FloatingWindowUI> {
 public:
  FloatingWindowUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIFloatingWindowHost) {}
};

// The WebUI for chrome://floating-window: the document rendered inside the
// floating window opened from the toolbar. It lists the profile's open tabs,
// grouped by browser window, and under each tab that page's outline — its
// level 1 and level 2 headings.
//
// A WebUIController is the browser-process object that sits behind one WebUI
// page. Its usual jobs are (a) registering the data source that serves the
// page's resources and (b) wiring up Mojo interfaces or message handlers that
// the page can call back into. This one only does (a). The page has dynamic
// content but still no script of its own: everything is rendered into the HTML
// in C++ while the response is produced, so there is no renderer-side code
// here that would need a channel back.
//
// The two halves of the page come from different places, which is what shapes
// the .cc. The tab list is browser-process state, read synchronously. The
// outlines are DOM content living in one renderer per tab, gathered with
// WebContents::RequestAXTreeSnapshot() and arriving asynchronously — so the
// response itself is produced asynchronously, which WebUIDataSource's
// GotDataCallback explicitly permits.
//
// Both halves are a snapshot taken when the page loads. The window is rebuilt
// on every toolbar press, so it refreshes on reopen rather than live.
class FloatingWindowUI : public content::WebUIController {
 public:
  explicit FloatingWindowUI(content::WebUI* web_ui);

  FloatingWindowUI(const FloatingWindowUI&) = delete;
  FloatingWindowUI& operator=(const FloatingWindowUI&) = delete;

  ~FloatingWindowUI() override;

  // Declares a per-class type tag (the address of a static int) so that callers
  // can safely downcast a WebUIController* back to FloatingWindowUI*. Paired
  // with WEB_UI_CONTROLLER_TYPE_IMPL in the .cc, which must exist exactly once.
  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_FLOATING_WINDOW_FLOATING_WINDOW_UI_H_
