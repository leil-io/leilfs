#!/usr/bin/env python3

import unittest
import tempfile
import shutil
import subprocess
import os
import sys

# Assuming 'tools' directory is sibling to 'tests' or in PYTHONPATH
# Or adjust path to find verify_changelogs.py and changelog_test_util
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG_TEST_UTIL_PATH = os.path.join(BASE_DIR, "build", "src", "tools", "changelog_test_util")
# Path might need adjustment based on actual build output location of the util
VERIFY_CHANGELOGS_PATH = os.path.join(BASE_DIR, "tools", "verify_changelogs.py")

if not os.path.exists(CHANGELOG_TEST_UTIL_PATH):
    # Try common alternative build path (e.g. if build dir is just 'build')
    CHANGELOG_TEST_UTIL_PATH = os.path.join(BASE_DIR, "build", "tools", "changelog_test_util")
    if not os.path.exists(CHANGELOG_TEST_UTIL_PATH):
         print(f"ERROR: changelog_test_util not found at default paths. Searched:\n"
               f"- {os.path.join(BASE_DIR, 'build', 'src', 'tools', 'changelog_test_util')}\n"
               f"- {os.path.join(BASE_DIR, 'build', 'tools', 'changelog_test_util')}\n"
               f"Please build it first or adjust CHANGELOG_TEST_UTIL_PATH.", file=sys.stderr)
        # sys.exit(1) # Or allow tests to fail more gracefully

