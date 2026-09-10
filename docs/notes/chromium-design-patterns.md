# Design patterns in this checkout

A companion to `docs/notes/chromium-cpp-idioms.md`. That note is about
expressions — what `std::move(cb).Run()` means, why `raw_ptr` exists. This one
is a level up: the *object-graph* patterns — who observes whom, who owns whom,
where per-object state is allowed to live, and which of the tree's famous
patterns are now being actively removed.

Chromium is unusual in that it **documents its own patterns in-tree**, in
`docs/patterns/` (12 write-ups) and `docs/chrome_browser_design_principles.md`.
Those documents are the primary source here; the code sampling exists to show
what the patterns actually look like at scale, and where the tree has already
moved past its own documentation.

Occurrence counts are from `*.cc`/`*.h` under `base/ chrome/ content/
components/ ui/ third_party/blink/renderer/`, on `chrome/VERSION` 153.0.8005.0.

---

## 1. The in-tree pattern catalogue

| Doc | Pattern | One-line summary |
|---|---|---|
| `docs/patterns/inversion-of-control.md` | Callback / Listener / Observer / Delegate | 404 lines; **the** foundational document, read it first |
| `docs/patterns/associated-data.md` | `SupportsUserData`, `WebContentsUserData`, `KeyedService` | how to attach per-object state you do not own |
| `docs/patterns/domain-lens.md` | Domain Lens | expose a narrow, purpose-built view of a fat class |
| `docs/patterns/builder-and-parameter-bundle.md` | Builder, Parameter Bundle | many configuration options, small runtime API |
| `docs/patterns/passkey.md` | Passkey | per-method access control, finer than `friend` |
| `docs/patterns/bool-init.md` | `bool Init()` | documented mainly to say **don't** |
| `docs/patterns/builder-lambda.md` | Builder Lambda | immediately-invoked lambda to scope construction temporaries |
| `docs/patterns/prefer-enums.md` | Enum parameters | two-value enums instead of bool arguments |
| `docs/patterns/testapi.md` | TestApi | test-only surface in a separate target |
| `docs/patterns/friend-the-tests.md` | Friend the tests | `FRIEND_TEST_ALL_PREFIXES`; discouraged |
| `docs/patterns/fortesting-methods.md` | `XForTesting()` | naming convention as a review signal |
| `docs/patterns/synchronous-runloop.md` | Synchronous RunLoop | block a thread until an event; test code only |

Beyond `docs/patterns/`:

* `docs/chrome_browser_design_principles.md` — the modern architecture of
  `//chrome/browser`: the four primitives, feature containers, dependency
  injection, and a list of named anti-patterns.
* `content/README.md` — why the `content`/`chrome` split exists, and the
  embedder-interface (`ContentClient`) escape hatch.
* `ui/base/unowned_user_data/README.md` — the newest associated-data variant.
* `docs/tab_helpers.md` — the TabHelper pattern, now carrying a deprecation
  notice at the top.

---

## 2. Inversion of control, and how to pick a flavour

`docs/patterns/inversion-of-control.md` defines the family: a **low-level class
defines an interface that a high-level class implements**, so control flows back
"up" the dependency graph. In Chromium this is also literal — `//content` cannot
depend on `//chrome`, so every time `content` needs something from `chrome`, it
happens through an inverted interface.

The document is unusually blunt about the trade-off:

> **Inversion of control should not be your first resort. It is sometimes
> useful for solving specific problems, but in general it is overused in
> Chromium.**

The family has four members, and the decision tree below picks between them, so
they need naming first:

* **Callback** — a `base::OnceCallback` or `RepeatingCallback` handed to the
  framework, which runs it. No interface, no inheritance, no registration.
* **Listener** — an interface with a single event on it, at most one instance
  per framework object. A pre-lambda construct; do not write new ones.
* **Observer** — an interface with several events on it, any number of
  registered instances, notified of things that have already happened. All
  methods return `void`.
* **Delegate** — an interface the framework consults *while* doing something, to
  make a decision or supply a missing piece. Its methods return values.

With those named, the tree's own rules for choosing:

```mermaid
flowchart TD
  Q0{"Does the client need to<br/>influence the outcome?"}
  Q0 -->|"yes, returns non-void"| D["<b>Delegate</b><br/><small>helps CAUSE the change</small>"]
  Q0 -->|"no, just notified"| Q1{"Could every call be<br/>made asynchronous?"}
  Q1 -->|"no"| D
  Q1 -->|"yes"| Q2{"More than one<br/>interested client?"}
  Q2 -->|"yes"| O["<b>Observer</b><br/><small>AddObserver / RemoveObserver</small>"]
  Q2 -->|"no"| Q3{"More than one<br/>kind of event?"}
  Q3 -->|"yes"| L["<b>Listener</b><br/><small>do not write new ones</small>"]
  Q3 -->|"no"| C["<b>Callback</b><br/><small>the doc's recommended default</small>"]

  classDef pref fill:#bfe3d0,stroke:#1e6b45,stroke-width:1.5px,color:#0d3b26
  classDef warn fill:#f6e7c1,stroke:#8a6d1f,color:#4a3a08
  class C,O pref
  class L warn
```

The three tests, quoted verbatim, are worth memorising because they settle most
arguments:

* "If every call to the client could be made asynchronous and the API would
  still work fine for your use case, you have an observer or listener, not a
  delegate."
* "If there might be multiple interested client objects instead of one, you have
  an observer, not a listener or delegate."
* "**If any method on your interface has any return type other than `void`, you
  have a delegate**, not an observer or listener."

Or, in one sentence from the same doc: an observer is *notified* of a change; a
delegate helps *cause* it.

### The rules that are easy to get wrong

Five design tips from that document that show up as real bugs when ignored:

