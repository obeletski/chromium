// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tasks.tab_management;

import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.Callback;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.content_public.browser.WebContents;

import java.util.List;

/**
 * Turns the open tabs into a prose summary by asking a model.
 *
 * <p>Everything of substance happens in C++ ({@code
 * chrome/browser/android/tab_summary/tab_summary_bridge.cc}), which gathers each tab's h1/h2
 * headings from its renderer and then hands titles and headings to {@code
 * FloatingWindowSummarizer} -- the same browser-process class the desktop floating window uses, and
 * the reason this feature did not need a second prompt, a second API key path or a second traffic
 * annotation. What crosses back is one string, or null.
 *
 * <p><b>Why the request is built in three calls.</b> {@link #requestSummary} opens a native request,
 * adds the tabs one at a time and then closes it. The alternative -- one call taking arrays -- founders
 * on the {@link WebContents} array: jni_zero converts {@code String[]} to {@code
 * std::vector<std::string>} out of the box but has no conversion for a vector of {@code
 * content::WebContents*}, so the array would have to be walked element by element in C++ anyway. Doing
 * the walk in Java keeps each JNI signature trivial and mirrors how the native side is built.
 *
 * <p>The handle is opaque and short-lived: it is valid only between {@code createRequest} and {@code
 * start}, all three calls happen in one uninterrupted loop below, and the native object frees itself
 * once its callback has run. Nothing in Java stores it.
 *
 * <p><b>Incognito.</b> Callers must pass a regular profile. The native side CHECKs it, because a
 * summary of Incognito tabs must not leave the device; the card that calls this is scoped to the
 * regular tab switcher pane, so the check documents an invariant rather than guarding a live path.
 */
@NullMarked
public class TabSummaryBridge {
    private TabSummaryBridge() {}

    /**
     * Whether a summary can be produced at all: the feature flag is on and an API key is configured.
     *
     * <p>Worth calling before {@link #requestSummary} rather than relying on the null reply, because
     * the two mean different things to the user. Unavailable is a build that was never going to
     * summarise anything, and the card should not promise one; a null reply is a request that was
     * made and failed.
     */
    public static boolean isAvailable() {
        return TabSummaryBridgeJni.get().isAvailable();
    }

    /**
     * Summarises the given tabs.
     *
     * @param profile The regular profile. Must not be off the record.
     * @param tabs The tabs to describe, in the order they should appear in the prompt. A tab with no
     *     live renderer contributes its title only -- ordinary on Android, and not an error.
     * @param callback Run once, on the UI thread, with the summary text, or with null if the model
     *     could not be reached, answered with an error, or returned nothing usable.
     */
    public static void requestSummary(
            Profile profile, List<TabInfo> tabs, Callback<@Nullable String> callback) {
        TabSummaryBridge.Natives jni = TabSummaryBridgeJni.get();
        long request = jni.createRequest(profile, callback);
        for (TabInfo tab : tabs) {
            jni.addTab(request, tab.title, tab.webContents);
        }
        jni.start(request);
    }

    /**
     * One tab, reduced to what the prompt is allowed to see.
     *
     * <p>Named TabInfo rather than Tab because {@code org.chromium.chrome.browser.tab.Tab} is the
     * type callers already have in scope, and this is deliberately not that: it is the narrow lens
     * on a tab that the model gets, so that which bytes leave the device is obvious at the call
     * site. {@code SummaryInput} plays the same role on the C++ side.
     */
    public static class TabInfo {
        public final String title;
        public final @Nullable WebContents webContents;

        public TabInfo(String title, @Nullable WebContents webContents) {
            this.title = title;
            this.webContents = webContents;
        }
    }

    @NativeMethods
    interface Natives {
        boolean isAvailable();

        long createRequest(Profile profile, Callback<@Nullable String> callback);

        void addTab(long requestHandle, @JniType("std::string") String title, @Nullable WebContents webContents);

        void start(long requestHandle);
    }
}
