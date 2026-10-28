import QtQuick
import QtTest
import "../../app/gui/settings/SettingsSearchIndex.js" as SearchIndex

// Pure ranking/normalization logic of the settings search. The live index is
// collected from the item tree (see tst_search_navigation.qml); here the
// entries are plain objects so every ranking rule can be pinned down.
TestCase {
    name: "SettingsSearchIndex"

    function entry(order, title, extra) {
        var e = {
            order: order,
            title: title,
            description: "",
            keywords: "",
            section: "",
            categoryKey: "basic",
            categoryTitle: "Streaming",
            categoryOrder: 0
        }
        for (var k in extra) {
            e[k] = extra[k]
        }
        return e
    }

    function titles(results) {
        return results.map(function(r) { return r.title })
    }

    function test_normalize_data() {
        return [
            { tag: "case and punctuation", input: "  Variable  Refresh-Rate (G-Sync)  ", expected: "variable refresh rate g sync" },
            { tag: "diacritics", input: "Résolution et fréquence", expected: "resolution et frequence" },
            { tag: "cjk kept", input: "可变刷新率（VRR）", expected: "可变刷新率 vrr" },
            { tag: "digits and colon", input: "YUV 4:4:4", expected: "yuv 4 4 4" },
            { tag: "empty", input: "", expected: "" },
            { tag: "undefined", input: undefined, expected: "" }
        ]
    }

    function test_normalize(data) {
        compare(SearchIndex.normalize(data.input), data.expected)
    }

    function corpus() {
        return [
            entry(0, "Resolution and FPS", { description: "Increasing resolution and frame rate requires more bandwidth" }),
            entry(1, "Video bitrate", { keywords: "bandwidth quality mbps" }),
            entry(2, "Frame pacing", { categoryKey: "display", categoryTitle: "Display Settings", categoryOrder: 1,
                                       description: "Frame pacing reduces micro-stutter" }),
            entry(3, "Variable refresh rate (G-Sync / FreeSync)", { categoryKey: "display", categoryTitle: "Display Settings",
                                                                    categoryOrder: 1, keywords: "vrr gsync freesync adaptive sync" }),
            entry(4, "V-Sync", { categoryKey: "display", categoryTitle: "Display Settings", categoryOrder: 1,
                                 description: "Disabling V-Sync allows tearing", keywords: "vsync vertical sync tearing" }),
            entry(5, "Enable HDR", { categoryKey: "display", categoryTitle: "Display Settings", categoryOrder: 1,
                                     section: "HDR", keywords: "high dynamic range" }),
            entry(6, "Audio configuration", { categoryKey: "audio", categoryTitle: "Audio Settings", categoryOrder: 2,
                                              keywords: "surround stereo 5.1 7.1 speakers" }),
            entry(7, "Mute host PC speakers while streaming", { categoryKey: "audio", categoryTitle: "Audio Settings",
                                                                categoryOrder: 2 }),
            entry(8, "Show local cursor", { categoryKey: "input", categoryTitle: "Input Settings", categoryOrder: 3,
                                            section: "Mouse", keywords: "pointer" }),
            entry(9, "可变刷新率", { categoryKey: "display", categoryTitle: "显示", categoryOrder: 1,
                                     keywords: "vrr gsync freesync" }),
            entry(10, "Driver selection", { categoryKey: "advanced", categoryTitle: "Compatibility and diagnostics",
                                            categoryOrder: 4 })
        ]
    }

    function prepared() {
        return SearchIndex.prepare(corpus())
    }

    function test_emptyQueryReturnsNothing() {
        compare(SearchIndex.search(prepared(), "", 50).length, 0)
        compare(SearchIndex.search(prepared(), "   ", 50).length, 0)
        compare(SearchIndex.search(prepared(), "()", 50).length, 0)
    }

    function test_keywordSynonyms_data() {
        return [
            { tag: "fps", query: "fps", first: "Resolution and FPS" },
            { tag: "vrr", query: "vrr", first: "Variable refresh rate (G-Sync / FreeSync)" },
            { tag: "gsync", query: "gsync", first: "Variable refresh rate (G-Sync / FreeSync)" },
            { tag: "hdr", query: "hdr", first: "Enable HDR" },
            { tag: "mouse via section", query: "mouse", first: "Show local cursor" },
            { tag: "surround", query: "surround", first: "Audio configuration" },
            { tag: "mbps", query: "mbps", first: "Video bitrate" }
        ]
    }

    function test_keywordSynonyms(data) {
        var results = SearchIndex.search(prepared(), data.query, 50)
        verify(results.length > 0, "no results for " + data.query)
        compare(results[0].title, data.first)
    }

    function test_titleBeatsDescription() {
        // "Frame pacing" has "frame" in its title; "Resolution and FPS" only in
        // its description. Title must win even though it is earlier in order.
        var results = titles(SearchIndex.search(prepared(), "frame", 50))
        compare(results[0], "Frame pacing")
        verify(results.indexOf("Resolution and FPS") > 0)
    }

    function test_allTokensMustMatch() {
        var results = titles(SearchIndex.search(prepared(), "frame stutter", 50))
        compare(results, ["Frame pacing"])
        compare(SearchIndex.search(prepared(), "frame banana", 50).length, 0)
    }

    function test_cjkSubstring() {
        var results = titles(SearchIndex.search(prepared(), "刷新", 50))
        compare(results[0], "可变刷新率")
    }

    function test_categoryKeyMatchesWholeCategoryWithLowerRank() {
        var results = SearchIndex.search(prepared(), "audio", 50)
        compare(titles(results).sort(), ["Audio configuration", "Mute host PC speakers while streaming"])
        // A title hit is ranked above an entry that only matched its category
        compare(results[0].title, "Audio configuration")
    }

    function test_categoryKeyMatchesWhenCategoryTitleIsTranslated() {
        var results = titles(SearchIndex.search(prepared(), "display", 50))
        verify(results.indexOf("可变刷新率") >= 0, "English category key must match translated category")
    }

    function test_singleLetterOnlyMatchesWordStarts() {
        var results = titles(SearchIndex.search(prepared(), "v", 50))
        verify(results.indexOf("V-Sync") >= 0)
        verify(results.indexOf("Video bitrate") >= 0)
        verify(results.indexOf("Driver selection") < 0, "single letters must not match mid-word")
    }

    function test_exactTitleRanksFirst() {
        var results = titles(SearchIndex.search(prepared(), "v sync", 50))
        compare(results[0], "V-Sync")
    }

    function test_tieBreakFollowsCategoryThenTreeOrder() {
        var entries = SearchIndex.prepare([
            entry(5, "Toggle B", { categoryOrder: 2 }),
            entry(9, "Toggle C", { categoryOrder: 1 }),
            entry(1, "Toggle A", { categoryOrder: 2 })
        ])
        compare(titles(SearchIndex.search(entries, "toggle", 50)), ["Toggle C", "Toggle A", "Toggle B"])
    }

    function test_limit() {
        var many = []
        for (var i = 0; i < 80; i++) {
            many.push(entry(i, "Option " + i))
        }
        compare(SearchIndex.search(SearchIndex.prepare(many), "option", 50).length, 50)
    }

    function test_queryDiacriticsAndCaseInsensitive() {
        compare(titles(SearchIndex.search(prepared(), "VÍDEO", 50))[0], "Video bitrate")
    }

    function test_resultsCarryScoreAndSource() {
        var source = corpus()
        var results = SearchIndex.search(SearchIndex.prepare(source), "hdr", 50)
        verify(results[0].score > 0)
        verify(results[0].source === source[5])
    }
}