1. **Observer methods get empty bodies in the header, not `= 0`.** A pure
   virtual forces every implementer to write a stub for events they do not care
   about, and hides whether the base implementation must be called.
2. **Delegates should have sensible base implementations too**, so subclasses
   override only their part.
3. **Always pass the framework object back** to the observer
   (`OnWidgetDestroying(views::Widget* widget)`, not `OnWidgetDestroying()`).
   Without it, one object cannot observe two sources.
4. **Name methods long and specific.** A client may implement several
   interfaces, and `OnLoadStarted` will collide with somebody. Callbacks avoid
   the problem entirely.
5. **The framework must not own its observers.** Raw pointers plus
   add/remove, or a scoping helper.

---

## 3. Observer, at scale

8,955 classes inherit something named `*Observer`. The mechanism is
`base::ObserverList` (1,047 uses), whose defining property is stated at the top
of `base/observer_list.h:31`:

> "Unlike a standard vector or list, this container can be modified during
> iteration without invalidating the iterator. So, it safely handles the case of
> an observer removing itself or other observers from the list while observers
> are being notified."

That is the whole reason it is not a `std::vector<Observer*>` — notification
frequently destroys observers, and re-entrancy during `Notify` is normal, not
exceptional. `base::CheckedObserver` (615 uses) adds the other half: an observer
that is destroyed while still registered is caught, rather than leaving a
dangling pointer to be dereferenced on the next notification.

Registration is almost never done by hand. `base::ScopedObservation` (2,706
uses) pairs the add with the remove, so an observer that dies mid-observation
cannot leave the source holding a dangling pointer. The floating-window feature
uses it in the smallest possible form —
`chrome/browser/ui/views/toolbar/floating_window_toolbar_button.h:67`:

```cpp
class FloatingWindowToolbarButton : public ToolbarButton,
                                    public views::WidgetObserver {
  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;
 private:
  raw_ptr<views::Widget> widget_ = nullptr;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
};
```

Note tip 3 in action: `OnWidgetDestroying` receives the widget, and the
implementation at `.cc:56` asserts `CHECK_EQ(widget, widget_)`. And note the
*reason* it observes at all — the button does not own the bubble, but needs to
know when something it did not initiate (Esc) closed it. That is the textbook
observer use: passive notification, multiple potential listeners, no return
value.

### The fat-observer problem, and what replaced it

Design tip 4 of `inversion-of-control.md` names `WebContentsObserver` as the
tree's cautionary example: whenever *any* event fires, *every* registered
observer is notified, however few care.

The numbers in this checkout make the point:

| Interface | Virtual methods | Implementers |
|---|---|---|
| `content::WebContentsObserver` | **124** | 677 |
| `content::WebContentsDelegate` | **131** | 89 |
| `content::ContentBrowserClient` | **366** | (342 overridden in `ChromeContentBrowserClient`) |

The modern replacement is not a smaller observer interface — it is **no
interface at all**. `base::CallbackList` plus `base::CallbackListSubscription`
(2,490 uses) turns each event into its own independently subscribable channel:

```cpp
// base/callback_list.h:34 — the documented shape
CallbackListSubscription RegisterCallback(CallbackList::CallbackType cb) {
  return callback_list_.Add(std::move(cb));
}
```

`components/tabs/public/tab_interface.h` is where this has been carried
furthest. Instead of one `TabObserver` with a dozen methods, it exposes **11
separate registration points**, each returning a subscription that unregisters
on destruction:

```cpp
virtual base::CallbackListSubscription RegisterWillDiscardContents(...);
virtual base::CallbackListSubscription RegisterDidActivate(...);
virtual base::CallbackListSubscription RegisterWillDeactivate(...);
virtual base::CallbackListSubscription RegisterDidBecomeVisible(...);
virtual base::CallbackListSubscription RegisterWillBecomeHidden(...);
virtual base::CallbackListSubscription RegisterWillDetach(...);
// ... 5 more
```

A feature subscribes only to what it needs, the subscription *is* the
unregistration handle (so there is no `RemoveObserver` to forget), and the
callback can be bound with whatever state it needs — none of which a
method-per-event observer interface offers.

`tabs::ContentsObservingTabFeature`
(`chrome/browser/ui/tabs/contents_observing_tab_feature.h:23`) shows the two
styles side by side in one 40-line class: it inherits the old
`WebContentsObserver` for DOM-level events, and holds a
`base::CallbackListSubscription tab_subscription_` for the tab-level ones.

---

## 4. Delegate, and the "configure, don't subclass" variant

A delegate implements a deliberately-missing piece of a framework class. The
classic form is a `Foo::Delegate` interface with policy questions on it
(`ShouldPersistKey()`, `IsValidValueForKey()`) — non-void returns, which is
exactly the test the doc gives.

But the most common delegate shape in `//chrome/browser/ui/views` is different,
and the floating-window bubble is a clean example of it. `views::WidgetDelegate`
and its `BubbleDialogDelegate` subclass are delegates you are expected to
**configure through setters rather than subclass**:

```cpp
// chrome/browser/ui/views/floating_window/floating_window_bubble.cc:58
auto delegate = std::make_unique<views::BubbleDialogDelegate>(
    anchor_view, views::BubbleBorder::TOP_RIGHT,
    views::BubbleBorder::DIALOG_SHADOW, /*autosize=*/true);
delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
delegate->SetShowCloseButton(false);
delegate->SetShowTitle(false);
delegate->SetAccessibleTitle(u"Floating window");
delegate->set_close_on_deactivate(false);
delegate->SetContentsView(std::move(web_view));
```

Two things worth extracting from this:

* **The View-flavoured `views::BubbleDialogDelegateView` cannot be subclassed by
  new code** — its constructors are private behind a `friend` allowlist. The
  supported path is `BubbleDialogDelegate` + `SetContentsView()`. This is the
  tree enforcing the design-principles rule "avoid subclassing existing concrete
  view subclasses".
