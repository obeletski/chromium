// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tasks.tab_management;

import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.build.annotations.NullMarked;
import org.chromium.content_public.browser.WebContents;

/**
 * Asks a tab's renderer for its h1/h2 headings.
 *
 * <p>The work happens in C++ ({@code chrome/browser/android/tab_outline/tab_outline_bridge.cc}),
 * because the headings come from an accessibility tree snapshot and
 * {@code WebContents::RequestAXTreeSnapshot} has no Java equivalent. What crosses back is already
 * reduced to display lines: the level is baked in as a leading indent, since the card renders into
 * a single TextView and has nowhere to hang structure.
 *
 * <p><b>The Android-specific catch.</b> A snapshot needs a live renderer, and on Android a
 * backgrounded tab usually does not have one -- {@link org.chromium.chrome.browser.tab.Tab#getWebContents}
 * returns null for a tab that has been discarded or never loaded in this session. That is an
 * ordinary state, not an error. Callers must null-check and account for the tab themselves; this
 * class deliberately refuses to paper over it, because "no headings" and "tab not loaded" are
 * different facts and the card should not conflate them.
 */
@NullMarked
public class TabOutlineBridge {
    private TabOutlineBridge() {}

    /**
     * Requests one tab's outline.
     *
     * @param webContents A live WebContents. Must not be null -- see the class comment.
     * @param callback Run once, on the UI thread, with one string per h1/h2 in document order.
     *     Level-2 headings arrive indented by four spaces. An empty array means either that the
     *     page has no headings or that the renderer did not answer within the snapshot timeout;
     *     the two are indistinguishable here, as they are on desktop.
     */
    public static void requestOutline(WebContents webContents, Callback<String[]> callback) {
        TabOutlineBridgeJni.get().requestOutline(webContents, callback);
    }

    @NativeMethods
    interface Natives {
        void requestOutline(WebContents webContents, Callback<String[]> callback);
    }
}