class TestChangelogSystem(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp(prefix="sauna_changelog_test_")
        self.cluster_id = "inttest01"
        self.hostname_master = "masterhost1"
        self.hostname_ml = "mlhost1"
        self.base_name_master = "changelog.sfs"
        self.base_name_ml = "changelog_ml.sfs"

    def tearDown(self):
        if os.path.exists(self.test_dir): # Make sure it exists before trying to remove
            shutil.rmtree(self.test_dir)

    def run_util(self, args):
        cmd = [CHANGELOG_TEST_UTIL_PATH] + args
        # print(f"Running util: {' '.join(cmd)}")
        try:
            process = subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=10)
            if process.returncode != 0:
                print(f"Util Error STDOUT:\n{process.stdout}", file=sys.stderr)
                print(f"Util Error STDERR:\n{process.stderr}", file=sys.stderr)
                process.check_returncode() # Raise CalledProcessError if returncode is non-zero
            return process.stdout.strip()
        except FileNotFoundError:
            self.fail(f"ERROR: {CHANGELOG_TEST_UTIL_PATH} not found. Compile the test utility.")
        except subprocess.TimeoutExpired:
            self.fail(f"ERROR: Command {' '.join(cmd)} timed out.")


    def verify_dir(self, data_path, expect_errors=False, error_substrings=None):
        cmd = [sys.executable, VERIFY_CHANGELOGS_PATH, "--dir", data_path]
        # print(f"Running verify: {' '.join(cmd)}")
        process = subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=10)

        # print(f"Verify STDOUT:\n{process.stdout}")
        # print(f"Verify STDERR:\n{process.stderr}")

        if expect_errors:
            self.assertNotEqual(0, process.returncode, f"verify_changelogs expected errors but found none in {data_path}.\nOutput:\n{process.stdout}")
            if error_substrings:
                for sub in error_substrings:
                    self.assertIn(sub, process.stdout + process.stderr, f"Expected error substring '{sub}' not found in verify_changelogs output.")
        else:
            if process.returncode != 0:
                 self.fail(f"verify_changelogs found unexpected errors in {data_path}.\nSTDOUT:\n{process.stdout}\nSTDERR:\n{process.stderr}")
            self.assertNotIn("ERROR:", process.stdout) # A bit more specific
            self.assertNotIn("WARN:", process.stdout) # Check for warnings too if desired

    def test_master_single_log_generation_and_rotation(self):
        if not os.path.exists(CHANGELOG_TEST_UTIL_PATH): self.skipTest("changelog_test_util not found")
        # min_bl=0, max_bl=50
        self.run_util(["init", self.test_dir, self.cluster_id, self.hostname_master, self.base_name_master, "0", "1", "0", "50"])

        ts1 = 1700000000
        self.run_util(["write", str(ts1), "CREATE(1,file1)"])
        ts2 = 1700000060
        self.run_util(["write", str(ts2), "UNLINK(1,file1)"])

        # Check .LIVE file exists and is valid (via verify_changelogs)
        self.verify_dir(self.test_dir)

        live_files = [f for f in os.listdir(self.test_dir) if ".LIVE." in f and self.base_name_master in f]
        self.assertEqual(1, len(live_files), "Should be one live file")

        self.run_util(["rotate"])

        live_files_after_rotate = [f for f in os.listdir(self.test_dir) if ".LIVE." in f and self.base_name_master in f]
        self.assertEqual(0, len(live_files_after_rotate), "Should be no live file immediately after rotate until next write")

        finalized_files = [f for f in os.listdir(self.test_dir) if ".LIVE." not in f and self.base_name_master in f and "dummy_test_util.cfg" not in f]
        self.assertEqual(1, len(finalized_files), "Should be one finalized file")

        # Verify again, now expecting one finalized, valid log
        self.verify_dir(self.test_dir)

    def test_metalogger_log_generation_and_rotation(self):
        if not os.path.exists(CHANGELOG_TEST_UTIL_PATH): self.skipTest("changelog_test_util not found")
        # min_bl=5, max_bl=1000
        self.run_util(["init", self.test_dir, self.cluster_id, self.hostname_ml, self.base_name_ml, "1", "1", "5", "1000"])

        ts1 = 1700000100
        self.run_util(["write", str(ts1), "ML_OP(A)"])
        ts2 = 1700000160
        self.run_util(["write", str(ts2), "ML_OP(B)"])

        self.verify_dir(self.test_dir)
        live_files = [f for f in os.listdir(self.test_dir) if ".LIVE." in f and self.base_name_ml in f]
        self.assertEqual(1, len(live_files))
        self.assertTrue(".mls." in live_files[0])


        self.run_util(["rotate"])
        finalized_files = [f for f in os.listdir(self.test_dir) if ".LIVE." not in f and self.base_name_ml in f and "dummy_test_util.cfg" not in f]
        self.assertEqual(1, len(finalized_files))
        self.assertTrue(".mls." in finalized_files[0])
        self.verify_dir(self.test_dir)

    def test_back_logs_handling(self):
        if not os.path.exists(CHANGELOG_TEST_UTIL_PATH): self.skipTest("changelog_test_util not found")
        # BACK_LOGS = 2, min=0, max=50
        self.run_util(["init", self.test_dir, self.cluster_id, self.hostname_master, self.base_name_master, "0", "2", "0", "50"])

        ts_base = 1700000000
        for i in range(4): # Create 4 logs
            self.run_util(["write", str(ts_base + i * 100), f"entry_set{i}_1"])
            self.run_util(["write", str(ts_base + i * 100 + 1), f"entry_set{i}_2"]) # ensure last_id != first_id
            self.run_util(["rotate"])

        finalized_files = [f for f in os.listdir(self.test_dir) if ".LIVE." not in f and self.base_name_master in f and "dummy_test_util.cfg" not in f]
        self.assertEqual(2, len(finalized_files), "Should keep only 2 back logs")
        self.verify_dir(self.test_dir) # Check sequence and validity of remaining logs

    def test_back_logs_zero(self):
        if not os.path.exists(CHANGELOG_TEST_UTIL_PATH): self.skipTest("changelog_test_util not found")
        # BACK_LOGS = 0
        self.run_util(["init", self.test_dir, self.cluster_id, self.hostname_master, self.base_name_master, "0", "0", "0", "50"])

        self.run_util(["write", "1700000000", "log1_entry1"])
        self.run_util(["write", "1700000001", "log1_entry2"])
        self.run_util(["rotate"]) # Log 1 finalized

        self.run_util(["write", "1700000100", "log2_entry1"])
        self.run_util(["write", "1700000101", "log2_entry2"])
        self.run_util(["rotate"]) # Log 2 finalized, Log 1 should be deleted by BACK_LOGS=0 rule

        finalized_files = [f for f in os.listdir(self.test_dir) if ".LIVE." not in f and self.base_name_master in f and "dummy_test_util.cfg" not in f]
        self.assertEqual(1, len(finalized_files), "BACK_LOGS=0 should keep only the most recent finalized log")
        self.verify_dir(self.test_dir)


if __name__ == "__main__":
    # Potentially adjust path for verify_changelogs if it's not in default python path
    # sys.path.insert(0, os.path.join(BASE_DIR, "tools"))
    if not os.path.exists(CHANGELOG_TEST_UTIL_PATH):
        print(f"CRITICAL: Test utility {CHANGELOG_TEST_UTIL_PATH} not found. Build the project first.", file=sys.stderr)
        # Allow running if only verify_changelogs is to be tested standalone with existing files,
        # but specific tests requiring the util will fail/skip.
        # For a full test run, this should be a hard exit or all tests should skip.
    unittest.main()