* **`views::WebView` is its own `WebContentsDelegate`.** The sizing chain
  documented at `floating_window_bubble.cc:115` runs
  `EnableSizingFromWebContents()` → renderer auto-resize →
  `WebView::ResizeDueToAutoResize()` → `SetPreferredSize()`. A framework class
  implementing the delegate interface *on behalf of* its user is common in
  views, and it is why so much works with no delegate code written at all.

### Client: the delegate that crosses a layer boundary

`ContentBrowserClient` is the same pattern applied to the `content`/`chrome`
boundary, and its own class comment (`content_browser_client.h:301`) is the
best statement of the cost:

> "Embedder API (or SPI) for participating in browser logic... Use this 'escape
> hatch' **sparingly, to avoid the embedder interface ballooning and becoming
> very specific to Chrome**."

At 366 virtual methods with 342 of them overridden in `chrome`, the interface
did balloon. `content/README.md` prefers the alternative where possible: expose
a generic extension point that features subscribe to, rather than a new client
method per feature.

---

## 5. Associated data: where per-object state is allowed to live

This is the pattern with the most variants, and picking the wrong one is the
most common architectural mistake in `//chrome/browser`.

The problem, from `docs/patterns/associated-data.md`: a central class `C` is
used by consumers in other modules that do not control `C`'s lifetime, and each
wants to store something per-instance-of-`C`. Putting the fields on `C` makes
`C` responsible for invariants that belong to strangers; putting a
`map<C*, Data>` in each consumer makes every consumer track `C`'s destruction.
The pattern splits it: **`C` owns the lifetime of the data, the consumer owns
its invariants.**

`base::SupportsUserData` is the raw mechanism — a keyed
`map<const void*, unique_ptr<Data>>` mixed into the central class, where the key
is conventionally the address of a static (or of the consumer instance itself,
when there can be several).

What matters in practice is that Chromium has a **ladder of scopes**, and each
rung has its own typed helper. Choosing the right rung is the design decision.

Every rung below is a browser-process object, and each names the thing whose
lifetime bounds the data attached to it. The four long-lived ones are the
product concepts from §6 — process, profile, browser window, tab. The four
short-lived ones are `//content` primitives and are worth knowing apart, because
they look interchangeable and are not: a **`WebContents`** is the renderer-backed
object a tab holds, and survives navigation; a **`Page`** is one primary page
within it, replaced on a cross-document navigation; a **`Document`** is one DOM
document, so a same-document navigation keeps it and a reload does not; and a
**`NavigationHandle`** exists only for the duration of a single navigation
attempt, which may never commit at all.

```mermaid
flowchart TD
  P["<b>Process</b><br/><small>BrowserProcess / GlobalFeatures</small>"]
  PR["<b>Profile</b><br/><small>KeyedService via<br/>ProfileKeyedServiceFactory — 1423</small>"]
  W["<b>Browser window</b><br/><small>BrowserWindowFeatures</small>"]
  T["<b>Tab</b><br/><small>TabFeatures / UnownedUserData</small>"]
  WC["<b>WebContents</b><br/><small>WebContentsUserData — 972</small>"]
  PG["<b>Page</b><br/><small>PageUserData</small>"]
  DOC["<b>Document</b><br/><small>DocumentUserData</small>"]
  NAV["<b>Navigation</b><br/><small>NavigationHandleUserData</small>"]

  P --> PR --> W --> T --> WC --> PG --> DOC --> NAV

  classDef long fill:#bfe3d0,stroke:#1e6b45,color:#0d3b26
  classDef short fill:#d9e4f5,stroke:#2b4c7e,color:#12274a
  class P,PR,W,T long
  class WC,PG,DOC,NAV short
```

`content/public/browser/web_contents_user_data.h:19` warns about exactly this
choice:

> "When considering using this class, please carefully consider the intended
> lifetime of the data... It is preferable to use a more specific UserData
> class, rather than storing non-WebContents state as a WebContentsUserData
> combined with using a WebContentsObserver to manually reset the state."

The mechanism itself is a CRTP template plus a macro pair — `WebContentsUserData<T>`
supplies `CreateForWebContents()`, `FromWebContents()` and the key, and
`WEB_CONTENTS_USER_DATA_KEY_DECL()` / `..._IMPL(T)` give the type its unique key
address. The constructor is private with `friend WebContentsUserData`, so an
instance can only come into existence attached to a `WebContents`.

### KeyedService: associated data with a dependency graph

`KeyedService` (701 direct subclasses) is the profile-scoped rung, and its
distinguishing feature over plain `SupportsUserData` is that factories **declare
dependencies on each other**, so construction and two-phase shutdown are
ordered. `ProfileKeyedServiceFactory` (1,423 uses) is the `//chrome` subclass.

The design-principles doc adds a rule the pattern doc does not:

> "Override `ServiceIsCreatedWithBrowserContext` to return `true`. This
> guarantees precise lifetime semantics. **Lazy instantiation is an
> anti-pattern.**"

The reasoning is about tests, and it generalises well beyond `KeyedService`:
production starts at `content::ContentMain()` and instantiates services in one
order; a test harness instantiates a *subset* at a different time in a different
order. Divergent behaviour gets papered over with early-exit conditionals in
production code. Eager construction removes the whole class of problem.

### UnownedUserData: the newest rung

`ui/base/unowned_user_data/README.md` describes the most recent addition (206
uses of `ScopedUnownedUserData<>`, 148 `DECLARE_USER_DATA`). It separates the
two jobs the older patterns fused together:

* **lifetime** stays with the owner (`TabFeatures`, `BrowserWindowFeatures`);
* **lookup** goes through the host, so `MyFeature::From(tab)` works.

