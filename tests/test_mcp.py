"""Exercise the actual stdio server, persistence, and transactional edits."""
import base64
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
LIVE = os.environ.get("PAINTBOARD_TEST_LIVE") == "1"
RECT = {"type": "rect", "x0": 200, "y0": 50, "x1": 400, "y1": 150, "fill": 2}


class Client:
    def __init__(self, path, initialize=True):
        self.stderr = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(
            [BINARY, "--mcp", *([] if LIVE else ["--headless"]), str(path)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
        )
        self.serial = 0
        self.buffer = b""
        if initialize:
            result = self.request("initialize", {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "paintboard-test", "version": "1"},
            })["result"]
            assert result["protocolVersion"] == "2025-11-25"
            assert result["capabilities"] == {"tools": {}}
            self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, message):
        self.proc.stdin.write(json.dumps(message).encode() + b"\n")
        self.proc.stdin.flush()

    def receive(self):
        while b"\n" not in self.buffer:
            if not select.select([self.proc.stdout], [], [], 10)[0]:
                raise AssertionError("MCP response timed out")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                self.stderr.seek(0)
                raise AssertionError("MCP exited: " + self.stderr.read().decode())
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def request(self, method, params=None):
        self.serial += 1
        msg = {"jsonrpc": "2.0", "id": self.serial, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        reply = self.receive()
        assert reply["jsonrpc"] == "2.0" and reply["id"] == self.serial, reply
        return reply

    def tool(self, name, **args):
        reply = self.request("tools/call", {"name": name, "arguments": args})
        assert "error" not in reply, reply
        return reply["result"]

    def data(self, name, **args):
        result = self.tool(name, **args)
        assert not result.get("isError"), result
        assert json.loads(result["content"][0]["text"]) == result["structuredContent"]
        return result["structuredContent"]

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise AssertionError("MCP did not exit on EOF")
        self.proc.stdout.close()
        self.stderr.seek(0)
        errors = self.stderr.read().decode()
        self.stderr.close()
        assert self.proc.returncode == 0, errors


class MCPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="paintboard-mcp-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "board.pb"

    def client(self, initialize=True):
        c = Client(self.path, initialize=initialize)
        self.addCleanup(c.close)
        return c

    def test_lifecycle_and_protocol_errors(self):
        c = self.client(initialize=False)
        self.assertEqual(c.request("ping")["result"], {})
        self.assertEqual(c.request("tools/list")["error"]["code"], -32600)
        c.proc.stdin.write(b"not JSON\n"); c.proc.stdin.flush()
        self.assertEqual(c.receive()["error"]["code"], -32700)
        init = c.request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
        self.assertEqual(init["result"]["protocolVersion"], "2024-11-05")
        self.assertEqual(c.request("tools/list")["error"]["code"], -32600)
        c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        tools = c.request("tools/list")["result"]["tools"]
        self.assertEqual({t["name"] for t in tools}, {"get_status", "get_canvas", "get_board", "add_items", "update_items", "delete_items", "undo", "redo", "save_board", "set_view", "export_png"})
        for tool in tools:
            self.assertEqual(tool["inputSchema"]["type"], "object")
            if tool["name"] in {"add_items", "update_items"}:
                self.assertIn("item", tool["inputSchema"]["$defs"])
        self.assertEqual(c.request("unknown")["error"]["code"], -32601)
        self.assertEqual(c.request("tools/call", {"name": "unknown"})["error"]["code"], -32602)
        c.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 99}})
        self.assertEqual(c.request("ping")["result"], {})

    def test_edit_roundtrip_and_history(self):
        c = self.client()
        shapes = [RECT, {"type": "line", "x0": 200, "y0": 200, "x1": 400, "y1": 200},
                  {"type": "arrow", "x0": 400, "y0": 200, "x1": 450, "y1": 100},
                  {"type": "ellipse", "x0": 460, "y0": 50, "x1": 600, "y1": 150},
                  {"type": "text", "x0": 230, "y0": 70, "text": "héllo\n世界"},
                  {"type": "pen", "points": [[210, 300], [220, 310], [230, 300]]}]
        added = c.data("add_items", items=shapes)
        self.assertEqual(added["indices"], list(range(6)))
        board = c.data("get_board")
        self.assertEqual(board["undo_steps"], 1)
        self.assertEqual(board["items"][4]["item"]["text"], "héllo\n世界")
        self.assertEqual(board["items"][5]["item"]["points"], shapes[5]["points"])
        page = c.data("get_board", offset=2, limit=2)
        self.assertEqual([e["index"] for e in page["items"]], [2, 3])
        self.assertEqual(page["next_offset"], 4)
        updated = c.data("update_items", revision=board["revision"], items=[{"index": 4, "item": {**shapes[4], "text": "changed"}}])
        undone = c.data("undo", revision=updated["revision"])
        self.assertEqual(c.data("get_board")["items"][4]["item"]["text"], "héllo\n世界")
        redone = c.data("redo", revision=undone["revision"])
        deleted = c.data("delete_items", revision=redone["revision"], indices=[1, 4])
        self.assertEqual(deleted["total"], 4)
        c.data("undo", revision=deleted["revision"])
        self.assertEqual(c.data("get_board")["items"][4]["item"]["text"], "changed")
        c.data("save_board")
        self.assertEqual(self.path.read_bytes()[:4], b"PB03")

    def test_validation_is_atomic_and_revisions_reject_stale_edits(self):
        c = self.client()
        before = c.data("get_board")
        bad_batches = [[RECT, {"type": "text", "x0": 0, "y0": 0, "text": ""}],
                       [{"type": "pen", "points": [[1, 2], [3, "bad"]]}],
                       [{**RECT, "width": 0}], [{**RECT, "color": 1.5}], [{**RECT, "x1": 1e99}],
                       [{**RECT, "unexpected": True}], [{"type": "text", "x0": 0, "y0": 0, "text": "bad\ttext"}]]
        for batch in bad_batches:
            self.assertTrue(c.tool("add_items", items=batch)["isError"])
            self.assertEqual(c.data("get_board"), before)
        added = c.data("add_items", items=[RECT, RECT])
        self.assertTrue(c.tool("delete_items", revision=before["revision"], indices=[0])["isError"])
        self.assertTrue(c.tool("delete_items", revision=added["revision"], indices=[0, 0])["isError"])
        self.assertTrue(c.tool("update_items", revision=added["revision"], items=[{"index": 0, "item": RECT}, {"index": 9, "item": RECT}])["isError"])
        self.assertEqual(c.data("get_board")["revision"], added["revision"])
        self.assertTrue(c.tool("undo", revision=before["revision"])["isError"])
        self.assertTrue(c.tool("save_board", path="elsewhere.pb")["isError"])

    def test_eof_saves_and_reopen_preserves_items(self):
        c = Client(self.path)
        try:
            c.data("add_items", items=[RECT, {"type": "text", "x0": 220, "y0": 80, "text": "persisted"}])
        finally:
            c.close()
        c = self.client()
        self.assertEqual(c.data("get_board")["total"], 2)
        self.assertEqual(c.data("get_board")["items"][1]["item"]["text"], "persisted")

    def test_pipelined_requests_are_not_lost(self):
        c = self.client()
        for i in range(20):
            c.send({"jsonrpc": "2.0", "id": f"batch-{i}", "method": "tools/call", "params": {"name": "add_items", "arguments": {"items": [RECT]}}})
        for i in range(20):
            response = c.receive()
            self.assertEqual(response["id"], f"batch-{i}")
            self.assertEqual(response["result"]["structuredContent"]["total"], i + 1)

    def test_oversized_input_is_rejected_and_connection_recovers(self):
        c = self.client()
        c.proc.stdin.write(b"x" * (1024 * 1024 + 1) + b"\n")
        c.proc.stdin.flush()
        self.assertEqual(c.receive()["error"]["code"], -32700)
        self.assertEqual(c.request("ping")["result"], {})
        self.assertEqual(c.data("get_board")["total"], 0)

    @unittest.skipIf(os.geteuid() == 0, "root bypasses file permission checks")
    def test_read_and_save_errors_preserve_the_board(self):
        c = Client(self.path)
        try:
            c.data("add_items", items=[RECT])
            c.data("save_board")
            saved = self.path.read_bytes()
            os.chmod(self.temp.name, 0o555)
            try:
                self.assertTrue(c.tool("save_board")["isError"])
                self.assertEqual(self.path.read_bytes(), saved)
            finally:
                os.chmod(self.temp.name, 0o700)
        finally:
            c.close()
        self.path.chmod(0o200)
        try:
            result = subprocess.run([BINARY, "--mcp", "--headless", str(self.path)], input=b"", capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"Permission denied", result.stderr)
        finally:
            self.path.chmod(0o600)
        self.assertEqual(self.path.read_bytes(), saved)

    def test_view_and_export(self):
        c = self.client()
        c.data("add_items", items=[RECT])
        c.data("set_view", panx=10, pany=20, zoom=1.5)
        view = c.data("get_board")["view"]
        self.assertEqual((view["panx"], view["pany"], view["zoom"]), (10, 20, 1.5))
        self.assertTrue(c.tool("set_view", panx=0, pany=0, zoom=0)["isError"])
        result = c.tool("export_png")
        if LIVE:
            self.assertFalse(result.get("isError"))
            image = next(content for content in result["content"] if content["type"] == "image")
            png = base64.b64decode(image["data"], validate=True)
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(Path(result["structuredContent"]["path"]).read_bytes(), png)
        else:
            self.assertTrue(result["isError"])
            self.assertIn("live window", result["content"][0]["text"])


if __name__ == "__main__":
    unittest.main()
