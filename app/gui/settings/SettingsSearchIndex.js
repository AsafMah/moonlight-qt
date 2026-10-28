.pragma library

// Settings search: normalization, ranking and index collection.
//
// There is no hand-maintained list of settings. collect() walks the live item
// tree and picks up every item that exposes `settingsSearchTitle` (SettingsRow
// and SettingsCard do so automatically; custom composites declare it). The
// category comes from the nearest ancestor with `settingsCategory`, the
// section from the nearest SettingsCard. tests/settings_search guards both.

var FIELD_WEIGHTS = {
    title:       { exact: 100, prefix: 90, contains: 60 },
    keywords:    { exact: 85,  prefix: 70, contains: 35 },
    section:     { exact: 40,  prefix: 35, contains: 20 },
    description: { exact: 30,  prefix: 25, contains: 15 },
    category:    { exact: 25,  prefix: 20, contains: 10 }
}
var CJK_CONTAINS_SCORE = 80
var EXACT_TITLE_BONUS = 200
var TITLE_PREFIX_BONUS = 60

// Scripts that are kept as searchable characters. Everything else (punctuation,
// symbols, full-width brackets, combining marks) becomes a word separator.
var NON_WORD = /[^0-9a-z\u00c0-\u024f\u0370-\u03ff\u0400-\u04ff\u0590-\u05ff\u0600-\u06ff\u0900-\u097f\u0e00-\u0e7f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uf900-\ufaff]+/g
var COMBINING = /[\u0300-\u036f]/g
var CJK = /[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uf900-\ufaff]/

function normalize(text) {
    if (text === undefined || text === null) {
        return ""
    }
    var s = String(text)
    if (typeof s.normalize === "function") {
        // NFKD splits accents off their letters and folds full-width forms
        s = s.normalize("NFKD")
    }
    s = s.replace(COMBINING, "").toLowerCase().replace(NON_WORD, " ")
    return s.replace(/^ +| +$/g, "")
}

function prepareField(text) {
    var n = normalize(text)
    return { text: n, words: n === "" ? [] : n.split(" ") }
}

// Entries: { title, description, keywords, section, categoryKey,
// categoryTitle, categoryOrder, order, ... }. Extra fields are kept on `source`.
function prepare(entries) {
    var out = []
    for (var i = 0; i < entries.length; i++) {
        var e = entries[i]
        out.push({
            source: e,
            title: prepareField(e.title),
            keywords: prepareField(e.keywords),
            section: prepareField((e.section || "") + " " + (e.sectionKeywords || "")),
            description: prepareField(e.description),
            category: prepareField((e.categoryTitle || "") + " " + (e.categoryKey || "")),
            categoryOrder: e.categoryOrder || 0,
            order: e.order || 0
        })
    }
    return out
}

function fieldScore(token, field, weights, allowContains, cjk) {
    var best = 0
    for (var i = 0; i < field.words.length; i++) {
        var w = field.words[i]
        if (w === token) {
            return weights.exact
        }
        if (best < weights.prefix && w.indexOf(token) === 0) {
            best = weights.prefix
        }
    }
    if (best === 0 && allowContains && field.text.indexOf(token) >= 0) {
        // CJK has no spaces between words, so a substring hit in a title is
        // as good as a word-prefix hit in Latin text.
        best = cjk && weights === FIELD_WEIGHTS.title ? CJK_CONTAINS_SCORE : weights.contains
    }
    return best
}

function tokenScore(entry, token) {
    var cjk = CJK.test(token)
    // A single Latin letter inside a word is noise ("v" in "driver")
    var allowContains = cjk || token.length >= 2
    var best = 0
    var names = ["title", "keywords", "section", "description", "category"]
    for (var i = 0; i < names.length; i++) {
        var s = fieldScore(token, entry[names[i]], FIELD_WEIGHTS[names[i]], allowContains, cjk)
        if (s > best) {
            best = s
        }
    }
    return best
}

