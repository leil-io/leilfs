#!/usr/bin/env python3
"""
SQLite Stress Test with Optional Binary Search, Error Summary, Colored Status Messages,
including timestamps and per-thread error summaries.

New option:
  --calibration : When set, run a calibration test (with one thread) that
                  performs one of each operation (insert, delete, update, select)
                  with a 2-second pause between them.

Usage:
  Fixed mode (default):
    ./sqlite_stress_test.py --threads=3 --operations_per_thread=750 --seed=1 --db_file=db

  Calibration mode:
    ./sqlite_stress_test.py --calibration --seed=1 --db_file=db

  Binary search mode:
    ./sqlite_stress_test.py --binary_search_start=5,10000 --seed=1
"""

import argparse
import sqlite3
import threading
import random
import string
import time
import os
import sys
from datetime import datetime

# Global overall error summary and per-thread error summaries.
error_counts = {}
thread_error_counts = {}
error_lock = threading.Lock()

# ANSI color codes.
COLOR_YELLOW = "\033[33m"
COLOR_GREEN = "\033[32m"
COLOR_RED = "\033[31m"
COLOR_RESET = "\033[0m"

def current_timestamp():
    """Return the current timestamp in HH:MM:SS.microseconds format."""
    return datetime.now().strftime("%H:%M:%S.%f")

def get_thread_id():
    """Return the native thread id (if available, Python 3.8+), else fallback."""
    return threading.get_native_id() if hasattr(threading, "get_native_id") else threading.current_thread().ident

def log_message(msg):
    """Print a log message with a timestamp and thread id."""
    tid = get_thread_id()
    print(f"{current_timestamp()} tid:{tid} {msg}")

def parse_args():
    parser = argparse.ArgumentParser(
        description="SQLite concurrency stress test with optional binary search, error summary, "
                    "colored messages, and a calibration mode."
    )
    parser.add_argument("--threads", type=int, default=5,
                        help="Number of threads to spawn (default: 5)")
    parser.add_argument("--operations_per_thread", type=int, default=10000,
                        help="Number of operations per thread (default: 10000)")
    parser.add_argument("--db_file", default="stress_test.db",
                        help="Path to the SQLite database file (default: stress_test.db)")
    parser.add_argument("--seed", type=int, default=None,
                        help="Random seed for reproducibility (default: None)")
    parser.add_argument("--binary_search_start", default="",
                        help=("If non-empty (format threads,operations), triggers binary search mode to "
                              "find the minimal failing configuration. (default: empty string)"))
    parser.add_argument("--calibration", action="store_true",
                        help="Run a calibration test (one thread, one op each with 2-second pauses).")
    return parser.parse_args()

def random_string(length=10):
    """Generate a random string of lowercase letters."""
    letters = string.ascii_lowercase
    return ''.join(random.choice(letters) for _ in range(length))

def categorize_error(msg):
    """Return a simplified category based on the error message."""
    msg_lower = msg.lower()
    if "locked" in msg_lower:
        return "locked"
    elif "disk i/o" in msg_lower:
        return "disk I/O error"
    elif "malformed" in msg_lower:
        return "database disk image is malformed"
    else:
        return msg

def record_error(msg):
    """Update overall and per-thread error summaries."""
    global error_counts, thread_error_counts
    category = categorize_error(msg)
    tid = get_thread_id()
    with error_lock:
        error_counts[category] = error_counts.get(category, 0) + 1
        if tid not in thread_error_counts:
            thread_error_counts[tid] = {}
        thread_error_counts[tid][category] = thread_error_counts[tid].get(category, 0) + 1

def try_exec(conn, sql, params=(), context=""):
    """Helper to execute an SQL statement and record errors."""
    try:
        conn.execute(sql, params)
    except sqlite3.Error as e:
        err_str = str(e)
        if "locked" in err_str.lower():
            log_message(f"{context} Correctly detected database locked: {err_str}")
            record_error(err_str)
            return False  # Not fatal.
        else:
            log_message(f"{context} Error: {err_str}")
            record_error(err_str)
            return True  # Fatal error.
    return False

