// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Lopes <diego95lopes@gmail.com>
//
// Headless console test (no SDL, no rendering, no Lua) for the temp asset
// directory Title::Load extracts a .ogt's ASSETS/ into, and the ownership of
// that directory across a move (title.h/title.cpp) — see the CLAUDE.md
// ".ogt file format" and "Title" sections for the contract this covers.
//
// The regression this guards: a reload path loads the replacement Title
// before destroying the original. If both Titles loaded from the same .ogt
// ever resolved to the same temp directory, the original's destructor would
// delete the assets the replacement is still using the moment it ran. The
// same collision hits opening one .ogt into a Scene twice.
//
//  - Two Titles loaded from the same .ogt get DIFFERENT temp directories
//    (observed via the resolved ImageElement::imagePath of each, since
//    m_tempAssetDir itself is private).
//  - Destroying the first of those Titles leaves the second one's asset file
//    on disk.
//  - A moved-from Title's destructor does not delete the moved-to Title's
//    asset file (the defaulted-move hazard the constructor/assignment
//    operator comments in title.h describe).

#include "element_image.h"
#include "script_test_util.h"
#include "title.h"
#include "uuid.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using script_test_util::Check;
using script_test_util::g_failures;

namespace fs = std::filesystem;

namespace {

// Builds a .ogt at `ogtPath` with one ImageElement ("img") pointing at a
// freshly-written source file at `sourcePath`. Title::Save only checks that a
// referenced asset is a readable file — it never decodes it — so the bytes
// don't need to be a real PNG, only present.
void BuildOgtWithImage(const std::string& ogtPath, const std::string& sourcePath)
{
    // Not a real PNG — a handful of bytes. Nothing in this test decodes it.
    std::ofstream f(sourcePath, std::ios::binary);
    f << "not a real png, just needs to exist";
    f.close();

    Title title;
    auto img = std::make_unique<ImageElement>();
    img->SetId("img");
    img->imagePath = sourcePath;  // not "@"-prefixed: Save() bundles it by absolute path
    title.GetRoot()->AddChild(img.get());
    title.elements.push_back(std::move(img));

    title.Save(ogtPath);
}

std::string ResolvedImagePath(Title& title)
{
    return static_cast<ImageElement&>(title.GetById("img")).imagePath;
}

}  // namespace

int main()
{
    std::error_code ec;
    const fs::path workDir = fs::temp_directory_path() / ("ogt-asset-test-" + uuid::GenerateV4());
    fs::create_directories(workDir, ec);

    const std::string ogtPath = (workDir / "test.ogt").string();
    const std::string sourcePath = (workDir / "source.png").string();
    BuildOgtWithImage(ogtPath, sourcePath);

    // ── Two live Titles from the same .ogt get distinct temp directories ──

    auto titleA = std::make_unique<Title>(Title::Load(ogtPath));
    auto titleB = std::make_unique<Title>(Title::Load(ogtPath));

    const std::string pathA = ResolvedImagePath(*titleA);
    const std::string pathB = ResolvedImagePath(*titleB);

    Check(pathA != pathB,
          "Load: two Titles loaded from the same .ogt resolve to different asset paths");
    Check(fs::exists(pathA), "Load: the first Title's extracted asset exists on disk");
    Check(fs::exists(pathB), "Load: the second Title's extracted asset exists on disk");

    // ── Destroying one Title must not touch the other's assets ──────────

    titleA.reset();
    Check(!fs::exists(pathA),
          "~Title: the destroyed Title's own asset directory is cleaned up");
    Check(fs::exists(pathB),
          "~Title: the surviving Title's asset file is untouched by the other's destructor");

    // ── A moved-from Title must not delete the moved-to Title's assets ──

    auto titleC = std::make_unique<Title>(Title::Load(ogtPath));
    const std::string pathC = ResolvedImagePath(*titleC);
    Check(fs::exists(pathC), "Load: setup for the move test — asset exists before any move");

    // Move-construct titleD from *titleC, then destroy the moved-from titleC.
    // A defaulted move leaves titleC->m_tempAssetDir holding a copy of the
    // same path (see title.h) rather than an empty one, so a wrong
    // implementation deletes pathC right here.
    Title titleD(std::move(*titleC));
    titleC.reset();

    Check(fs::exists(pathC),
          "move ctor: a moved-from Title's destructor does not delete the moved-to Title's asset");
    Check(ResolvedImagePath(titleD) == pathC,
          "move ctor: the moved-to Title still resolves to the same asset path");

    fs::remove_all(workDir, ec);

    if (g_failures == 0) {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", g_failures);
    return 1;
}
