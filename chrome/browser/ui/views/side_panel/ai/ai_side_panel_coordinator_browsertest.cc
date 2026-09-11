// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/side_panel/ai/ai_side_panel_coordinator.h"

#include <optional>
#include <vector>

#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_action_callback.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_enums.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "chrome/browser/ui/views/side_panel/side_panel_coordinator.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/test/browser_test.h"
#include "ui/actions/actions.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/compositor_switches.h"
#include "ui/compositor/test/draw_waiter_for_test.h"
#include "ui/snapshot/snapshot.h"

namespace {

class AiSidePanelCoordinatorBrowserTest : public InProcessBrowserTest {
 public:
  AiSidePanelCoordinatorBrowserTest() {
    features_.InitAndEnableFeature(features::kAiSidePanel);
  }
  ~AiSidePanelCoordinatorBrowserTest() override = default;

  SidePanelCoordinator* coordinator() {
    return SidePanelCoordinator::From(browser());
  }

  SidePanel* GetSidePanel() {
    return BrowserView::GetBrowserViewForBrowser(browser())->side_panel();
  }

  tabs::TabInterface* GetTabAt(int index) {
    return browser()->tab_strip_model()->GetTabAtIndex(index);
  }

  bool IsEntryRegisteredForTab(tabs::TabInterface* tab) {
    SidePanelRegistry* registry = SidePanelRegistry::From(tab);
    return registry && registry->GetEntryForKey(SidePanelEntry::Key(
                           SidePanelEntry::Id::kAiSidePanel));
  }

 private:
  base::test::ScopedFeatureList features_;
};

// The entry is registered per tab, not per window, so every tab gets one.
IN_PROC_BROWSER_TEST_F(AiSidePanelCoordinatorBrowserTest,
                       EntryIsRegisteredOnEveryTab) {
  EXPECT_TRUE(IsEntryRegisteredForTab(GetTabAt(0)));

  ASSERT_TRUE(AddTabAtIndex(1, GURL("about:blank"), ui::PAGE_TRANSITION_TYPED));
  EXPECT_TRUE(IsEntryRegisteredForTab(GetTabAt(1)));
  EXPECT_NE(AiSidePanelCoordinator::From(GetTabAt(0)),
            AiSidePanelCoordinator::From(GetTabAt(1)));
}

IN_PROC_BROWSER_TEST_F(AiSidePanelCoordinatorBrowserTest, ShowSidePanel) {
  coordinator()->SetNoDelaysForTesting(true);
  coordinator()->Show(SidePanelEntryId::kAiSidePanel);

  EXPECT_TRUE(
      base::test::RunUntil([&]() { return GetSidePanel()->GetVisible(); }));
  EXPECT_EQ(coordinator()->GetCurrentEntryId(), SidePanelEntryId::kAiSidePanel);
}

IN_PROC_BROWSER_TEST_F(AiSidePanelCoordinatorBrowserTest, ShowFromAppMenu) {
  coordinator()->SetNoDelaysForTesting(true);
  chrome::ExecuteCommandWithContext(
      browser(), IDC_SHOW_AI_SIDE_PANEL,
      actions::ActionInvocationContext::Builder()
          .SetProperty(kSidePanelOpenTriggerKey, SidePanelOpenTrigger::kAppMenu)
          .Build());

  EXPECT_TRUE(
      base::test::RunUntil([&]() { return GetSidePanel()->GetVisible(); }));
  EXPECT_EQ(coordinator()->GetCurrentEntryId(), SidePanelEntryId::kAiSidePanel);
}

// Opening the panel on one tab must not leak it into another tab.
IN_PROC_BROWSER_TEST_F(AiSidePanelCoordinatorBrowserTest,
                       PanelIsScopedToItsTab) {
  coordinator()->SetNoDelaysForTesting(true);
  ASSERT_TRUE(AddTabAtIndex(1, GURL("about:blank"), ui::PAGE_TRANSITION_TYPED));

  browser()->tab_strip_model()->ActivateTabAt(0);
  coordinator()->Show(SidePanelEntryId::kAiSidePanel);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return GetSidePanel()->GetVisible(); }));

  browser()->tab_strip_model()->ActivateTabAt(1);
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return coordinator()->GetCurrentEntryId() != SidePanelEntryId::kAiSidePanel;
  }));

  browser()->tab_strip_model()->ActivateTabAt(0);
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return coordinator()->GetCurrentEntryId() == SidePanelEntryId::kAiSidePanel;
  }));
}

// Enables real pixel output. Browser tests run with a no-op compositor by
// default, which is why reading back composited output otherwise yields a blank
// image.
class AiSidePanelScreenshotBrowserTest
    : public AiSidePanelCoordinatorBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitch(::switches::kEnablePixelOutputInTests);
    InProcessBrowserTest::SetUpCommandLine(command_line);
  }
};