def worker_thread(thread_id, db_file, operations_per_thread, error_event):
    """
    Each thread connects to the SQLite DB, sets PRAGMA options,
    and then performs random operations (INSERT, UPDATE, SELECT).
    """
    try:
        conn = sqlite3.connect(db_file, check_same_thread=False)
    except Exception as e:
        log_message(f"[Thread {thread_id}] Connection error: {e}")
        record_error(str(e))
        error_event.set()
        return

    # Set PRAGMA options.
    if try_exec(conn, "PRAGMA journal_mode = WAL;", context=f"[Thread {thread_id}] PRAGMA journal_mode:"):
        error_event.set()
    if try_exec(conn, "PRAGMA synchronous = NORMAL;", context=f"[Thread {thread_id}] PRAGMA synchronous:"):
        error_event.set()
    if try_exec(conn, "PRAGMA temp_store = MEMORY;", context=f"[Thread {thread_id}] PRAGMA temp_store:"):
        error_event.set()

    for _ in range(operations_per_thread):
        op_type = random.choice(["insert", "update", "select"])
        if op_type == "insert":
            data_value = random_string(20)
            try:
                conn.execute("BEGIN;")
                conn.execute("INSERT INTO test_table (data) VALUES (?);", (data_value,))
                conn.execute("COMMIT;")
            except sqlite3.Error as e:
                err_str = str(e)
                if "locked" in err_str.lower():
                    log_message(f"[Thread {thread_id}] Correctly detected database locked on INSERT: {err_str}")
                    record_error(err_str)
                else:
                    log_message(f"[Thread {thread_id}] Insert error: {err_str}")
                    record_error(err_str)
                    error_event.set()
                try:
                    conn.execute("ROLLBACK;")
                except sqlite3.Error:
                    pass
        elif op_type == "update":
            data_value = random_string(20)
            row_id = random.randint(1, 10000)
            try:
                conn.execute("BEGIN;")
                conn.execute("UPDATE test_table SET data=? WHERE id=?;", (data_value, row_id))
                conn.execute("COMMIT;")
            except sqlite3.Error as e:
                err_str = str(e)
                if "locked" in err_str.lower():
                    log_message(f"[Thread {thread_id}] Correctly detected database locked on UPDATE: {err_str}")
                    record_error(err_str)
                else:
                    log_message(f"[Thread {thread_id}] Update error: {err_str}")
                    record_error(err_str)
                    error_event.set()
                try:
                    conn.execute("ROLLBACK;")
                except sqlite3.Error:
                    pass
        else:  # select
            try:
                conn.execute("SELECT id, data FROM test_table ORDER BY RANDOM() LIMIT 1;").fetchall()
            except sqlite3.Error as e:
                err_str = str(e)
                if "locked" in err_str.lower():
                    log_message(f"[Thread {thread_id}] Correctly detected database locked on SELECT: {err_str}")
                    record_error(err_str)
                else:
                    log_message(f"[Thread {thread_id}] Select error: {err_str}")
                    record_error(err_str)
                    error_event.set()
    conn.close()
    log_message(f"[Thread {thread_id}] Finished.")

