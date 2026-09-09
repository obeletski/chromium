// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_FLOATING_WINDOW_FLOATING_WINDOW_BUBBLE_H_
#define CHROME_BROWSER_UI_VIEWS_FLOATING_WINDOW_FLOATING_WINDOW_BUBBLE_H_

class Profile;

namespace views {
class View;
class Widget;
}  // namespace views

namespace floating_window {

// Creates and shows a floating, non-modal surface anchored to `anchor_view`
// that renders chrome://floating-window.
//
// Why a bubble and not a bare top-level views::Widget: a bubble already *is* an
// independent top-level widget with a shadow and a themed frame — the only
// thing "bubble" adds is that it positions itself relative to an anchor view
// and closes when that anchor goes away. Building a widget by hand would mean
// re-implementing Esc handling, focus, theming and teardown to arrive at the
// same behaviour.
//
// This is a free function rather than a class because
// views::BubbleDialogDelegate needs no subclassing here: everything is
// configured through setters, and the contents are supplied with
// SetContentsView(). (Subclassing the View flavour,
// views::BubbleDialogDelegateView, is not open to new code anyway — its
// constructors are private behind a `friend` allowlist.)
//
// Ownership: the returned Widget owns the delegate, which owns the contents
// view. Callers must not retain anything inside it; observe the Widget and drop
// the pointer in OnWidgetDestroying().
views::Widget* CreateAndShow(views::View* anchor_view, Profile* profile);

}  // namespace floating_window

#endif  // CHROME_BROWSER_UI_VIEWS_FLOATING_WINDOW_FLOATING_WINDOW_BUBBLE_H_
