// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): verifies
//  - Title::onTriggerIn / onTriggerOut fire on every TriggerIn/TriggerOut call,
//    with both a host-UI-style listener and ScriptDataSource's own listener
//    (registered via SetOwner) coexisting on the same event lists
//  - ScriptDataSource's listener relays to the Lua _on_trigger_in/_on_trigger_out
//    functions, if the script defines them (now via ScriptDataSource's own
//    worker thread, so these assertions poll-with-timeout instead of checking
//    immediately)
//  - Lua scripts can call trigger_in()/trigger_out() to drive the owning Title
//    (marshaled back to the host thread through GetData()'s pending-trigger
//    drain, so these also poll)
//  - Title::duration auto-fires TriggerOut() once Visible time elapses
//  - The periodic data poll in Title::Tick only runs while state == Visible
//  - TriggerIn from Hidden blocks (via GetDataBlocking) until real, fresh data
//    is available, even when _get_data is slow — no polling needed there

#include "title.h"
#include "element_rectangle.h"
#include "element_text.h"
#include "script.h"
#include "script_test_util.h"

#include <cstdlib>
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
local lastIdx, lastDur = -1, -1

function _get_data()
    return { { in_count = tostring(inCount), out_count = tostring(outCount),
               last_idx = tostring(lastIdx), last_dur = tostring(lastDur) } }
end

function _on_trigger_in(recordIndex, duration)
    inCount = inCount + 1
    lastIdx = recordIndex
    lastDur = duration
end

function _on_trigger_out()
    outCount = outCount + 1
end
)lua");

    ScriptDataSource ds(path);
    Title title;
    title.dataSource = &ds;

    bool cppIn = false, cppOut = false;
    size_t cppIdx = 999;
    double cppDur = -999.0;
    // A host-UI-style listener, registered alongside whatever ScriptDataSource
    // registers for itself once it learns its owner (see Title::UpdateData).
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
    // poll ds.GetData() (which never blocks) until the counters catch up.
    Check(
        PollUntil([&] {
            auto recs = ds.GetData();
            return !recs.empty() && recs[0].count("in_count") && recs[0].at("in_count") == "1";
        }),
        "ScenarioA: Lua _on_trigger_in fired exactly once (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = ds.GetData();
            return !recs.empty() && recs[0].count("out_count") && recs[0].at("out_count") == "1";
        }),
        "ScenarioA: Lua _on_trigger_out fired exactly once (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = ds.GetData();
            return !recs.empty() && recs[0].count("last_idx") && recs[0].at("last_idx") == "4";
        }),
        "ScenarioA: Lua _on_trigger_in saw recordIndex 4 (async)"
    );
    Check(
        PollUntil([&] {
            auto recs = ds.GetData();
            return !recs.empty() && recs[0].count("last_dur") && recs[0].at("last_dur") == "1.25";
        }),
        "ScenarioA: Lua _on_trigger_in saw duration 1.25 (async)"
    );
}

// ── Scenario B: Lua trigger_in()/trigger_out() drive the owning Title ──────────

void ScenarioB()
{
    std::string path = WriteScript("scenario_b", R"lua(
local fired = false
function _get_data()
    if not fired then
        fired = true
        trigger_in(9, 2.5)
    end
    return {}
end
)lua");

    ScriptDataSource ds(path);
    Title title;
    ds.SetOwner(&title);

    Check(title.state == TitleState::Hidden, "ScenarioB: Title starts Hidden");
    // Lua's trigger_in() now only stashes a pending request on the worker
    // thread; ds.GetData() (called here on the host thread) is what drains
    // and applies it to Title, so poll instead of asserting immediately.
    Check(
        PollUntil([&] {
            ds.GetData();
            return title.state == TitleState::AnimatingIn;
        }),
        "ScenarioB: Lua trigger_in() drove state to AnimatingIn (async)"
    );
    Check(title.dataRecordIndex == 9, "ScenarioB: Lua trigger_in() forwarded recordIndex");
    Check(title.duration == 2.5, "ScenarioB: Lua trigger_in() forwarded duration");

    std::string path2 = WriteScript("scenario_b2", R"lua(
local fired = false
function _get_data()
    if not fired then
        fired = true
        trigger_out()
    end
    return {}
end
)lua");

    ScriptDataSource ds2(path2);
    Title title2;
    ds2.SetOwner(&title2);
    title2.state = TitleState::Visible;

    Check(
        PollUntil([&] {
            ds2.GetData();
            return title2.state == TitleState::AnimatingOut;
        }),
        "ScenarioB: Lua trigger_out() drove state to AnimatingOut (async)"
    );
}