The point is testability: a `MockTabInterface` has no `TabFeatures`, so nothing
reachable through the old path is injectable. With an `UnownedUserDataHost` on
the mock, a single real feature can be attached to an otherwise mocked tab. The
cost is about seven lines:

```cpp
class MyTabFeature {
  DECLARE_USER_DATA(MyTabFeature);              // + DEFINE_USER_DATA in the .cc
  static MyTabFeature* From(TabInterface* tab); // convenience wrapper
 private:
  ScopedUnownedUserData<MyTabFeature> scoped_unowned_user_data_;
};
```

---

## 6. Feature containers and core controllers

`docs/chrome_browser_design_principles.md` names four primitives — profile,
browser window, tab, `WebContents` — and states where the state for each lives:

| Primitive | Represented by | State lives in |
|---|---|---|
| process | `BrowserProcess` | `GlobalFeatures` |
| profile | `Profile` | `ProfileKeyedService` |
| browser window | `BrowserWindowInterface` | `BrowserWindowFeatures` |
| tab | `TabInterface` | `TabFeatures` |

Every feature is expected to have "a core controller with precise lifetime
semantics", owned and instantiated by exactly one of those containers, with its
**dependencies injected in the constructor**. The doc's own example:

```cpp
// Do not do this:
FooFeature(Browser* browser) : browser_(browser) {}
FooFeature::DoStuff() { DoStuffWith(browser_->profile()->GetPrefs()); }

// Do this:
FooFeature(PrefService* prefs) : prefs_(prefs) {}
FooFeature::DoStuff() { DoStuffWith(prefs_); }
```

`class Browser` is named, in the tree's own documentation, as "a god-object
anti-pattern"; Project Bedrock exists to remove it. The replacement interfaces
are `BrowserWindowInterface` and `TabInterface`.

**The scoping bug this prevents** is worth stating on its own, because it is the
reason the rule exists rather than a style preference: a tab-scoped feature that
caches a `Browser*` or `BrowserView*` points at the wrong window as soon as the
tab is dragged out into a new one. A tab feature is supposed to hold
`TabInterface* tab_` and reach the window dynamically through
`tab_->GetBrowserWindowInterface()->GetFeatures()`. Similarly, anything
profile-scoped — the doc's example is extension install/uninstall — belongs in a
`KeyedService`, not on a tab feature, or two tabs racing will uninstall it while
the other is using it.

The floating-window feature is on the correct side of this by construction:
`FloatingWindowToolbarButton` holds a `raw_ptr<Browser>` only to reach
`browser_->GetProfile()` once, and `HandleRequest()` is bound to a `Profile*`,
never a `Browser*` — which is precisely why an Incognito window's floating
window lists only Incognito tabs.

---

## 7. Domain lens

A domain lens is a purpose-built, narrow view of a fat class, so a client can
depend on the view instead of the class. `docs/patterns/domain-lens.md` gives
two motivations: testing a UI component should not require constructing an
entire `NetworkRequest`, and a second backing type should not have to subclass
the first just to be displayable.

The in-tree example the doc cites is `tabs::TabData`
(`chrome/browser/ui/tabs/tab_data.h:35`), and it is exactly the shape described
— a plain copyable/movable struct with a translation function, no methods that
mutate anything, no `WebContents` access:

```cpp
struct TabData {
  static TabData FromTabInterface(tabs::TabInterface* tab);
  TabData(const TabData& other);
  TabData(TabData&& other);
  bool operator==(const TabData& other) const;
  // ... only the fields needed to draw a tab
};
```

`TabEntry` in `chrome/browser/ui/webui/floating_window/floating_window_ui.cc:262`
is the same pattern arrived at for a different reason — and its comment gives
the second, sharper motivation:

```cpp
// Note that this deliberately holds *copies* rather than a WebContents*. The
// snapshots complete asynchronously, and a tab can be closed while they are in
// flight; holding the title and URL by value means a tab that disappears
// mid-gather still renders as the row it was, instead of dangling.
struct TabEntry {
  int window_number = 0;
  bool is_active = false;
  std::u16string title;
  std::string url;
  std::vector<Heading> outline;
  bool snapshot_returned = false;
};
```

A lens over a mutable object is also a **snapshot**, and that turns a lifetime
problem into a non-problem. The warning in the pattern doc still applies:
domain lenses are easy to overuse, since every client uses only a subset of a
fat class and that alone does not justify a new type.

---

## 8. Factory and registry

Three distinct things get called "factory" here.

**Static `Create()` returning null on failure.** Constructors cannot fail
without exceptions, so a class whose construction can fail exposes a factory and
keeps the constructor private or trivial.
`third_party/blink/renderer/modules/digitclassifier/digit_classifier_model.cc`
is the minimal form — it constructs, calls a private `Initialize()`, and returns
`nullptr` if any GPU resource could not be created. This is the *safe* use of
the `bool Init()` pattern; `docs/patterns/bool-init.md` exists mainly to say
that a **public** `Init()` clients must remember to call is the anti-pattern.

**Service factories with a dependency graph** — `BrowserContextKeyedServiceFactory`
(955) / `ProfileKeyedServiceFactory` (1,423), covered above.

**Registries that map a key to a constructor.** The floating-window feature adds
one entry to a global registry, and its header
(`chrome/browser/ui/webui/floating_window/floating_window_ui.h:15`) documents
the whole lifecycle:

```cpp
// The lifecycle is:
//   1. RegisterChromeWebUIConfigs() adds one instance of this class to the
//      global WebUIConfigMap at startup (see chrome_web_ui_configs.cc).
//   2. When something navigates to chrome://floating-window, content looks the
//      host up in that map.
//   3. The map calls CreateWebUIController(), which builds a FloatingWindowUI
//      for the WebContents doing the navigating.
class FloatingWindowUIConfig
    : public content::DefaultWebUIConfig<FloatingWindowUI> {
  FloatingWindowUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUIFloatingWindowHost) {}
};
```

