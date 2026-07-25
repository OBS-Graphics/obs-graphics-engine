// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): verifies
//  - Title::onTriggerIn / onTriggerOut fire on every TriggerIn/TriggerOut call,
//    with both a host-UI-style listener and a DataPool-installed listener
//    (registered via DataPool::Bind) coexisting on the same event lists
//  - The DataPool-installed listener relays into a bound ScriptDataSource's
//    Lua _on_trigger_in(titleId, ...)/_on_trigger_out(titleId) functions, if the
//    script defines them (via ScriptDataSource's own worker thread, so these
//    assertions poll-with-timeout instead of checking immediately)
//  - Lua scripts can call trigger_out(titleId) to hide one specific bound
//    title, or bare trigger_out() to hide every title bound to that source —
//    both marshaled back to the host through DataPool::Tick's drain/apply
//    step, so these also poll (DataPool::Tick must actually be driven, not
//    just anything touching the source). There is no trigger_in()
//    counterpart — whether a title is on-screen at all is the host's call.
//  - Title::duration auto-fires TriggerOut() once Visible time elapses
//  - The periodic data poll in DataPool::Tick only refreshes/delivers while
//    (at least one bound) Title is Visible
//  - TriggerIn from Hidden applies records the *caller* fetched up front via
//    DataPool::DataBlocking() (Title itself never fetches), even when
//    _get_data is slow — no polling needed there
//  - Two Titles bound to the same ScriptDataSource both receive records, each
//    gets its own _on_trigger_in(titleId, ...) with the right titleId, and
//    trigger_out("a") hides only "a", leaving "b" Visible

#include "title.h"
#include "data-pool.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "script.h"
#include "script_test_util.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

using script_test_util::Check;
using script_test_util::g_failures;
using script_test_util::PollUntil;
using script_test_util::WriteScript;

namespace {

std::unique_ptr<RectangleElement> MakeShortAnimElement(const char* id)
{
    auto el = std::make_unique<RectangleElement>();
    el->SetId(id);
    el->SetBounds({0.0, 0.0, 100.0, 100.0});
    el->inAnimation  = {AnimationType::Fade, Easing::Linear, 0.05f, 0.00f};
    el->outAnimation = {AnimationType::Fade, Easing::Linear, 0.05f, 0.00f};
    return el;
}

// ── Scenario A: _on_trigger_in / _on_trigger_out + Title::onTriggerIn/onTriggerOut ──

void ScenarioA()
{
    std::string path = WriteScript("scenario_a", R"lua(
local inCount, outCount = 0, 0
local lastId, lastIdx, lastDur = "", -1, -1

function _get_data()
    return { { in_count = tostring(inCount), out_count = tostring(outCount),
               last_id = lastId, last_idx = tostring(lastIdx), last_dur = tostring(lastDur) } }
end

function _on_trigger_in(titleId, recordIndex, duration)
    inCount = inCount + 1
    lastId = titleId
    lastIdx = recordIndex
    lastDur = duration
end

function _on_trigger_out(titleId)
    outCount = outCount + 1
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    ScriptDataSource* dsPtr = ds.get();

    DataPool pool;
    pool.Add("a", std::move(ds));
    Title title;
    pool.Bind(&title, "a", "a");

    bool cppIn = false, cppOut = false;
    size_t cppIdx = 999;
    double cppDur = -999.0;
    // A host-UI-style listener, registered alongside whatever DataPool
    // installs for its binding once Bind() is called (see Title::onTriggerIn).
    // These fire synchronously on the host thread, regardless of
    // ScriptDataSource's worker-thread internals.
    title.onTriggerIn.push_back([&](size_t idx, double dur) { cppIn = true; cppIdx = idx; cppDur = dur; });
    title.onTriggerOut.push_back([&] { cppOut = true; });

    title.TriggerIn(4, 1.25);
    Check(cppIn, "ScenarioA: host UI onTriggerIn listener fired");
    Check(cppIdx == 4, "ScenarioA: onTriggerIn recordIndex forwarded");
    Check(cppDur == 1.25, "ScenarioA: onTriggerIn duration forwarded");

    title.TriggerOut();
    Check(cppOut, "ScenarioA: host UI onTriggerOut listener fired");

    // The Lua-side _on_trigger_in/_on_trigger_out only run once ScriptDataSource's
    // worker thread dequeues the job and, separately, _get_data() is re-run —
    // poll dsPtr->GetData() (which never blocks) until the counters catch up.
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("in_count") && recs[0].at("in_count") == "1";
        }),
        "ScenarioA: Lua _on_trigger_in fired exactly once (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("out_count") && recs[0].at("out_count") == "1";
        }),
        "ScenarioA: Lua _on_trigger_out fired exactly once (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("last_id") && recs[0].at("last_id") == "a";
        }),
        "ScenarioA: Lua _on_trigger_in saw titleId 'a' (the DataPool bindName) (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("last_idx") && recs[0].at("last_idx") == "4";
        }),
        "ScenarioA: Lua _on_trigger_in saw recordIndex 4 (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("last_dur") && recs[0].at("last_dur") == "1.25";
        }),
        "ScenarioA: Lua _on_trigger_in saw duration 1.25 (async)"
    );
}