def run_stress_test(threads, operations_per_thread, db_file, seed):
    """
    Runs the stress test with the specified parameters.
    Deletes an existing DB file, creates the table, runs threads, then prints overall
    and per-thread error summaries.
    Returns True if any fatal (non-lock) error occurred.
    """
    global error_counts, thread_error_counts
    with error_lock:
        error_counts.clear()
        thread_error_counts.clear()

    if seed is not None:
        random.seed(seed)
        log_message(f"Using random seed: {seed}")

    if os.path.exists(db_file):
        os.remove(db_file)
        log_message(f"{COLOR_YELLOW}Removed existing DB file: {db_file}{COLOR_RESET}")

    # Create the initial table.
    conn = sqlite3.connect(db_file)
    conn.execute("CREATE TABLE IF NOT EXISTS test_table (id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT);")
    conn.close()

    error_event = threading.Event()
    threads_list = []
    start_time = time.time()
    for i in range(threads):
        t = threading.Thread(target=worker_thread, args=(i, db_file, operations_per_thread, error_event))
        t.start()
        threads_list.append(t)
    for t in threads_list:
        t.join()
    elapsed = time.time() - start_time
    log_message(f"Test run with threads={threads}, operations={operations_per_thread} completed in {elapsed:.2f} seconds.")

    # Overall summary.
    print("\nOverall Error Summary:")
    if error_counts:
        with error_lock:
            for key, count in error_counts.items():
                print(f"  {key}: {count}")
    else:
        print("  No errors recorded.")

    # Per-thread summary.
    print("\nError Summary by Thread (tid):")
    if thread_error_counts:
        with error_lock:
            for tid, errors in thread_error_counts.items():
                print(f"  Thread tid:{tid}:")
                for key, count in errors.items():
                    print(f"    {key}: {count}")
    else:
        print("  No errors recorded per thread.")

    return error_event.is_set()

def run_calibration(db_file, seed):
    """
    Runs a calibration test in a single thread.
    The calibration test:
      - Initializes the DB as usual.
      - Waits 2 seconds.
      - Performs one INSERT.
      - Waits 2 seconds.
      - Performs one DELETE.
      - Waits 2 seconds.
      - Performs one UPDATE (first inserting a row if needed).
      - Waits 2 seconds.
      - Performs one SELECT.
      - Each operation is logged with a timestamp and thread ID.
    """
    log_message("Starting calibration test.")
    if seed is not None:
        random.seed(seed)
        log_message(f"Using random seed: {seed}")

    if os.path.exists(db_file):
        os.remove(db_file)
        log_message(f"{COLOR_YELLOW}Removed existing DB file: {db_file}{COLOR_RESET}")

    # Create the initial table.
    conn = sqlite3.connect(db_file)
    conn.execute("CREATE TABLE IF NOT EXISTS test_table (id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT);")
    conn.commit()

    # Wait 2 seconds before first operation.
    time.sleep(2)

    # 1. INSERT operation.
    log_message("Calibration: Performing INSERT operation.")
    try:
        conn.execute("BEGIN;")
        conn.execute("INSERT INTO test_table (data) VALUES ('calibration_insert');")
        conn.execute("COMMIT;")
    except sqlite3.Error as e:
        log_message(f"Calibration INSERT error: {e}")
    time.sleep(2)

    # 2. DELETE operation.
    log_message("Calibration: Performing DELETE operation.")
    try:
        conn.execute("BEGIN;")
        conn.execute("DELETE FROM test_table;")
        conn.execute("COMMIT;")
    except sqlite3.Error as e:
        log_message(f"Calibration DELETE error: {e}")
    time.sleep(2)

    # 3. UPDATE operation.
    # First insert a row so that we can update it.
    log_message("Calibration: Inserting a row for UPDATE operation.")
    try:
        conn.execute("BEGIN;")
        conn.execute("INSERT INTO test_table (data) VALUES ('calibration_for_update');")
        conn.execute("COMMIT;")
    except sqlite3.Error as e:
        log_message(f"Calibration INSERT (for UPDATE) error: {e}")
    time.sleep(2)
    log_message("Calibration: Performing UPDATE operation.")
    try:
        # Get the id of the inserted row.
        cursor = conn.execute("SELECT id FROM test_table LIMIT 1;")
        row = cursor.fetchone()
        if row:
            row_id = row[0]
            conn.execute("BEGIN;")
            conn.execute("UPDATE test_table SET data='calibration_updated' WHERE id=?;", (row_id,))
            conn.execute("COMMIT;")
        else:
            log_message("Calibration: No row found to update.")
    except sqlite3.Error as e:
        log_message(f"Calibration UPDATE error: {e}")
    time.sleep(2)

    # 4. SELECT operation.
    log_message("Calibration: Performing SELECT operation.")
    try:
        cursor = conn.execute("SELECT * FROM test_table;")
        rows = cursor.fetchall()
        log_message(f"Calibration SELECT returned {len(rows)} row(s).")
    except sqlite3.Error as e:
        log_message(f"Calibration SELECT error: {e}")
    time.sleep(2)

    conn.close()
    log_message("Calibration test complete.")