`content::WebUIConfig` is the abstract factory (`CreateWebUIController()` plus
policy hooks like `IsWebUIEnabled()` and `ShouldHandleURL()`);
`DefaultWebUIConfig<T>` is the template-method specialisation that fills in the
common case, `std::make_unique<T>(web_ui)`. Deriving from the bare `WebUIConfig`
is only needed when construction takes more than the `WebUI*`, or when the page
must be conditionally disabled. Registration is one line:
`chrome/browser/ui/webui/chrome_web_ui_configs.cc:340`.

---

## 9. Builder and parameter bundle

The problem stated in `docs/patterns/builder-and-parameter-bundle.md`: a class
with many configuration options is forced to choose between a huge constructor
and public setters that make logically-`const` members non-`const` and move
validation to runtime.

* **Parameter bundle** — a `Foo::Params` struct. Note the doc's caveat: since
  constructors cannot fail, it must be *impossible to make an invalid Params*,
  or the class needs a factory to validate.
* **Builder** — a `Foo::Builder` whose setters return `Builder&`, so
  configuration chains and no temporary needs naming:
  ```cpp
  auto c = C::Builder().set_name(name).set_color(color).Build();
  ```
* **Optional-only bundle** — required arguments stay positional, optional ones
  go in a chainable `Params` with a default: `C("foo", ..., Params().set_is_cool(true))`.

In-tree: `ui::DialogModel::Builder` (`ui/base/models/dialog_model.h:170`), and
`views::Builder<>` (614 uses in `chrome/` and `ui/`), which extends the idea to
declarative construction of whole view hierarchies.

The **builder lambda** (`docs/patterns/builder-lambda.md`) is a smaller relative
worth recognising, because it is the one place the style guide's ban on default
capture-by-reference is explicitly lifted:

```cpp
C foo = [&]() -> auto {
  A a;
  a.set_property(value);
  return MakeC(a);
}();
```

The lambda gives block scope for the temporary `A` while letting `foo` stay
immutable. It is safe because the lambda provably cannot escape the scope.

---

## 10. Access control patterns

**Passkey** (`docs/patterns/passkey.md`, 2,003 uses) — a parameter type only one
class can construct, so a public method is callable by exactly that class:

```cpp
using BarPassKey = base::PassKey<Bar>;
void HelpBarOut(BarPassKey, ...);   // leave the parameter unnamed
```

Unlike `friend`, it is per-method rather than per-class, and the capability can
be *delegated* — `Bar` may hand a passkey to a helper. It costs nothing but a
few bytes of argument space. Its most common use is letting
`std::make_unique` / `blink::MakeGarbageCollected` construct a class without
handing construction rights to everyone.

**The test-access ladder** — three patterns for the same problem, in increasing
weight, and the tree documents when each stops being appropriate:

| Pattern | Shape | Use when | Breaks down when |
|---|---|---|---|
| `friend the tests` | `FRIEND_TEST_ALL_PREFIXES(FooTest, X)` — 4,744 uses | one test suite needs a field | many suites need it; couples tests to implementation |
| `ForTesting methods` | `void DoStuffForTesting();` | a small amount of test surface | there are lots of them |
| `TestApi` | separate `foo_test_api.{h,cc}`, linked only into test targets | a widely-used class needs real test surface | an extensive TestApi means the class is doing too much |

All three docs push toward the same conclusion — "test the contract, not the
implementation" — and `friend-the-tests.md` says outright that testing private
methods separately is usually a sign the private behaviour is unnecessary.

---

### Sum types and exhaustive visitation

`std::variant` visited through `absl::Overload` is how the tree models "one of
these N things" when an inheritance hierarchy would be too heavy. The pattern's
value is not the storage but the **exhaustiveness check**: `std::visit` fails to
compile unless every alternative is handled, so adding a case to the variant
produces a build error at every visitation site rather than a silently missed
branch. Same argument as enums over bools, one level up. The mechanics are in
§11.2 of the idioms note.

### Type-safe identifiers

`base::StrongAlias<Tag, T>` (318 uses) and its specialisations
`base::IdType32/64<Foo>` (93) and `base::TokenType<Foo>` (15) make a distinct
type out of a primitive, so a `TabId` cannot be passed where a `WindowId` was
meant. `docs/patterns/prefer-enums.md` is the small cousin of this — a two-value
enum instead of a `bool` parameter — and `blink-c++.md` offers
`base::StrongAlias<class ForReloadTag, bool>` as the alternative for exactly
that case. The common thread is turning a class of caller mistakes from a
runtime bug into a compile error.

## 11. Feature flags: every behaviour change is switchable

Nothing ships on unconditionally. This is arguably the most pervasive pattern in
the tree and the easiest to overlook, because it looks like configuration rather
than design. In the added lines of the last 1,000 commits,
`base::FeatureList::IsEnabled` appears 98 times and `BASE_FEATURE` 53 — roughly
one new flag for every twenty commits.

The declaration goes in a `.cc` (never a header, which the macro's comment
states outright), and the check is a free function wrapping global state, which
is the one kind of global function `chrome_browser_design_principles.md`
explicitly permits:

```cpp
BASE_FEATURE(kMyFeature, base::FEATURE_DISABLED_BY_DEFAULT);

bool IsMyFeatureEnabled() {
  return base::FeatureList::IsEnabled(kMyFeature);
}
```

Three parts of the design are worth knowing:

* **The name is derived from the identifier.** The two-argument form turns
  `kMyFeature` into the string `"MyFeature"`; the three-argument form exists
  only for the rare case where they must differ. The macro dispatches on
  argument count — the usual `GET_MACRO(__VA_ARGS__, THREE_ARGS, TWO_ARGS)`
  trick — so both spellings share one name.
