from __future__ import annotations

import contextlib
import io
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS_DIR = REPO_ROOT / "scripts"
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import nav_inputs  # noqa: E402


def _create_city_node_db(db_path: Path) -> None:
    with contextlib.closing(sqlite3.connect(db_path)) as conn, conn:
        conn.execute(
            "CREATE TABLE city_node ("
            "guid INTEGER PRIMARY KEY, name TEXT, x REAL, y REAL, z REAL, yaw REAL)"
        )
        conn.execute("CREATE TABLE abilities (id INTEGER PRIMARY KEY, power INTEGER)")
        conn.executemany(
            "INSERT INTO city_node (guid, name, x, y, z, yaw) VALUES (?, ?, ?, ?, ?, ?)",
            [
                (20, "Winstead", 100.5, -200.25, 15.0, 90.0),
                (10, "Lionhold", -50.125, 300.75, 8.5, 180.0),
                (30, "Halcyon", 0.0, 42.0, -3.25, 270.5),
            ],
        )
        conn.execute("INSERT INTO abilities (id, power) VALUES (1, 50)")


def _run_nav_inputs(args: list[str]) -> tuple[int, str, str]:
    stdout_buf = io.StringIO()
    stderr_buf = io.StringIO()
    with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
        rc = nav_inputs.main(["nav_inputs.py", *args])
    return rc, stdout_buf.getvalue(), stderr_buf.getvalue()


class NavInputsTests(unittest.TestCase):
    def test_main_emits_city_node_rows_ordered_by_guid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "content.db"
            _create_city_node_db(db_path)

            rc, out, err = _run_nav_inputs([str(db_path)])

            self.assertEqual(rc, 0)
            self.assertEqual(err, "")
            self.assertEqual(
                out.splitlines(),
                [
                    "10 -50.125 300.75 8.5 180.0",
                    "20 100.5 -200.25 15.0 90.0",
                    "30 0.0 42.0 -3.25 270.5",
                ],
            )

    def test_output_invariant_to_non_node_changes_and_sensitive_to_node_pose(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "content.db"
            _create_city_node_db(db_path)
            rc_base, out_base, _ = _run_nav_inputs([str(db_path)])
            self.assertEqual(rc_base, 0)

            with contextlib.closing(sqlite3.connect(db_path)) as conn, conn:
                conn.execute("UPDATE abilities SET power = 999 WHERE id = 1")
                conn.execute("UPDATE city_node SET name = 'Renamed' WHERE guid = 10")
            rc_same, out_same, _ = _run_nav_inputs([str(db_path)])
            self.assertEqual(rc_same, 0)
            self.assertEqual(out_same, out_base)

            with contextlib.closing(sqlite3.connect(db_path)) as conn, conn:
                conn.execute("UPDATE city_node SET yaw = 45.0 WHERE guid = 10")
            rc_changed, out_changed, _ = _run_nav_inputs([str(db_path)])
            self.assertEqual(rc_changed, 0)
            self.assertNotEqual(out_changed, out_base)

    def test_main_returns_exit_code_2_on_invalid_argc(self) -> None:
        rc_none, out_none, err_none = _run_nav_inputs([])
        self.assertEqual(rc_none, 2)
        self.assertEqual(out_none, "")
        self.assertIn("usage: nav_inputs.py CONTENT_DB", err_none)

        rc_extra, out_extra, err_extra = _run_nav_inputs(["a.db", "b.db"])
        self.assertEqual(rc_extra, 2)
        self.assertEqual(out_extra, "")
        self.assertIn("usage: nav_inputs.py CONTENT_DB", err_extra)

    def test_main_returns_exit_code_1_when_path_is_missing_or_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            missing_db = tmp_path / "missing_content.db"

            rc_missing, out_missing, err_missing = _run_nav_inputs([str(missing_db)])
            self.assertEqual(rc_missing, 1)
            self.assertEqual(out_missing, "")
            self.assertIn("is missing", err_missing)

            rc_dir, out_dir, err_dir = _run_nav_inputs([str(tmp_path)])
            self.assertEqual(rc_dir, 1)
            self.assertEqual(out_dir, "")
            self.assertIn("is missing", err_dir)

    def test_main_returns_exit_code_1_when_city_node_table_missing_or_corrupt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            empty_db = tmp_path / "empty.db"
            with contextlib.closing(sqlite3.connect(empty_db)) as conn, conn:
                conn.execute("CREATE TABLE unrelated (id INTEGER PRIMARY KEY)")

            rc_no_table, out_no_table, err_no_table = _run_nav_inputs([str(empty_db)])
            self.assertEqual(rc_no_table, 1)
            self.assertEqual(out_no_table, "")
            self.assertIn("city_node cannot be read", err_no_table)

            corrupt_db = tmp_path / "corrupt.db"
            corrupt_db.write_text("not a valid sqlite3 file", encoding="utf-8")

            rc_corrupt, out_corrupt, err_corrupt = _run_nav_inputs([str(corrupt_db)])
            self.assertEqual(rc_corrupt, 1)
            self.assertEqual(out_corrupt, "")
            self.assertIn("city_node cannot be read", err_corrupt)


if __name__ == "__main__":
    unittest.main()
