// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): verifies
//  - Title::onTriggerIn / onTriggerOut fire on every TriggerIn/TriggerOut call
//    for a host-UI-style listener
//  - A Title's show/hide is relayed into the ScriptDataSource it reads, and
//    surfaces in Lua as _on_trigger_in(title, recordIndex, duration) /
//    _on_trigger_out(title), where `title` is a { id = uuid, name = ... }
//    handle (via ScriptDataSource's own worker thread, so these assertions
//    poll-with-timeout instead of checking immediately)
//  - Lua scripts can call trigger_out(title) with a handle from
//    scene.find_titles(name) to hide one specific title, or bare trigger_out()
//    to hide every title reading that source — both marshaled back to the host
//    through Scene::Tick's drain/apply step, so these also poll (Scene::Tick
//    must actually be driven). There is no trigger_in() counterpart — whether
//    a title is on-screen at all is the host's call.
//  - Title::duration auto-fires TriggerOut() once Visible time elapses
//  - TriggerIn from Hidden blocks on a fresh fetch (DataPool::DataBlocking)
//    even when _get_data is slow, so the first frame shows real data
//  - Two Titles reading the same ScriptDataSource both receive records, each
//    gets its own _on_trigger_in(title, ...) with the right handle, and a
//    script-side scene.find_titles("a") -> trigger_out(t) hides only "a"

#include "element_rectangle.h"
#include "element_text.h"
#include "scene.h"
#include "script.h"
#include "script_test_util.h"
#include "title.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

using script_test_util::Check;
using script_test_util::g_failures;
using script_test_util::PollUntil;
using script_test_util::WriteScript;

namespace {

std::unique_ptr<Title> MakeTitle(const std::string& name)
{
    auto title = std::make_unique<Title>();
    title->name = name;
    return title;
}

std::unique_ptr<RectangleElement> MakeShortAnimElement(const char* id)
{
    auto el = std::make_unique<RectangleElement>();
    el->SetId(id);
    el->SetBounds({0.0, 0.0, 100.0, 100.0});
    el->inAnimation  = {AnimationType::Fade, Easing::Linear, 0.05f, 0.00f};
    el->outAnimation = {AnimationType::Fade, Easing::Linear, 0.05f, 0.00f};
    return el;
}

// Adds a text element with the given id to `title` and returns it.
TextElement* AddText(Title& title, const char* id)
{
    auto el = std::make_unique<TextElement>();
    el->SetId(id);
    TextElement* raw = el.get();
    title.GetRoot()->AddChild(raw);
    title.elements.push_back(std::move(el));
    return raw;
}

// ── Scenario A: _on_trigger_in / _on_trigger_out + Title::onTriggerIn/onTriggerOut ──

void ScenarioA()
{
    std::string path = WriteScript("scenario_a", R"lua(
local inCount, outCount = 0, 0
local lastId, lastName, lastIdx, lastDur = "", "", -1, -1

function _get_data()
    return { { in_count = tostring(inCount), out_count = tostring(outCount),
               last_id = lastId, last_name = lastName,
               last_idx = tostring(lastIdx), last_dur = tostring(lastDur) } }
end

function _on_trigger_in(title, recordIndex, duration)
    inCount = inCount + 1
    lastId = title.id
    lastName = title.name
    lastIdx = recordIndex
    lastDur = duration
end

function _on_trigger_out(title)
    outCount = outCount + 1
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    ScriptDataSource* dsPtr = ds.get();

    Scene scene;
    const std::string sourceId = scene.Pool().Add(std::move(ds));
    Title* title = scene.AddTitle(MakeTitle("lower_third"));
    title->dataSourceId = sourceId;

    bool cppIn = false, cppOut = false;
    size_t cppIdx = 999;
    double cppDur = -999.0;
    // A host-UI-style listener. These fire synchronously on the host thread,
    // regardless of ScriptDataSource's worker-thread internals.
    title->onTriggerIn.push_back([&](size_t idx, double dur) { cppIn = true; cppIdx = idx; cppDur = dur; });
    title->onTriggerOut.push_back([&] { cppOut = true; });

    title->TriggerIn(4, 1.25);
    Check(cppIn, "ScenarioA: host UI onTriggerIn listener fired");
    Check(cppIdx == 4, "ScenarioA: onTriggerIn recordIndex forwarded");
    Check(cppDur == 1.25, "ScenarioA: onTriggerIn duration forwarded");

    title->TriggerOut();
    Check(cppOut, "ScenarioA: host UI onTriggerOut listener fired");

    // The Lua-side _on_trigger_in/_on_trigger_out only run once
    // ScriptDataSource's worker thread dequeues the job and, separately,
    // _get_data() is re-run — poll dsPtr->GetData() (which never blocks) until
    // the counters catch up.
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
            return !recs.empty() && recs[0].count("last_id") && recs[0].at("last_id") == title->id;
        }),
        "ScenarioA: Lua _on_trigger_in saw the title's uuid (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("last_name") && recs[0].at("last_name") == "lower_third";
        }),
        "ScenarioA: Lua _on_trigger_in saw the title's name (async)"
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