* **Parameters ride along.** `BASE_FEATURE_PARAM` and `BASE_FEATURE_ENUM_PARAM`
  attach server-controllable values to a flag, so a rollout can tune a timeout
  or a threshold without a binary change. The header carries a warning worth
  repeating: if the feature is disabled or the param is unset or invalid,
  `Get()` returns the compiled-in default — "typically ... regardless of the
  server-side config in control groups."
* **`FEATURE_DISABLED_BY_DEFAULT` is not the runtime state.** A field trial or a
  command-line switch overrides it, which is what makes a flag a kill switch
  rather than merely a compile-time toggle.

The pattern this belongs to is *deferred binding of a decision*: the code
supports both behaviours, and which one runs is chosen after the binary is
built. The costs are real and worth stating — every flag is a branch that has to
be tested in both states, and a flag that is never cleaned up is permanent dead
code in one arm. `agents/skills/` includes a `feature-flag-removal` skill for
exactly that cleanup.

A related instance sits in this checkout's own `digitclassifier` branch, where
the gate is a **runtime-enabled feature** rather than a `base::Feature`:
`runtime_enabled_features.json5` with `status: "test"`, requiring
`--enable-blink-features=DigitClassifier`. Blink has its own flag system for
web-exposed API surface, because the decision there is per-document and tied to
origin trials rather than per-process.

## 12. Cross-process: Mojo as inversion of control over a pipe

Everything above assumes both halves are in one process. Across a process
boundary the same patterns appear, generated rather than hand-written: a
`.mojom` interface definition produces a `mojo::Remote<T>` (4,657) on the
calling side and a `mojo::Receiver<T>` (6,819 `PendingReceiver` uses) on the
implementing side. A `Remote` is a proxy; a `Receiver` binds an implementation
to a pipe.

Two things carry over from the in-process patterns and are worth watching for:

* **The reply callback is a `OnceCallback` that may be dropped, not just
  delayed.** If the other process dies, the callback is destroyed without
  running. `mojo::WrapCallbackWithDefaultInvokeIfNotRun()` is the standard
  guard; `floating_window_ui.cc:628` uses it around an accessibility-snapshot
  request for exactly this reason.
* **A dropped pipe is the disconnect event**, which is the cross-process
  equivalent of `OnWidgetDestroying` — you observe teardown by observing the
  connection, not by being told.

> For how a Mojo message actually crosses the boundary on Linux — the socket,
> `sendmsg()` with `SCM_RIGHTS`, and the `epoll` loop that receives it — see
> [`chromium-linux-processes-and-ipc.md`](chromium-linux-processes-and-ipc.md).

**Type mapping is the other half of Mojo, and it is a pattern.** `StructTraits`
appears 238 times in one sampled batch. A `.mojom` struct does not have to
surface in C++ as a generated struct: a `StructTraits` specialisation teaches
the bindings to serialise an existing native type directly, so `gfx.mojom.Rect`
arrives in C++ as a `gfx::Rect` rather than something you must convert at every
boundary. `mojo/public/cpp/bindings/README.md` §"Type Mapping" is the reference.
This is the same decoupling instinct as the domain lens in §7 — the wire format
and the in-memory type are allowed to differ, with one declarative adapter
between them instead of conversion code at every call site.

The floating-window feature is a useful negative example here: it deliberately
has **no `.mojom` of its own**. It needed browser→renderer data, and got it by
reusing an existing generic extension point (`RequestAXTreeSnapshot`) rather
than adding an interface — which is precisely the preference `content/README.md`
states.

---

## 13. Blink's own patterns

`third_party/blink/renderer/` uses the same vocabulary with different spellings,
plus two patterns that have no `//chrome` equivalent.

**Supplement** (884 uses) is Blink's associated-data pattern, and it inverts the
dependency the same way `KeyedService` does: a module bolts a property onto a
`core/` class without `core/` knowing the module exists. The
`digitclassifier` branch adds `navigator.digitclassifier` this way:

```cpp
class NavigatorDigitClassifier final
    : public GarbageCollected<NavigatorDigitClassifier>,
      public Supplement<NavigatorBase> {
  static const char kSupplementName[];
  static DigitClassifier* digitclassifier(NavigatorBase& navigator);
};

DigitClassifier* NavigatorDigitClassifier::digitclassifier(
    NavigatorBase& navigator) {
  auto* supplement = Supplement<NavigatorBase>::From<NavigatorDigitClassifier>(
      navigator);
  if (!supplement) {
    supplement = MakeGarbageCollected<NavigatorDigitClassifier>(navigator);
    ProvideTo(navigator, supplement);
  }
  return supplement->digit_classifier_.Get();
}
```

The `From()` / `ProvideTo()` lazy-instantiation dance is the same shape as
`WebContentsUserData::GetOrCreateForWebContents()` — keyed by
`kSupplementName` instead of a static address.

**Visitor** is how Oilpan traces the object graph. Every GC object implements
`void Trace(Visitor*) const`, visits each `Member<>`, and **calls its base
class's `Trace`** — omitting either is a use-after-free that only appears under
GC pressure. It is a genuine Visitor: the object knows its own structure, the
visitor decides what to do with each edge.

`blink-c++.md` also contributes one composition rule with no analogue elsewhere:
**a class has either `Create()` factories or public constructors, never both** —
and Passkey is the sanctioned way to have a `Create()` that can still call
`MakeGarbageCollected`.

---

## 14. Testing patterns

Sampling added lines across the last 2,000 commits, the test vocabulary is the
single largest undocumented cluster here: `IN_PROC_BROWSER_TEST*` 613, `TEST_P(`
330, `base::test::ScopedFeatureList` 289, `INSTANTIATE_TEST_SUITE_P` 38. §10
covered how production code *exposes* a test surface; this is the shape of the
tests themselves.

**Three tiers, and the macro tells you which one you are in.**

