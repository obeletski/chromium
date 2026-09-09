// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_TOOLBAR_FLOATING_WINDOW_TOOLBAR_BUTTON_H_
#define CHROME_BROWSER_UI_VIEWS_TOOLBAR_FLOATING_WINDOW_TOOLBAR_BUTTON_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

class Browser;

// Toolbar button that toggles the floating chrome://floating-window surface.
// ToolbarView adds it immediately before the avatar button, so it sits between
// the extensions area and the profile / Incognito indicator.
//
// Deriving from ToolbarButton (rather than views::Button) is what makes it look
// like a toolbar control: the ink drop, the hover highlight, the toolbar-sized
// icon and insets, and the standard focus ring all come from there.
//
// Ownership: this class owns nothing. The bubble's Widget owns its own delegate
// and contents. The only state kept here is `widget_`, a non-owning pointer
// used to answer one question — "is the surface currently up?" — so that a
// second press closes it instead of stacking a second one. Because the bubble
// can also close itself (Esc), that pointer has to be invalidated from the
// outside, which is what the WidgetObserver base is for.
class FloatingWindowToolbarButton : public ToolbarButton,
                                    public views::WidgetObserver {
  // Registers the class with the views metadata system, which powers
  // view-tree introspection, the views inspector and IsViewClass<T>() casts.
  // Paired with BEGIN_METADATA/END_METADATA in the .cc.
  METADATA_HEADER(FloatingWindowToolbarButton, ToolbarButton)

 public:
  explicit FloatingWindowToolbarButton(Browser* browser);
  FloatingWindowToolbarButton(const FloatingWindowToolbarButton&) = delete;
  FloatingWindowToolbarButton& operator=(const FloatingWindowToolbarButton&) =
      delete;
  ~FloatingWindowToolbarButton() override;

  bool IsWindowShowing() const { return widget_ != nullptr; }

  // views::WidgetObserver:
  // Called when the bubble goes away for any reason we did not initiate —
  // Esc, or the browser window being torn down.
  void OnWidgetDestroying(views::Widget* widget) override;

 private:
  // Bound as the button's PressedCallback in the constructor.
  void OnPressed();

  // Idempotent: safe to call when nothing is showing.
  void CloseWindow();

  const raw_ptr<Browser> browser_;

  // Non-owning. Null exactly when no floating window is showing.
  raw_ptr<views::Widget> widget_ = nullptr;

  // Scoped so the observation is torn down automatically if this button is
  // destroyed while still observing, which would otherwise leave the Widget
  // holding a dangling observer pointer.
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
};

#endif  // CHROME_BROWSER_UI_VIEWS_TOOLBAR_FLOATING_WINDOW_TOOLBAR_BUTTON_H_
