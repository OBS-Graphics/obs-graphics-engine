// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no Lua) for Scene (scene.h/cpp) and the
// Title side of the pull-model data path. Fake IDataSource implementations
// stand in for a scripted source, so nothing here needs Lua.
//
//  - AddTitle wires a Title to the Scene's pool and guarantees id uniqueness
//    (the same .ogt opened twice arrives with the same persisted uuid).
//  - FindByName returns every match (names are not unique); FindById is exact.
//  - A Visible Title pulls changed records from the pool and applies them; a
//    Hidden one doesn't, and an unchanged cache costs nothing.
//  - TriggerIn from Hidden applies a *blocking* fresh fetch before animating
//    in, and relays NotifyTriggerIn; TriggerOut relays NotifyTriggerOut.
//  - The TriggerIn overload taking already-fetched records fetches nothing,
//    relays the same notifications, and leaves the puller correctly versioned.
//  - The title directory reaches every source that is behind it — including
//    one added or replaced long after the directory last changed — without
//    republishing to sources that are already current.
//  - Script-originated trigger_out requests are dispatched by uuid and by the
//    bare "every title on this source" sentinel, deduplicated within one
//    drain, and skipped for a title already Hidden/AnimatingOut.
//  - Scene::Render draws non-Hidden titles in zOrder order.
//  - SetDataSource re-points a Visible Title and applies the new source's
//    records even when both caches sit at the same version.

#include "element_rectangle.h"
#include "element_text.h"
#include "scene.h"
#include "script_test_util.h"
#include "uuid.h"

#include <cairo/cairo.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using script_test_util::Check;
using script_test_util::g_failures;

namespace {

// Long enough that a full 0.25s pool refresh slot is crossed in one Tick().
constexpr float kRefreshDt = 0.30f;

// ── Fakes ────────────────────────────────────────────────────────────────

// Serves whatever `value` currently is, counts fetches, records what the pool
// relays into it, and lets the test queue trigger_out requests by hand.
struct FakeSource : IDataSource {
    mutable int calls = 0;
    mutable int blockingCalls = 0;
    std::string value = "first";
    std::vector<TitleRef> directory;
    int directoryPublishes = 0;
    std::vector<std::string> pending;
    int inCount = 0, outCount = 0;
    TitleRef lastIn, lastOut;

    std::vector<Record> GetData() const override
    {
        ++calls;
        return { Record{{"greeting", value}} };
    }
    std::vector<Record> GetDataBlocking() const override
    {
        ++blockingCalls;
        return GetData();
    }
    std::string GetFilePath() const override { return ""; }