| Macro | Tier | What exists |
|---|---|---|
| `TEST_F(Fixture, Name)` | unit test | just the fixture. Runs in `*_unittests`, no browser, no renderer. |
| `IN_PROC_BROWSER_TEST_F(Fixture, Name)` | browser test | a real browser in the test process — profiles, tab strips, renderers. `browser_test.h:10` calls it "analogous to `TEST_F()`", and there is no `TEST()` form because "browser tests always require a test fixture". |
| `TEST_P` / `IN_PROC_BROWSER_TEST_P` + `INSTANTIATE_TEST_SUITE_P` | parameterized | the same body run once per parameter. |

The tier is a cost decision, not a style one — a browser test starts a browser,
so the guidance everywhere is to push a case down to the cheapest tier that can
still express it. `tools/autotest.py` maps a source file to whichever target
holds its tests, which is why `CLAUDE.md` says to run it rather than guessing a
target.

**Parameterized tests are how a feature flag gets both arms covered.** This is
the pairing worth internalising, because §11 noted that every flag is a branch
requiring coverage in both states, and `TEST_P` plus `ScopedFeatureList` is the
mechanism:

```cpp
class MyFeatureTest : public testing::TestWithParam<bool> {
 public:
  MyFeatureTest() {
    scoped_feature_list_.InitWithFeatureState(kMyFeature, GetParam());
  }
 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};
INSTANTIATE_TEST_SUITE_P(All, MyFeatureTest, testing::Bool());
```

`base::test::ScopedFeatureList` is an RAII override in the §7 family: it
"resets the global `FeatureList` instance to a new instance and restores the
original instance upon destruction." Because it is scoped, a test cannot leak
flag state into the next one — the failure mode that makes flag-dependent
suites order-dependent and flaky.

**Waiting is a pattern with its own document.**
`docs/patterns/synchronous-runloop.md` covers the `base::RunLoop` idiom for
blocking until an event, and rests on two properties: `Quit()` is idempotent,
and `Run()` on an already-quit loop is a no-op — so the quit may legally happen
before the run. It is "almost never appropriate outside test code."

## 15. Metrics as a cross-cutting concern

`base::UmaHistogram*` appears 146 times in the sampled added lines, spread thinly
across features rather than concentrated — the signature of a cross-cutting
concern rather than a subsystem.

The pattern has two halves, and the second is the one that surprises people: the
C++ call site is trivial, and the *registration* is where the work is. A new
histogram needs an entry in `tools/metrics/histograms/`, with an owner and an
expiry date, and presubmit will reject a mismatch — which is why `CLAUDE.md`
lists "presubmit-clean metrics XML" among the conventions this checkout keeps
even though nothing here is uploaded. `agents/skills/` carries a histograms
skill for the mechanics.

The design consequence worth noting: because registration is enforced
out-of-band, a metric cannot be added silently, and the expiry date means an
un-renewed metric stops recording rather than accumulating forever. That is the
same "make the decision visible and revisit it" instinct behind
feature-flag cleanup and `base::NotFatalUntil` milestones.

## 16. What the tree is migrating away from

The most useful thing to know about Chromium's patterns is which ones are on
the way out, because the code is full of both sides and the older side is more
numerous.

```mermaid
flowchart LR
  subgraph Old["Legacy — still the majority of the code"]
    O1["TabHelper<br/><small>WebContentsUserData +<br/>WebContentsObserver</small>"]
    O2["Fat observer interfaces<br/><small>124 virtuals on<br/>WebContentsObserver</small>"]
    O3["Lazy KeyedService<br/>instantiation"]
    O4["Browser* passed everywhere<br/><small>god object</small>"]
    O5["Global lookup helpers<br/><small>chrome::FindBrowserWithTab</small>"]
  end
  subgraph New["Current guidance"]
    N1["TabFeatures / BrowserWindowFeatures<br/><small>owned, dependency-injected</small>"]
    N2["Per-event CallbackListSubscription<br/><small>11 on TabInterface</small>"]
    N3["ServiceIsCreatedWithBrowserContext<br/><small>eager, test-equivalent</small>"]
    N4["TabInterface / BrowserWindowInterface"]
    N5["Constructor injection<br/><small>+ UnownedUserData for lookup</small>"]
  end
  O1 --> N1
  O2 --> N2
  O3 --> N3
  O4 --> N4
  O5 --> N5

  classDef old fill:#f6e7c1,stroke:#8a6d1f,color:#4a3a08
  classDef new fill:#bfe3d0,stroke:#1e6b45,stroke-width:1.5px,color:#0d3b26
  class O1,O2,O3,O4,O5 old
  class N1,N2,N3,N4,N5 new
```

`docs/tab_helpers.md` now opens with the deprecation notice:

> "Tab helpers are in the process of being deprecated. When possible prefer to
> use TabFeatures over TabHelpers."

The design-principles doc adds explicit anti-pattern labels: lazy
`WebContentsUserData` instantiation, `NoDestructor` singletons as core
controllers, self-owned view/controller objects, subclassing `Widget`, nested
message loops, and global functions that access non-global state (naming static
methods on `BrowserList`).

There is also a general one worth quoting, because it explains several of the
others:

> "Avoid tight coupling of unrelated features. This results in O(N²)
> complexity, since every pair of features ends up implicitly coupled. The
> proper solution is to work with UX to use consistent design language, which in
> turn results in O(N) complexity."

---

## 17. One feature, read as a stack of patterns

The floating-window feature is small enough to hold in one diagram and uses
seven of the patterns above. Every box is a browser-process object:
`FloatingWindowToolbarButton` is the toolbar control (§3), `BubbleDialogDelegate`
and `views::WebView` are the views classes behind the floating surface (§4),
`FloatingWindowUIConfig` and `FloatingWindowUI` are the WebUI registry entry and
its controller (§8), and `OutlineCollector` and `TabEntry` are the file-local
helpers in `floating_window_ui.cc` that gather the outlines and hold one row's
worth of data (§7).

