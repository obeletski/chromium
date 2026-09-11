// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/floating_window/floating_window_ui.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_collection.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/floating_window/floating_window_summarizer.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_role_properties.h"
#include "ui/accessibility/ax_tree_update.h"
#include "url/gurl.h"

namespace {

// The static half of the page: everything up to the point where the generated
// table is spliced in.
//
// The usual way to ship a WebUI page is a build_webui() GN target: HTML/TS/CSS
// files on disk, compiled and packed into a .pak, reached from C++ by resource
// ID (IDR_*) via WebUIDataSource::AddResourcePath(). That is the right shape
// for anything with real markup or script. This page has neither — the markup
// is assembled in C++ below — so the bytes live here instead.
//
// Content Security Policy note, and the reason the table is built in C++ at
// all. Data sources get a default CSP from
// URLDataSource::GetContentSecurityPolicy(). For a trusted chrome:// source it
// leaves style-src unset, so the inline <style> below is allowed. script-src,
// however, resolves to "chrome://resources 'self'" (see
// content/public/browser/url_data_source.cc), which permits no *inline*
// script at all — an inline <script> here would be silently blocked at
// runtime, with no console error reaching anywhere visible. Serving a separate
// same-origin .js file would satisfy 'self' and is how a live-updating version
// of this page would have to be written (plus a WebUIMessageHandler or Mojo
// interface to push tab changes across the process boundary). This page takes
// the cheaper route: it renders everything into the response in the browser
// process. For the tab list that is nearly free, since the browser already
// holds it. For the per-page outlines it is not -- those live in each tab's
// renderer -- but they are fetched with an accessibility-tree snapshot rather
// than script in this page, so the document served here stays inert. See
// OutlineCollector below for how the replies are gathered, and the class
// comment there for why the response has to be produced asynchronously.
//
// The colors are CSS system colors plus `color-scheme: light dark`, so the page
// follows the OS/browser light or dark theme without the browser having to push
// any color values into it. `color-mix(... canvastext ...)` derives the muted
// and border tones from that same system color, so they track the theme too
// rather than being hardcoded to one of light/dark.
constexpr char kPageHead[] = R"(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Open tabs</title>
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
      /* This min-width is what actually sizes the window, and it is not
         cosmetic. The bubble sizes itself from the renderer's reported content
         size (RenderWidgetHostView auto-resize; see the sizing chain in
         floating_window_bubble.cc). A `width: 100%` table has no intrinsic
         width -- it is happy at any size -- so with nothing to report the
         window collapsed to the auto-resize *minimum* and every column
         ellipsized down to a few characters. Giving the page a floor gives
         auto-resize a number to grow to. It stays below the width in kMaxSize
         so the bubble is never clamped horizontally. */
      min-width: 660px;
      padding: 16px 18px;
    }
    h1 {
      font-size: 13px;
      font-weight: 600;
      margin: 0 0 10px;
    }
    .count {
      color: color-mix(in srgb, canvastext 55%, canvas);
      font-weight: 400;
    }
    table {
      border-collapse: collapse;
      /* table-layout: fixed with explicit column widths is what lets the URL
         cell ellipsize. With the default `auto` layout the cell would size to
         its content, so `text-overflow` would never have an overflow to act
         on and every long URL would widen the window to the bubble's maximum. */
      table-layout: fixed;
      width: 100%;
    }
    th, td {
      overflow: hidden;
      padding: 5px 8px 5px 0;
      text-align: left;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    /* A rule per row would draw a line under every heading too, turning the
       outline into a grid. Bordering the top of each tab row instead means one
       line per tab, with its outline hanging below it inside the group. */
    tr.tab td {
      border-top: 1px solid color-mix(in srgb, canvastext 15%, canvas);
    }
    thead th {
      border-bottom: 1px solid color-mix(in srgb, canvastext 15%, canvas);
    }
    /* Outline rows. The indent is on the cell rather than the row because the
       row also holds the empty index cell, which must stay aligned with the
       tab indices above it. */
    tr.hd td {
      padding-bottom: 1px;
      padding-top: 1px;
    }
    tr.hd.lvl1 td:last-child {
      padding-left: 16px;
    }
    tr.hd.lvl2 td:last-child {
      color: color-mix(in srgb, canvastext 70%, canvas);
      padding-left: 34px;
    }
    tr.hd.note td:last-child {
      color: color-mix(in srgb, canvastext 45%, canvas);
      font-style: italic;
      padding-left: 16px;
    }
    /* The last outline row of a tab needs a little air before the next tab. */
    tr.hd:last-child td {
      padding-bottom: 5px;
    }
    thead th, tbody th {
      color: color-mix(in srgb, canvastext 55%, canvas);
      font-weight: 600;
    }
    /* One <tbody> per browser window, introduced by a full-width header row. */
    tbody th[scope="colgroup"] {
      padding-top: 12px;
    }
    .idx {
      color: color-mix(in srgb, canvastext 55%, canvas);
      text-align: right;
      width: 2.4em;
    }
    .url {
      color: color-mix(in srgb, canvastext 55%, canvas);
      width: 45%;
    }
    /* The tab that is foreground in its own window. */
    tr[aria-current="true"] td {
      font-weight: 600;
    }
    tr[aria-current="true"] .idx::after {
      /* A marker in the index column rather than a background tint: a tint
         would fight the system canvas color in one of the two themes. */
      content: "\25B8";  /* Black right-pointing small triangle. */
      padding-left: 4px;
    }
    .empty {
      color: color-mix(in srgb, canvastext 55%, canvas);
      margin: 0;
    }
    /* The summary frame. A fixed height is not a stylistic choice: an iframe
       cannot resize its parent without script, and this page has none, so the
       frame has to reserve its space up front and scroll if the summary runs
       long. It is sized for the two or three sentences the prompt asks for. */
    .summary {
      background: color-mix(in srgb, canvastext 4%, canvas);
      border: 1px solid color-mix(in srgb, canvastext 15%, canvas);
      border-radius: 4px;
      display: block;
      height: 76px;
      margin-top: 14px;
      overflow: auto;
      padding: 0;
      width: 100%;
    }
  </style>
</head>
<body>
)";

