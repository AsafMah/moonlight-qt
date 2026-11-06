"""Source gates for display groups: an ordinary session must stay byte-for-byte unchanged."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def flat(text):
    return " ".join(text.split())


class DisplayGroupGateTests(unittest.TestCase):
    def test_opted_out_launch_makes_no_extra_request(self):
        source = (ROOT / "app/streaming/sessiondisplaygroup.cpp").read_text(encoding="utf-8")
        body = flat(function(source, "QString Session::prepareDisplayGroupLaunch("))
        opt_out = body.index("if (requested < 2) {")
        self.assertLess(opt_out, body.index("getServerInfo("),
                        "the capability probe must only run for users who opted in")
        self.assertIn("arguments. return {}; }", body[opt_out:opt_out + 120])

    def test_explicit_cli_head_count_overrides_the_preference(self):
        source = (ROOT / "app/streaming/sessiondisplaygroup.cpp").read_text(encoding="utf-8")
        setter = flat(function(source, "void Session::setDisplayGroupLeaderHeads("))
        self.assertIn("m_DisplayGroup.role = DisplayGroup::Role::Leader;", setter,
                      "--display-heads 1 must turn a saved preference off")
        body = flat(function(source, "QString Session::prepareDisplayGroupLaunch("))
        self.assertIn("m_DisplayGroup.role == DisplayGroup::Role::Leader ? "
                      "m_DisplayGroup.requestedHeads : m_Preferences->displayGroupHeads", body)

    def test_group_args_only_append_to_the_legacy_query(self):
        source = (ROOT / "app/backend/nvhttp.cpp").read_text(encoding="utf-8")
        body = function(source, "void NvHTTP::startApp(")
        self.assertEqual(body.count("displayGroupQueryArgs"), 2,
                         "the group args are a parameter and one append, nothing else")
        self.assertIn("query += displayGroupQueryArgs;\n    query += LiGetLaunchUrlQueryParameters();",
                      body)

    def test_member_never_quits_the_leaders_app(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        task = source[source.index("class DeferredSessionCleanupTask"):]
        decision = task[task.index("bool shouldQuit ="):task.index(";", task.index("bool shouldQuit ="))]
        self.assertIn("!m_Session->isDisplayGroupMember()", decision)

    def test_only_group_sessions_register_the_layout_callback(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        body = flat(function(source, "bool Session::startConnectionAsync("))
        self.assertIn("LiSetDisplayGroupCallback( m_DisplayGroup.role != DisplayGroup::Role::None ? "
                      "Session::clDisplayGroup : nullptr);", body)


if __name__ == "__main__":
    unittest.main()
