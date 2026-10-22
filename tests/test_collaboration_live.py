"""Run on a private X11 display with PAINTBOARD_TEST_X11/XTST library paths."""
import ctypes as C
import ctypes.util
import os
import json
import struct
import zlib
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
sys.dont_write_bytecode = True
from test_collaboration import bridge, BINARY, BRIDGE
ARTIFACTS = Path(os.environ.get("PAINTBOARD_TEST_ARTIFACTS", Path(tempfile.gettempdir()) / "paintboard-e2e"))


class LiveCollaborationTests(unittest.TestCase):
    def setUp(self):
        x11 = os.environ.get("PAINTBOARD_TEST_X11") or ctypes.util.find_library("X11")
        xtst = os.environ.get("PAINTBOARD_TEST_XTST") or ctypes.util.find_library("Xtst")
        if not x11 or not xtst:
            self.skipTest("X11 and XTest libraries required")
        self.x = C.CDLL(x11); self.xt = C.CDLL(xtst)
        self.x.XOpenDisplay.argtypes = [C.c_char_p]; self.x.XOpenDisplay.restype = C.c_void_p
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            self.skipTest("An X11 test display is required")
        self.x.XCloseDisplay.argtypes = [C.c_void_p]
        self.addCleanup(self.x.XCloseDisplay, self.display)
        self.x.XSync.argtypes = [C.c_void_p, C.c_int]
        self.xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.x.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]; self.x.XKeysymToKeycode.restype = C.c_uint
        self.temp = tempfile.TemporaryDirectory(prefix="paintboard-live-collab-")
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        fake = self.dir / "fake-codex"
        fake.write_text(f"#!{sys.executable}\n" + '''import json, os, pathlib, sys, time
sys.stdin.read()
time.sleep(float(os.environ.get('FAKE_CODEX_DELAY', '0')))
pathlib.Path(sys.argv[sys.argv.index('--output-last-message') + 1]).write_text(json.dumps({'annotations':[
{'type':'text','x0':400,'y0':250,'x1':0,'y1':0,'text':'Automatic reply'}]}))
''')
        fake.chmod(0o700)
        self.fake = fake
        self.claude = self.dir / "fake-claude"
        self.claude.write_text(f"#!{sys.executable}\n" + '''import json, sys
json.loads(sys.stdin.read())
print(json.dumps({'type':'result','structured_output':{'annotations':[
{'type':'text','x0':400,'y0':250,'x1':0,'y1':0,'text':'Claude reply'}]}}))
''')
        self.claude.chmod(0o700)

    def start(self, delay="0", real=False, files=None):
        self.log = tempfile.TemporaryFile(); self.addCleanup(self.log.close)
        files = [str(self.dir / "board.pb")] if files is None else files
        self.proc = subprocess.Popen([BINARY, *files], stdout=self.log, stderr=self.log,
                                     env={**os.environ, "PAINTBOARD_CODEX": "codex" if real else str(self.fake), "FAKE_CODEX_DELAY": delay,
                                          "PAINTBOARD_CLAUDE": str(self.claude), "PAINTBOARD_OPENCODE": str(self.dir / "missing"),
                                          "XDG_CONFIG_HOME": str(self.dir / "config")})
        def stop():
            if self.proc.poll() is None:
                # Stop the worker using the UI before terminating this temporary test canvas.
                try:
                    if self.status()["collaborating"]:
                        self.click(84, 774); time.sleep(.2)
                except (OSError, bridge.ToolError):
                    pass
                self.proc.terminate(); self.proc.wait(timeout=5)
            Path(self.endpoint).unlink(missing_ok=True)
        self.addCleanup(stop)
        self.endpoint = f"/tmp/paintboard-{os.getuid()}/{self.proc.pid}.sock"
        self.wait(lambda: Path(self.endpoint).exists())
        self.wait(lambda: self.status().get("collaborating") is False)
        self.wait(lambda: not self.status()["agent_scanning"])
        time.sleep(.1)

    def status(self):
        return bridge.data(self.endpoint, "get_status")

    def wait(self, condition, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(.05)
        self.log.seek(0)
        self.fail("Timed out: " + self.log.read().decode(errors="replace"))

    def move(self, x, y):
        self.xt.XTestFakeMotionEvent(self.display, -1, x, y, 0)
        self.x.XSync(self.display, 0)

    def button(self, down):
        self.xt.XTestFakeButtonEvent(self.display, 1, down, 0)
        self.x.XSync(self.display, 0)

    def click(self, x, y):
        self.move(x, y); self.button(1); self.button(0)

    def key(self, keysym, control=False):
        ctrl = self.x.XKeysymToKeycode(self.display, 0xffe3)
        keycode = self.x.XKeysymToKeycode(self.display, keysym)
        if control: self.xt.XTestFakeKeyEvent(self.display, ctrl, 1, 0)
        self.xt.XTestFakeKeyEvent(self.display, keycode, 1, 0)
        self.xt.XTestFakeKeyEvent(self.display, keycode, 0, 0)
        if control: self.xt.XTestFakeKeyEvent(self.display, ctrl, 0, 0)
        self.x.XSync(self.display, 0)

    def type_command(self, command):
        # Fixture paths use lowercase ASCII and '-' only, so no keyboard layout shifts are needed.
        for character in command:
            self.key(ord(character))

    def test_custom_dialog_saves_runs_reloads_and_removes(self):
        script = self.dir / "custom-agent"
        script.write_text(f"#!{sys.executable}\n" + '''import json, sys
assert 'world' in sys.stdin.read()
print(json.dumps({'annotations':[{'type':'text','x0':400,'y0':250,'text':'Custom reply'}]}))
''')
        script.chmod(0o700)
        # Python's temporary directory names may contain '_'; pass a simple relative command through PATH.
        with unittest.mock.patch.dict(os.environ, {"PATH": str(self.dir) + os.pathsep + os.environ.get("PATH", "")}):
            self.start()
        self.click(116, 828)
        self.wait(lambda: self.status()["busy"])
        before = self.status()["human_revision"]
        self.type_command("custom-agent")
        self.key(0xff09)  # Tab: ACP -> command/prompt
        self.key(0xff0d)  # Enter: save and select
        self.wait(lambda: self.status()["automatic_agent"] == "custom")
        self.wait(lambda: not self.status()["busy"])
        self.assertEqual(self.status()["human_revision"], before)
        config = self.dir / "config/paintboard/custom-agent.json"
        self.assertEqual(json.loads(config.read_text()), {"command": "custom-agent", "transport": "command"})
        self.assertEqual(config.stat().st_mode & 0o777, 0o600)
        self.draw(); self.click(84, 774)
        self.wait(lambda: self.status()["total"] == 2)
        self.assertEqual(bridge.data(self.endpoint, "get_board")["items"][1]["item"]["text"], "Custom reply")
        self.click(84, 774); self.wait(lambda: not self.status()["collaborating"])
        self.proc.terminate(); self.proc.wait(timeout=5)
        Path(self.endpoint).unlink(missing_ok=True)
        with unittest.mock.patch.dict(os.environ, {"PATH": str(self.dir) + os.pathsep + os.environ.get("PATH", "")}):
            self.start()
        self.assertTrue(self.status()["custom_agent_configured"])
        self.assertIn("custom", self.status()["available_agents"])
        self.click(116, 828); self.wait(lambda: self.status()["busy"])
        self.key(ord('a'), control=True); self.key(0xff08); self.key(0xff0d)
        self.wait(lambda: not self.status()["custom_agent_configured"])
        self.wait(lambda: not self.status()["busy"])
        self.assertNotIn("custom", self.status()["available_agents"])

    def test_custom_dialog_invalid_command_and_cancel(self):
        self.start()
        self.click(116, 828); self.wait(lambda: self.status()["busy"])
        self.type_command("definitely-missing-paintboard-agent")
        self.key(0xff0d)
        self.wait(lambda: bool(self.status()["custom_agent_error"]))
        self.assertTrue(self.status()["busy"])
        self.assertNotIn("custom", self.status()["available_agents"])
        self.key(0xff1b)  # Esc closes the dialog without drawing.
        self.wait(lambda: not self.status()["busy"])
        self.assertEqual(self.status()["total"], 0)

    def draw(self):
        before = self.status()["human_revision"]
        self.move(300, 200); self.button(1); self.move(350, 220); self.button(0)
        self.wait(lambda: self.status()["human_revision"] > before and not self.status()["busy"])

    def screenshot(self, name):
        """Save the window (top left of the X screen) as a PNG artifact for a person to check."""
        class XImage(C.Structure):
            _fields_ = [("width", C.c_int), ("height", C.c_int), ("xoffset", C.c_int), ("format", C.c_int),
                        ("data", C.POINTER(C.c_ubyte)), ("byte_order", C.c_int), ("bitmap_unit", C.c_int),
                        ("bitmap_bit_order", C.c_int), ("bitmap_pad", C.c_int), ("depth", C.c_int),
                        ("bytes_per_line", C.c_int), ("bits_per_pixel", C.c_int)]
        time.sleep(.3)
        self.x.XDefaultRootWindow.argtypes = [C.c_void_p]; self.x.XDefaultRootWindow.restype = C.c_ulong
        self.x.XGetImage.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_ulong, C.c_int]
        self.x.XGetImage.restype = C.POINTER(XImage)
        width, height = 1088, 870  # 85% of the 1280x1024 test screen
        image = self.x.XGetImage(self.display, self.x.XDefaultRootWindow(self.display), 0, 0, width, height, 0xffffffff, 2)
        if not image or image.contents.bits_per_pixel != 32:
            return
        stride, data = image.contents.bytes_per_line, image.contents.data
        raw = C.string_at(data, stride * height)
        rows = b"".join(b"\0" + bytes(b for x in range(width) for b in raw[y * stride + 4 * x:y * stride + 4 * x + 3][::-1])
                        for y in range(height))
        def chunk(kind, body):
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) \
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        (ARTIFACTS / f"{name}.png").write_bytes(png)

    def drag(self, x0, y0, x1, y1):
        before = self.status()["human_revision"]
        # GLFW reads the pointer live on X11, so each step waits until the window has handled the one before.
        for step in (lambda: self.move(x0, y0), lambda: self.button(1), lambda: self.move((x0 + x1) // 2, (y0 + y1) // 2),
                     lambda: self.move(x1, y1), lambda: self.button(0)):
            step(); time.sleep(.1)
        self.wait(lambda: self.status()["human_revision"] > before and not self.status()["busy"])

    def test_drawn_arrows_link_and_follow_moved_shapes(self):
        board = self.dir / "links.pb"
        self.start(files=[str(board)])
        self.key(ord('r')); self.drag(400, 300, 520, 380); self.drag(700, 300, 820, 380)
        self.key(ord('a')); self.drag(460, 340, 760, 340)  # starts in A, ends in B: both ends link
        self.key(ord('l')); self.drag(300, 700, 360, 700)  # a free line

        self.key(ord('v')); self.drag(790, 370, 790, 570)  # move B down; the arrow end follows
        self.click(330, 700)
        self.drag(367, 693, 460, 340)  # drag the line's end handle onto A: that end links
        self.screenshot("linked-arrows-live")
        self.key(ord('s'), control=True)
        self.wait(lambda: board.exists() and board.read_bytes()[:4] == b"PB05")
        lines = subprocess.run([BINARY, "--mcp", "--headless", str(board)], capture_output=True, timeout=10, input="\n".join(json.dumps(m) for m in [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "get_board", "arguments": {}}}]).encode() + b"\n").stdout.splitlines()
        items = [entry["item"] for entry in json.loads(lines[1])["result"]["structuredContent"]["items"]]
        self.assertEqual([i["type"] for i in items], ["rect", "rect", "arrow", "line"])
        self.assertEqual(items[1]["y0"], 500)
        arrow, line = items[2], items[3]
        self.assertEqual((arrow["from"], arrow["to"]), (0, 1))
        self.assertTrue(500 < arrow["x0"] < 540 and 480 < arrow["y1"] < 500, arrow)  # A's right side to B's top
        self.assertEqual((line["to"], "from" in line, line["x0"], line["y0"]), (0, False, 300, 700))
        self.assertTrue(380 < line["y1"] < 400, line)

    def test_tabs_file_dialog_close_rules_and_session(self):

        # The file dialog starts in $HOME for an Untitled tab; typed names stay lowercase ASCII.
        with unittest.mock.patch.dict(os.environ, {"HOME": str(self.dir)}):
            self.start()
        board, second = self.dir / "board.pb", self.dir / "second.pb"
        self.assertEqual((self.status()["path"], self.status()["tabs"]), (str(board), 1))
        self.draw()
        self.key(ord('n'), control=True)
        self.wait(lambda: self.status()["tabs"] == 2 and self.status()["path"] is None and self.status()["total"] == 0)
        self.draw()
        self.key(ord('s'), control=True)  # Untitled: Save opens the dialog
        self.wait(lambda: self.status()["busy"])
        self.type_command("second"); self.screenshot("1-save-dialog")
        self.key(0xff0d)
        self.wait(lambda: self.status()["path"] == str(second) and not self.status()["busy"])
        self.assertTrue(second.exists())
        self.screenshot("2-two-tabs")
        self.click(272, 16)  # first tab in the tab bar
        self.wait(lambda: self.status()["path"] == str(board))
        self.key(0xff09, control=True); self.wait(lambda: self.status()["path"] == str(second))
        self.key(0xff09, control=True); self.wait(lambda: self.status()["path"] == str(board))  # wraps around
        (self.dir / "bad.pb").write_bytes(b"not a board")
        self.key(ord('o'), control=True); self.wait(lambda: self.status()["busy"])
        self.type_command("bad.pb"); self.key(0xff0d)  # a corrupt file fails only that open
        self.wait(lambda: not self.status()["busy"])
        self.assertEqual((self.status()["path"], self.status()["tabs"]), (str(board), 2))
        self.screenshot("3a-corrupt-file-message")
        self.key(ord('o'), control=True); self.wait(lambda: self.status()["busy"])
        self.type_command("second.pb"); self.screenshot("3-open-dialog")
        self.key(0xff0d)  # already open: switches to its tab instead of opening a copy
        self.wait(lambda: self.status()["path"] == str(second) and self.status()["tabs"] == 2)
        self.key(ord('w'), control=True)
        self.wait(lambda: self.status()["tabs"] == 1 and self.status()["path"] == str(board))
        self.key(ord('n'), control=True); self.wait(lambda: self.status()["tabs"] == 2)
        self.draw()
        self.key(ord('w'), control=True)  # Untitled with a drawing asks first
        self.wait(lambda: self.status()["busy"]); self.screenshot("4-ask-before-close")
        self.key(0xff1b)  # Esc: cancel keeps the tab
        self.wait(lambda: not self.status()["busy"]); self.assertEqual(self.status()["tabs"], 2)
        self.key(ord('w'), control=True); self.wait(lambda: self.status()["busy"])
        self.click(334, 484)  # Discard
        self.wait(lambda: self.status()["tabs"] == 1 and not self.status()["busy"])
        self.assertFalse(board.exists())
        self.key(ord('q'), control=True)  # a named board with unsaved changes asks too
        self.wait(lambda: self.status()["busy"]); self.screenshot("4a-ask-named-board")
        self.key(0xff0d)  # Enter: save, then quit
        self.proc.wait(timeout=5)
        self.assertEqual(self.proc.returncode, 0)
        self.assertTrue(board.exists())
        self.assertEqual((self.dir / "config/paintboard/session").read_text(), f"0\n{board}\n")
        Path(self.endpoint).unlink(missing_ok=True)
        self.start(files=[])  # no FILE: the last session comes back
        self.assertEqual((self.status()["path"], self.status()["tabs"], self.status()["total"]), (str(board), 1, 1))
        self.screenshot("5-session-restored")

    def test_toggle_manual_and_automatic_share_the_canvas(self):
        self.start()
        with self.assertRaises(bridge.ToolError):
            bridge.data(self.endpoint, "get_board")
        self.draw()
        self.click(84, 774)
        self.wait(lambda: self.status()["collaborating"])
        self.wait(lambda: self.status()["total"] == 2)
        state = self.status()
        self.assertEqual(state["human_revision"], 1)
        board = bridge.data(self.endpoint, "get_board")
        self.assertEqual(board["items"][1]["item"]["text"], "Automatic reply")
        time.sleep(2)
        self.assertEqual(self.status()["total"], 2)
        bridge.data(self.endpoint, "add_items", revision=state["revision"], collaboration_epoch=state["collaboration_epoch"],
                    items=[{"type": "arrow", "x0": 400, "y0": 200, "x1": 350, "y1": 220}])
        self.assertEqual(self.status()["human_revision"], 1)
        preview = bridge.tool(self.endpoint, "get_canvas")
        self.assertIn("image", [p["type"] for p in preview["content"]])
        self.assertFalse((self.dir / "board.png").exists())
        self.click(84, 774)
        self.wait(lambda: not self.status()["collaborating"])
        with self.assertRaises(bridge.ToolError):
            bridge.data(self.endpoint, "add_items", items=[{"type": "text", "x0": 0, "y0": 0, "text": "blocked"}])
        self.click(84, 774)
        self.wait(lambda: self.status()["collaborating"])
        with self.assertRaises(bridge.ToolError):
            bridge.data(self.endpoint, "add_items", revision=self.status()["revision"], collaboration_epoch=state["collaboration_epoch"],
                        items=[{"type": "text", "x0": 0, "y0": 0, "text": "stale epoch"}])

    def asked_call(self, name, **args):
        """Call a tool as a named client, as the MCP proxy does, without waiting for the human's answer."""
        outcome = {}
        def run():
            try:
                outcome["result"] = bridge.tool(self.endpoint, name, client="test-agent", **args)
            except bridge.ToolError as error:
                outcome["error"] = str(error)
        thread = threading.Thread(target=run); thread.start()
        time.sleep(.5)
        return thread, outcome

    def headless(self, board, *calls):
        """Edit a board file in another process, the way a script does."""
        args = [part for name, arguments in calls for part in ("--call", name, "--args", json.dumps(arguments))]
        done = subprocess.run([sys.executable, str(BRIDGE), "--file", str(board), *args], capture_output=True, text=True, timeout=20)
        self.assertEqual(done.returncode, 0, done.stderr)
        return [json.loads(line) for line in done.stdout.splitlines()] if len(calls) > 1 else [json.loads(done.stdout)]

    def test_agent_call_asks_while_collaborate_is_off(self):
        self.start()
        with self.assertRaises(bridge.ToolError):  # no client name, as from the watcher: refused at once
            bridge.data(self.endpoint, "get_board")
        thread, outcome = self.asked_call("get_board")
        self.screenshot("permit-dialog")
        self.key(0xff0d)  # Enter: allow once
        thread.join(5)
        self.assertEqual(outcome["result"]["structuredContent"]["total"], 0)
        self.assertFalse(self.status()["collaborating"])
        thread, outcome = self.asked_call("add_items", items=[{"type": "text", "x0": 400, "y0": 300, "text": "denied"}])
        self.key(0xff1b)  # Esc: deny
        thread.join(5)
        self.assertIn("declined", outcome["error"])
        self.assertEqual(self.status()["total"], 0)
        thread, outcome = self.asked_call("add_items", items=[{"type": "text", "x0": 400, "y0": 300, "text": "allowed"}])
        self.click(727, 484)  # Allow for this session
        thread.join(5)
        state = self.status()
        self.assertEqual((state["total"], state["collaborating"], state["status"]), (1, True, "Chat tools connected"))
        self.draw(); time.sleep(2.5)
        self.assertEqual(self.status()["total"], 2)  # no automatic reply: the session allowed only tools
        bridge.tool(self.endpoint, "get_board", client="test-agent")  # no question while Collaborate is on

    def test_reload_and_overwrite_when_another_program_saves(self):
        board = self.dir / "shared.pb"
        box = lambda x: {"type": "rect", "x0": x, "y0": 400, "x1": x + 80, "y1": 460}
        self.headless(board, ("add_items", {"items": [box(300)]}))
        self.start(files=[str(board)])
        self.headless(board, ("add_items", {"items": [box(400)]}))
        self.key(ord('n'), control=True); self.wait(lambda: self.status()["tabs"] == 2)
        self.key(0xff09, control=True)  # back to the clean tab: it reloads without a question
        self.wait(lambda: self.status()["path"] == str(board) and self.status()["total"] == 2)
        self.draw()  # unsaved work in the window
        self.headless(board, ("add_items", {"items": [box(500), box(600)]}))
        self.key(0xff09, control=True); time.sleep(.3); self.key(0xff09, control=True)  # away and back
        self.wait(lambda: self.status()["busy"]); self.screenshot("disk-changed-dialog")
        self.key(0xff0d)  # Enter: reload
        self.wait(lambda: not self.status()["busy"] and self.status()["total"] == 4)
        self.key(ord('z'), control=True)  # Undo brings the window's version back
        self.wait(lambda: self.status()["total"] == 3)
        self.headless(board, ("add_items", {"items": [box(700)]}))
        self.key(ord('s'), control=True)  # the file changed again: Save asks first
        self.wait(lambda: self.status()["busy"])
        self.click(642, 484)  # Overwrite
        self.wait(lambda: not self.status()["busy"])
        self.assertEqual(self.headless(board, ("get_status", {}))[0]["total"], 3)

    def test_double_click_labels_and_containers_carry_children(self):
        board = self.dir / "nest.pb"
        self.headless(board, ("add_items", {"items": [
            {"type": "rect", "id": "frame", "x0": 300, "y0": 200, "x1": 700, "y1": 500, "label": "Frame"},
            {"type": "rect", "id": "child", "parent": "frame", "x0": 360, "y0": 300, "x1": 480, "y1": 380},
            {"type": "ellipse", "id": "outside", "x0": 800, "y0": 300, "x1": 900, "y1": 380}]}))
        self.start(files=[str(board)])
        self.key(ord('v'))
        self.click(420, 340); time.sleep(.05); self.click(420, 340)  # double-click a shape to label it
        self.wait(lambda: self.status()["busy"])
        self.type_command("step"); self.screenshot("label-editing")
        self.key(0xff1b)
        self.wait(lambda: not self.status()["busy"])
        self.drag(600, 450, 600, 550)  # the frame moves 100 down and takes its child along
        self.drag(850, 340, 550, 500)  # dropped inside the frame, the ellipse joins it
        self.drag(420, 440, 420, 700)  # dragged out of the frame, the child leaves it
        self.screenshot("containers")
        self.key(ord('s'), control=True)
        self.wait(lambda: not self.status()["path"] or board.stat().st_mtime > 0)
        time.sleep(.3)
        items = {e["item"]["id"]: e["item"] for e in self.headless(board, ("get_board", {}))[0]["items"]}
        self.assertEqual((items["frame"]["y0"], items["child"]["label"]), (300, "step"))
        self.assertEqual((items["child"]["y0"], "parent" in items["child"]), (660, False))
        self.assertEqual((items["outside"]["x0"], items["outside"]["parent"]), (500, 0))

    def test_panel_dash_elbow_and_frame(self):
        board = self.dir / "styles.pb"
        self.start(files=[str(board)])
        self.click(84, 482); time.sleep(.2)  # the pen takes no dashes, so Dash is greyed out and ignores the click
        self.screenshot("panel-pen-tool")
        self.key(ord('e')); self.screenshot("panel-eraser-tool")  # colors, fill, and width do nothing for the eraser
        self.key(ord('r')); self.drag(400, 300, 520, 380)
        self.click(84, 482); time.sleep(.2)  # live for the rectangle tool: the next rectangle is dashed
        self.drag(700, 450, 820, 530)
        self.key(ord('a')); self.click(136, 482); time.sleep(.2)  # Elbow is live for arrows
        self.drag(460, 340, 760, 490)  # linked at both ends, routed with right angles
        self.key(ord('a'), control=True)
        self.key(ord('g'), control=True)  # frame the selection
        self.wait(lambda: self.status()["total"] == 4)
        self.screenshot("dash-elbow-frame")
        time.sleep(.2); self.click(84, 482); time.sleep(.2)  # with the frame selected, Dash turns its dashes off
        self.key(ord('s'), control=True)
        time.sleep(.5)
        items = [e["item"] for e in self.headless(board, ("get_board", {}))[0]["items"]]
        self.assertEqual([i["type"] for i in items], ["rect", "rect", "rect", "arrow"])
        frame, first, second, arrow = items
        self.assertEqual((frame["color"], "dashed" in frame, "parent" in frame), (7, False, False))
        self.assertEqual((first["parent"], second["parent"], arrow["parent"]), (0, 0, 0))
        self.assertTrue("dashed" not in first and second["dashed"] and arrow["dashed"] and arrow["elbow"])
        self.assertEqual((arrow["from"], arrow["to"]), (1, 2))
        self.assertTrue(frame["x0"] < 400 and frame["y0"] <= 260 and frame["x1"] > 820 and frame["y1"] > 530, frame)
        self.key(ord('v')); self.drag(390, 270, 390, 370)  # drag the frame by its top band: everything moves
        self.key(ord('s'), control=True); time.sleep(.5)
        moved = [e["item"] for e in self.headless(board, ("get_board", {}))[0]["items"]]
        self.assertEqual((moved[1]["y0"], moved[2]["y0"]), (400, 550))

    def test_toggle_off_cancels_thinking(self):
        self.start(delay="5")
        self.draw(); self.click(84, 774)
        self.wait(lambda: self.status()["status"] == "Thinking...")
        self.click(84, 774)
        self.wait(lambda: not self.status()["collaborating"])
        time.sleep(2)
        self.assertEqual(self.status()["total"], 1)

    def test_switch_cancels_old_agent_and_chat_only_keeps_manual_access(self):
        self.start(delay="5")
        self.assertEqual(self.status()["available_agents"], ["codex", "claude"])
        self.assertEqual(self.status()["automatic_agent"], "codex")
        self.draw(); self.click(84, 774)
        self.wait(lambda: self.status()["status"] == "Thinking...")
        epoch = self.status()["collaboration_epoch"]
        self.click(64, 828)
        self.wait(lambda: self.status()["automatic_agent"] == "claude")
        self.assertGreater(self.status()["collaboration_epoch"], epoch)
        self.wait(lambda: self.status()["total"] == 2)
        self.assertEqual(bridge.data(self.endpoint, "get_board")["items"][1]["item"]["text"], "Claude reply")
        self.click(64, 828)
        self.wait(lambda: self.status()["automatic_agent"] == "none")
        self.assertTrue(self.status()["collaborating"])
        self.draw()
        time.sleep(2)
        self.assertEqual(self.status()["total"], 3)
        bridge.data(self.endpoint, "add_items", items=[{"type":"text", "x0":500, "y0":300, "text":"Prompted"}])
        self.assertEqual(self.status()["total"], 4)

    def test_refresh_preserves_selection_and_handles_removed_executable(self):
        self.start()
        self.click(64, 828)
        self.wait(lambda: self.status()["automatic_agent"] == "claude")
        epoch = self.status()["collaboration_epoch"]
        self.click(146, 828); time.sleep(.3)
        self.wait(lambda: not self.status()["agent_scanning"])
        self.assertEqual(self.status()["automatic_agent"], "claude")
        self.assertEqual(self.status()["collaboration_epoch"], epoch)
        self.claude.unlink()
        self.click(146, 828)
        self.wait(lambda: self.status()["available_agents"] == ["codex"])
        self.assertEqual(self.status()["automatic_agent"], "none")

    @unittest.skipUnless(os.environ.get("PAINTBOARD_TEST_REAL_CODEX") == "1", "Opt-in real Codex smoke test")
    def test_real_codex_reply(self):
        self.start(real=True)
        self.draw(); self.click(84, 774)
        self.wait(lambda: self.status()["collaborating"])
        bridge.data(self.endpoint, "add_items", items=[
            {"type": "rect", "x0": 250, "y0": 300, "x1": 450, "y1": 420},
            {"type": "text", "x0": 280, "y0": 340, "text": "Web server"},
            {"type": "arrow", "x0": 460, "y0": 360, "x1": 590, "y1": 360},
            {"type": "rect", "x0": 600, "y0": 300, "x1": 800, "y1": 420},
            {"type": "text", "x0": 630, "y0": 340, "text": "Database"},
            {"type": "text", "x0": 260, "y0": 480, "text": "Where could this fail?"},
        ])
        self.wait(lambda: self.status()["status"] == "Thinking...")
        self.wait(lambda: self.status()["status"] != "Thinking...", timeout=145)
        state = self.status()
        if state["status"] != "Watching your drawing":
            self.log.seek(0); self.fail(self.log.read().decode(errors="replace"))
        self.assertGreater(state["total"], 7, "Expected a visual response to the question on the board")


if __name__ == "__main__":
    unittest.main()
