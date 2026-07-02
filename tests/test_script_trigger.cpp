// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering): verifies
//  - Title::onTriggerIn / onTriggerOut fire on every TriggerIn/TriggerOut call,
//    with both a host-UI-style listener and ScriptDataSource's own listener
//    (registered via SetOwner) coexisting on the same event lists
//  - ScriptDataSource's listener relays to the Lua _on_trigger_in/_on_trigger_out
//    functions, if the script defines them
//  - Lua scripts can call trigger_in()/trigger_out() to drive the owning Title
//  - Title::duration auto-fires TriggerOut() once Visible time elapses
//  - The periodic data poll in Title::Tick only runs while state == Visible

#include "title.h"
#include "element_rectangle.h"
#include "script.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

std::string WriteScript(const std::string& name, const std::string& content)
{
    std::string path = (fs::temp_directory_path() / ("ogt-test-" + name + ".lua")).string();
    std::ofstream f(path, std::ios::trunc);
    f << content;
    f.close();
    return path;
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
    title.onTriggerIn.push_back([&](size_t idx, double dur) { cppIn = true; cppIdx = idx; cppDur = dur; });
    title.onTriggerOut.push_back([&] { cppOut = true; });

    title.TriggerIn(4, 1.25);
    Check(cppIn, "ScenarioA: host UI onTriggerIn listener fired");
    Check(cppIdx == 4, "ScenarioA: onTriggerIn recordIndex forwarded");
    Check(cppDur == 1.25, "ScenarioA: onTriggerIn duration forwarded");

    title.TriggerOut();
    Check(cppOut, "ScenarioA: host UI onTriggerOut listener fired");

    auto records = ds.GetData();
    Check(!records.empty(), "ScenarioA: GetData returned a record");
    if (!records.empty()) {
        auto& rec = records[0];
        Check(rec["in_count"] == "1", "ScenarioA: Lua _on_trigger_in fired exactly once");
        Check(rec["out_count"] == "1", "ScenarioA: Lua _on_trigger_out fired exactly once");
        Check(rec["last_idx"] == "4", "ScenarioA: Lua _on_trigger_in saw recordIndex 4");
        Check(rec["last_dur"] == "1.25", "ScenarioA: Lua _on_trigger_in saw duration 1.25");
    }
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
    ds.GetData();  // Lua calls trigger_in(9, 2.5) -> Title::TriggerIn(9, 2.5)
    Check(title.state == TitleState::AnimatingIn, "ScenarioB: Lua trigger_in() drove state to AnimatingIn");
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

    ds2.GetData();  // Lua calls trigger_out() -> Title::TriggerOut()
    Check(title2.state == TitleState::AnimatingOut, "ScenarioB: Lua trigger_out() drove state to AnimatingOut");
}

} // namespace

int main()
{
    ScenarioA();
    ScenarioB();

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