constexpr char kPageTail[] = R"(</body>
</html>
)";

// The summary lives in its own document, framed by the main one.
//
// The point of the iframe is latency. A summary needs a model round trip of a
// second or more, and the tab list does not -- so rather than making the whole
// page wait (which is what folding the request into OutlineCollector would
// mean), the tab list is served at once and the summary is served separately,
// by a second request whose GotDataCallback is simply held until the model
// answers. Both are still HTML composed in C++, both are still script-free, and
// the user still sees one navigation.
//
// The height is fixed because an iframe cannot resize its parent without
// script, and script is exactly what this page does not have. `overflow:auto`
// makes an over-long summary scroll rather than clip.
constexpr char kSummaryFrameFormat[] = R"(<iframe class="summary" src="%s"
  title="Summary of open tabs" sandbox="allow-same-origin"></iframe>
)";

// The framed document. Minimal by design: it inherits nothing from the parent,
// so it carries its own colours, and it is the only place model-authored text
// is rendered.
constexpr char kSummaryDocHead[] = R"(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <style>
    html, body { margin: 0; padding: 0; }
    body {
      background: canvas;
      color: canvastext;
      color-scheme: light dark;
      font: 13px system-ui, sans-serif;
    }
    p { margin: 0; }
    .muted { color: color-mix(in srgb, canvastext 55%, canvas); font-style: italic; }
  </style>
</head>
<body>
)";

constexpr char kSummaryDocTail[] = R"(</body>
</html>
)";

// Escapes `text` for interpolation into the document as an element's text
// content. base::EscapeForHTML() covers &, <, >, " and ', which is also enough
// for a double-quoted attribute value — used below for the title= tooltip.
//
// Escaping is not optional here even though the strings come from inside the
// browser: a page title is attacker-controlled content (a site picks its own
// <title>), and this response is served on a chrome:// origin with access to
// whatever that origin can reach. Interpolating a raw title would be a
// straightforward injection into privileged UI.
std::string Escaped(std::u16string_view text) {
  return base::EscapeForHTML(base::UTF16ToUTF8(text));
}

std::string Escaped(std::string_view text) {
  return base::EscapeForHTML(text);
}

// Caps and deadlines. Every one of these exists because the outline data comes
// from N other processes, and any of them can be slow, huge, or unresponsive.
//
// kSnapshotTimeout is handed to the renderer: it truncates the tree it
// serializes. kOverallDeadline is enforced here instead, and covers the case
// the renderer-side timeout cannot -- a renderer that never answers at all, so
// that neither the reply nor the callback's destruction ever arrives. Without
// it a single wedged tab would leave the floating window blank forever, since
// the WebUI response is only produced once every tab has reported.
constexpr int kMaxHeadingsPerTab = 12;
constexpr size_t kMaxHeadingBytes = 300;
constexpr size_t kMaxAxNodesPerTab = 20000;
constexpr base::TimeDelta kSnapshotTimeout = base::Milliseconds(1200);
constexpr base::TimeDelta kOverallDeadline = base::Milliseconds(2000);