function search(prepared, query, limit) {
    var q = normalize(query)
    if (q === "") {
        return []
    }
    var tokens = q.split(" ")
    var hits = []
    for (var i = 0; i < prepared.length; i++) {
        var entry = prepared[i]
        var total = 0
        for (var t = 0; t < tokens.length; t++) {
            var s = tokenScore(entry, tokens[t])
            if (s === 0) {
                total = 0
                break
            }
            total += s
        }
        if (total === 0) {
            continue
        }
        if (entry.title.text === q) {
            total += EXACT_TITLE_BONUS
        }
        else if (entry.title.text.indexOf(q) === 0) {
            total += TITLE_PREFIX_BONUS
        }
        hits.push({ entry: entry, score: total })
    }
    hits.sort(function(a, b) {
        return (b.score - a.score)
               || (a.entry.categoryOrder - b.entry.categoryOrder)
               || (a.entry.order - b.entry.order)
    })
    var out = []
    for (var h = 0; h < hits.length && (limit === undefined || out.length < limit); h++) {
        var src = hits[h].entry.source
        out.push({
            title: src.title || "",
            description: src.description || "",
            section: src.section || "",
            categoryKey: src.categoryKey || "",
            categoryTitle: src.categoryTitle || "",
            score: hits[h].score,
            source: src
        })
    }
    return out
}

function isSearchable(item) {
    return typeof item.settingsSearchTitle === "string" && item.settingsSearchTitle !== ""
}

// Walks the live item tree under `root` and returns search entries.
//
// context: { categories: [{ key, title }], currentCategory: "basic",
//            uncategorized: [] (optional, receives searchables without a category) }
//
// Availability: items with `applicable === false` (and their subtrees) are
// skipped. In the current category the real `visible` is used as well. Other
// categories are hidden right now, so only `applicable` can be evaluated there
// — which is why settings use `applicable` instead of `visible` bindings (the
// metadata gate enforces this).
function collect(root, context) {
    var categoryIndex = {}
    for (var c = 0; c < context.categories.length; c++) {
        categoryIndex[context.categories[c].key] = c
    }
    var entries = []
    var counter = { order: 0 }

    function walk(item, category, section, sectionKeywords) {
        if (!item || item.applicable === false) {
            return 0
        }
        if (typeof item.settingsCategory === "string" && item.settingsCategory !== "") {
            category = item.settingsCategory
        }
        var searchable = isSearchable(item)
        var isCard = searchable && item.settingsSearchKind === "card"
        var childSection = isCard ? item.settingsSearchTitle : section
        // A card's keywords describe everything in it, so its rows match them too
        var childKeywords = isCard ? (item.settingsSearchKeywords || "") : sectionKeywords
        var myOrder = counter.order++

        var found = 0
        var kids = item.children
        for (var i = 0; i < kids.length; i++) {
            found += walk(kids[i], category, childSection, childKeywords)
        }

        // A card is only its own result when nothing finer inside it is
        // searchable; otherwise its title acts as the section of its rows.
        if (!searchable || (isCard && found > 0)) {
            return found
        }
        if (isCard && item.hasVisibleContent === false) {
            return found
        }
        if (category === undefined) {
            if (context.uncategorized) {
                context.uncategorized.push(item)
            }
            return found
        }
        if (!(category in categoryIndex)) {
            return found
        }
        if (category === context.currentCategory && !item.visible) {
            return found
        }
        entries.push({
            item: item,
            kind: item.settingsSearchKind || "item",
            title: item.settingsSearchTitle,
            description: item.settingsSearchDescription || "",
            keywords: item.settingsSearchKeywords || "",
            section: isCard ? "" : (section || ""),
            sectionKeywords: isCard ? "" : (sectionKeywords || ""),
            categoryKey: category,
            categoryTitle: context.categories[categoryIndex[category]].title,
            categoryOrder: categoryIndex[category],
            order: myOrder
        })
        return found + 1
    }

    walk(root, undefined, "", "")
    return entries
}

function isFocusable(item) {
    return item.activeFocusOnTab && item.visible && item.enabled
            && item.width > 0 && item.height > 0
}

// The control to focus for a result: an explicit settingsSearchFocusTarget,
// else the first visible, enabled, Tab-focusable descendant, else null.
function focusTargetFor(item) {
    if (item.settingsSearchFocusTarget) {
        return item.settingsSearchFocusTarget
    }
    if (isFocusable(item)) {
        return item
    }
    var kids = item.children
    for (var i = 0; i < kids.length; i++) {
        if (!kids[i].visible) {
            continue
        }
        var found = focusTargetFor(kids[i])
        if (found) {
            return found
        }
    }
    return null
}