// ── Scenario B: bare Lua trigger_out() hides every title on that source ──────
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
    Scene scene;
    const std::string sourceId = scene.Pool().Add(std::move(ds));
    Title* title = scene.AddTitle(MakeTitle("b"));
    title->dataSourceId = sourceId;
    title->state = TitleState::Visible;

    // A Title with no elements finishes any animation within the same tick it
    // starts (there is nothing left to wait on), and Scene::Tick both applies
    // the trigger and advances the title — so give it one short-animation
    // element, or AnimatingOut would never be observable from out here.
    auto el = MakeShortAnimElement("a");
    title->GetRoot()->AddChild(el.get());
    title->elements.push_back(std::move(el));

    // Lua's bare trigger_out() only stashes a pending request (the empty-string
    // "every title reading this source" sentinel) on the worker thread;
    // Scene::Tick is what drains and applies it, so poll by driving Tick()
    // instead of asserting immediately.
    Check(
        PollUntil([&] {
            scene.Tick(0.01f);
            return title->state == TitleState::AnimatingOut;
        }),
        "ScenarioB: bare Lua trigger_out() drove state to AnimatingOut via Scene::Tick (async)"
    );

    // A script calling the removed trigger_in() must fail loudly (Lua "attempt
    // to call a nil value") rather than silently no-op. _get_data() errors are
    // caught by the protected call, so the observable effect is that no records
    // ever land. This doesn't need a Scene at all — it's purely about the raw
    // ScriptDataSource's Lua environment.
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

// ── Scenario F: a script's own trigger_out() applies regardless of state ────
// The title is deliberately parked in AnimatingIn (behind a long in-animation)
// for the whole test — Scene::Tick's drain/apply step is unconditional on a
// title's state (other than the already-Hidden/AnimatingOut guard), so it must
// still land even though the title never becomes Visible.

void ScenarioF()
{
    std::string path = WriteScript("scenario_f", R"lua(
local armed, fired = false, false

-- Arm only once the host has actually shown us, so the worker's startup fetch
-- can't stash a trigger_out before the title is even triggered.
function _on_trigger_in(title, recordIndex, duration)
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
    Scene scene;
    const std::string sourceId = scene.Pool().Add(std::move(ds));
    Title* title = scene.AddTitle(MakeTitle("f"));
    title->dataSourceId = sourceId;

    // Long in-animation so the title stays in AnimatingIn for the whole test.
    auto el = std::make_unique<RectangleElement>();
    el->SetId("a");
    el->SetBounds({0.0, 0.0, 100.0, 100.0});
    el->inAnimation  = {AnimationType::Fade, Easing::Linear, 5.0f, 0.0f};
    el->outAnimation = {AnimationType::Fade, Easing::Linear, 0.05f, 0.0f};
    title->GetRoot()->AddChild(el.get());
    title->elements.push_back(std::move(el));

    title->TriggerIn();
    Check(title->state == TitleState::AnimatingIn, "ScenarioF: host TriggerIn puts Title in AnimatingIn");

    Check(
        PollUntil([&] {
            scene.Tick(0.01f);
            return title->state == TitleState::AnimatingOut;
        }),
        "ScenarioF: Lua's own trigger_out() applied via Scene::Tick alone, "
        "with the title stuck in AnimatingIn the whole time"
    );
}

// ── Scenario E: TriggerIn from Hidden blocks for fresh data ─────────────────
// Title::TriggerIn fetches through DataPool::DataBlocking(), which waits for a
// real, fresh fetch (even when _get_data is slow) before returning — so no
// polling is needed here, unlike every other async path above.

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
    Scene scene;
    const std::string sourceId = scene.Pool().Add(std::move(ds));
    Title* title = scene.AddTitle(MakeTitle("e"));
    title->dataSourceId = sourceId;
    TextElement* el = AddText(*title, "greeting");

    Check(title->state == TitleState::Hidden, "ScenarioE: Title starts Hidden");

    title->TriggerIn();
    Check(
        el->text == "hello",
        "ScenarioE: TriggerIn blocked until fresh (slow) data was ready and applied it instantly"
    );
}