// The accessibility mode requested for each snapshot.
//
// kExtendedProperties is the load-bearing half and the trap. Blink only
// serializes a heading's level under that flag
// (third_party/blink/renderer/modules/accessibility/ax_object.cc, the
// `IsHeading(role) && HeadingLevel()` branch), so asking for plain kWebContents
// yields heading nodes whose kHierarchicalLevel is absent -- every heading
// reads as level 0 and the h1/h2 filter below drops all of them. Nothing warns
// about it; the outline simply comes back empty.
//
// The in-tree snapshot callers all pass ui::kAXModeWebContentsOnly, which is
// these two flags plus kInlineTextBoxes. That third flag makes the renderer lay
// out and serialize per-word text boxes for the whole document, which is real
// work multiplied by every open tab, and nothing here reads them. So the mode
// is spelled out rather than reused.
constexpr ui::AXMode kOutlineAXMode(ui::AXMode::kWebContents |
                                    ui::AXMode::kExtendedProperties);

// One entry of a page's outline.
struct Heading {
  int level;
  std::string text;
};

// Everything needed to render one row, captured up front.
//
// Note that this deliberately holds *copies* rather than a WebContents*. The
// snapshots complete asynchronously, and a tab can be closed while they are in
// flight; holding the title and URL by value means a tab that disappears
// mid-gather still renders as the row it was, instead of dangling.
struct TabEntry {
  int window_number = 0;
  int window_tab_count = 0;
  int index_in_window = 0;
  bool is_active = false;
  std::u16string title;
  std::string url;

  // Filled in asynchronously. `snapshot_returned` stays false for a tab whose
  // renderer was not live, or which missed kOverallDeadline -- which is a
  // different thing from a page that genuinely has no headings, and is
  // rendered differently.
  std::vector<Heading> outline;
  bool snapshot_returned = false;
};

// Pulls the h1/h2 outline out of one accessibility tree snapshot.
//
// An AXTreeUpdate is a flat vector of nodes, not a tree -- the structure lives
// in each node's child_ids. It does not have to be walked recursively here
// because the snapshotter serializes in document order, which is exactly the
// order an outline wants; reading `nodes` straight through preserves it.
//
// Levels are the *accessible* heading level, which is not quite the tag name.
// Blink's HeadingLevel() (ax_node_object.cc) resolves, in order:
//
//   1. aria-level on a role="heading" element, if it is in 1..9. So a
//      <div role="heading" aria-level="2"> counts as an h2 here, and an
//      aria-level="4" is excluded — both verified against a real page.
//   2. otherwise the tag, h1..h6, plus GetComputedHeadingOffset().
//
// That offset is worth knowing about and easy to over-read. It comes from the
// `headingoffset` / `headingreset` content attributes (element.cc), *not* from
// nesting inside <section> or <article>, and it is gated on the HeadingOffset
// runtime feature, which is status "experimental" and so off in a default
// build. On any normal page today level == tag number: an <h1> nested two
// <section>s deep still reports 1, which was checked rather than assumed.
std::vector<Heading> ExtractOutline(const ui::AXTreeUpdate& update) {
  std::vector<Heading> outline;
  for (const ui::AXNodeData& node : update.nodes) {
    if (!ui::IsHeading(node.role)) {
      continue;
    }
    const int level =
        node.GetIntAttribute(ax::mojom::IntAttribute::kHierarchicalLevel);
    if (level != 1 && level != 2) {
      continue;
    }

    // The accessible name of a heading is its computed text content, which can
    // carry the source's line breaks and indentation. Collapse it so a heading
    // wrapped across several lines in the markup renders as one line.
    std::string text = base::CollapseWhitespaceASCII(
        node.GetStringAttribute(ax::mojom::StringAttribute::kName),
        /*trim_sequences_with_line_breaks=*/true);
    if (text.empty()) {
      continue;
    }

    // CSS ellipsizes what is too wide to show, but the bytes would still be in
    // the response, and a page is free to have a pathologically long heading.
    // TruncateUTF8ToByteSize() cuts on a character boundary rather than
    // mid-sequence, which naive substr() would not.
    if (text.size() > kMaxHeadingBytes) {
      text = std::string(base::TruncateUTF8ToByteSize(text, kMaxHeadingBytes));
    }
    outline.push_back({level, std::move(text)});
  }
  return outline;
}