// Not a correctness test: regenerates the screenshots embedded in
// docs-ob/ai_side_panel_design.md. MANUAL_ tests are skipped unless
// --run-manual is passed. Run it with:
//
//   testing/xvfb.py out/Desktop/browser_tests \
//       --gtest_filter=AiSidePanelScreenshotBrowserTest.MANUAL_Screenshots \
//       --run-manual --single-process-tests
//
// Captures the same window twice, once per tab, to show that each tab carries
// its own AI side panel with its own page context.
IN_PROC_BROWSER_TEST_F(AiSidePanelScreenshotBrowserTest, MANUAL_Screenshots) {
  BrowserView* const browser_view =
      BrowserView::GetBrowserViewForBrowser(browser());
  views::Widget* const widget = browser_view->GetWidget();
  widget->SetBounds(gfx::Rect(0, 0, 1280, 800));

  coordinator()->SetNoDelaysForTesting(true);
  coordinator()->DisableAnimationsForTesting();

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath src_root;
  ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &src_root));
  const base::FilePath out_dir =
      src_root.AppendASCII("docs-ob").AppendASCII("images");
  ASSERT_TRUE(base::CreateDirectory(out_dir));

  // Shows the panel on the active tab, waits for it to be laid out and drawn,
  // then writes the composited window to `filename`.
  auto capture = [&](const char* filename) {
    coordinator()->Show(SidePanelEntryId::kAiSidePanel);
    ASSERT_TRUE(base::test::RunUntil([&]() {
      browser_view->DeprecatedLayoutImmediately();
      return GetSidePanel()->GetVisible() && GetSidePanel()->width() > 0 &&
             coordinator()->GetCurrentEntryId() ==
                 SidePanelEntryId::kAiSidePanel;
    }));

    ui::Compositor* const compositor = widget->GetCompositor();
    ASSERT_TRUE(compositor);
    compositor->ScheduleFullRedraw();
    ui::DrawWaiterForTest::WaitForCompositingEnded(compositor);

    base::RunLoop run_loop;
    scoped_refptr<base::RefCountedMemory> png;
    ui::GrabWindowSnapshotAsPNG(
        widget->GetNativeWindow(),
        gfx::Rect(widget->GetWindowBoundsInScreen().size()),
        base::BindOnce(
            [](base::RunLoop* loop, scoped_refptr<base::RefCountedMemory>* out,
               scoped_refptr<base::RefCountedMemory> data) {
              *out = std::move(data);
              loop->Quit();
            },
            &run_loop, &png));
    run_loop.Run();
    ASSERT_TRUE(png);

    const base::FilePath out_file = out_dir.AppendASCII(filename);
    ASSERT_TRUE(base::WriteFile(out_file, *png));
    LOG(INFO) << "Wrote " << out_file << " (" << png->size()
              << " bytes), side panel at "
              << GetSidePanel()->bounds().ToString();
  };

  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<title>First tab</title>"
                      "<body style='font:16px system-ui;padding:48px'>"
                      "<h1>First tab</h1><p>Contents of the first tab.</p>")));
  ASSERT_TRUE(AddTabAtIndex(
      1,
      GURL("data:text/html,<title>Second tab</title>"
           "<body style='font:16px system-ui;padding:48px'>"
           "<h1>Second tab</h1><p>Contents of the second tab.</p>"),
      ui::PAGE_TRANSITION_TYPED));
  ASSERT_EQ(2, browser()->tab_strip_model()->count());

  browser()->tab_strip_model()->ActivateTabAt(0);
  ASSERT_NO_FATAL_FAILURE(capture("ai_side_panel_tab1.png"));

  browser()->tab_strip_model()->ActivateTabAt(1);
  ASSERT_NO_FATAL_FAILURE(capture("ai_side_panel_tab2.png"));
}

class AiSidePanelCoordinatorDisabledBrowserTest : public InProcessBrowserTest {
 public:
  AiSidePanelCoordinatorDisabledBrowserTest() {
    features_.InitAndDisableFeature(features::kAiSidePanel);
  }
  ~AiSidePanelCoordinatorDisabledBrowserTest() override = default;

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(AiSidePanelCoordinatorDisabledBrowserTest,
                       NothingIsRegistered) {
  tabs::TabInterface* tab = browser()->tab_strip_model()->GetTabAtIndex(0);
  EXPECT_EQ(AiSidePanelCoordinator::From(tab), nullptr);

  SidePanelRegistry* registry = SidePanelRegistry::From(tab);
  ASSERT_TRUE(registry);
  EXPECT_FALSE(registry->GetEntryForKey(
      SidePanelEntry::Key(SidePanelEntry::Id::kAiSidePanel)));
}

}  // namespace
