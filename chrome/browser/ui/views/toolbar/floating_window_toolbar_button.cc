// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/toolbar/floating_window_toolbar_button.h"

#include "base/functional/bind.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/floating_window/floating_window_bubble.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/accessibility/view_accessibility.h"

FloatingWindowToolbarButton::FloatingWindowToolbarButton(Browser* browser)
    // ToolbarButton takes a PressedCallback. base::Unretained is safe here
    // because the callback is owned by this button and cannot outlive it.
    : ToolbarButton(base::BindRepeating(&FloatingWindowToolbarButton::OnPressed,
                                        base::Unretained(this))),
      browser_(browser) {
  // Vector icons are compiled from .icon files under chrome/app/vector_icons/
  // into a generated header, so they re-rasterize crisply at any scale factor
  // and are recolored from the theme rather than shipped per-theme. Passing the
  // icon to ToolbarButton (rather than setting an image directly) is what keeps
  // it in step with theme and touch-mode changes.
  SetVectorIcon(kNewWindowIcon);

  // TODO(crbug.com/None): Move these strings to generated_resources.grd when
  // this stops being a demo surface. Hardcoded here to avoid requiring
  // translation screenshots for a string that is not shipping.
  GetViewAccessibility().SetName(u"Floating window");
  SetTooltipText(u"Floating window");

  // Tells assistive technology that activating this control opens a dialog,
  // rather than navigating or toggling something in place.
  GetViewAccessibility().SetHasPopup(ax::mojom::HasPopup::kDialog);
}

FloatingWindowToolbarButton::~FloatingWindowToolbarButton() {
  // The bubble is anchored to this view. If the button is destroyed first (the
  // browser window closing, or the toolbar being rebuilt), close the surface
  // rather than leaving it anchored to a dead view.
  CloseWindow();
}

void FloatingWindowToolbarButton::OnWidgetDestroying(views::Widget* widget) {
  // Only reached when something other than CloseWindow() closed the bubble,
  // because CloseWindow() stops observing before it closes. In practice: Esc.
  CHECK_EQ(widget, widget_);
  widget_observation_.Reset();
  widget_ = nullptr;
}

void FloatingWindowToolbarButton::OnPressed() {
  // This is the whole toggle, and it is only this simple because the bubble
  // sets close_on_deactivate(false). With the default setting, the click that
  // reaches this button would already have deactivated and closed the bubble,
  // `widget_` would be null, and this would re-open it — the surface would look
  // like it could never be closed by its own button.
  if (widget_) {
    CloseWindow();
    return;
  }

  widget_ = floating_window::CreateAndShow(this, browser_->GetProfile());

  // Start observing so that a close we did not initiate (Esc) still clears
  // `widget_`; otherwise the next press would call Close() on a dead Widget.
  widget_observation_.Observe(widget_.get());
}

void FloatingWindowToolbarButton::CloseWindow() {
  if (!widget_) {
    return;
  }

  // Order matters. Widget::Close() is asynchronous — it posts the destruction
  // rather than performing it inline — so OnWidgetDestroying() would arrive
  // later. Clearing the tracking up front means the button reports "not
  // showing" immediately, so a press that lands in the gap opens a fresh window
  // instead of being swallowed by a stale non-null `widget_`.
  //
  // Widget::CloseNow() would destroy synchronously and avoid the gap, but it is
  // discouraged: it can tear the widget down underneath code still on the stack
  // (event handling, the compositor).
  views::Widget* const widget = widget_;
  widget_observation_.Reset();
  widget_ = nullptr;
  widget->Close();
}

BEGIN_METADATA(FloatingWindowToolbarButton)
END_METADATA