// Renders the outline rows that follow one tab's row.
//
// These are rows of the same table rather than a nested <ul> so that the index
// column keeps its width and the outline stays visually hung off it. Each row
// spans the title and URL columns, and the indent comes from the level class.
std::string BuildOutlineRowsHtml(const TabEntry& tab) {
  if (!tab.snapshot_returned) {
    return "<tr class=\"hd note\"><td class=\"idx\"></td>"
           "<td colspan=\"2\">outline unavailable</td></tr>\n";
  }
  if (tab.outline.empty()) {
    return "<tr class=\"hd note\"><td class=\"idx\"></td>"
           "<td colspan=\"2\">no level 1 or 2 headings</td></tr>\n";
  }

  std::string html;
  int shown = 0;
  for (const Heading& heading : tab.outline) {
    if (shown == kMaxHeadingsPerTab) {
      base::StrAppend(&html, {"<tr class=\"hd note\"><td class=\"idx\"></td>"
                              "<td colspan=\"2\">+ ",
                              base::NumberToString(tab.outline.size() - shown),
                              " more</td></tr>\n"});
      break;
    }
    base::StrAppend(
        &html,
        {"<tr class=\"hd lvl", base::NumberToString(heading.level),
         "\"><td class=\"idx\"></td><td colspan=\"2\" title=\"",
         Escaped(heading.text), "\">", Escaped(heading.text), "</td></tr>\n"});
    ++shown;
  }
  return html;
}

// Renders the whole document body from the gathered entries. `summary_frame` is
// the <iframe> element for the summary, or empty when the summary is off.
std::string BuildPageBodyHtml(const std::vector<TabEntry>& tabs,
                              const std::string& summary_frame) {
  if (tabs.empty()) {
    return "<p class=\"empty\">No open tabs.</p>\n";
  }

  std::string rows;
  int current_window = 0;
  int window_count = 0;
  for (const TabEntry& tab : tabs) {
    // A new <tbody> per browser window. The entries arrive grouped by window
    // (they were collected that way), so a change in window_number is the
    // group boundary.
    if (tab.window_number != current_window) {
      if (current_window != 0) {
        rows += "</tbody>\n";
      }
      current_window = tab.window_number;
      ++window_count;
      base::StrAppend(
          &rows,
          {"<tbody><tr><th scope=\"colgroup\" colspan=\"3\">Window ",
           base::NumberToString(tab.window_number), " · ",
           base::NumberToString(tab.window_tab_count),
           tab.window_tab_count == 1 ? " tab" : " tabs", "</th></tr>\n"});
    }

    base::StrAppend(
        &rows,
        {"<tr class=\"tab\"", tab.is_active ? " aria-current=\"true\"" : "",
         "><td class=\"idx\">", base::NumberToString(tab.index_in_window),
         "</td><td title=\"", Escaped(tab.title), "\">",
         tab.title.empty() ? std::string("&mdash;") : Escaped(tab.title),
         "</td><td class=\"url\" title=\"", Escaped(tab.url), "\">",
         Escaped(tab.url), "</td></tr>\n"});
    rows += BuildOutlineRowsHtml(tab);
  }
  rows += "</tbody>\n";

  return base::StrCat(
      {"<h1>Open tabs <span class=\"count\">",
       base::NumberToString(tabs.size()),
       tabs.size() == 1 ? " tab in " : " tabs in ",
       base::NumberToString(window_count),
       window_count == 1 ? " window" : " windows",
       "</span></h1>\n<table><thead><tr><th class=\"idx\">#</th><th>Title</th>"
       "<th>URL</th></tr></thead>\n",
       rows, "</table>\n",
       // After the table, per the requirement. Empty when the summary is off,
       // so a build without a key renders exactly what it did before.
       summary_frame});
}

// Holds a summary that is being fetched, so the two requests can find it.
//
// The problem this solves: the tab list and the summary arrive as *two*
// navigations, each with its own GotDataCallback. The first request starts the
// model call; the second, moments later, has to attach to it. Something has to
// outlive both, and it cannot be the data source -- the subframe navigation
// constructs a second FloatingWindowUI, whose CreateAndAdd() *replaces* the
// source. In-flight loads survive that (they hold a scoped_refptr to it), but
// anything stored on the source would be lost.
//
// So it lives on the BrowserContext, which is a base::SupportsUserData and
// outlives every navigation in the profile. The token in the iframe URL is a
// correlation id rather than a capability: both requests are same-origin from
// the same profile, and it exists so that two windows opened in quick
// succession do not collide.
class SummaryRegistry : public base::SupportsUserData::Data {
 public:
  static constexpr char kUserDataKey[] = "FloatingWindowSummaryRegistry";

  struct Entry {
    Entry();
    ~Entry();
    Entry(Entry&&);
    Entry& operator=(Entry&&);

    std::unique_ptr<floating_window::FloatingWindowSummarizer> summarizer;

    // Whether the model has answered *at all*. This is deliberately separate
    // from `summary` having a value: nullopt is a legitimate answer meaning
    // "the request failed", and conflating the two made a failed request park
    // the frame forever, waiting for a reply that had already come. Two
    // questions, two fields.
    bool answered = false;
    std::optional<std::string> summary;

    // Set if the frame asked before the model answered.
    content::WebUIDataSource::GotDataCallback waiting;
  };

