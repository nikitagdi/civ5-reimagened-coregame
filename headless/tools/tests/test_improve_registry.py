"""Minimal tests for civsim.improve: python -m unittest discover -s headless\\tools\\tests -p "test_improve*.py"."""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from civsim.improve import check as C  # noqa: E402
from civsim.improve import journal as J  # noqa: E402
from civsim.improve import registry as R  # noqa: E402


class RegistryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.reg = R.load()

    def test_schema_valid(self) -> None:
        self.assertEqual(R.validate(self.reg), [])

    def test_check_registry_passes_on_repo(self) -> None:
        errs, _notes = C.check(self.reg)
        self.assertEqual(errs, [])

    def test_awaiting_user_is_not_in_release(self) -> None:
        release = {e["id"] for e in self.reg.flagsets["release"]}
        for rec in self.reg.fixes:
            if rec["approval"] == "awaiting-user":
                self.assertNotIn(rec["id"], release)
                self.assertFalse(rec["release"])

    def test_unknown_flag_in_set_is_reported(self) -> None:
        reg = copy.deepcopy(self.reg)
        reg.flagsets["fix"].append({"id": "FIX-0001", "flag": "LEKMOD_HEADLESS_FIX_9999"})
        errs, _ = C.check(reg)
        self.assertTrue(any("FIX_9999" in e for e in errs), errs)

    def test_release_with_unapproved_entry_is_reported(self) -> None:
        reg = copy.deepcopy(self.reg)
        for rec in reg.fixes:
            if rec["id"] == "FIX-0005":
                rec["flag"] = "LEKMOD_HEADLESS_FIX_0002"  # any flag the sources use
        reg.flagsets["release"].append({"id": "FIX-0005", "flag": "LEKMOD_HEADLESS_FIX_0002"})
        errs, _ = C.check(reg)
        self.assertTrue(errs)

    def test_opt_needs_justification_and_code_in_tree(self) -> None:
        reg = copy.deepcopy(self.reg)
        self.assertTrue(reg.opts, "expected OPT records in the repo registry")
        opt = reg.opts[0]
        opt["rng"] = " "
        opt["status"] = "proposed"  # still in the opt/release sets -> must be reported
        errs, _ = C.check(reg)
        self.assertTrue(any("empty justification 'rng'" in e for e in errs), errs)
        self.assertTrue(any(opt["id"] in e and "status is 'proposed'" in e for e in errs), errs)

    def test_scan_sources_finds_flags_only_in_conditionals(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "a.cpp").write_text(
                "// LEKMOD_HEADLESS_FIX_0001 in a comment\n#if defined(LEKMOD_HEADLESS_FIX_0001) && defined(X)\n#endif\n"
                "#ifdef LEKMOD_HEADLESS_OPT_VERIFY\n#endif\n", encoding="latin-1")
            found = C.scan_sources(Path(d))
            self.assertEqual(list(found), ["LEKMOD_HEADLESS_FIX_0001"])


class JournalTests(unittest.TestCase):
    def test_render_is_deterministic_and_complete(self) -> None:
        reg = R.load()
        a, b = J.render_behavior_changes(reg), J.render_behavior_changes(reg)
        self.assertEqual(a, b)
        for rec in reg.fixes:
            self.assertIn(f"### {rec['id']} - ", a)
        self.assertIn("HYG-0001", J.render_optimizations(reg))

    @unittest.skipUnless((R.DOCS_DIR / "BEHAVIOR-CHANGES.md").exists(), "generated headless/generated-docs is not committed (run: python -m civsim.improve journal render)")
    def test_committed_documents_are_up_to_date(self) -> None:
        self.assertEqual(J.write(R.load(), check_only=True), [])


if __name__ == "__main__":
    unittest.main()
