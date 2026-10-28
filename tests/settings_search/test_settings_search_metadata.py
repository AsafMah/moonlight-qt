"""Settings search metadata gate.

The settings search index is collected at runtime from the live item tree
(app/gui/settings/SettingsSearchIndex.js), so there is no list to keep in
sync. What *can* drift is the metadata the collector relies on. These checks
fail when:

  R1  a SettingsRow/SettingsCard-family item has no title to search by;
  R2  an interactive control has no searchable ancestor (it could never be
      found by search);
  R3  a searchable item can't be resolved to a settings category;
  R4  a settingsCategory names a category that SettingsView doesn't have, or
      a category has no settings at all;
  R5  a category is compared against a string literal instead of
      `settingsCategory` (the category must have a single source of truth);
  R6  a searchable item, or a container between it and its category, uses a
      `visible:` binding instead of `applicable` (search can only evaluate
      `applicable` for categories that aren't open).

Adding a settings section (e.g. Clipboard): use SettingsCard + SettingsRow /
ToggleRow / ChoiceRow, put it on a page instance or card with
`settingsCategory`, and add English `settingsSearchKeywords` where the title
doesn't contain the words people will type. Custom composite controls declare
`property string settingsSearchTitle` themselves.

Run: python -m unittest discover -s tests/settings_search -p "test_*.py"
"""

import os
import re
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GUI = os.path.join(REPO, "app", "gui")
SETTINGS = os.path.join(GUI, "settings")

# Controls a user operates. HardLink (plain links) is deliberately absent.
INTERACTIVE = {
    "HardSwitch", "HardSlider", "HardTextField", "HardButton", "HardCheckBox",
    "TextField", "SpinBox", "ComboBox", "AutoResizingComboBox", "CheckBox",
    "Switch", "Slider", "RadioButton", "Button", "AbstractButton", "ItemDelegate",
}

# Subtrees that are not settings content (dialogs/popups own their own UI).
EXEMPT_SUBTREES = {
    "NavigableDialog", "NavigableMessageDialog", "ErrorMessageDialog", "Dialog",
    "Popup", "Menu", "FileDialog", "FolderDialog", "ToolTip", "UsbForwardingBindDialog",
    "GamepadKeyboard",
}

# Component files that are not settings themselves. Each needs a reason.
NON_SETTING_FILES = {
    "SettingsView.qml": "the shell: rail, search box, scroll area",
    "CategoryRail.qml": "category navigation",
    "SettingsSearchField.qml": "the search box",
    "SettingsSearchResults.qml": "search results",
    "SettingsSearchController.qml": "search logic",
    "SettingsScrollArea.qml": "scroll container",
    "PlatformNavButton.qml": "ecosystem navigation button, not a setting",
    "EcosystemPlatformCatalog.qml": "download links catalog inside the searchable Clients card",
    "UsbForwardingBindDialog.qml": "dialog",
}

# Generic building blocks pulled in from app/gui that aren't settings content.
GUI_PRIMITIVES = {
    "AutoResizingComboBox", "NavigableDialog", "NavigableMessageDialog", "ErrorMessageDialog",
    "NavigableMenu", "NavigableMenuItem", "NavigableItemDelegate", "NavigableToolButton",
    "GamepadKeyboard", "ScreenCombinationPreview", "HdrBrightnessHandle", "ShortcutSettingsGroupBox",
}


# --------------------------------------------------------------------------
# Lexical QML object-tree parser. Deliberately small: it only needs object
# types, nesting, and the first part of property values.
# --------------------------------------------------------------------------

TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r]+)
  | (?P<nl>\n)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`)
  | (?P<ident>[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*)
  | (?P<num>\d+(?:\.\d+)?)
  | (?P<op>.)
""", re.VERBOSE | re.DOTALL)


def tokenize(text):
    tokens = []
    line = 1
    for m in TOKEN_RE.finditer(text):
        kind = m.lastgroup
        value = m.group()
        if kind == "nl":
            tokens.append(("nl", value, line))
        elif kind == "bc":
            if "\n" in value:
                tokens.append(("nl", "\n", line))
        elif kind not in ("ws", "lc"):
            tokens.append((kind, value, line))
        line += value.count("\n")
    tokens.append(("eof", "", line))
    return tokens


class Obj:
    def __init__(self, type_name, line, parent=None, via=None):
        self.type = type_name
        self.line = line
        self.parent = parent
        self.via = via  # property name when declared as a value (background: Rectangle {})
        self.props = {}
        self.children = []

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

    def ancestors(self):
        p = self.parent
        while p is not None:
            yield p
            p = p.parent

    def __repr__(self):
        return "%s@%d" % (self.type, self.line)


OPEN = {"{": "}", "(": ")", "[": "]"}


class Parser:
    def __init__(self, text):
        self.t = tokenize(text)
        self.i = 0

    def peek(self, k=0):
        return self.t[min(self.i + k, len(self.t) - 1)]

    def is_type(self, tok):
        return tok[0] == "ident" and tok[1].split(".")[-1][:1].isupper()

    def object_start(self):
        """`Type {` or `Type on prop {` at the cursor? Returns length or 0."""
        if not self.is_type(self.peek()):
            return 0
        if self.peek(1)[1] == "{":
            return 2
        if self.peek(1) == ("ident", "on", self.peek(1)[2]) and self.peek(2)[0] == "ident" and self.peek(3)[1] == "{":
            return 4
        return 0

    def skip_balanced(self):
        """Cursor on an opening bracket: skip to after its match."""
        depth = 0
        while self.peek()[0] != "eof":
            v = self.peek()[1]
            self.i += 1
            if self.peek(-1)[0] == "op":
                if v in OPEN:
                    depth += 1
                elif v in OPEN.values():
                    depth -= 1
                    if depth == 0:
                        return

    def value_text(self):
        """Consume a property value up to the end of its statement."""
        parts = []
        depth = 0
        while True:
            kind, v, _ = self.peek()
            if kind == "eof":
                break
            if depth == 0 and (kind == "nl" or v in (";", "}")):
                break
            if kind == "op" and v in OPEN:
                depth += 1
            elif kind == "op" and v in OPEN.values():
                if depth == 0:
                    # closer of an enclosing expression we joined mid-way
                    self.i += 1
                    continue
                depth -= 1
            if kind != "nl":
                parts.append(v)
            self.i += 1
        return " ".join(parts)

    def parse_file(self):
        while self.peek()[0] != "eof":
            n = self.object_start()
            if n:
                return self.parse_object(n, None, None)
            self.i += 1
        return None

    def parse_object(self, n, parent, via):
        obj = Obj(self.peek()[1], self.peek()[2], parent, via)
        self.i += n  # past "{"
        while True:
            kind, v, _ = self.peek()
            if kind == "eof":
                return obj
            if v == "}" and kind == "op":
                self.i += 1
                return obj
            if kind == "nl" or v == ";":
                self.i += 1
                continue
            n = self.object_start()
            if n:
                obj.children.append(self.parse_object(n, obj, None))
                continue
            if kind == "ident" and v in ("function", "enum"):
                while self.peek()[1] != "{" and self.peek()[0] != "eof":
                    self.i += 1
                self.skip_balanced()
                continue
            if kind == "ident" and v == "signal":
                self.value_text()
                continue
            if kind == "ident" and v == "component" and self.peek(2)[1] == ":":
                # inline component: component Chip: Rectangle { ... }
                self.i += 3
                n = self.object_start()
                if n:
                    obj.children.append(self.parse_object(n, obj, "component"))
                continue
            if kind == "ident" and self.peek(1)[1] == "{":
                # grouped property: anchors { fill: parent }
                self.i += 1
                self.skip_balanced()
                continue
            name = None
            if kind == "ident" and v in ("property", "readonly", "default", "required"):
                while self.peek()[1] in ("readonly", "default", "required"):
                    self.i += 1
                if self.peek()[1] == "property":
                    self.i += 1
                    # type (may be `list<Item>`), then name
                    self.i += 1
                    if self.peek()[1] == "<":
                        while self.peek()[1] != ">":
                            self.i += 1
                        self.i += 1
                    name = self.peek()[1]
                    self.i += 1
                    if self.peek()[1] != ":":
                        obj.props.setdefault(name, "")
                        continue
                else:
                    name = self.peek()[1]
                    self.i += 1
            elif kind == "ident" and self.peek(1)[1] == ":":
                name = v
                self.i += 1
            if name is None:
                # Continuation of a multi-line expression (`? a`, `: b`, `&& c`)
                self.value_text()
                continue
            self.i += 1  # past ":"
            n = self.object_start()
            if n and self.peek(n - 1)[1] == "{":
                obj.children.append(self.parse_object(n, obj, name))
                continue
            obj.props[name] = self.value_text()
        return obj


def parse_qml(text):
    return Parser(text).parse_file()


# --------------------------------------------------------------------------
# Rules
# --------------------------------------------------------------------------

def has_title(obj):
    for key in ("title", "settingsSearchTitle"):
        v = obj.props.get(key)
        if v is not None and v.strip() not in ("", '""', "''", "title"):
            return True
    return False


def declares_category(obj):
    v = obj.props.get("settingsCategory", "")
    return re.fullmatch(r'"\w+"', v.strip()) is not None


def category_of(obj):
    return obj.props["settingsCategory"].strip().strip('"')


def check(files, raw_category_keys):
    """files: {basename: root Obj}. Returns a list of violation strings."""
    errors = []

    # Types whose instances are search entries: the row/card family (root
    # declares settingsSearchKind or derives from such a type) plus custom
    # components whose root declares settingsSearchTitle.
    searchable_types = set()
    family = set()
    changed = True
    while changed:
        changed = False
        for name, root in files.items():
            t = name[:-4]
            if t in searchable_types:
                continue
            in_family = "settingsSearchKind" in root.props or root.type in family
            if in_family or "settingsSearchTitle" in root.props or root.type in searchable_types:
                searchable_types.add(t)
                if in_family:
                    family.add(t)
                changed = True
    default_titled = {name[:-4] for name, root in files.items() if has_title(root)}

    def searchable(o):
        return o.type in searchable_types or "settingsSearchTitle" in o.props

    def exempt(o):
        return o.type in EXEMPT_SUBTREES or any(a.type in EXEMPT_SUBTREES for a in o.ancestors())

    for name, root in files.items():
        where = lambda o: "%s:%d %s" % (name, o.line, o.type)
        for o in root.walk():
            if o is root:
                continue
            # R1
            if o.type in family and not has_title(o) and o.type not in default_titled:
                errors.append("R1 %s has no title to search by" % where(o))
            # R2
            if name not in NON_SETTING_FILES and o.type in INTERACTIVE and o.via is None and not exempt(o):
                if not (searchable(root) or searchable(o) or any(searchable(a) for a in o.ancestors())):
                    errors.append("R2 %s is not inside any searchable setting "
                                  "(wrap it in a SettingsRow or declare settingsSearchTitle)" % where(o))
            # R6
            if searchable(o) and not exempt(o):
                for a in [o] + list(o.ancestors()):
                    if declares_category(a):
                        break
                    vis = a.props.get("visible")
                    if vis is not None and not vis.strip().startswith("applicable") and a is not root:
                        errors.append("R6 %s binds `visible: %s` above a searchable item at line %d; "
                                      "use `applicable` instead" % ("%s:%d %s" % (name, a.line, a.type), vis, o.line))

    # R3: every searchable item resolves to a category
    shell = files.get("SettingsView.qml")
    page_instances = {}
    if shell is not None:
        for o in shell.walk():
            if o.type + ".qml" in files and o.type.endswith("Page"):
                page_instances[o.type] = o
    for page_type, inst in page_instances.items():
        if declares_category(inst):
            continue
        root = files[page_type + ".qml"]
        for o in root.walk():
            if o is not root and searchable(o) and not exempt(o):
                if not any(declares_category(a) for a in [o] + list(o.ancestors())):
                    errors.append("R3 %s:%d %s has no category: declare settingsCategory on the page "
                                  "instance in SettingsView.qml or on an enclosing card" % (page_type + ".qml", o.line, o.type))

    # R4: categories referenced exist, and every category is claimed
    used = set()
    for name, root in files.items():
        for o in root.walk():
            if declares_category(o):
                key = category_of(o)
                used.add(key)
                if key not in raw_category_keys:
                    errors.append("R4 %s:%d settingsCategory \"%s\" is not a SettingsView category" % (name, o.line, key))
    for key in raw_category_keys:
        if key not in used:
            errors.append("R4 SettingsView category \"%s\" has no settingsCategory anywhere" % key)
    return errors


LITERAL_COMPARE = re.compile(r"""\bcategory\s*[!=]==?\s*["']|["']\s*[!=]==?\s*\w*\.?category\b""")


def check_literal_compares(texts):
    """R5 over {basename: text}."""
    errors = []
    for name, text in texts.items():
        for n, line in enumerate(text.splitlines(), 1):
            code = line.split("//")[0]
            if LITERAL_COMPARE.search(code):
                errors.append("R5 %s:%d compares a category to a literal; use settingsCategory: %s"
                              % (name, n, line.strip()))
    return errors


def raw_category_keys(settings_view_text):
    block = re.search(r"rawCategories\s*:\s*\[(.*?)\n\s*\]", settings_view_text, re.S)
    return re.findall(r'key:\s*"(\w+)"', block.group(1)) if block else []


def load_repo():
    texts = {}
    for f in sorted(os.listdir(SETTINGS)):
        if f.endswith(".qml"):
            with open(os.path.join(SETTINGS, f), encoding="utf-8") as fh:
                texts[f] = fh.read()
    with open(os.path.join(GUI, "SettingsView.qml"), encoding="utf-8") as fh:
        texts["SettingsView.qml"] = fh.read()
    # Settings content implemented in app/gui (e.g. ScreenCombinationModeSelector)
    used = set()
    for text in texts.values():
        used.update(re.findall(r"^\s*([A-Z]\w*)\s*\{", text, re.M))
    for t in sorted(used):
        path = os.path.join(GUI, t + ".qml")
        if t not in GUI_PRIMITIVES and t + ".qml" not in texts and os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                texts[t + ".qml"] = fh.read()
    return texts


def run_all(texts):
    files = {name: parse_qml(text) for name, text in texts.items()}
    keys = raw_category_keys(texts.get("SettingsView.qml", ""))
    return check(files, keys) + check_literal_compares(texts)


class RepositoryGate(unittest.TestCase):
    def test_settings_search_metadata(self):
        texts = load_repo()
        self.assertIn("ScreenCombinationModeSelector.qml", texts,
                      "settings content from app/gui must be in scope")
        errors = run_all(texts)
        self.assertEqual(errors, [], "\n" + "\n".join(errors))

    def test_every_rail_category_is_parsed(self):
        texts = load_repo()
        keys = raw_category_keys(texts["SettingsView.qml"])
        self.assertGreaterEqual(len(keys), 10)
        self.assertIn("basic", keys)

    def test_parser_sees_every_object_declaration(self):
        # Guards the gate against a parser that silently stops descending
        # (e.g. an unhandled construct ending an object early).
        decl = re.compile(r"^\s*[A-Z][\w.]*(\s+on\s+[\w.]+)?\s*\{", re.M)
        for name, text in load_repo().items():
            stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            stripped = "\n".join(l.split("//")[0] for l in stripped.splitlines())
            expected = len(decl.findall(stripped))
            root = parse_qml(text)
            got = sum(1 for o in root.walk() if o.via is None)
            self.assertEqual(got, expected, name)


# --------------------------------------------------------------------------
# Fixtures: each rule must fire on a minimal violation and stay quiet on the
# fixed version, so the gate can't silently stop checking.
# --------------------------------------------------------------------------

BASE = {
    "SettingsRow.qml": 'FocusScope {\n    property string title: ""\n'
                       '    readonly property string settingsSearchKind: "row"\n'
                       '    property string settingsSearchTitle: title\n'
                       '    visible: applicable\n}\n',
    "ToggleRow.qml": 'SettingsRow {\n    HardSwitch { onToggled: { if (x) { y() } } }\n}\n',
    "SettingsCard.qml": 'Item {\n    property string title: ""\n'
                        '    readonly property string settingsSearchKind: "card"\n'
                        '    property string settingsSearchTitle: title\n'
                        '    visible: applicable && hasVisibleContent\n}\n',
    "AudioPage.qml": 'Column {\n    SettingsCard {\n        title: qsTr("Audio")\n'
                     '        ToggleRow { title: qsTr("Mute") }\n    }\n}\n',
}

SHELL = ('FocusScope {\n    readonly property var rawCategories: [\n'
         '        { key: "audio", title: qsTr("Audio") }\n    ]\n'
         '    SettingsScrollArea {\n        AudioPage {\n'
         '            readonly property string settingsCategory: "audio"\n'
         '            visible: settingsPage.category === settingsCategory\n'
         '        }\n    }\n}\n')


def fixture(**overrides):
    texts = dict(BASE)
    texts["SettingsView.qml"] = SHELL
    texts.update(overrides)
    return run_all(texts)


class GateFixtures(unittest.TestCase):
    def assertRule(self, rule, errors):
        self.assertTrue(any(e.startswith(rule) for e in errors), "%s not reported in %r" % (rule, errors))

    def test_clean_fixture_passes(self):
        self.assertEqual(fixture(), [])

    def test_parser_handles_js_blocks_strings_and_value_objects(self):
        root = parse_qml(
            'import QtQuick\n'
            'Item {\n'
            '    // Fake { in a comment\n'
            '    property var m: [{ key: "a{" }, { key: "b" }]\n'
            '    function f(x) { if (x) { return "}" } return /* } */ 1 }\n'
            '    Behavior on opacity { NumberAnimation {} }\n'
            '    anchors { fill: parent; margins: 4 }\n'
            '    title: cond\n'
            '           ? qsTr("A")\n'
            '           : qsTr("B")\n'
            '    background: Rectangle { color: "red" }\n'
            '    contentItem: Rectangle {\n'
            '        color: a ? b\n'
            '                 : (c || d ? Theme.surface2 : "transparent")\n'
            '        Rectangle { width: 1 }\n'
            '    }\n'
            '    Keys.onPressed: function(event) { if (event.key) { event.accepted = true } }\n'
            '    Text { text: "x" }\n'
            '    component Chip: Rectangle { Text {} }\n'
            '}\n')
        self.assertEqual(root.type, "Item")
        self.assertEqual([c.type for c in root.children], ["Behavior", "Rectangle", "Rectangle", "Text", "Rectangle"])
        self.assertEqual(root.children[4].via, "component")
        self.assertEqual([c.type for c in root.children[4].children], ["Text"])
        self.assertEqual(root.children[1].via, "background")
        self.assertEqual([c.type for c in root.children[2].children], ["Rectangle"])
        self.assertEqual(root.props["title"], "cond")
        self.assertIn("m", root.props)

    def test_R1_row_without_title(self):
        self.assertRule("R1", fixture(**{"AudioPage.qml": BASE["AudioPage.qml"].replace('title: qsTr("Mute")', 'id: r')}))

    def test_R2_control_outside_any_setting(self):
        page = 'Column {\n    SettingsCard {\n        title: "A"\n        ToggleRow { title: "B" }\n    }\n    HardButton {}\n}\n'
        self.assertRule("R2", fixture(**{"AudioPage.qml": page}))
        ok = page.replace("    HardButton {}\n", "    Item {\n        property string settingsSearchTitle: qsTr(\"Do\")\n        HardButton {}\n    }\n")
        self.assertEqual(fixture(**{"AudioPage.qml": ok}), [])

    def test_R2_dialog_subtree_is_exempt(self):
        page = BASE["AudioPage.qml"].replace("}\n}\n", "}\n    NavigableDialog { HardButton {} }\n}\n")
        self.assertEqual(fixture(**{"AudioPage.qml": page}), [])

    def test_R3_page_without_category(self):
        shell = SHELL.replace('            readonly property string settingsCategory: "audio"\n', "") \
                     .replace("visible: settingsPage.category === settingsCategory", "")
        self.assertRule("R3", fixture(**{"SettingsView.qml": shell}))

    def test_R3_mixed_page_with_card_categories_passes(self):
        shell = SHELL.replace('            readonly property string settingsCategory: "audio"\n', "") \
                     .replace("visible: settingsPage.category === settingsCategory", "")
        page = BASE["AudioPage.qml"].replace('title: qsTr("Audio")',
                                             'settingsCategory: "audio"\n        title: qsTr("Audio")')
        self.assertEqual(fixture(**{"SettingsView.qml": shell, "AudioPage.qml": page}), [])

    def test_R4_unknown_and_unclaimed_categories(self):
        errors = fixture(**{"SettingsView.qml": SHELL.replace('"audio"\n', '"clipboard"\n')})
        self.assertRule("R4", errors)
        self.assertTrue(any("\"clipboard\" is not a SettingsView category" in e for e in errors), errors)
        self.assertTrue(any("\"audio\" has no settingsCategory" in e for e in errors), errors)

    def test_R5_literal_category_compare(self):
        page = BASE["AudioPage.qml"].replace('title: qsTr("Audio")',
                                             'visible: settingsPage.category === "audio"\n        title: qsTr("Audio")')
        self.assertRule("R5", fixture(**{"AudioPage.qml": page}))

    def test_R6_visible_binding_above_searchable(self):
        page = BASE["AudioPage.qml"].replace('ToggleRow { title: qsTr("Mute") }',
                                             'Item {\n            visible: isMac\n            ToggleRow { title: qsTr("Mute") }\n        }')
        self.assertRule("R6", fixture(**{"AudioPage.qml": page}))
        ok = page.replace("visible: isMac", "property bool applicable: isMac\n            visible: applicable")
        self.assertEqual(fixture(**{"AudioPage.qml": ok}), [])


if __name__ == "__main__":
    unittest.main()