```mermaid
flowchart TD
  TB["<b>FloatingWindowToolbarButton</b><br/><small>ToolbarButton + WidgetObserver</small>"]
  SO["ScopedObservation<br/><small>RAII registration</small>"]
  BD["views BubbleDialogDelegate<br/><small>delegate, configured not subclassed</small>"]
  WV["views WebView<br/><small>is its own WebContentsDelegate</small>"]
  CFG["FloatingWindowUIConfig<br/><small>registry entry + abstract factory</small>"]
  UI["FloatingWindowUI<br/><small>WebUIController</small>"]
  OC["OutlineCollector<br/><small>refcounted fan-in + deadline</small>"]
  TE["TabEntry<br/><small>domain lens, by value</small>"]

  TB -->|"observes teardown"| SO
  TB -->|"creates"| BD
  BD -->|"SetContentsView"| WV
  WV -->|"navigates to the host"| CFG
  CFG -->|"CreateWebUIController"| UI
  UI -->|"moves GotDataCallback into"| OC
  OC -->|"holds N of"| TE

  classDef ioc fill:#d9e4f5,stroke:#2b4c7e,color:#12274a
  classDef data fill:#bfe3d0,stroke:#1e6b45,color:#0d3b26
  class SO,BD,WV ioc
  class TE,OC data
```

Reading the seams is the useful part:

1. **Observer** — the button observes the widget because the bubble can close
   itself (Esc), so `widget_` must be invalidated from outside
   (`floating_window_toolbar_button.cc:53`).
2. **Delegate, configured not subclassed** — no `BubbleDialogDelegate` subclass
   exists in the feature.
3. **Delegate supplied by the framework** — `views::WebView` is already a
   `WebContentsDelegate`, so auto-resize works with no delegate code written.
4. **Registry + abstract factory** — one line in `chrome_web_ui_configs.cc`
   makes `chrome://floating-window` exist.
5. **Inverted control through a callback** — `WebUIDataSource::GotDataCallback`
   is the low-level interface `content` defines and `chrome` fills in. The whole
   asynchronous redesign needed *no new mechanism*, because that interface was
   already callback-shaped.
6. **Fan-in with a deadline** — `OutlineCollector` is refcounted because two
   independent things keep it alive (the reply callbacks and its own timer), and
   `Finish()` is idempotent because it is reachable from both.
7. **Domain lens** — `TabEntry` holds copies, so a tab closed mid-gather still
   renders as the row it was.

---

## 18. Quick reference

| Pattern | Canonical header | In-tree doc |
|---|---|---|
| Callback / Observer / Delegate | `base/functional/callback.h` | `docs/patterns/inversion-of-control.md` |
| Observer list | `base/observer_list.h`, `base/observer_list_types.h` | ↑ |
| Scoped registration | `base/scoped_observation.h` | ↑ |
| Per-event subscription | `base/callback_list.h` | ↑ |
| Associated data | `base/supports_user_data.h` | `docs/patterns/associated-data.md` |
| Per-WebContents data | `content/public/browser/web_contents_user_data.h` | `docs/tab_helpers.md` (deprecated) |
| Per-profile service | `components/keyed_service/core/keyed_service.h` | `docs/patterns/associated-data.md` |
| Injectable feature lookup | `ui/base/unowned_user_data/scoped_unowned_user_data.h` | `ui/base/unowned_user_data/README.md` |
| Feature containers | `chrome/browser/ui/tabs/public/tab_features.h`, `chrome/browser/ui/browser_window/public/browser_window_features.h` | `docs/chrome_browser_design_principles.md` |
| Tab / window interfaces | `components/tabs/public/tab_interface.h` | ↑ |
| Domain lens | `chrome/browser/ui/tabs/tab_data.h` | `docs/patterns/domain-lens.md` |
| Builder | `ui/base/models/dialog_model.h` | `docs/patterns/builder-and-parameter-bundle.md` |
| Passkey | `base/types/pass_key.h` | `docs/patterns/passkey.md` |
| Embedder client | `content/public/browser/content_browser_client.h` | `content/README.md` |
| WebUI registry | `content/public/browser/webui_config.h` | — |
| Cross-process interfaces | `mojo/public/cpp/bindings/README.md` | `mojo/README.md` |
| Blink supplement | `third_party/blink/renderer/platform/supplementable.h` | `styleguide/c++/blink-c++.md` |
| Feature flags | `base/feature.h`, `base/feature_list.h` | — (`agents/skills/feature-flag-removal`) |
| Sum types | `absl::Overload` + `std::variant` | — |
| Type-safe identifiers | `base/types/strong_alias.h`, `base/types/id_type.h`, `base/types/token_type.h` | `docs/patterns/prefer-enums.md` |
| Errors as values | `base/types/expected.h`, `base/types/expected_macros.h` | — |
| Test tiers | `content/public/test/browser_test.h` | `docs/testing/` |
| Flag overrides in tests | `base/test/scoped_feature_list.h` | — |
| Blocking on an event | `base/run_loop.h` | `docs/patterns/synchronous-runloop.md` |
| Sequences, locks, task posting | `base/task/sequenced_task_runner.h`, `base/synchronization/lock.h` | [`chromium-threading.md`](chromium-threading.md), `docs/threading_and_tasks.md` |
| Metrics | `base/metrics/histogram_functions.h` | `tools/metrics/histograms/`, `agents/skills/` |
| Mojo type mapping | `mojo/public/cpp/bindings/README.md` §Type Mapping | — |
| Test access | — | `docs/patterns/{testapi,friend-the-tests,fortesting-methods}.md` |
| Blocking on an event in tests | `base/run_loop.h` | `docs/patterns/synchronous-runloop.md` |
