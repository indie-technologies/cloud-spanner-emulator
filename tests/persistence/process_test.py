# Copyright 2026 Google LLC
# Licensed under the Apache License, Version 2.0.
"""Exercise real gateway/native startup, signals, periodic saves and crashes.

Run directly with Python 3 and paths to emulator_main and gateway_main, or with
bazel test //tests/persistence:process_test. Uses only the Python standard library.
"""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

NATIVE, GATEWAY = map(os.path.abspath, sys.argv[1:3])
del sys.argv[1:3]
DB = "projects/test-project/instances/test-instance/databases/db"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class PersistenceProcessTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="spanner-process-")
        self.path = Path(self.directory.name) / "snapshot"
        self.process = None
        self.log = None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=10)
        if self.log:
            self.log.close()
        self.directory.cleanup()

    def start(self, interval="1h"):
        self.http_port = free_port()
        self.log = (Path(self.directory.name) / "process.log").open("ab")
        self.process = subprocess.Popen(
            [GATEWAY, "--grpc_binary=" + NATIVE,
             "--hostname=127.0.0.1", "--grpc_port=" + str(free_port()),
             "--http_port=" + str(self.http_port), "--state_file=" + str(self.path),
             "--checkpoint_interval=" + interval],
            stdout=self.log, stderr=self.log, start_new_session=True)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail((Path(self.directory.name) / "process.log").read_text())
            try:
                self.request("projects/test-project/instanceConfigs")
                return
            except (OSError, urllib.error.URLError):
                time.sleep(0.02)
        self.fail("Emulator did not become ready")

    def request(self, resource, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.http_port}/v1/{resource}", data=data,
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            error.msg = error.read().decode()
            error.close()
            raise

    def session(self):
        return self.request(DB + "/sessions", {})["name"]

    def write(self, session, value):
        return self.request(session + ":commit", {
            "singleUseTransaction": {"readWrite": {}},
            "mutations": [{"insertOrUpdate": {"table": "T", "columns": ["id", "v"],
                           "values": [["1", value]]}}]})

    def read(self, session):
        return self.request(session + ":executeSql", {"sql": "SELECT v FROM T"})["rows"]

    def stop(self):
        # Signal ONLY the gateway to verify forwarding to the native child.
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=20), 0)
        self.log.close()
        self.log = None

    def test_shutdown_periodic_restore_and_crash(self):
        self.start()
        self.request("projects/test-project/instances", {"instanceId": "test-instance", "instance": {
            "config": "emulator-config", "displayName": "development", "nodeCount": 1}})
        self.request("projects/test-project/instances/test-instance/databases", {
            "createStatement": "CREATE DATABASE db",
            "extraStatements": ["CREATE TABLE T (id INT64, v STRING(MAX)) PRIMARY KEY(id)"]})
        old_session = self.session()
        self.write(old_session, "committed\u0000value")
        self.stop()
        self.assertTrue(self.path.exists())
        previous = self.path.read_bytes()

        self.start("100ms")
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.read(old_session)
        self.assertEqual(error.exception.code, 404)
        session = self.session()
        self.assertEqual(self.read(session), [["committed\u0000value"]])
        self.write(session, "periodic")
        deadline = time.monotonic() + 10
        while self.path.read_bytes() == previous and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertNotEqual(self.path.read_bytes(), previous)
        # Crash both processes, skipping the graceful final save.
        os.killpg(self.process.pid, signal.SIGKILL)
        self.process.wait(timeout=10)
        self.log.close()
        self.log = None
        Path(str(self.path) + ".tmp.interrupted").write_bytes(b"partial checkpoint")
        self.start()
        self.assertEqual(self.read(self.session()), [["periodic"]])
        self.stop()

    def test_invalid_file_exits_and_preserves_bytes(self):
        invalid = b"not a native snapshot"
        self.path.write_bytes(invalid)
        result = subprocess.run([NATIVE, "--host_port=127.0.0.1:" + str(free_port()),
                                 "--state_file=" + str(self.path)],
                                capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Cannot restore state file", result.stderr)
        self.assertEqual(self.path.read_bytes(), invalid)


if __name__ == "__main__":
    unittest.main()