  static SummaryRegistry& GetOrCreate(Profile* profile) {
    auto* existing =
        static_cast<SummaryRegistry*>(profile->GetUserData(kUserDataKey));
    if (!existing) {
      auto owned = std::make_unique<SummaryRegistry>();
      existing = owned.get();
      profile->SetUserData(kUserDataKey, std::move(owned));
    }
    return *existing;
  }

  SummaryRegistry() = default;
  ~SummaryRegistry() override = default;

  Entry& Create(const std::string& token) {
    // Bound the map. Each window press mints a token, and a user who toggles
    // the window repeatedly would otherwise accumulate entries for summaries
    // nobody will ever read.
    if (entries_.size() >= kMaxEntries) {
      entries_.clear();
    }
    return entries_[token];
  }

  Entry* Find(const std::string& token) {
    auto it = entries_.find(token);
    return it == entries_.end() ? nullptr : &it->second;
  }

  void Erase(const std::string& token) { entries_.erase(token); }

 private:
  static constexpr size_t kMaxEntries = 8;
  std::map<std::string, Entry> entries_;
};

SummaryRegistry::Entry::Entry() = default;
SummaryRegistry::Entry::~Entry() = default;
SummaryRegistry::Entry::Entry(Entry&&) = default;
SummaryRegistry::Entry& SummaryRegistry::Entry::operator=(Entry&&) = default;

// Renders the summary document served into the iframe.
//
// The model's output is untrusted text rendered into a chrome:// document, so
// it goes through Escaped() exactly like a page title does. It is inserted as
// text, never as markup: the prompt asks for plain text precisely so that
// rendering it verbatim is both safe and legible.
std::string BuildSummaryDocument(const std::optional<std::string>& summary) {
  std::string body;
  // std::nullopt is failure -- no key, no network, an API error. An empty
  // string would mean the model answered with nothing, which it should not do;
  // both render as unavailable rather than as a confident blank.
  if (!summary.has_value() || summary->empty()) {
    body = "<p class=\"muted\">Summary unavailable.</p>\n";
  } else {
    base::StrAppend(&body, {"<p>", Escaped(*summary), "</p>\n"});
  }
  return base::StrCat({kSummaryDocHead, body, kSummaryDocTail});
}

// Owns the in-flight gather and produces the response when it settles.
//
// Ref-counted because two independent things keep it alive and either may
// outlive the other: the per-tab snapshot callbacks, and this object's own
// deadline timer. The last reference to drop destroys it.
//
// The whole class exists because of one asymmetry. The tab *list* is
// browser-process state and is read synchronously; the tab *outlines* live in
// N renderer processes and arrive one reply at a time. WebUIDataSource's
// GotDataCallback is explicitly allowed to be answered later, so the page is
// simply not composed until the replies settle -- no placeholder document, no
// second navigation, and still no script in the page.
class OutlineCollector : public base::RefCounted<OutlineCollector> {
 public:
  OutlineCollector(std::vector<TabEntry> tabs,
                   content::WebUIDataSource::GotDataCallback callback)
      : tabs_(std::move(tabs)), callback_(std::move(callback)) {}

  // The <iframe> element to place after the table, or empty for no summary.
  // Set before the snapshots settle; the collector never inspects it.
  void set_summary_frame(std::string frame) {
    summary_frame_ = std::move(frame);
  }

  // Read once the snapshots have settled, to build the model's prompt. Returns
  // only http(s) tabs: the tab list also contains chrome://settings,
  // devtools:// and file:// tabs, whose headings ("Passwords", "Payment
  // methods") must not leave the machine.
  std::vector<floating_window::SummaryInput> BuildSummaryInput() const {
    std::vector<floating_window::SummaryInput> input;
    for (const TabEntry& tab : tabs_) {
      const GURL url(tab.url);
      if (!url.SchemeIsHTTPOrHTTPS()) {
        continue;
      }
      floating_window::SummaryInput entry;
      entry.title = base::UTF16ToUTF8(tab.title);
      for (const Heading& heading : tab.outline) {
        entry.headings.push_back(heading.text);
      }
      input.push_back(std::move(entry));
    }
    return input;
  }

  OutlineCollector(const OutlineCollector&) = delete;
  OutlineCollector& operator=(const OutlineCollector&) = delete;

  // Balanced by ResolveOne(). Callers hold one extra "still issuing" count
  // across the request loop so that a snapshot completing synchronously cannot
  // drive the count to zero and publish a half-issued page.
  void AddPending() { ++pending_; }

  void ResolveOne() {
    CHECK_GT(pending_, 0u);
    if (--pending_ == 0) {
      Finish();
    }
  }

  void OnSnapshot(size_t index, ui::AXTreeUpdate& update) {
    tabs_[index].snapshot_returned = true;
    tabs_[index].outline = ExtractOutline(update);
    ResolveOne();
  }