def binary_search_ops(fixed_threads, ops_high, seed):
    low = 1
    high = ops_high
    minimal_ops = ops_high
    while low <= high:
        mid = (low + high) // 2
        db_file = f"{fixed_threads}_{mid}_{seed}.db"
        print(f"\n[Binary Search - Ops] Testing configuration: threads={fixed_threads}, operations={mid}")
        error = run_stress_test(fixed_threads, mid, db_file, seed)
        if error:
            minimal_ops = mid
            high = mid - 1
            print(f"Fatal error occurred with operations={mid}. Trying lower ops (new high = {high}).")
        else:
            low = mid + 1
            print(f"No fatal error with operations={mid}. Trying higher ops (new low = {low}).")
    return minimal_ops

def binary_search_threads(fixed_ops, threads_high, seed):
    low = 1
    high = threads_high
    minimal_threads = threads_high
    while low <= high:
        mid = (low + high) // 2
        db_file = f"{mid}_{fixed_ops}_{seed}.db"
        print(f"\n[Binary Search - Threads] Testing configuration: threads={mid}, operations={fixed_ops}")
        error = run_stress_test(mid, fixed_ops, db_file, seed)
        if error:
            minimal_threads = mid
            high = mid - 1
            print(f"Fatal error occurred with threads={mid}. Trying lower threads (new high = {high}).")
        else:
            low = mid + 1
            print(f"No fatal error with threads={mid}. Trying higher threads (new low = {low}).")
    return minimal_threads

def run_binary_search(bs_threads, bs_ops, seed):
    print(f"Starting binary search with initial configuration: threads={bs_threads}, operations={bs_ops}, seed={seed}")
    db_file = f"{bs_threads}_{bs_ops}_{seed}.db"
    if not run_stress_test(bs_threads, bs_ops, db_file, seed):
        print("Initial binary search configuration did not produce a fatal error. Exiting binary search mode.")
        sys.exit(0)
    min_ops = binary_search_ops(bs_threads, bs_ops, seed)
    min_threads = binary_search_threads(min_ops, bs_threads, seed)
    print(f"\nMinimal failing configuration found: threads={min_threads}, operations={min_ops}, seed={seed}")

def main():
    args = parse_args()
    if args.calibration:
        # Run the calibration test.
        run_calibration(args.db_file, args.seed)
        sys.exit(0)
    # Otherwise, check for binary search mode.
    if args.binary_search_start.strip() != "":
        parts = args.binary_search_start.split(',')
        if len(parts) != 2:
            print("Error: --binary_search_start must be in format threads,operations")
            sys.exit(1)
        try:
            bs_threads = int(parts[0])
            bs_ops = int(parts[1])
        except ValueError:
            print("Error: --binary_search_start values must be integers")
            sys.exit(1)
        run_binary_search(bs_threads, bs_ops, args.seed)
    else:
        # Fixed mode.
        error = run_stress_test(args.threads, args.operations_per_thread, args.db_file, args.seed)
        if error:
            log_message(f"{COLOR_RED}Test completed with fatal errors.{COLOR_RESET}")
        else:
            log_message(f"{COLOR_GREEN}Test completed without fatal errors.{COLOR_RESET}")

if __name__ == "__main__":
    main()