// ── Scenario B: Lua trigger_out() drives the owning Title, via DataPool::Tick ──
// (and trigger_in is gone from the script API entirely)

void ScenarioB()
{
    std::string path = WriteScript("scenario_b", R"lua(
local fired = false
function _get_data()
    if not fired then
        fired = true
        trigger_out()
    end
    return {}
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    DataPool pool;
    pool.Add("b", std::move(ds));
    Title title;
    pool.Bind(&title, "b", "b");
    title.state = TitleState::Visible;

    // Lua's bare trigger_out() only stashes a pending bind-name request
    // (the empty-string "every title bound to this source" sentinel) on the
    // worker thread; DataPool::Tick is what drains and applies it to every
    // bound Title, so poll by driving Tick() instead of asserting immediately.
    Check(
        PollUntil([&] {
            pool.Tick(0.01f);
            return title.state == TitleState::AnimatingOut;
        }),
        "ScenarioB: Lua trigger_out() drove state to AnimatingOut via DataPool::Tick (async)"
    );

    // A script calling the removed trigger_in() must fail loudly (Lua "attempt
    // to call a nil value") rather than silently no-op. _get_data() errors are
    // caught by the protected call, so the observable effect is that no
    // records ever land. This doesn't need a DataPool at all — it's purely
    // about the raw ScriptDataSource's Lua environment.
    std::string path2 = WriteScript("scenario_b2", R"lua(
function _get_data()
    trigger_in(9, 2.5)
    return { { greeting = "unreachable" } }
end
)lua");

    ScriptDataSource ds2(path2);
    Title title2;

    Check(
        !PollUntil(
            [&] { return !ds2.GetData().empty(); },
            std::chrono::milliseconds(400)
        ),
        "ScenarioB: trigger_in() is no longer bound — calling it errors out of _get_data()"
    );
    Check(title2.state == TitleState::Hidden, "ScenarioB: removed trigger_in() cannot show a Title");
}

// ── Scenario F: a script's own trigger_out() takes effect purely through
// DataPool::Tick's drain/apply step, with no Title::Tick() and no direct
// GetData() call at all. The title is deliberately parked in AnimatingIn
// (behind a long in-animation) for the whole test — trigger_out()'s
// mapping-and-apply in DataPool::Tick is unconditional on any binding's
// state (see step (a) in data-pool.h), so it must still land even though the
// title is never Visible and Title::Tick is never called here.

void ScenarioF()
{
    std::string path = WriteScript("scenario_f", R"lua(
local armed, fired = false, false

-- Arm only once the host has actually shown us, so the worker's startup
-- fetch can't stash a trigger_out before the title is even triggered.
function _on_trigger_in(titleId, recordIndex, duration)
    armed = true
end

function _get_data()
    if armed and not fired then
        fired = true
        trigger_out()
    end
    return {}
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    DataPool pool;
    pool.Add("f", std::move(ds));
    Title title;
    pool.Bind(&title, "f", "f");

    // Long in-animation so the title stays in AnimatingIn for the whole test.
    auto el = std::make_unique<RectangleElement>();
    el->SetId("a");
    el->SetBounds({0.0, 0.0, 100.0, 100.0});
    el->inAnimation  = {AnimationType::Fade, Easing::Linear, 5.0f, 0.0f};
    el->outAnimation = {AnimationType::Fade, Easing::Linear, 0.05f, 0.0f};
    title.GetRoot()->AddChild(el.get());
    title.elements.push_back(std::move(el));

    title.TriggerIn();
    Check(title.state == TitleState::AnimatingIn, "ScenarioF: host TriggerIn puts Title in AnimatingIn");

    // Only DataPool::Tick is driven from here on — never title.Tick(), never
    // any GetData() call.
    Check(
        PollUntil([&] {
            pool.Tick(0.01f);
            return title.state == TitleState::AnimatingOut;
        }),
        "ScenarioF: Lua's own trigger_out() applied via DataPool::Tick alone, "
        "with the title stuck in AnimatingIn the whole time"
    );
}

// ── Scenario E: TriggerIn from Hidden applies caller-fetched data ──────────────
// Title never fetches its own data anymore — the caller (here, the test
// itself, standing in for the host) must fetch via DataPool::DataBlocking()
// up front and hand the records to TriggerIn. DataBlocking() waits for a
// real, fresh fetch (even when _get_data is slow) before returning — no
// polling needed here, unlike every other async path above.

void ScenarioE()
{
    std::string path = WriteScript("scenario_e", R"lua(
function _get_data()
    -- Deliberately slow: a busy-wait, standing in for a slow HTTP call.
    local start = os.clock()
    while os.clock() - start < 0.2 do end
    return { { greeting = "hello" } }
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    DataPool pool;
    pool.Add("e", std::move(ds));
    Title title;
    pool.Bind(&title, "e", "e");

    auto el = std::make_unique<TextElement>();
    el->SetId("greeting");
    title.GetRoot()->AddChild(el.get());
    TextElement* elPtr = el.get();
    title.elements.push_back(std::move(el));

    Check(title.state == TitleState::Hidden, "ScenarioE: Title starts Hidden");

    // Stand-in for what a host does before showing a Hidden title: fetch
    // fresh data (blocking, bounded) and pass it straight to TriggerIn.
    auto records = pool.DataBlocking("e");
    title.TriggerIn(0, -1.0, records);
    Check(
        elPtr->text == "hello",
        "ScenarioE: DataBlocking() blocked until fresh (slow) data was ready; "
        "TriggerIn applied it instantly, no poll needed"
    );
}

// ── Scenario G: one ScriptDataSource bound to TWO titles ───────────────────────
// The whole point of DataPool: a single source, shared by more than one
// Title, without the old single-owner SetOwner ping-pong. Verifies both
// titles receive records, each gets its own _on_trigger_in(titleId, ...) with
// the right titleId, and trigger_out("a") hides only "a" — "b" stays Visible.

void ScenarioG()
{
    std::string path = WriteScript("scenario_g", R"lua(
local insA, insB = false, false
local firedOutA = false

function _get_data()
    -- Once both titles have checked in, ask the pool to hide "a" specifically —
    -- exercises trigger_out(titleId) targeting one binding out of several.
    if insA and insB and not firedOutA then
        firedOutA = true
        trigger_out("a")
    end
    return { { greeting = "hi", seen_a = tostring(insA), seen_b = tostring(insB) } }
end

function _on_trigger_in(titleId, recordIndex, duration)
    if titleId == "a" then insA = true end
    if titleId == "b" then insB = true end
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    ScriptDataSource* dsPtr = ds.get();

    DataPool pool;
    pool.Add("g", std::move(ds));

    Title titleA, titleB;
    auto elA = std::make_unique<TextElement>();
    elA->SetId("greeting");
    titleA.GetRoot()->AddChild(elA.get());
    TextElement* elAPtr = elA.get();
    titleA.elements.push_back(std::move(elA));

    auto elB = std::make_unique<TextElement>();
    elB->SetId("greeting");
    titleB.GetRoot()->AddChild(elB.get());
    TextElement* elBPtr = elB.get();
    titleB.elements.push_back(std::move(elB));

    pool.Bind(&titleA, "g", "a");
    pool.Bind(&titleB, "g", "b");

    // (a) Both titles receive records: fetch once via DataBlocking (as a
    // host would before showing a Hidden title) and apply to both.
    auto records = pool.DataBlocking("g");
    titleA.TriggerIn(0, -1.0, records);
    titleB.TriggerIn(0, -1.0, records);
    Check(elAPtr->text == "hi", "ScenarioG: title 'a' received the shared source's records");
    Check(elBPtr->text == "hi", "ScenarioG: title 'b' received the shared source's records");

    titleA.state = TitleState::Visible;
    titleB.state = TitleState::Visible;

    // (b) _on_trigger_in fired once per title, each with its own titleId —
    // TriggerIn above already fired the pool-installed onTriggerIn
    // subscriber for each binding; poll for both to land in the script.
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("seen_a") && recs[0].at("seen_a") == "true"
                && recs[0].count("seen_b") && recs[0].at("seen_b") == "true";
        }),
        "ScenarioG: _on_trigger_in fired once per title, each with the correct titleId (async)"
    );

    // (c) Once the script sees both titles checked in, it calls
    // trigger_out("a") itself — DataPool::Tick's drain/apply step must hide
    // only the binding named "a" and leave "b" untouched.
    Check(
        PollUntil([&] {
            pool.Tick(0.01f);
            return titleA.state == TitleState::AnimatingOut;
        }),
        "ScenarioG: script's trigger_out(\"a\") hid title 'a' (async)"
    );
    Check(titleB.state == TitleState::Visible, "ScenarioG: title 'b' was left untouched by trigger_out(\"a\")");
}

} // namespace