  // Called just before the page is published, with the headings complete. This
  // is the earliest moment the prompt can be built, and deliberately the last
  // thing that happens before the tab list goes out -- the model call must not
  // delay it.
  void set_on_finished(base::OnceCallback<void(const OutlineCollector&)> cb) {
    on_finished_ = std::move(cb);
  }

  void StartDeadline() {
    // base::Unretained is safe: the timer is a member, so it is cancelled by
    // this object's destructor and cannot outlive it.
    deadline_.Start(
        FROM_HERE, kOverallDeadline,
        base::BindOnce(&OutlineCollector::Finish, base::Unretained(this)));
  }

 private:
  friend class base::RefCounted<OutlineCollector>;
  ~OutlineCollector() = default;

  // Reached either by the last snapshot resolving or by the deadline, and it
  // must run exactly once: GotDataCallback is a OnceCallback, and running it
  // twice would be a use-after-move. Tabs that have not reported by now render
  // as "outline unavailable".
  void Finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    deadline_.Stop();
    if (on_finished_) {
      std::move(on_finished_).Run(*this);
    }
    std::move(callback_).Run(
        base::MakeRefCounted<base::RefCountedString>(base::StrCat(
            {kPageHead, BuildPageBodyHtml(tabs_, summary_frame_), kPageTail})));
  }

  std::vector<TabEntry> tabs_;
  std::string summary_frame_;
  content::WebUIDataSource::GotDataCallback callback_;
  size_t pending_ = 0;
  bool finished_ = false;
  base::OneShotTimer deadline_;
  base::OnceCallback<void(const OutlineCollector&)> on_finished_;
};

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
// The response is handed back through a callback rather than returned because a
// data source is allowed to answer asynchronously, and this one now genuinely
// needs to. The tab list is read synchronously here; the outlines are requested
// from every tab's renderer and arrive later, so `callback` is moved into an
// OutlineCollector and run once the replies settle.
//
// `profile` is bound by the constructor below. A raw pointer is safe because
// the data source is owned by the URLDataManager keyed on that same
// BrowserContext, so the source is torn down with the profile and this callback
// cannot outlive it.
// Runs when the model answers. Either the frame is already waiting -- in which
// case its parked callback runs now -- or the frame has not asked yet and the
// answer is stored for it to collect.
void OnSummaryReady(Profile* profile,
                    const std::string& token,
                    std::optional<std::string> summary) {
  SummaryRegistry& registry = SummaryRegistry::GetOrCreate(profile);
  SummaryRegistry::Entry* entry = registry.Find(token);
  if (!entry) {
    return;  // The window was closed, or the entry was evicted.
  }

  entry->answered = true;
  entry->summary = std::move(summary);
  if (entry->waiting) {
    std::move(entry->waiting)
        .Run(base::MakeRefCounted<base::RefCountedString>(
            BuildSummaryDocument(entry->summary)));
    registry.Erase(token);
  }
  // Otherwise the frame has not navigated yet; HandleSummaryRequest() will find
  // the stored answer and serve it immediately.
}

// Serves the framed summary document for "summary?token=...".
//
// Three outcomes, and the middle one is the reason the iframe exists: if the
// model has not answered yet, this callback is *parked* on the registry entry
// and run later. The frame stays blank meanwhile, and the tab list -- already
// on screen -- is unaffected.
void HandleSummaryRequest(Profile* profile,
                          const std::string& path,
                          content::WebUIDataSource::GotDataCallback callback) {
  const size_t query = path.find('?');
  const std::string token =
      query == std::string::npos ? std::string() : path.substr(query + 1);

  SummaryRegistry::Entry* entry =
      SummaryRegistry::GetOrCreate(profile).Find(token);
  if (!entry) {
    // No such request in flight: a stale frame from a previous window, or a
    // hand-typed URL. Render the same "unavailable" the timeout renders.
    std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
        BuildSummaryDocument(std::nullopt)));
    return;
  }

  if (entry->answered) {
    std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
        BuildSummaryDocument(entry->summary)));
    SummaryRegistry::GetOrCreate(profile).Erase(token);
    return;
  }

  // Still waiting on the model. Hold the callback; OnSummaryReady() runs it.
  entry->waiting = std::move(callback);
}