    void NotifyTriggerIn(const TitleRef& t, size_t, double) override { ++inCount; lastIn = t; }
    void NotifyTriggerOut(const TitleRef& t) override { ++outCount; lastOut = t; }
    void SetTitleDirectory(std::vector<TitleRef> titles) override
    {
        directory = std::move(titles);
        ++directoryPublishes;
    }
    std::vector<std::string> DrainOutTriggerRequests() override
    {
        auto out = std::move(pending);
        pending.clear();
        return out;
    }
};

// A TextElement that counts how many times content was actually applied to it,
// so a test can tell "applied once" from "applied, then redundantly reapplied".
struct CountingText : TextElement {
    int applies = 0;
    void ApplyContent(const std::string& value) override
    {
        ++applies;
        TextElement::ApplyContent(value);
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────

// A Title with one text element whose id matches the fake source's record key,
// and animations short enough to step through quickly. The element is always a
// CountingText, so a test that cares can static_cast the out-param.
std::unique_ptr<Title> MakeTitle(const std::string& name, TextElement** outElement = nullptr)
{
    auto title = std::make_unique<Title>();
    title->name = name;

    auto el = std::make_unique<CountingText>();
    el->SetId("greeting");
    el->inAnimation  = {AnimationType::Fade, Easing::Linear, 0.05f, 0.00f};
    el->outAnimation = {AnimationType::Fade, Easing::Linear, 0.05f, 0.00f};
    if (outElement) *outElement = el.get();
    title->GetRoot()->AddChild(el.get());
    title->elements.push_back(std::move(el));

    return title;
}

// Drives scene.Tick() until `title` leaves the state it's animating through.
void RunUntilSettled(Scene& scene, Title& title)
{
    const float dt = 0.01f;
    float t = 0.0f;
    while ((title.state == TitleState::AnimatingIn || title.state == TitleState::AnimatingOut) && t < 2.0f) {
        scene.Tick(dt);
        t += dt;
    }
}

// ── Registration & lookup ────────────────────────────────────────────────

void TestAddTitleWiresAndDeduplicates()
{
    Scene scene;
    Title* a = scene.AddTitle(MakeTitle("Lower Third"));
    Check(a != nullptr, "AddTitle: returns a borrowed pointer");
    Check(a->dataPool == &scene.Pool(), "AddTitle: points the title at the scene's pool");
    Check(uuid::IsV4(a->id), "AddTitle: a fresh Title carries a generated v4 uuid");

    // The same .ogt opened twice: ids round-trip through the file, so the
    // second arrival claims an id that's already taken.
    auto clone = MakeTitle("Lower Third");
    clone->id = a->id;
    Title* b = scene.AddTitle(std::move(clone));
    Check(b->id != a->id, "AddTitle: a duplicate id is reassigned");
    Check(uuid::IsV4(b->id), "AddTitle: the reassigned id is a valid uuid");

    Check(scene.FindByName("Lower Third").size() == 2, "FindByName: returns every title sharing a name");
    Check(scene.FindByName("nope").empty(), "FindByName: unknown name yields an empty list");
    Check(scene.FindById(a->id) == a && scene.FindById(b->id) == b, "FindById: exact uuid lookup");
    Check(scene.FindById("no-such-id") == nullptr, "FindById: unknown uuid yields nullptr");

    scene.RemoveTitle(a);
    Check(scene.Titles().size() == 1, "RemoveTitle: drops exactly one title");
    Check(scene.FindById(b->id) == b, "RemoveTitle: leaves the others intact");

    scene.Clear();
    Check(scene.Titles().empty(), "Clear: removes every title");
}

// ── Data pull ────────────────────────────────────────────────────────────

void TestVisibleTitlePullsChangedData()
{
    Scene scene;
    auto srcOwned = std::make_unique<FakeSource>();
    FakeSource* src = srcOwned.get();
    const std::string sourceId = scene.Pool().Add(std::move(srcOwned));

    TextElement* el = nullptr;
    Title* title = scene.AddTitle(MakeTitle("Ticker", &el));
    title->dataSourceId = sourceId;

    // Hidden: the pool still refreshes (it has no idea what's on screen), but
    // the title must not apply anything.
    scene.Tick(kRefreshDt);
    Check(el->text.empty(), "Pull: a Hidden title applies nothing");

    title->TriggerIn();
    Check(el->text == "first", "Pull: TriggerIn applied the fetched records instantly");
    RunUntilSettled(scene, *title);
    Check(title->state == TitleState::Visible, "Pull: the title reached Visible");

    // Unchanged source: many ticks, no change applied.
    src->value = "first";
    for (int i = 0; i < 5; ++i) scene.Tick(kRefreshDt);
    Check(el->text == "first", "Pull: an unchanged cache leaves the content alone");

    src->value = "second";
    scene.Tick(kRefreshDt);
    Check(el->text == "second", "Pull: a Visible title picks up a changed cache");

    // Hidden again: further changes must not be applied.
    title->state = TitleState::Hidden;
    src->value = "third";
    for (int i = 0; i < 3; ++i) scene.Tick(kRefreshDt);
    Check(el->text == "second", "Pull: a title that went Hidden stops applying changes");
}

void TestTriggerInFetchesFreshAndRelays()
{
    Scene scene;
    auto srcOwned = std::make_unique<FakeSource>();
    FakeSource* src = srcOwned.get();
    const std::string sourceId = scene.Pool().Add(std::move(srcOwned));

    TextElement* el = nullptr;
    Title* title = scene.AddTitle(MakeTitle("Lower Third", &el));
    title->dataSourceId = sourceId;

    // Value changed since Add()'s priming fetch: only a *blocking* fetch at
    // TriggerIn time can show the new one immediately.
    src->value = "fresh";
    const int blockingBefore = src->blockingCalls;

    title->TriggerIn(0, 5.0);
    Check(src->blockingCalls == blockingBefore + 1, "TriggerIn: performed one blocking fetch");
    Check(el->text == "fresh", "TriggerIn: applied the fresh records instantly, before animating in");
    Check(title->state == TitleState::AnimatingIn, "TriggerIn: entered AnimatingIn");
    Check(src->inCount == 1 && src->lastIn.id == title->id && src->lastIn.name == "Lower Third",
          "TriggerIn: relayed NotifyTriggerIn with this title's ref");

    title->TriggerOut();
    Check(src->outCount == 1 && src->lastOut.id == title->id,
          "TriggerOut: relayed NotifyTriggerOut with this title's ref");

    // A title with no source at all must not touch the pool.
    Title* orphan = scene.AddTitle(MakeTitle("Orphan"));
    orphan->TriggerIn();
    Check(orphan->state == TitleState::AnimatingIn, "TriggerIn: a title with no dataSourceId still shows");
    Check(src->inCount == 1, "TriggerIn: a title with no dataSourceId relays nothing");
}

// The overload a host uses when it fetched the records itself, off whatever
// thread its Scene lock would otherwise have frozen. It must do everything the
// 2-arg form does EXCEPT the fetch.
void TestTriggerInWithSuppliedRecords()
{
    Scene scene;
    auto srcOwned = std::make_unique<FakeSource>();
    FakeSource* src = srcOwned.get();
    const std::string sourceId = scene.Pool().Add(std::move(srcOwned));

    TextElement* el = nullptr;
    Title* title = scene.AddTitle(MakeTitle("Lower Third", &el));
    auto* counting = static_cast<CountingText*>(el);
    title->dataSourceId = sourceId;

    // What a host does: pull off-thread (this is the call that can take
    // seconds), then hand the result to TriggerIn under its own lock.
    src->value = "prefetched";
    const std::vector<Record> records = scene.Pool().DataBlocking(sourceId);
    const int blockingBefore = src->blockingCalls;

    title->TriggerIn(0, 5.0, records);
    Check(src->blockingCalls == blockingBefore,
          "TriggerIn(records): performed no fetch of its own");
    Check(el->text == "prefetched", "TriggerIn(records): applied the supplied records instantly");
    Check(title->state == TitleState::AnimatingIn, "TriggerIn(records): entered AnimatingIn");
    Check(title->duration == 5.0, "TriggerIn(records): took the duration");
    Check(src->inCount == 1 && src->lastIn.id == title->id && src->lastIn.name == "Lower Third",
          "TriggerIn(records): still relayed NotifyTriggerIn with this title's ref");

    // The version sync: the caller's fetch bumped the cache version, so the
    // first Visible tick must NOT read that bump as a change and re-apply.
    const int appliesAfterTrigger = counting->applies;
    RunUntilSettled(scene, *title);
    Check(title->state == TitleState::Visible, "TriggerIn(records): reached Visible");
    Check(counting->applies == appliesAfterTrigger,
          "TriggerIn(records): an unchanged cache is not reapplied once Visible");

    // ...but a genuinely new value still arrives, so the sync didn't wedge the
    // puller at a stale version.
    src->value = "later";
    scene.Tick(kRefreshDt);
    Check(el->text == "later", "TriggerIn(records): a later real change is still picked up");

    // Empty records mean "the source returned nothing" — apply nothing, and
    // above all don't fall back to a fetch.
    TextElement* el2 = nullptr;
    Title* other = scene.AddTitle(MakeTitle("Empty", &el2));
    other->dataSourceId = sourceId;
    const int blockingBefore2 = src->blockingCalls;
    other->TriggerIn(0, -1.0, {});
    Check(src->blockingCalls == blockingBefore2, "TriggerIn(records): empty records still fetch nothing");
    Check(el2->text.empty(), "TriggerIn(records): empty records apply nothing");
    Check(other->state == TitleState::AnimatingIn, "TriggerIn(records): empty records still show the title");
}

// ── Title directory ──────────────────────────────────────────────────────

void TestDirectoryPublishedOnChangeOnly()
{
    Scene scene;
    auto srcOwned = std::make_unique<FakeSource>();
    FakeSource* src = srcOwned.get();
    scene.Pool().Add(std::move(srcOwned));

    Title* a = scene.AddTitle(MakeTitle("Alpha"));
    scene.Tick(0.01f);
    Check(src->directoryPublishes == 1, "Directory: published once after the first tick");
    Check(src->directory.size() == 1 && src->directory[0].name == "Alpha" && src->directory[0].id == a->id,
          "Directory: carries {id, name} for every title in the scene");

    for (int i = 0; i < 5; ++i) scene.Tick(0.01f);
    Check(src->directoryPublishes == 1, "Directory: an unchanged scene does not republish");

    scene.AddTitle(MakeTitle("Beta"));
    scene.Tick(0.01f);
    Check(src->directoryPublishes == 2 && src->directory.size() == 2, "Directory: a new title republishes");

    a->name = "Renamed";
    scene.Tick(0.01f);
    Check(src->directoryPublishes == 3 && src->directory[0].name == "Renamed",
          "Directory: a rename republishes");
}

// The regression: publishing was gated on "did the directory change", which is
// the wrong question. The right one is "is THIS source behind" — a source
// registered after the last change never got a directory at all, so its
// script's scene.titles()/find_titles() silently saw an empty scene forever.
void TestDirectoryReachesLateAndReplacedSources()
{
    Scene scene;
    Title* a = scene.AddTitle(MakeTitle("Alpha"));
    scene.AddTitle(MakeTitle("Beta"));

    // Settle: the directory is now unchanging, which is exactly the state that
    // used to starve a newcomer.
    scene.Tick(0.01f);
    scene.Tick(0.01f);

    auto lateOwned = std::make_unique<FakeSource>();
    FakeSource* late = lateOwned.get();
    const std::string lateId = scene.Pool().Add(std::move(lateOwned));
    Check(late->directory.size() == 2,
          "Directory: a source added after the titles gets the directory at Add()");
    Check(!late->directory.empty() && late->directory[0].id == a->id,
          "Directory: the late source's copy carries the real ids");

    const int publishesAfterAdd = late->directoryPublishes;
    for (int i = 0; i < 5; ++i) scene.Tick(0.01f);
    Check(late->directoryPublishes == publishesAfterAdd,
          "Directory: a source that is already current is not republished to");

    // The Reload path: Add() over the same id replaces the source in place.
    // The replacement is a different object and starts with nothing.
    auto replacementOwned = std::make_unique<FakeSource>();
    FakeSource* replacement = replacementOwned.get();
    replacementOwned->SetId(lateId);
    scene.Pool().Add(std::move(replacementOwned));
    Check(replacement->directory.size() == 2,
          "Directory: a source replaced under the same id (Reload) gets the directory too");

    // And a change after all that still reaches it.
    a->name = "Renamed";
    scene.Tick(0.01f);
    Check(replacement->directory.size() == 2 && replacement->directory[0].name == "Renamed",
          "Directory: the replacement keeps receiving later changes");
}

// ── trigger_out dispatch ─────────────────────────────────────────────────

void TestTriggerOutDispatch()
{
    Scene scene;
    auto srcOwned = std::make_unique<FakeSource>();
    FakeSource* src = srcOwned.get();
    const std::string sourceId = scene.Pool().Add(std::move(srcOwned));

    Title* a = scene.AddTitle(MakeTitle("A"));
    Title* b = scene.AddTitle(MakeTitle("B"));
    a->dataSourceId = sourceId;
    b->dataSourceId = sourceId;
    a->state = TitleState::Visible;
    b->state = TitleState::Visible;

    // Named: hits exactly one title.
    src->pending = {a->id};
    scene.Tick(0.01f);
    Check(a->state == TitleState::AnimatingOut, "Dispatch: a named request hid that title");
    Check(b->state == TitleState::Visible, "Dispatch: it left the other title alone");
    Check(src->outCount == 1, "Dispatch: relayed exactly one NotifyTriggerOut");

    // Bare sentinel: hits every title reading this source. `a` is already
    // AnimatingOut, so only `b` moves and `a` is not re-fired.
    src->pending = {""};
    scene.Tick(0.01f);
    Check(b->state == TitleState::AnimatingOut, "Dispatch: the bare sentinel hid the remaining title");
    Check(src->outCount == 2, "Dispatch: an already-AnimatingOut title is skipped, not re-fired");

    // Dedup: the same title named twice (and via the sentinel) in one drain.
    RunUntilSettled(scene, *a);
    RunUntilSettled(scene, *b);
    a->state = TitleState::Visible;
    const int outBefore = src->outCount;
    src->pending = {a->id, a->id, ""};
    scene.Tick(0.01f);
    Check(src->outCount == outBefore + 1, "Dispatch: duplicate requests in one drain fire TriggerOut once");

    // An already-Hidden title must not be re-hidden.
    RunUntilSettled(scene, *a);
    Check(a->state == TitleState::Hidden, "Dispatch setup: title settled Hidden");
    const int outHidden = src->outCount;
    src->pending = {a->id};
    scene.Tick(0.01f);
    Check(src->outCount == outHidden, "Dispatch: an already-Hidden title is not re-fired");
    Check(a->state == TitleState::Hidden, "Dispatch: it stays Hidden (no replayed out-animation)");

    // A title in the scene but reading a *different* source can still be
    // named directly — a script sees the whole scene.
    Title* other = scene.AddTitle(MakeTitle("Other"));
    other->state = TitleState::Visible;
    src->pending = {other->id};
    scene.Tick(0.01f);
    Check(other->state == TitleState::AnimatingOut, "Dispatch: a named title on another source is still reachable");

    // ...but the bare sentinel is scoped to this source's own titles.
    Title* unrelated = scene.AddTitle(MakeTitle("Unrelated"));
    unrelated->state = TitleState::Visible;
    src->pending = {""};
    scene.Tick(0.01f);
    Check(unrelated->state == TitleState::Visible, "Dispatch: the bare sentinel skips titles on other sources");

    // An unknown uuid is a silent no-op.
    src->pending = {"no-such-title"};
    scene.Tick(0.01f);
    Check(true, "Dispatch: an unknown uuid is a silent no-op");
}

// ── Rendering ────────────────────────────────────────────────────────────

// Two full-canvas rectangles, different colors, different zOrder: whichever
// draws last wins the pixel, so the pixel tells us the order Scene used.
void TestRenderZOrder()
{
    Scene scene;

    auto makeRect = [&](const char* name, int z, double r, double g, double b) {
        auto title = std::make_unique<Title>();
        title->name = name;
        title->zOrder = z;
        title->state = TitleState::Visible;

        auto el = std::make_unique<RectangleElement>();
        el->SetId("bg");
        el->SetBounds({0.0, 0.0, 8.0, 8.0});
        el->fill = Paint::Solid(r, g, b, 1.0);
        title->GetRoot()->AddChild(el.get());
        title->elements.push_back(std::move(el));
        return scene.AddTitle(std::move(title));
    };

    // Added lowest-zOrder-last on purpose: insertion order must not decide.
    makeRect("red-on-top", 10, 1.0, 0.0, 0.0);
    Title* blue = makeRect("blue-below", 1, 0.0, 0.0, 1.0);

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t* cr = cairo_create(surface);
    scene.Render(cr);
    cairo_surface_flush(surface);

    uint32_t px = *reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(surface));
    const uint8_t red  = (px >> 16) & 0xFF;
    const uint8_t blue8 = px & 0xFF;
    Check(red > 200 && blue8 < 50, "Render: the higher zOrder title drew last (on top)");

    // A Hidden title must not draw at all: hide the red one and the blue one
    // shows through.
    scene.Titles()[0]->state = TitleState::Hidden;
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    scene.Render(cr);
    cairo_surface_flush(surface);

    px = *reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(surface));
    Check(((px >> 16) & 0xFF) < 50 && (px & 0xFF) > 200, "Render: a Hidden title is skipped");
    Check(blue->state == TitleState::Visible, "Render: the remaining title is untouched");

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

}  // namespace

// Rebinding a Visible Title to a different source must apply that source's
// records, even when the two caches happen to share a version number.
// UpdateData() compares the pool's cache version against m_lastDataVersion,
// which carries over from whichever source was read before -- and a freshly
// registered source is primed to version 1 while a source that never changes
// sits at version 1 for the whole session. A bare `dataSourceId = id` could
// therefore leave the title comparing 1 against 1, reading "unchanged", and
// rendering the old source's record forever. Title::SetDataSource resets the
// counter, which is the whole reason it exists.
void TestRebindAppliesNewSourceData()
{
    Scene scene;

    auto srcA = std::make_unique<FakeSource>();
    srcA->value = "from A";
    auto* rawA = srcA.get();
    const std::string idA = scene.Pool().Add(std::move(srcA));

    TextElement* el = nullptr;
    Title* title = scene.AddTitle(MakeTitle("rebind", &el));
    title->dataSourceId = idA;

    title->TriggerIn();
    RunUntilSettled(scene, *title);
    Check(title->state == TitleState::Visible, "Rebind: title is Visible after TriggerIn");
    Check(el->text == "from A", "Rebind: title shows source A's record");

    // A never changes again, so its cache stays at the version the title
    // recorded when it was triggered in -- exactly the collision above.
    auto srcB = std::make_unique<FakeSource>();
    srcB->value = "from B";
    const std::string idB = scene.Pool().Add(std::move(srcB));
    Check(scene.Pool().DataVersion(idA) == scene.Pool().DataVersion(idB),
          "Rebind: both sources really are at the same cache version");
    (void)rawA;

    title->SetDataSource(idB);
    scene.Tick(kRefreshDt);
    Check(el->text == "from B", "Rebind: SetDataSource makes the next tick apply source B");

    // Unbinding still works and stops any further pull.
    title->SetDataSource("");
    scene.Tick(kRefreshDt);
    Check(el->text == "from B", "Rebind: unbinding leaves the last applied content alone");
}

int main()
{
    TestAddTitleWiresAndDeduplicates();
    TestVisibleTitlePullsChangedData();
    TestTriggerInFetchesFreshAndRelays();
    TestTriggerInWithSuppliedRecords();
    TestDirectoryPublishedOnChangeOnly();
    TestDirectoryReachesLateAndReplacedSources();
    TestTriggerOutDispatch();
    TestRenderZOrder();
    TestRebindAppliesNewSourceData();

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
