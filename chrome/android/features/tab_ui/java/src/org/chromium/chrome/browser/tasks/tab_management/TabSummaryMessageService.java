// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tasks.tab_management;

import static org.chromium.chrome.browser.tasks.tab_management.TabListModel.CardProperties.CARD_ALPHA;
import static org.chromium.chrome.browser.tasks.tab_management.TabListModel.CardProperties.CARD_TYPE;
import static org.chromium.chrome.browser.tasks.tab_management.TabListModel.CardProperties.ModelType.MESSAGE;

import android.content.Context;

import org.chromium.build.annotations.NullMarked;
import org.chromium.chrome.browser.tasks.tab_management.MessageCardView.ServiceDismissActionProvider;
import org.chromium.chrome.browser.tasks.tab_management.MessageCardViewProperties.MessageCardScope;
import org.chromium.chrome.browser.tasks.tab_management.TabProperties.UiType;
import org.chromium.chrome.browser.tasks.tab_management.TabSwitcherMessageManager.MessageType;
import org.chromium.chrome.tab_ui.R;
import org.chromium.ui.modelutil.PropertyModel;

/**
 * Serves the tab summary card that sits above the tab grid in the Hub's TAB_SWITCHER pane.
 *
 * <p>This is option A of {@code docs/floating_window/tab-summary-android-ui-alternatives.md}: the
 * summary appears where the user is already asking the question it answers, with the tabs it
 * describes on the same screen.
 *
 * <p><b>First step only.</b> The text is a hardcoded placeholder. Nothing here talks to a model
 * yet; the browser-process summarizer that produces the real text on desktop ({@code
 * FloatingWindowSummarizer}) is reusable as-is, and wiring it up means a JNI hop and a heading
 * source on Android, neither of which exists. See the TODO on {@link #PLACEHOLDER_TEXT}.
 *
 * <p>Why a {@link MessageService} rather than a view added to the pane's layout: the tab switcher
 * is a {@code RecyclerView}, and a card added outside it would not scroll with the grid, would not
 * survive the pane's view being swapped by {@code HubPaneHostMediator}, and would push the grid
 * down when it appeared. Message cards are list items -- {@link TabListCoordinator} inserts them
 * into the same model list as the tabs -- so they scroll, recycle and animate like everything else
 * in the grid. Registration is free: {@code MessageHostDelegateFactory} reads {@link #getUiType()},
 * {@link #getLayout()} and {@link #getBinder()} off this object when the service is subscribed.
 *
 * <p>The card is inserted at index 0 by {@link TabSwitcherMessageManager}, the same position {@code
 * ARCHIVED_TABS_MESSAGE} uses, which is what puts it above the first row of thumbnails rather than
 * after them.
 */
