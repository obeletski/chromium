// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/floating_window/floating_window_ui.h"

#include <string>
#include <string_view>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"

namespace {

// The entire page, as a string literal.
//
// The usual way to ship a WebUI page is a build_webui() GN target: HTML/TS/CSS
// files on disk, compiled and packed into a .pak, reached from C++ by resource
// ID (IDR_*) via WebUIDataSource::AddResourcePath(). That is the right shape
// for anything with real markup or script. For a static one-line page it is all
// overhead — a GN target, a .grd entry, a generated resources map and a build
// step to produce a single <p>. So the bytes live here instead.
//
// Content Security Policy note. Data sources get a default CSP from
// URLDataSource::GetContentSecurityPolicy(). For a trusted chrome:// source it
// leaves style-src unset, so the inline <style> below is allowed. It does *not*
// relax script-src (only chrome://resources and 'self' are permitted, and
// require-trusted-types-for is on), which is why there is deliberately no
// inline script here — one would be silently blocked at runtime.
//
// The colors are CSS system colors plus `color-scheme: light dark`, so the page
// follows the OS/browser light or dark theme without the browser having to push
// any color values into it.
constexpr char kFloatingWindowHtml[] = R"(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Floating window</title>
  <style>
    html, body {
      margin: 0;
      padding: 0;
    }
    body {
      background: canvas;
      color: canvastext;
      color-scheme: light dark;
      font: 13px system-ui, sans-serif;
      padding: 24px 28px;
      text-align: center;
    }
    p {
      margin: 0;
      white-space: nowrap;
    }
  </style>
</head>
<body>
  <p>I am the floating window</p>
</body>
</html>
)";

// First half of SetRequestFilter(): decides whether this source wants to answer
// a given path at all. Returning false falls through to the normal
// AddResourcePath()/SetDefaultResource() lookup, which here would find nothing.
// Returning true unconditionally means every path under the host — "/", but
// also "/anything" — is served the same document, so a stray trailing path
// segment produces the page rather than a blank window.
bool ShouldHandleRequest(const std::string& /*path*/) {
  return true;
}

// Second half of SetRequestFilter(): produces the bytes.
//
// The response is handed back through a callback rather than returned, because
// a data source is allowed to answer asynchronously (reading from disk, waiting
// on a service). This one has the bytes already, so it runs the callback
// immediately. base::RefCountedMemory is used because the network stack may
// keep the buffer alive past this call.
void HandleRequest(const std::string& /*path*/,
                   content::WebUIDataSource::GotDataCallback callback) {
  std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
      std::string(kFloatingWindowHtml)));
}

}  // namespace

FloatingWindowUI::FloatingWindowUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  // Data sources are per-BrowserContext, so an Incognito window registers its
  // own. CreateAndAdd() both constructs the source and installs it as the
  // handler for chrome://floating-window/* on this profile; re-registering on a
  // later navigation simply replaces the previous one.
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      Profile::FromWebUI(web_ui), chrome::kChromeUIFloatingWindowHost);

  // SetRequestFilter() is the escape hatch from the resource-ID model: it lets
  // a source compute a response in C++. It is checked first in
  // WebUIDataSourceImpl::StartDataRequest(), ahead of the IDR lookup.
  //
  // Note that SetResourcePathToResponse() looks like a shorter way to do this
  // and is used that way in content's own browsertests, but it only fills a map
  // consumed by PopulateWebUIResources() for the LocalResourceLoaderConfig
  // path; StartDataRequest() never reads it. The request filter is the path
  // that is honoured unconditionally.
  source->SetRequestFilter(base::BindRepeating(&ShouldHandleRequest),
                           base::BindRepeating(&HandleRequest));
}

FloatingWindowUI::~FloatingWindowUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(FloatingWindowUI)