void HandleRequest(Profile* profile,
                   const std::string& path,
                   content::WebUIDataSource::GotDataCallback callback) {
  // Two documents come from this one filter: the tab list at any other path,
  // and the summary fragment at "summary". ShouldHandleRequest() answers every
  // path unconditionally, so this branch is the whole routing table.
  if (base::StartsWith(path, "summary", base::CompareCase::SENSITIVE)) {
    HandleSummaryRequest(profile, path, std::move(callback));
    return;
  }

  // Profile scoping. ProfileBrowserCollection is per-profile, so an Incognito
  // window's floating window lists only Incognito tabs and a regular window's
  // lists only regular ones. That falls out of the data source being registered
  // per-BrowserContext (see the constructor below) and is the behaviour you
  // want: leaking Incognito titles into a regular-profile page would be a real
  // bug.
  ProfileBrowserCollection* collection =
      ProfileBrowserCollection::GetForProfile(profile);
  if (!collection) {
    // GetForProfile() is a KeyedService lookup and returns null for profiles
    // that do not get one (its own GetOffTheRecordBrowserCount() null-checks
    // the same call), so this is a real case, not a defensive flourish.
    std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
        base::StrCat({kPageHead,
                      "<p class=\"empty\">No browser windows for this "
                      "profile.</p>\n",
                      kPageTail})));
    return;
  }

  std::vector<TabEntry> tabs;

  // Held only for the length of this function: the snapshot requests are issued
  // below before anything can run in between, so these cannot go stale. Nothing
  // retains a WebContents* past the loop -- see the note on TabEntry.
  std::vector<content::WebContents*> snapshot_targets;
  std::vector<size_t> snapshot_target_rows;

  int window_number = 0;

  // ForEach() rather than the vector-returning GetAllBrowserWindowInterfaces():
  // the callback form exists specifically so a window cannot be destroyed
  // mid-iteration and leave a dangling pointer behind (crbug.com/405910169).
  // Order::kCreation is the default and is the deterministic one — kActivation
  // would reshuffle the table every time the user focused a different window.
  //
  // Only TYPE_NORMAL windows are listed. The collection also holds popups, PWA
  // windows, DevTools windows and picture-in-picture windows, each of which
  // technically owns a one-entry tab strip; including them would list "tabs" no
  // user thinks of as tabs. IsDeleteScheduled() drops windows that are
  // mid-teardown, matching what ProfileBrowserCollection's own
  // FindTabbedBrowser() skips.
  collection->ForEach(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
            browser->IsDeleteScheduled()) {
          return true;  // Keep iterating.
        }

        TabStripModel* tab_strip = browser->GetTabStripModel();
        const int count = tab_strip->count();
        ++window_number;

        for (int i = 0; i < count; ++i) {
          // GetWebContentsAt() returns the tab's live WebContents. It is never
          // null for an index below count().
          content::WebContents* contents = tab_strip->GetWebContentsAt(i);

          TabEntry entry;
          entry.window_number = window_number;
          entry.window_tab_count = count;
          entry.index_in_window = i + 1;
          // active_index() is per-tab-strip, so every window contributes one
          // active row.
          entry.is_active = i == tab_strip->active_index();
          // GetTitle() falls back to a URL-derived string when a page has no
          // <title>, so it does not need a null check — but it can still be
          // empty (a blank new tab), which BuildPageBodyHtml() substitutes for.
          entry.title = contents->GetTitle();
          // GetLastCommittedURL(), not GetVisibleURL(): the visible URL is what
          // the omnibox shows, which during a pending navigation is the
          // destination the tab has not arrived at yet. The committed URL is
          // what the tab is actually displaying, which is what a listing of
          // current state wants.
          entry.url = contents->GetLastCommittedURL().spec();

          // A discarded or crashed tab has no renderer to ask. Requesting a
          // snapshot anyway would not crash, but the reply would never come and
          // the tab would sit pending until kOverallDeadline, delaying the
          // whole window for every other tab too. Skipping it here leaves
          // snapshot_returned false, which renders as "outline unavailable" —
          // which is exactly what it is.
          content::RenderFrameHost* main_frame =
              contents->GetPrimaryMainFrame();
          if (main_frame && main_frame->IsRenderFrameLive()) {
            snapshot_targets.push_back(contents);
            snapshot_target_rows.push_back(tabs.size());
          }
          tabs.push_back(std::move(entry));
        }
        return true;
      },
      BrowserCollection::Order::kCreation);

  auto collector = base::MakeRefCounted<OutlineCollector>(std::move(tabs),
                                                          std::move(callback));

  // The summary, if it is switched on and this is not Incognito. The token is
  // minted now so the <iframe> element can carry it, but the request itself
  // cannot start until the headings exist -- which is what set_on_finished()
  // below waits for.
  if (floating_window::FloatingWindowSummarizer::IsAvailable() &&
      !profile->IsOffTheRecord()) {
    const std::string token = base::UnguessableToken::Create().ToString();
    collector->set_summary_frame(
        base::StringPrintf(kSummaryFrameFormat, ("summary?" + token).c_str()));
    collector->set_on_finished(base::BindOnce(
        [](Profile* profile, std::string token,
           const OutlineCollector& finished) {
          SummaryRegistry::Entry& entry =
              SummaryRegistry::GetOrCreate(profile).Create(token);
          entry.summarizer =
              std::make_unique<floating_window::FloatingWindowSummarizer>();
          // base::Unretained(profile) is safe for the same reason the request
          // filter's bound Profile* is: this data source is owned by the
          // URLDataManager keyed on that profile, so neither can outlive it.
          entry.summarizer->Summarize(
              profile, finished.BuildSummaryInput(),
              base::BindOnce(&OnSummaryReady, base::Unretained(profile),
                             std::move(token)));
        },
        base::Unretained(profile), token));
  }

  // The "still issuing" count described on AddPending(). Dropped by the
  // ResolveOne() below, after every request is in flight — so a snapshot that
  // somehow completes inline cannot publish the page early, and a run with zero
  // live renderers still finishes here rather than waiting for the deadline.
  collector->AddPending();

  for (size_t i = 0; i < snapshot_targets.size(); ++i) {
    collector->AddPending();

    // WrapCallbackWithDefaultInvokeIfNotRun covers the case where the reply
    // callback is *destroyed* without running — a renderer going away while the
    // request is in flight. Without it that tab would never resolve, and the
    // page would wait out kOverallDeadline for no reason. It does not cover a
    // renderer that stays alive and simply never answers; the deadline does.
    ui::AXTreeUpdate on_failure;
    snapshot_targets[i]->RequestAXTreeSnapshot(
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&OutlineCollector::OnSnapshot, collector,
                           snapshot_target_rows[i]),
            base::OwnedRef(std::move(on_failure))),
        kOutlineAXMode, kMaxAxNodesPerTab, kSnapshotTimeout,
        // Same-origin pruning: a cross-origin iframe's headings are not part of
        // this page's outline, and reaching into them would widen what a
        // chrome:// page reads out of arbitrary sites for no benefit here.
        content::WebContents::AXTreeSnapshotPolicy::
            kSameOriginDirectDescendants);
  }

  collector->StartDeadline();
  collector->ResolveOne();
}

}  // namespace