// ── Scenario G: one ScriptDataSource read by TWO titles ─────────────────────
// A single source shared by more than one Title. Verifies both titles receive
// records, each gets its own _on_trigger_in(title, ...) with the right handle,
// and that the script can look a title up by name (scene.find_titles) and hide
// exactly that one — leaving the other Visible.

void ScenarioG()
{
    std::string path = WriteScript("scenario_g", R"lua(
local insA, insB = false, false
local firedOutA = false
local foundCount = -1

function _get_data()
    local matches = scene.find_titles("a")
    foundCount = #matches

    -- Once both titles have checked in, ask the host to hide "a" specifically.
    if insA and insB and not firedOutA and foundCount == 1 then
        firedOutA = true
        trigger_out(matches[1])
    end

    return { { greeting = "hi",
               seen_a = tostring(insA), seen_b = tostring(insB),
               found = tostring(foundCount),
               total = tostring(#scene.titles()) } }
end

function _on_trigger_in(title, recordIndex, duration)
    if title.name == "a" then insA = true end
    if title.name == "b" then insB = true end
end
)lua");

    auto ds = std::make_unique<ScriptDataSource>(path);
    ScriptDataSource* dsPtr = ds.get();

    Scene scene;
    const std::string sourceId = scene.Pool().Add(std::move(ds));

    Title* titleA = scene.AddTitle(MakeTitle("a"));
    Title* titleB = scene.AddTitle(MakeTitle("b"));
    titleA->dataSourceId = sourceId;
    titleB->dataSourceId = sourceId;
    TextElement* elA = AddText(*titleA, "greeting");
    TextElement* elB = AddText(*titleB, "greeting");

    // Publish the directory before the script looks anything up.
    scene.Tick(0.01f);

    // (a) Both titles fetch the shared source's records for themselves.
    titleA->TriggerIn();
    titleB->TriggerIn();
    Check(elA->text == "hi", "ScenarioG: title 'a' received the shared source's records");
    Check(elB->text == "hi", "ScenarioG: title 'b' received the shared source's records");

    titleA->state = TitleState::Visible;
    titleB->state = TitleState::Visible;

    // (b) _on_trigger_in fired once per title, each with its own handle.
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("seen_a") && recs[0].at("seen_a") == "true"
                && recs[0].count("seen_b") && recs[0].at("seen_b") == "true";
        }),
        "ScenarioG: _on_trigger_in fired once per title, each with the correct handle (async)"
    );

    // (c) The script sees the whole scene through the published directory.
    Check(
        PollUntil([&] {
            auto recs = dsPtr->GetData();
            return !recs.empty() && recs[0].count("found") && recs[0].at("found") == "1"
                && recs[0].count("total") && recs[0].at("total") == "2";
        }),
        "ScenarioG: scene.find_titles(\"a\") found one title, scene.titles() saw both (async)"
    );

    // (d) trigger_out(handle) hides only the title that handle names.
    Check(
        PollUntil([&] {
            scene.Tick(0.01f);
            return titleA->state == TitleState::AnimatingOut;
        }),
        "ScenarioG: script's trigger_out(scene.find_titles(\"a\")[1]) hid title 'a' (async)"
    );
    Check(titleB->state == TitleState::Visible, "ScenarioG: title 'b' was left untouched");
}

}  // namespace

int main()
{
    ScenarioA();
    ScenarioB();
    ScenarioF();
    ScenarioE();

    // Scenario C: timed titles — duration auto-fires TriggerOut() once Visible.
    // No data source involved, so this drives Title::Tick directly.
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

    ScenarioG();

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
