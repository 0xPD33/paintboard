"""Exercise the actual stdio server, persistence, and transactional edits."""
import base64
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

BINARY = str(Path(sys.argv.pop(1)).resolve())
LIVE = os.environ.get("PAINTBOARD_TEST_LIVE") == "1"
ARTIFACTS = Path(os.environ.get("PAINTBOARD_TEST_ARTIFACTS", Path(tempfile.gettempdir()) / "paintboard-e2e"))
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
            assert result["capabilities"] == {"tools": {}, "resources": {}, "prompts": {}}
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
        self.assertEqual({t["name"] for t in tools}, {"get_status", "get_canvas", "get_board", "add_items", "update_items", "delete_items", "undo", "redo", "save_board", "set_view", "export_png", "export_svg",
                                                       "list_tabs", "switch_tab", "new_board", "open_board", "close_tab", "measure_text", "replace_items", "add_graph"})
        for tool in tools:
            self.assertEqual(tool["inputSchema"]["type"], "object")
            if tool["name"] in {"add_items", "update_items", "replace_items"}:
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
        self.assertEqual(self.path.read_bytes()[:4], b"PB05")

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
        self.assertTrue(c.tool("save_board", file="elsewhere.pb")["isError"])

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
        if not LIVE and result.get("isError"):  # headless needs an EGL driver; the Nix build sandbox has none
            self.assertIn("EGL", result["content"][0]["text"])
        else:
            self.assertFalse(result.get("isError"))
            image = next(content for content in result["content"] if content["type"] == "image")
            png = base64.b64decode(image["data"], validate=True)
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(Path(result["structuredContent"]["path"]).read_bytes(), png)

    def test_agent_guide_helpers_and_tabs(self):
        c = self.client()
        guide = c.request("resources/read", {"uri": "paintboard://guide"})["result"]["contents"][0]["text"]
        self.assertIn('"from": "a"', guide)
        self.assertEqual(c.request("prompts/get", {"name": "paintboard_guide"})["result"]["messages"][0]["content"]["text"], guide)
        self.assertEqual(c.request("resources/read", {"uri": "paintboard://other"})["error"]["code"], -32602)
        size = c.data("measure_text", text="Plan")
        self.assertTrue(size["width"] > 0 and size["height"] > 0)
        added = c.data("add_items", items=[
            {"type": "rect", "id": "a", "x0": 0, "y0": 0, "x1": 100, "y1": 60, "label": "Plan"},
            {"type": "rect", "id": "b", "x0": 300, "y0": 0, "x1": 400, "y1": 60},
            {"type": "arrow", "from": "a", "to": "b"}])
        self.assertEqual(added["indices"], [0, 1, 2])  # a label belongs to its shape
        self.assertEqual(added["bounds"][1], [300, 0, 400, 60])
        items = c.data("get_board")["items"]
        self.assertEqual((items[0]["item"]["id"], items[0]["item"]["label"]), ("a", "Plan"))
        arrow = items[2]["item"]
        self.assertEqual((arrow["y0"], arrow["y1"]), (30, 30))
        self.assertTrue(100 < arrow["x0"] < 120 and 280 < arrow["x1"] < 300, arrow)
        error = c.tool("add_items", items=[RECT, {"type": "arrow", "from": "missing", "to": 0}])
        self.assertTrue(error["isError"])
        self.assertIn("items[1]: from, to, and parent must name another item", error["content"][0]["text"])
        error = c.tool("add_items", items=[{"type": "rect", "x0": 0, "y0": 0, "x1": 1, "y1": 1, "colour": 2}])
        self.assertIn("rect takes", error["content"][0]["text"])
        self.assertEqual(c.data("get_board")["total"], 3)

        other, bad = Path(self.temp.name) / "other.pb", Path(self.temp.name) / "bad.pb"
        c.data("new_board")
        self.assertEqual((c.data("get_status")["path"], c.data("get_status")["tabs"]), (None, 2))
        self.assertTrue(c.tool("save_board")["isError"])
        c.data("add_items", items=[RECT])
        self.assertEqual(c.data("save_board", path=str(other))["path"], str(other))
        self.assertTrue(other.exists())
        tabs = c.data("list_tabs")
        self.assertEqual(([t["path"] for t in tabs["tabs"]], tabs["active"]), ([str(self.path), str(other)], 1))
        self.assertTrue(c.tool("save_board", path=str(self.path))["isError"])  # open in another tab
        c.data("switch_tab", index=0)
        self.assertEqual(c.data("get_status")["total"], 3)
        self.assertTrue(c.tool("close_tab", index=0)["isError"])  # unsaved changes
        closed = c.data("close_tab", index=0, discard=True)
        self.assertEqual((closed["tabs"], closed["path"]), (1, str(other)))
        bad.write_bytes(b"not a board")
        failed = c.tool("open_board", path=str(bad))
        self.assertTrue(failed["isError"])
        self.assertIn("not a paintboard file", failed["content"][0]["text"])
        opened = c.data("open_board", path=str(other))  # already open: no second tab
        self.assertEqual(opened["tabs"], 1)
        opened = c.data("open_board", path=str(self.path))
        self.assertEqual((opened["tabs"], opened["path"], opened["total"]), (2, str(self.path), 0))
        image = c.tool("get_canvas")
        if not LIVE and image.get("isError"):
            self.assertIn("EGL", image["content"][0]["text"])
        else:
            self.assertIn("image", [part["type"] for part in image["content"]])

    def test_linked_arrows_follow_items_and_svg_export(self):
        c = Client(self.path)  # closed by hand, so the next session reads the saved board
        c.data("add_items", items=[
            {"type": "rect", "id": "a", "x0": 0, "y0": 0, "x1": 100, "y1": 60, "fill": 3, "color": 3, "label": "Plan"},
            {"type": "rect", "id": "b", "x0": 300, "y0": 0, "x1": 400, "y1": 60, "fill": 2, "color": 2},
            {"type": "arrow", "from": "a", "to": "b"},
            {"type": "line", "from": "b", "x1": 500, "y1": 200}])
        board = c.data("get_board")
        arrow, line = board["items"][2]["item"], board["items"][3]["item"]
        self.assertEqual((arrow["from"], arrow["to"]), (0, 1))
        self.assertEqual((line["from"], "to" in line, line["x1"], line["y1"]), (1, False, 500, 200))
        # Moving b by update_items drags both linked ends with it.
        moved = c.data("update_items", revision=board["revision"], items=[
            {"index": 1, "item": {"type": "rect", "x0": 300, "y0": 300, "x1": 400, "y1": 360, "fill": 2, "color": 2}}])
        items = c.data("get_board")["items"]
        arrow = items[2]["item"]
        self.assertTrue(arrow["y1"] > 290 and arrow["x1"] < 400, arrow)
        self.assertTrue(items[3]["item"]["y0"] > 290, items[3])
        # Resending a connector with from/to keeps it linked; its stale coordinates are recomputed.
        c.data("update_items", revision=moved["revision"], items=[
            {"index": 2, "item": {**arrow, "color": 1, "x1": 9999, "y1": 9999}}])
        arrow = c.data("get_board")["items"][2]["item"]
        self.assertEqual((arrow["color"], arrow["to"]), (1, 1))
        self.assertTrue(arrow["x1"] < 400, arrow)
        error = c.tool("update_items", revision=c.data("get_board")["revision"], items=[{"index": 2, "item": {**arrow, "from": 2}}])
        self.assertIn("items[0]: from, to, and parent must name another item", error["content"][0]["text"])
        # Deleting a linked item frees the end in place and shifts later links.
        before = c.data("get_board")
        c.data("delete_items", revision=before["revision"], indices=[0])
        arrow = c.data("get_board")["items"][1]["item"]
        self.assertEqual(("from" in arrow, arrow["to"]), (False, 0))
        self.assertEqual((arrow["x0"], arrow["y0"]), (before["items"][2]["item"]["x0"], before["items"][2]["item"]["y0"]))
        c.data("undo", revision=c.data("get_board")["revision"])
        self.assertEqual(c.data("get_board")["items"][2]["item"]["from"], 0)
        c.data("save_board")
        c.close()  # a failed assertion above leaves the process to exit with the test run
        self.assertEqual(self.path.read_bytes()[:4], b"PB05")

        c = self.client()
        items = c.data("get_board")["items"]
        self.assertEqual((items[2]["item"]["from"], items[2]["item"]["to"], items[3]["item"]["from"]), (0, 1, 1))
        out = Path(c.data("export_svg")["path"])
        self.assertEqual(out, self.path.with_suffix(".svg"))
        svg = ET.parse(out).getroot()
        ns = "{http://www.w3.org/2000/svg}"
        self.assertEqual(len(svg.findall(f".//{ns}polyline")), 3)  # the arrow, its head, and the line
        self.assertEqual(len(svg.findall(f".//{ns}polygon")), 2)
        self.assertEqual([t.text for t in svg.iter(f"{ns}text")], ["Plan"])
        self.assertIn("font/ttf;base64,", svg.find(f"{ns}style").text)
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        (ARTIFACTS / "linked-arrows.svg").write_bytes(out.read_bytes())
        (ARTIFACTS / "linked-arrows.pb").write_bytes(self.path.read_bytes())
        c.data("new_board")
        self.assertIn("save_board", c.tool("export_svg")["content"][0]["text"])
        rendered = subprocess.run([BINARY, "--render", str(self.path), str(ARTIFACTS / "linked-arrows-cli.svg")], capture_output=True, timeout=10)
        self.assertEqual(rendered.returncode, 0, rendered.stderr)
        self.assertEqual((ARTIFACTS / "linked-arrows-cli.svg").read_bytes(), out.read_bytes())


    def test_ids_persist_and_edit_without_revision(self):
        c = Client(self.path)
        c.data("add_items", items=[{"type": "rect", "id": "plan", "x0": 0, "y0": 0, "x1": 100, "y1": 60, "label": "Plan"}])
        c.data("add_items", items=[{"type": "rect", "id": "ship", "x0": 300, "y0": 0, "x1": 400, "y1": 60},
                                   {"type": "arrow", "id": "go", "from": "plan", "to": "ship"}])  # a board id from an earlier batch
        error = c.tool("add_items", items=[{"type": "text", "id": "plan", "x0": 0, "y0": 0, "text": "twice"}])
        self.assertIn("another board item has that id", error["content"][0]["text"])
        c.data("save_board")
        c.close()

        c = self.client()
        items = c.data("get_board")["items"]
        self.assertEqual([e["item"].get("id") for e in items], ["plan", "ship", "go"])
        self.assertEqual((items[2]["item"]["from"], items[2]["item"]["to"]), (0, 1))
        # By id, a script needs no revision; the item keeps its id.
        c.data("update_items", items=[{"id": "plan", "item": {"type": "rect", "x0": 0, "y0": 100, "x1": 100, "y1": 160, "color": 1, "label": "Plan"}}])
        plan = c.data("get_board")["items"][0]
        self.assertEqual((plan["item"]["id"], plan["item"]["color"], plan["item"]["y0"]), ("plan", 1, 100))
        self.assertGreater(c.data("get_board")["items"][2]["item"]["y0"], 100)  # the linked arrow followed
        error = c.tool("update_items", items=[{"index": 0, "item": RECT}])
        self.assertIn("Board indices need the revision", error["content"][0]["text"])
        error = c.tool("update_items", items=[{"id": "ship", "item": {**RECT, "id": "plan"}}])
        self.assertIn("another board item has that id", error["content"][0]["text"])
        c.data("update_items", items=[{"id": "ship", "item": {**RECT, "id": "launch"}}])  # a new id renames
        c.data("update_items", items=[{**RECT, "id": "launch", "color": 4}])  # an item that carries its id needs no wrapper
        self.assertEqual(c.data("get_board")["items"][1]["item"]["color"], 4)
        self.assertEqual(c.data("get_board")["items"][1]["item"]["id"], "launch")
        c.data("delete_items", ids=["go"])
        self.assertEqual(c.data("get_board")["total"], 2)
        self.assertIn("No board item has the id gone", c.tool("delete_items", ids=["gone"])["content"][0]["text"])
        # replace_items swaps the whole board as one undo step; an empty list clears it.
        swapped = c.data("replace_items", items=[{"type": "ellipse", "id": "only", "x0": 0, "y0": 0, "x1": 50, "y1": 50},
                                                 {"type": "text", "x0": 10, "y0": 70, "text": "inside", "parent": "only"}])
        self.assertEqual((swapped["total"], swapped["indices"]), (2, [0, 1]))
        self.assertEqual(c.data("get_board")["items"][1]["item"]["parent"], 0)
        self.assertIn("ids of earlier items", c.tool("replace_items", items=[{**RECT, "parent": 0}])["content"][0]["text"])
        c.data("undo", revision=swapped["revision"])
        self.assertEqual([e["item"].get("id") for e in c.data("get_board")["items"]], ["plan", "launch"])
        self.assertEqual(c.data("replace_items", items=[])["total"], 0)

    def test_drawing_features_roundtrip_and_export(self):
        c = Client(self.path)
        rows = [["Stays", "Changes", "Goes"], ["The reader", "Labels move with their shapes", ""]]
        items = [
            {"type": "rect", "id": "box", "x0": 0, "y0": 0, "x1": 500, "y1": 260, "dashed": True, "label": "Pipeline"},
            {"type": "rect", "id": "a", "parent": "box", "x0": 40, "y0": 60, "x1": 200, "y1": 140, "label": "Parse", "caption": "tokens to tree", "label_size": 3.5},
            {"type": "diamond", "id": "b", "parent": "box", "x0": 300, "y0": 50, "x1": 460, "y1": 150, "label": "Valid?"},
            {"type": "arrow", "id": "ab", "from": "a", "to": "b", "label": "tree"},
            {"type": "arrow", "id": "back", "from": "b", "to": "a", "elbow": True, "points": [[380, 320], [120, 320]], "label": "fix"},
            {"type": "line", "id": "free", "x0": 600, "y0": 0, "x1": 700, "y1": 100, "dashed": True},
            {"type": "table", "id": "t", "x0": 600, "y0": 200, "x1": 900, "fill": 0, "rows": rows},
            {"type": "text", "id": "note", "x0": 0, "y0": 400, "wrap": 200, "text": "This sentence is long enough to wrap twice at two hundred units"}]
        c.data("add_items", items=items)
        c.data("save_board")
        c.close()

        c = self.client()
        board = {e["item"]["id"]: e for e in c.data("get_board")["items"]}
        self.assertEqual((board["a"]["item"]["caption"], board["a"]["item"]["label_size"], board["a"]["item"]["parent"]), ("tokens to tree", 3.5, 0))
        self.assertEqual((board["ab"]["item"]["label"], board["box"]["item"]["dashed"]), ("tree", True))
        back = board["back"]["item"]
        self.assertEqual((back["elbow"], back["points"], back["from"], back["to"]), (True, [[380, 320], [120, 320]], 2, 1))
        self.assertEqual((back["x0"], back["y0"]), (380, 158))  # leaves the diamond's bottom point, 8 units out
        self.assertEqual((back["x1"], back["y1"]), (120, 148))  # enters Parse from below
        ab = board["ab"]["item"]
        self.assertEqual((ab["x0"], ab["y0"], ab["x1"]), (208, 100, 292))  # the diamond's left point, not its box
        self.assertEqual(board["t"]["item"]["rows"], rows)
        self.assertNotIn("y1", board["t"]["item"])
        self.assertGreater(board["t"]["bounds"][3], 260)  # the wrapped middle cell makes its row taller
        note = board["note"]
        self.assertEqual(note["item"]["wrap"], 200)
        self.assertLessEqual(note["bounds"][2], 200)
        self.assertGreater(note["bounds"][3] - note["bounds"][1], 3 * 21)  # three or more lines
        # Moving the container by id carries what it holds and the arrows between them.
        c.data("update_items", items=[{"id": "box", "item": {**items[0], "x0": 100, "x1": 600}}])
        moved = {e["item"]["id"]: e["item"] for e in c.data("get_board")["items"]}
        self.assertEqual((moved["a"]["x0"], moved["b"]["x0"], moved["t"]["x0"]), (140, 400, 600))
        self.assertEqual(moved["back"]["points"], [[480, 320], [220, 320]])
        error = c.tool("update_items", items=[{"id": "box", "item": {**items[0], "parent": "a"}}])
        self.assertIn("loop", error["content"][0]["text"])
        out = Path(c.data("export_svg")["path"])
        svg = ET.parse(out).getroot()
        ns = "{http://www.w3.org/2000/svg}"
        texts = [t.text for t in svg.iter(f"{ns}text")]
        for word in ("Pipeline", "Parse", "tokens to tree", "Valid?", "tree", "fix", "Stays", "Goes", "reader"):  # cells wrap to their column
            self.assertIn(word, texts)
        self.assertGreaterEqual(sum(1 for t in texts if t and t.startswith(("This", "wrap", "twice"))), 1)
        self.assertGreaterEqual(len([e for e in svg.iter() if e.get("stroke-dasharray")]), 2)
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        (ARTIFACTS / "drawing-features.svg").write_bytes(out.read_bytes())
        c.data("save_board")
        (ARTIFACTS / "drawing-features.pb").write_bytes(self.path.read_bytes())

    def test_old_pb04_boards_still_open(self):
        item = struct.pack("<4i7f4i", 3, 2, 3, 0, 0, 0, 100, 60, 3, 1, 1, 0, 0, 0, 0)  # PB04 rect, no ids or labels
        self.path.write_bytes(b"PB04" + struct.pack("<3i", 1, 0, 0) + item)
        c = self.client()
        rect = c.data("get_board")["items"][0]["item"]
        self.assertEqual((rect["type"], rect["color"], rect["fill"], rect["x1"], "id" in rect, "label" in rect), ("rect", 2, 3, 100, False, False))

    def test_add_graph_lays_out_ranks_groups_and_cycles(self):
        c = self.client()
        c.data("add_items", items=[{"type": "text", "id": "legend", "x0": 200, "y0": 40, "text": "Legend"}])
        graph = c.data("add_graph", nodes=[
            {"id": "req", "label": "Request", "fill": 3, "group": "Client"},
            {"id": "auth", "label": "Authenticated?", "shape": "diamond", "fill": 4},
            {"id": "load", "label": "Load board", "caption": "from disk", "fill": 2, "group": "Server"},
            {"id": "edit", "label": "Apply edit", "fill": 2, "group": "Server"},
            {"id": "save", "label": "Save", "fill": 5, "group": "Server"},
            {"id": "deny", "label": "Deny", "shape": "ellipse", "fill": 1}],
            edges=[{"from": "req", "to": "auth"}, {"from": "auth", "to": "load", "label": "yes"},
                   {"from": "auth", "to": "deny", "label": "no", "dashed": True}, {"from": "load", "to": "edit"},
                   {"from": "edit", "to": "save"}, {"from": "save", "to": "req", "label": "next"},
                   {"from": "deny", "to": "legend", "type": "line"}])
        items = {e["item"].get("id"): e for e in c.data("get_board")["items"] if e["item"].get("id")}
        box = lambda name: items[name]["bounds"]
        self.assertGreaterEqual(box("Client")[1], 80)  # below what was on the board
        for a, b in (("req", "auth"), ("auth", "load"), ("load", "edit"), ("edit", "save")):
            self.assertLess(box(a)[2], box(b)[0], (a, b))  # each rank sits right of the one before
        self.assertEqual(box("load")[1] + box("load")[3], box("edit")[1] + box("edit")[3])  # a chain stays on one line
        self.assertLess(box("deny")[2], box("edit")[0])  # deny shares load's rank
        nodes = [box(n) for n in ("req", "auth", "load", "edit", "save", "deny")]
        for i, p in enumerate(nodes):
            for q in nodes[i + 1:]:
                self.assertFalse(p[0] < q[2] and q[0] < p[2] and p[1] < q[3] and q[1] < p[3], (p, q))
        for name in ("load", "edit", "save"):
            self.assertEqual(items[name]["item"]["parent"], items["Server"]["index"])
            outer, inner = box("Server"), box(name)
            self.assertTrue(outer[0] < inner[0] and outer[1] < inner[1] and inner[2] < outer[2] and inner[3] < outer[3])
        arrows = [e["item"] for e in c.data("get_board")["items"] if e["item"]["type"] == "arrow"]
        cycle = next(a for a in arrows if a.get("label") == "next")
        self.assertTrue(cycle["elbow"] and len(cycle["points"]) == 2 and cycle["points"][0][1] > box("Client")[3])
        self.assertTrue(next(a for a in arrows if a.get("label") == "no")["dashed"])
        self.assertEqual(len(graph["indices"]), 2 + 6 + 7)  # frames, nodes, edges
        error = c.tool("add_graph", nodes=[{"id": "req"}])
        self.assertIn("nodes[0]: a board item already has that id", error["content"][0]["text"])
        error = c.tool("add_graph", nodes=[{"id": "x"}], edges=[{"from": "x", "to": "nowhere"}])
        self.assertIn("edges[0]", error["content"][0]["text"])
        down = c.data("add_graph", direction="down", x=0, y=2000, nodes=[{"id": "top"}, {"id": "bottom"}],
                      edges=[{"from": "top", "to": "bottom"}, {"from": "bottom", "to": "top", "label": "again and again"}])
        top, bottom = down["bounds"][0], down["bounds"][1]
        self.assertEqual((top[0], top[1]), (0, 2000))
        self.assertLess(top[3], bottom[1])
        self.assertGreater(down["bounds"][3][0], max(top[2], bottom[2]))  # the loop and its label stay clear of the boxes
        c.data("save_board")
        out = Path(c.data("export_svg")["path"])
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        (ARTIFACTS / "graph.svg").write_bytes(out.read_bytes())
        (ARTIFACTS / "graph.pb").write_bytes(self.path.read_bytes())

    def test_changes_on_disk_are_not_overwritten(self):
        a = Client(self.path)
        a.data("add_items", items=[RECT])
        a.data("save_board")
        a.data("add_items", items=[RECT])  # A has unsaved work
        b = Client(self.path)
        b.data("add_items", items=[RECT, RECT, RECT])
        b.data("save_board")
        b.close()
        refused = a.tool("save_board")
        self.assertIn("changed on disk", refused["content"][0]["text"])
        reloaded = a.data("open_board", path=str(self.path), reload=True)
        self.assertEqual(reloaded["total"], 4)
        a.data("undo", revision=reloaded["revision"])
        self.assertEqual(a.data("get_board")["total"], 2)  # Undo brings A's version back
        a.data("save_board", overwrite=True)
        a.data("add_items", items=[RECT])
        b = Client(self.path)
        b.data("add_items", items=[RECT])
        b.data("save_board")
        b.close()
        a.proc.stdin.close()  # EOF saves only when the file did not change underneath
        self.assertEqual(a.proc.wait(timeout=10), 1)
        a.stderr.seek(0)
        self.assertIn(b"changed on disk", a.stderr.read())
        a.proc.stdout.close(); a.stderr.close()
        c = self.client()
        self.assertEqual(c.data("get_board")["total"], 3)  # B's save stands


if __name__ == "__main__":

    unittest.main()