FloatingWindowUI::FloatingWindowUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  // Data sources are per-BrowserContext, so an Incognito window registers its
  // own. CreateAndAdd() both constructs the source and installs it as the
  // handler for chrome://floating-window/* on this profile; re-registering on a
  // later navigation simply replaces the previous one.
  Profile* profile = Profile::FromWebUI(web_ui);
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      profile, chrome::kChromeUIFloatingWindowHost);

  // SetRequestFilter() is the escape hatch from the resource-ID model: it lets
  // a source compute a response in C++. It is checked first in
  // WebUIDataSourceImpl::StartDataRequest(), ahead of the IDR lookup. For this
  // page that is not merely a shortcut — it is the only one of the two paths
  // that can produce a *different* document per request, which is what listing
  // live tab state requires.
  //
  // Note that SetResourcePathToResponse() looks like a shorter way to do this
  // and is used that way in content's own browsertests, but it only fills a map
  // consumed by PopulateWebUIResources() for the LocalResourceLoaderConfig
  // path; StartDataRequest() never reads it. The request filter is the path
  // that is honoured unconditionally.
  // The summary is served into a same-origin <iframe>, and two separate
  // defaults stand in the way of that: frame-src defaults to 'none' for a
  // WebUI source, and so does frame-ancestors.
  //
  // The trap is DisableDenyXFrameOptions(), which is *not* called here and must
  // not be. It looks like the belt-and-braces companion to AddFrameAncestor()
  // -- the header even comments it as "deprecated and AddFrameAncestors should
  // be used instead", which reads as though both were needed until the
  // deprecation lands. Calling it does the opposite of helping:
  // url_data_manager_backend.cc:207 appends the frame-ancestors directive
  // *only if* ShouldDenyXFrameOptions() is true, so disabling XFO silently
  // drops frame-ancestors from the response as well, and AddFrameAncestor()
  // above becomes dead code. What is left is a document with neither
  // protection, embeddable by any chrome:// page.
  //
  // AddFrameAncestor() alone is sufficient, and the reason is one level down:
  // X-Frame-Options: DENY is still sent, but AncestorThrottle checks the two
  // against each other and returns PROCEED when a frame-ancestors CSP is
  // present (ancestor_throttle.cc:264, per
  // https://www.w3.org/TR/CSP/#frame-ancestors-and-frame-options). The CSP then
  // decides, and it names this host.
  const GURL host(chrome::kChromeUIFloatingWindowURL);
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FrameSrc,
      base::StrCat({"frame-src ", host.spec(), ";"}));
  source->AddFrameAncestor(host);

  source->SetRequestFilter(
      base::BindRepeating(&ShouldHandleRequest),
      base::BindRepeating(&HandleRequest, base::Unretained(profile)));
}

FloatingWindowUI::~FloatingWindowUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(FloatingWindowUI)