int main()
{
    ScenarioA();
    ScenarioB();
    ScenarioF();
    ScenarioE();

    // Scenario C: timed titles — duration auto-fires TriggerOut() once Visible.
    {
        Title title;
        auto el = MakeShortAnimElement("a");
        title.GetRoot()->AddChild(el.get());
        title.elements.push_back(std::move(el));

        title.TriggerIn(0, 0.20);  // stay Visible for 0.20s before auto-out
        Check(title.state == TitleState::AnimatingIn, "ScenarioC: starts AnimatingIn");

        const float dt = 0.01f;
        float t = 0.0f;
        while (title.state == TitleState::AnimatingIn && t < 1.0f) {
            title.Tick(dt);
            t += dt;
        }
        Check(title.state == TitleState::Visible, "ScenarioC: reaches Visible after in-animation");

        // Should still be Visible well before the 0.20s duration elapses.
        title.Tick(0.05f);
        Check(title.state == TitleState::Visible, "ScenarioC: still Visible before duration elapses");

        t = 0.0f;
        while (title.state == TitleState::Visible && t < 1.0f) {
            title.Tick(dt);
            t += dt;
        }
        Check(title.state == TitleState::AnimatingOut,
              "ScenarioC: duration elapsed -> auto TriggerOut() (AnimatingOut)");

        t = 0.0f;
        while (title.state == TitleState::AnimatingOut && t < 1.0f) {
            title.Tick(dt);
            t += dt;
        }
        Check(title.state == TitleState::Hidden, "ScenarioC: reaches Hidden after out-animation");
    }

    // Scenario D: periodic data poll (DataPool::Tick) only refreshes/delivers
    // while (at least one bound) Title is Visible.
    {
        struct CountingDataSource : IDataSource {
            mutable int calls = 0;
            std::vector<Record> GetData() const override { ++calls; return {}; }
            std::string GetFilePath() const override { return ""; }
        };

        auto counterOwned = std::make_unique<CountingDataSource>();
        CountingDataSource* counter = counterOwned.get();
        DataPool pool;
        pool.Add("d", std::move(counterOwned));
        Title title;
        pool.Bind(&title, "d", "d");

        auto el = MakeShortAnimElement("a");
        title.GetRoot()->AddChild(el.get());
        title.elements.push_back(std::move(el));

        const float dt = 0.01f;
        for (int i = 0; i < 20; ++i) {
            title.Tick(dt);
            pool.Tick(dt);
        }  // still Hidden and never triggered
        Check(counter->calls == 0, "ScenarioD: no polling while Hidden and never triggered");

        // Instant update while Hidden is now the caller's job: fetch once via
        // DataBlocking (which, for a synchronous source, just calls GetData())
        // and hand it to TriggerIn.
        auto records = pool.DataBlocking("d");
        Check(counter->calls == 1, "ScenarioD: DataBlocking() while Hidden does one instant fetch");
        title.TriggerIn(0, -1.0, records);

        float t = 0.0f;
        while (title.state == TitleState::AnimatingIn && t < 1.0f) {
            title.Tick(dt);
            pool.Tick(dt);
            t += dt;
        }
        Check(title.state == TitleState::Visible, "ScenarioD: reached Visible");
        Check(counter->calls == 1, "ScenarioD: no polling during AnimatingIn");

        t = 0.0f;
        while (t < 0.30f) {  // more than one 0.25s poll slot
            title.Tick(dt);
            pool.Tick(dt);
            t += dt;
        }
        Check(counter->calls == 2, "ScenarioD: exactly one periodic poll after 0.25s Visible");

        title.TriggerOut();
        t = 0.0f;
        while (title.state != TitleState::Hidden && t < 1.0f) {
            title.Tick(dt);
            pool.Tick(dt);
            t += dt;
        }
        Check(counter->calls == 2, "ScenarioD: no further polling once no longer Visible");
    }

    ScenarioG();

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
