// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/floating_window/floating_window_bubble.h"

#include <memory>
#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace floating_window {

namespace {

// Bounds handed to the renderer's auto-resize mode (see the sizing chain in
// CreateAndShow below). The page is a single line of text, so in practice the
// rendered size lands near the minimum; the maximum only stops a future edit to
// the page from producing an unbounded window.
constexpr gfx::Size kMinSize(220, 60);
constexpr gfx::Size kMaxSize(600, 400);

}  // namespace

views::Widget* CreateAndShow(views::View* anchor_view, Profile* profile) {
  // TOP_RIGHT is the arrow position, i.e. the bubble hangs below the anchor and
  // is right-aligned with it — correct for a button near the right edge of the
  // toolbar, where a left-aligned bubble would run off-screen.
  //
  // autosize=true is what makes the widget re-size itself whenever its contents
  // view reports a new preferred size. That is load-bearing here: the WebView's
  // preferred size is not known until the renderer has laid the page out, which
  // happens well after this function returns.
  auto delegate = std::make_unique<views::BubbleDialogDelegate>(
      anchor_view, views::BubbleBorder::TOP_RIGHT,
      views::BubbleBorder::DIALOG_SHADOW, /*autosize=*/true);

  // A BubbleDialogDelegate is a DialogDelegate, so by default it would grow an
  // OK/Cancel button row, a title and a close button. This surface is a plain
  // panel, so all three are turned off.
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  delegate->SetShowCloseButton(false);
  delegate->SetShowTitle(false);

  // Even with no visible title, the widget still needs a name for screen
  // readers; without one the accessibility tree exposes an unnamed dialog.
  // TODO(crbug.com/None): Localize once this graduates from a demo surface.
  delegate->SetAccessibleTitle(u"Floating window");

  // Two reasons this is off, and both matter:
  //
  // 1. Behaviour. A bubble that closes on deactivation is a menu — it vanishes
  //    the moment you click anything else. The requirement here is a window
  //    that stays up while you keep using the browser.
  //
  // 2. Correctness of the toggle. Deactivation is delivered *before* the
  //    anchor button's click handler runs. With close-on-deactivate on,
  //    clicking the button while the bubble is open would close it and then
  //    immediately re-open it, so the bubble would appear never to close.
  //    Turning it off means the widget is still alive when the handler runs,
  //    and the handler can simply close it. (The alternative in the tree is
  //    WebUIBubbleReopenSuppressor, which times the close to suppress the
  //    reopen; not needed once deactivation stops closing the bubble.)
  delegate->set_close_on_deactivate(false);

  // Default dialog margins would inset the web contents. The page supplies its
  // own padding, so the WebView should fill the bubble edge to edge.
  delegate->set_margins(gfx::Insets());

  // views::WebView is a View that hosts a content::WebContents. Constructing it
  // with a BrowserContext lets it create the WebContents lazily on first load.
  auto web_view = std::make_unique<views::WebView>(profile);

  // Esc handling. By default WebView::SkipDefaultKeyEventProcessing() returns
  // true for a live WebContents, which tells the FocusManager *not* to treat
  // key events as accelerators — the page gets first refusal, which is what you
  // want for real web content that may bind Esc.
  //
  // For a WebView hosting browser UI that is backwards: Esc must close the
  // surface. set_allow_accelerators(true) flips the check so only tab-traversal
  // keys are skipped, and Esc reaches the FocusManager. There it matches the
  // VKEY_ESCAPE accelerator that DialogClientView registers, which closes the
  // widget. So Esc-to-close is inherited, not implemented here.
  web_view->set_allow_accelerators(true);

  // Creates the WebContents and starts a browser-initiated navigation. Because
  // the target is a registered chrome:// host, this ends up instantiating
  // FloatingWindowUI, which registers the data source that serves the HTML.
  web_view->LoadInitialURL(GURL(chrome::kChromeUIFloatingWindowURL));

  // The sizing chain, end to end:
  //   EnableSizingFromWebContents() stores the bounds and turns on the
  //   renderer's auto-resize mode (RenderWidgetHostView::EnableAutoResize) ->
  //   after layout the renderer reports its content size ->
  //   WebView::ResizeDueToAutoResize() (WebView is its own WebContentsDelegate)
  //   calls SetPreferredSize() -> that invalidates layout -> the bubble's
  //   autosize flag makes the Widget resize to fit.
  //
  // Safe to call before the renderer exists: WebView re-applies the bounds from
  // SetUpNewMainFrame() whenever a new main frame appears.
  web_view->EnableSizingFromWebContents(kMinSize, kMaxSize);

  // Hands the WebView to the delegate as the bubble's contents, transferring
  // ownership into the view hierarchy.
  delegate->SetContentsView(std::move(web_view));

  // NATIVE_WIDGET_OWNS_WIDGET means the platform widget owns the views::Widget,
  // which owns the delegate and the contents. Nothing here needs to be kept
  // alive by the caller; the caller only tracks *whether* it exists.
  //
  // The "Deprecated" suffix refers to the ownership mode, not the call: the
  // preferred CreateBubble() overload returns a std::unique_ptr<Widget> under
  // CLIENT_OWNS_WIDGET, which would make the toolbar button responsible for the
  // widget's lifetime and for calling MakeCloseSynchronous(). Widget-owned is
  // the simpler contract for a surface that can also close itself (Esc).
  views::Widget* widget = views::BubbleDialogDelegate::CreateBubbleDeprecated(
      std::move(delegate),
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);

  // Shown immediately rather than waiting for the page to load. The bubble
  // starts at its minimum size and grows once the renderer reports back.
  widget->Show();
  return widget;
}

}  // namespace floating_window
