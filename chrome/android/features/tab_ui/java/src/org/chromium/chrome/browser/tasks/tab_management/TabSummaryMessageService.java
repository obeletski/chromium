// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tasks.tab_management;

import static org.chromium.build.NullUtil.assumeNonNull;
import static org.chromium.chrome.browser.tasks.tab_management.TabListModel.CardProperties.CARD_ALPHA;
import static org.chromium.chrome.browser.tasks.tab_management.TabListModel.CardProperties.CARD_TYPE;
import static org.chromium.chrome.browser.tasks.tab_management.TabListModel.CardProperties.ModelType.MESSAGE;

import android.content.Context;
import android.text.TextUtils;

import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tasks.tab_management.MessageCardView.ServiceDismissActionProvider;
import org.chromium.chrome.browser.tasks.tab_management.MessageCardViewProperties.MessageCardScope;
import org.chromium.chrome.browser.tasks.tab_management.TabProperties.UiType;
import org.chromium.chrome.browser.tasks.tab_management.TabSwitcherMessageManager.MessageType;
import org.chromium.chrome.tab_ui.R;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.modelutil.PropertyModel;
import org.chromium.url.GURL;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Supplier;

/**
 * Serves the tab summary card that sits above the tab grid in the Hub's TAB_SWITCHER pane.
 *
 * <p>This is option A of {@code docs/floating_window/tab-summary-android-ui-alternatives.md}: the
 * summary appears where the user is already asking the question it answers, with the tabs it
 * describes on the same screen.
 *
 * <p><b>What the card shows, in order of preference.</b> When {@link TabSummaryBridge#isAvailable}
 * is true -- the {@code FloatingWindowSummary} flag is on and an API key is configured -- the card
 * shows a model-written summary of the open tabs, produced by the same browser-process summarizer
 * the desktop floating window uses. Otherwise, and on any failure, it falls back to listing the
 * tabs and their h1/h2 headings through {@link TabOutlineBridge}. The listing is what the feature
 * was before the model call existed; keeping it as the fallback means a build with no key still
 * shows something true rather than an error.
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
    // How many tabs the card will name before it stops and counts the rest. A phone can easily
    // carry dozens of tabs, and this card sits above the grid -- letting it grow without bound
    // would push every thumbnail off screen, which is the one thing the design forbids.
    private static final int MAX_TABS = 6;

    // TODO: Replace this listing with the model's summary once the Android side can reach one.
    // Listing the tabs is scaffolding: it proves the card can read real state and re-render,
    // which is the part the summary will need. No bug id -- nothing here is filed upstream.
    //
    // All user-visible strings below are hardcoded rather than added to strings.xml: a new IDS_
    // string needs a translation screenshot that presubmit blocks on, and none of this will ship.
    // See CLAUDE.md, "Why this checkout exists".
    private static final String EMPTY_TEXT = "No open tabs.";
    private static final String SUMMARISING_TEXT = "Summarising your tabs...";

    // Prefixes the model's prose, and only the model's prose. The card has no title and no icon,
    // and the two things it can show -- a written summary and the heading listing -- are both
    // plain text in the same slot, so nothing on screen says which branch of refresh() produced
    // what is there. That matters because the fallback is silent by design: a missing key or a
    // failed request lands on listHeadings() without an error, and a reader who does not already
    // know the feature reads the listing as the summary having done a poor job.
    private static final String SUMMARY_PREFIX = "Summary: ";
    private static final String UNTITLED_TEXT = "(untitled)";
    private static final String NOT_LOADED_TEXT = "    (not loaded)";
    private static final String NO_HEADINGS_TEXT = "    (no headings)";

    private final Context mContext;
    private final Supplier<@Nullable TabModel> mTabModelSupplier;

    // The model behind the card, kept so the text can be rewritten after the fact. See refresh().
    private @Nullable PropertyModel mModel;

    // Incremented by each refresh() so that replies from a superseded gather can be dropped.
    private int mGeneration;

    /**
     * @param context Used only to resolve the dismiss button's content description.
     * @param tabModelSupplier The current tab model, read each time the card is refreshed. May
     *     supply null before the model exists, which {@link #refresh} treats as "no tabs".
     */
    TabSummaryMessageService(Context context, Supplier<@Nullable TabModel> tabModelSupplier) {
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
        mTabModelSupplier = tabModelSupplier;
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
     * Rewrites the card's text from the current tab model.
     *
     * <p>Two shapes, chosen by whether a model can be reached. With one, the tabs go to {@link
     * TabSummaryBridge} and the card shows the prose that comes back. Without, the card falls back
     * to listing the tabs and the h1/h2 headings gathered one tab at a time through {@link
     * TabOutlineBridge}. The fallback also catches a request that was made and failed, so a dead
     * network shows the listing rather than an error.
     *
     * <p>Neither shape is a simple getter. The model is built at {@link #initialize} -- {@link
     * MessageService#queueMessage} runs its factory immediately, and initialize() happens during
     * subscription, long before a tab is loaded -- so the text cannot be correct at construction.
     * And headings are not browser-side state: each one costs a round trip to a renderer, so the
     * text is assembled asynchronously and written when the last reply lands.
     *
     * <p>Setting {@code DESCRIPTION_TEXT} on the live model is enough to redraw. It is a writable
     * key, so the change processor calls {@code MessageCardViewBinder} for that key alone and the
     * view updates in place, keeping the card's position -- removing and re-adding would lose it.
     */
    public void refresh() {
        if (mModel == null) return;

        TabModel tabModel = mTabModelSupplier.get();
        if (tabModel == null || tabModel.getCount() == 0) {
            mModel.set(MessageCardViewProperties.DESCRIPTION_TEXT, EMPTY_TEXT);
            return;
        }

        // Guards against a late reply writing into a card that has been superseded by a newer
        // refresh(). Without it, switching panes twice in quick succession interleaves two runs.
        // Incremented once here, and passed down, so that a summary falling back to the listing
        // does not invalidate its own continuation.
        final int generation = ++mGeneration;

        // isOffTheRecord() is belt-and-braces: MessageCardScope.REGULAR already keeps this card out
        // of the Incognito pane, and the native side CHECKs. Three layers, because the failure this
        // prevents -- Incognito page headings reaching a remote endpoint -- is not recoverable.
        if (TabSummaryBridge.isAvailable() && !tabModel.isOffTheRecord()) {
            summarise(tabModel, generation);
        } else {
            listHeadings(tabModel, generation);
        }
    }

    /** Asks the model to describe the tabs, and falls back to the listing if it cannot. */
    private void summarise(TabModel tabModel, int generation) {
        assumeNonNull(mModel);
        Profile profile = tabModel.getProfile();
        if (profile == null) {
            listHeadings(tabModel, generation);
            return;
        }

        // Every tab, not just the first MAX_TABS: the cap below is about how many lines the card
        // can show, and the prompt has its own, larger cap (kMaxTabsToSnapshot, matching the
        // summarizer's kMaxTabsInPrompt). A summary that silently described the first six of forty
        // tabs would be wrong in a way the user could not see.
        List<TabSummaryBridge.TabInfo> tabs = new ArrayList<>();
        for (int i = 0; i < tabModel.getCount(); i++) {
            Tab tab = tabModel.getTabAt(i);
            if (tab == null) continue;
            tabs.add(new TabSummaryBridge.TabInfo(titleFor(tab), tab.getWebContents()));
        }
        if (tabs.isEmpty()) {
            mModel.set(MessageCardViewProperties.DESCRIPTION_TEXT, EMPTY_TEXT);
            return;
        }

        // Write the waiting state before the request, not after: the round trip is a renderer
        // snapshot plus a network call, which is seconds rather than frames, and the card would
        // otherwise sit showing the previous run's text with no sign that anything is happening.
        mModel.set(MessageCardViewProperties.DESCRIPTION_TEXT, SUMMARISING_TEXT);

        TabSummaryBridge.requestSummary(
                profile,
                tabs,
                summary -> {
                    if (generation != mGeneration || mModel == null) return;
                    if (summary == null) {
                        // No key, no network, an API error, or a reply with nothing usable in it.
                        // The bridge deliberately does not say which -- the details can carry quota
                        // information and key fragments, and they are logged natively instead.
                        listHeadings(tabModel, generation);
                        return;
                    }
                    mModel.set(
                            MessageCardViewProperties.DESCRIPTION_TEXT,
                            SUMMARY_PREFIX + summary.trim());
                });
    }

    /**
     * Lists the tabs and their headings: the card's original behaviour, now the fallback.
     *
     * <p>Kept rather than replaced because it is the only view of what the summary was built from.
     * When the prose is wrong, this is how to tell whether the model misread the headings or never
     * got them.
     */
    private void listHeadings(TabModel tabModel, int generation) {
        assumeNonNull(mModel);
        int total = tabModel.getCount();
        int shown = Math.min(total, MAX_TABS);

        // One slot per tab, filled in as replies arrive. Indexing by position rather than
        // appending is what keeps the output in tab order: the renderers answer in whatever order
        // they please, and a page with no headings answers instantly while a heavy one does not.
        List<String @Nullable []> outlines = new ArrayList<>();
        List<String> titles = new ArrayList<>();
        for (int i = 0; i < shown; i++) {
            outlines.add(null);
            titles.add("");
        }

        // Counts replies still outstanding. Starts at one extra so that a tab answering
        // synchronously -- which the null-WebContents path does -- cannot drive the count to zero
        // and publish a half-issued listing before the loop has finished. Released after the loop.
        // The desktop OutlineCollector holds the same extra count for the same reason.
        int[] pending = new int[] {1};

        for (int i = 0; i < shown; i++) {
            Tab tab = tabModel.getTabAt(i);
            if (tab == null) continue;
            titles.set(i, titleFor(tab));

            WebContents webContents = tab.getWebContents();
            if (webContents == null) {
                // Ordinary on Android: a backgrounded tab is frequently discarded, and a tab
                // restored from disk has never had a renderer this session. There is nothing to
                // ask, so say so rather than leaving the tab looking heading-less.
                outlines.set(i, new String[] {NOT_LOADED_TEXT});
                continue;
            }

            final int index = i;
            pending[0]++;
            TabOutlineBridge.requestOutline(
                    webContents,
                    headings -> {
                        if (generation != mGeneration) return;
                        outlines.set(index, headings);
                        if (--pending[0] == 0) publish(titles, outlines, total, shown);
                    });
        }

        pending[0]--;
        if (pending[0] == 0) publish(titles, outlines, total, shown);
    }

    /** Composes the finished listing and writes it to the card. */
    private void publish(
            List<String> titles, List<String @Nullable []> outlines, int total, int shown) {
        if (mModel == null) return;

        StringBuilder text = new StringBuilder();
        for (int i = 0; i < shown; i++) {
            if (text.length() > 0) text.append('\n');
            text.append(titles.get(i));

            String @Nullable [] headings = outlines.get(i);
            if (headings == null || headings.length == 0) {
                // Empty covers both "page has no h1/h2" and "the renderer did not answer inside
                // the snapshot timeout". The bridge cannot tell them apart, so neither can this.
                text.append('\n').append(NO_HEADINGS_TEXT);
                continue;
            }
            for (String heading : headings) {
                text.append('\n').append("    ").append(heading);
            }
        }
        if (total > shown) {
            text.append('\n').append("+ ").append(total - shown).append(" more tabs");
        }
        mModel.set(MessageCardViewProperties.DESCRIPTION_TEXT, text.toString());
    }

    /** The tab's title, or its URL where the title is empty. */
    private String titleFor(Tab tab) {
        String title = tab.getTitle();
        if (!TextUtils.isEmpty(title)) {
            return title;
        }
        // A tab restored from disk and not loaded has no title. The URL is the next best
        // identifier, and the spec rather than the host so two tabs on one site stay distinct.
        GURL url = tab.getUrl();
        return (url == null || url.getSpec().isEmpty()) ? UNTITLED_TEXT : url.getSpec();
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
        mModel =
                new PropertyModel.Builder(MessageCardViewProperties.ALL_KEYS)
                        .with(
                                MessageCardViewProperties.MESSAGE_TYPE,
                                MessageType.TAB_SUMMARY_MESSAGE)
                        .with(
                                MessageCardViewProperties.MESSAGE_IDENTIFIER,
                                MessageService.DEFAULT_MESSAGE_IDENTIFIER)
                        // Without this key the binder never attaches a dismiss listener: it wires
                        // the
                        // listener inside the branch that handles the content description, so
                        // omitting the
                        // description silently leaves the button inert rather than merely
                        // unlabelled.
                        .with(
                                MessageCardViewProperties.DISMISS_BUTTON_CONTENT_DESCRIPTION,
                                mContext.getString(
                                        R.string.accessibility_tab_suggestion_dismiss_button))
                        .with(
                                MessageCardViewProperties.MESSAGE_SERVICE_DISMISS_ACTION_PROVIDER,
                                serviceDismissActionProvider)
                        .with(
                                MessageCardViewProperties.UI_DISMISS_ACTION_PROVIDER,
                                this::onDismissed)
                        // Correct as of now, which is usually "no tabs"; refresh() writes the real
                        // listing once the grid has been populated.
                        .with(MessageCardViewProperties.DESCRIPTION_TEXT, EMPTY_TEXT)
                        // No action button: there is nothing to accept or review yet. The property
                        // has to
                        // be set explicitly -- ALL_KEYS leaves it false-by-default only because
                        // false is
                        // the boolean default, and being explicit is what documents the intent.
                        .with(MessageCardViewProperties.ACTION_BUTTON_VISIBLE, false)
                        .with(MessageCardViewProperties.IS_ICON_VISIBLE, false)
                        .with(MessageCardViewProperties.IS_INCOGNITO, false)
                        // REGULAR, not BOTH. The summary is produced from page headings sent to a
                        // remote
                        // endpoint, so it must not appear over Incognito tabs -- the same rule the
                        // desktop
                        // feature enforces with CHECK(!profile->IsOffTheRecord()). The Hub gives
                        // Incognito
                        // its own pane (INCOGNITO_TAB_SWITCHER), so this is a visible boundary here
                        // rather
                        // than a filter applied to a mixed list.
                        .with(
                                MessageCardViewProperties
                                        .MESSAGE_CARD_VISIBILITY_CONTROL_IN_REGULAR_AND_INCOGNITO_MODE,
                                MessageCardScope.REGULAR)
                        .with(CARD_TYPE, MESSAGE)
                        .with(CARD_ALPHA, 1f)
                        .build();
        return mModel;
    }
}