@NullMarked
public class TabSummaryMessageService
        extends MessageService<@MessageType Integer, @UiType Integer> {
    // TODO: Replace with the model's summary once the Android side can reach it. No bug id:
    // nothing in this checkout is filed upstream.
    // Hardcoded rather than added to strings.xml on purpose: a new IDS_ string needs a translation
    // screenshot that presubmit blocks on, and this text is scaffolding that will not ship. See
    // CLAUDE.md, "Why this checkout exists".
    private static final String PLACEHOLDER_TEXT = "Tabs summary view";

    private final Context mContext;

    /**
     * @param context Used only to resolve the dismiss button's content description.
     */
    TabSummaryMessageService(Context context) {
        // The existing small message-card layout and binder are reused rather than adding a new
        // view: at this stage the card is one line of text, and MessageCardView already handles
        // the dismiss button, the incognito palette and the grid's card metrics. A bespoke view
        // becomes worthwhile when the card grows the shimmer-while-loading state and the "Updated
        // just now" footer the design calls for.
        super(
                MessageType.TAB_SUMMARY_MESSAGE,
                UiType.TAB_SUMMARY_MESSAGE,
                R.layout.tab_grid_message_card_item,
                MessageCardViewBinder::bind);
        mContext = context;
    }

    /**
     * Queues the single card this service ever shows.
     *
     * <p>Queueing happens here rather than in the constructor because {@link #queueMessage} asserts
     * that the dismiss provider supplied by {@code initialize()} is already set -- the base class
     * needs somewhere to route a dismissal before a message can exist.
     */
    @Override
    public void initialize(
            ServiceDismissActionProvider<@MessageType Integer> serviceDismissActionProvider) {
        super.initialize(serviceDismissActionProvider);
        queueMessage(this::buildModel);
    }

    /**
     * Clears this service's record of the shown message, so that dismissing the card actually
     * dismisses it.
     *
     * <p>This is load-bearing and the reason is not local. The card's dismiss button runs two
     * providers in turn ({@code MessageCardViewBinder}): this one, then the *service* dismiss
     * provider, which is {@code TabSwitcherMessageManager#dismissHandler}. That handler removes the
     * item from the list and then, for every message type except PRICE_MESSAGE,
     * INCOGNITO_REAUTH_PROMO_MESSAGE and ARCHIVED_TABS_MESSAGE, calls {@code appendNextMessage()}
     * to put the next message of that type on screen. TAB_SUMMARY_MESSAGE is not in that exclusion
     * list, so without this call {@link MessageService#getNextMessageItem} would still be holding
     * the same message as "shown" and would hand it straight back -- the card would be removed and
     * re-added in one gesture, and the dismiss button would look broken.
     *
     * <p>{@link #dismissShownMessage} both clears that field and routes through the service dismiss
     * provider itself, so the removal still happens exactly once from the list's point of view.
     * {@code IphMessageService#dismiss} does the same thing for the same reason.
     */
    private void onDismissed() {
        dismissShownMessage();
    }

    private PropertyModel buildModel(
            ServiceDismissActionProvider<@MessageType Integer> serviceDismissActionProvider) {
        return new PropertyModel.Builder(MessageCardViewProperties.ALL_KEYS)
                .with(MessageCardViewProperties.MESSAGE_TYPE, MessageType.TAB_SUMMARY_MESSAGE)
                .with(
                        MessageCardViewProperties.MESSAGE_IDENTIFIER,
                        MessageService.DEFAULT_MESSAGE_IDENTIFIER)
                // Without this key the binder never attaches a dismiss listener: it wires the
                // listener inside the branch that handles the content description, so omitting the
                // description silently leaves the button inert rather than merely unlabelled.
                .with(
                        MessageCardViewProperties.DISMISS_BUTTON_CONTENT_DESCRIPTION,
                        mContext.getString(R.string.accessibility_tab_suggestion_dismiss_button))
                .with(
                        MessageCardViewProperties.MESSAGE_SERVICE_DISMISS_ACTION_PROVIDER,
                        serviceDismissActionProvider)
                .with(MessageCardViewProperties.UI_DISMISS_ACTION_PROVIDER, this::onDismissed)
                .with(MessageCardViewProperties.DESCRIPTION_TEXT, PLACEHOLDER_TEXT)
                // No action button: there is nothing to accept or review yet. The property has to
                // be set explicitly -- ALL_KEYS leaves it false-by-default only because false is
                // the boolean default, and being explicit is what documents the intent.
                .with(MessageCardViewProperties.ACTION_BUTTON_VISIBLE, false)
                .with(MessageCardViewProperties.IS_ICON_VISIBLE, false)
                .with(MessageCardViewProperties.IS_INCOGNITO, false)
                // REGULAR, not BOTH. The summary is produced from page headings sent to a remote
                // endpoint, so it must not appear over Incognito tabs -- the same rule the desktop
                // feature enforces with CHECK(!profile->IsOffTheRecord()). The Hub gives Incognito
                // its own pane (INCOGNITO_TAB_SWITCHER), so this is a visible boundary here rather
                // than a filter applied to a mixed list.
                .with(
                        MessageCardViewProperties
                                .MESSAGE_CARD_VISIBILITY_CONTROL_IN_REGULAR_AND_INCOGNITO_MODE,
                        MessageCardScope.REGULAR)
                .with(CARD_TYPE, MESSAGE)
                .with(CARD_ALPHA, 1f)
                .build();
    }
}