// ── Scenario F: a script's own trigger_in() takes effect purely through
// Title::Tick's unconditional PumpEvents() call, with no direct GetData() or
// host-driven TriggerIn() call at all. This is the production path for a
// title sitting idle Hidden whose script decides to trigger itself — the gap
// the design review caught: nothing else ever polls a Hidden title's data
// source, so without this pump the self-trigger would never apply.

void ScenarioF()
{
    std::string path = WriteScript("scenario_f", R"lua(
local fired = false
function _get_data()
    if not fired then
        fired = true
        trigger_in(7, -1.0)
    end
    return {}
end
)lua");

    ScriptDataSource ds(path);
    Title title;
    title.dataSource = &ds;
    ds.SetOwner(&title);

    Check(title.state == TitleState::Hidden, "ScenarioF: Title starts Hidden");
    // Only Title::Tick is driven here — never ds.GetData()/title.TriggerIn()
    // directly. No elements are attached, so once triggered, Tick's
    // all-done check is vacuously true and state advances straight past
    // AnimatingIn to Visible within the same tick — check "left Hidden",
    // not a specific intermediate state.
    Check(
        PollUntil([&] {
            title.Tick(0.01f);
            return title.state != TitleState::Hidden;
        }),
        "ScenarioF: Lua's own trigger_in() applied via Title::Tick's PumpEvents while Hidden"
    );
    Check(title.dataRecordIndex == 7, "ScenarioF: self-trigger forwarded recordIndex");
}

// ── Scenario E: TriggerIn from Hidden waits for real, fresh data ───────────────
// (even when _get_data is slow) before returning — no polling needed here,
// unlike every other async path above. This is what makes GetDataBlocking()
// meaningfully different from the plain non-blocking GetData().

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

    ScriptDataSource ds(path);
    Title title;
    title.dataSource = &ds;

    auto el = std::make_unique<TextElement>();
    el->SetId("greeting");
    title.GetRoot()->AddChild(el.get());
    TextElement* elPtr = el.get();
    title.elements.push_back(std::move(el));

    Check(title.state == TitleState::Hidden, "ScenarioE: Title starts Hidden");
    title.TriggerIn();  // Hidden -> instant path must wait for real data before returning
    Check(
        elPtr->text == "hello",
        "ScenarioE: TriggerIn-from-Hidden blocked until fresh (slow) data was applied, no poll needed"
    );
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

    // Scenario D: periodic data poll only runs while Visible.
    {
        struct CountingDataSource : IDataSource {
            mutable int calls = 0;
            std::vector<Record> GetData() const override { ++calls; return {}; }
            std::string GetFilePath() const override { return ""; }
        };

        CountingDataSource counter;
        Title title;
        title.dataSource = &counter;

        auto el = MakeShortAnimElement("a");
        title.GetRoot()->AddChild(el.get());
        title.elements.push_back(std::move(el));

        const float dt = 0.01f;
        for (int i = 0; i < 20; ++i) title.Tick(dt);  // still Hidden, never triggered
        Check(counter.calls == 0, "ScenarioD: no polling while Hidden and never triggered");

        title.TriggerIn();  // instant update while Hidden
        Check(counter.calls == 1, "ScenarioD: TriggerIn while Hidden does one instant update");

        float t = 0.0f;
        while (title.state == TitleState::AnimatingIn && t < 1.0f) {
            title.Tick(dt);
            t += dt;
        }
        Check(title.state == TitleState::Visible, "ScenarioD: reached Visible");
        Check(counter.calls == 1, "ScenarioD: no polling during AnimatingIn");

        t = 0.0f;
        while (t < 0.30f) {  // more than one 0.25s poll slot
            title.Tick(dt);
            t += dt;
        }
        Check(counter.calls == 2, "ScenarioD: exactly one periodic poll after 0.25s Visible");

        title.TriggerOut();
        t = 0.0f;
        while (title.state != TitleState::Hidden && t < 1.0f) {
            title.Tick(dt);
            t += dt;
        }
        Check(counter.calls == 2, "ScenarioD: no further polling once no longer Visible");
    }

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
