"""Run on a private X11 display with PAINTBOARD_TEST_X11/XTST library paths."""
import ctypes as C
import ctypes.util
import os
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
sys.dont_write_bytecode = True
from test_collaboration import bridge, BINARY


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

    def start(self, delay="0", real=False):
        self.log = tempfile.TemporaryFile(); self.addCleanup(self.log.close)
        self.proc = subprocess.Popen([BINARY, str(self.dir / "board.pb")], stdout=self.log, stderr=self.log,
                                     env={**os.environ, "PAINTBOARD_CODEX": "codex" if real else str(self.fake), "FAKE_CODEX_DELAY": delay,
                                          "PAINTBOARD_CLAUDE": str(self.claude), "PAINTBOARD_OPENCODE": str(self.dir / "missing"),
                                          "XDG_CONFIG_HOME": str(self.dir / "config")})
        def stop():
            if self.proc.poll() is None:
                # Stop the worker using the UI before terminating this temporary test canvas.
                try:
                    if self.status()["collaborating"]:
                        self.click(84, 688); time.sleep(.2)
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
        self.click(116, 742)
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
        self.draw(); self.click(84, 688)
        self.wait(lambda: self.status()["total"] == 2)
        self.assertEqual(bridge.data(self.endpoint, "get_board")["items"][1]["item"]["text"], "Custom reply")
        self.click(84, 688); self.wait(lambda: not self.status()["collaborating"])
        self.proc.terminate(); self.proc.wait(timeout=5)
        Path(self.endpoint).unlink(missing_ok=True)
        with unittest.mock.patch.dict(os.environ, {"PATH": str(self.dir) + os.pathsep + os.environ.get("PATH", "")}):
            self.start()
        self.assertTrue(self.status()["custom_agent_configured"])
        self.assertIn("custom", self.status()["available_agents"])
        self.click(116, 742); self.wait(lambda: self.status()["busy"])
        self.key(ord('a'), control=True); self.key(0xff08); self.key(0xff0d)
        self.wait(lambda: not self.status()["custom_agent_configured"])
        self.wait(lambda: not self.status()["busy"])
        self.assertNotIn("custom", self.status()["available_agents"])

    def test_custom_dialog_invalid_command_and_cancel(self):
        self.start()
        self.click(116, 742); self.wait(lambda: self.status()["busy"])
        self.type_command("definitely-missing-paintboard-agent")
        self.key(0xff0d)
        self.wait(lambda: bool(self.status()["custom_agent_error"]))
        self.assertTrue(self.status()["busy"])
        self.assertNotIn("custom", self.status()["available_agents"])
        self.key(0xff1b)  # Esc closes the dialog without drawing.
        self.wait(lambda: not self.status()["busy"])
        self.assertEqual(self.status()["total"], 0)

    def draw(self):
        self.move(300, 200); self.button(1); self.move(350, 220); self.button(0)
        self.wait(lambda: self.status()["human_revision"] > 0 and not self.status()["busy"])

    def test_toggle_manual_and_automatic_share_the_canvas(self):
        self.start()
        with self.assertRaises(bridge.ToolError):
            bridge.data(self.endpoint, "get_board")
        self.draw()
        self.click(84, 688)
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
        self.click(84, 688)
        self.wait(lambda: not self.status()["collaborating"])
        with self.assertRaises(bridge.ToolError):
            bridge.data(self.endpoint, "add_items", items=[{"type": "text", "x0": 0, "y0": 0, "text": "blocked"}])
        self.click(84, 688)
        self.wait(lambda: self.status()["collaborating"])
        with self.assertRaises(bridge.ToolError):
            bridge.data(self.endpoint, "add_items", revision=self.status()["revision"], collaboration_epoch=state["collaboration_epoch"],
                        items=[{"type": "text", "x0": 0, "y0": 0, "text": "stale epoch"}])

    def test_toggle_off_cancels_thinking(self):
        self.start(delay="5")
        self.draw(); self.click(84, 688)
        self.wait(lambda: self.status()["status"] == "Thinking...")
        self.click(84, 688)
        self.wait(lambda: not self.status()["collaborating"])
        time.sleep(2)
        self.assertEqual(self.status()["total"], 1)

    def test_switch_cancels_old_agent_and_chat_only_keeps_manual_access(self):
        self.start(delay="5")
        self.assertEqual(self.status()["available_agents"], ["codex", "claude"])
        self.assertEqual(self.status()["automatic_agent"], "codex")
        self.draw(); self.click(84, 688)
        self.wait(lambda: self.status()["status"] == "Thinking...")
        epoch = self.status()["collaboration_epoch"]
        self.click(64, 742)
        self.wait(lambda: self.status()["automatic_agent"] == "claude")
        self.assertGreater(self.status()["collaboration_epoch"], epoch)
        self.wait(lambda: self.status()["total"] == 2)
        self.assertEqual(bridge.data(self.endpoint, "get_board")["items"][1]["item"]["text"], "Claude reply")
        self.click(64, 742)
        self.wait(lambda: self.status()["automatic_agent"] == "none")
        self.assertTrue(self.status()["collaborating"])
        self.draw()
        time.sleep(2)
        self.assertEqual(self.status()["total"], 3)
        bridge.data(self.endpoint, "add_items", items=[{"type":"text", "x0":500, "y0":300, "text":"Prompted"}])
        self.assertEqual(self.status()["total"], 4)

    def test_refresh_preserves_selection_and_handles_removed_executable(self):
        self.start()
        self.click(64, 742)
        self.wait(lambda: self.status()["automatic_agent"] == "claude")
        epoch = self.status()["collaboration_epoch"]
        self.click(146, 742); time.sleep(.3)
        self.wait(lambda: not self.status()["agent_scanning"])
        self.assertEqual(self.status()["automatic_agent"], "claude")
        self.assertEqual(self.status()["collaboration_epoch"], epoch)
        self.claude.unlink()
        self.click(146, 742)
        self.wait(lambda: self.status()["available_agents"] == ["codex"])
        self.assertEqual(self.status()["automatic_agent"], "none")

    @unittest.skipUnless(os.environ.get("PAINTBOARD_TEST_REAL_CODEX") == "1", "Opt-in real Codex smoke test")
    def test_real_codex_reply(self):
        self.start(real=True)
        self.draw(); self.click(84, 688)
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
