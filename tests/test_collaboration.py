"""Scheduling, cancellation, Codex adapter, and optional live UI collaboration tests."""
import base64
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

BINARY = str(Path(sys.argv.pop(1)).resolve())
BRIDGE = Path(__file__).resolve().parents[1] / "paintboard-bridge"
loader = importlib.machinery.SourceFileLoader("paintboard_bridge", str(BRIDGE))
spec = importlib.util.spec_from_loader(loader.name, loader)
bridge = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
loader.exec_module(bridge)


class WatcherTests(unittest.TestCase):
    def run_watcher(self, generate=None, busy_until=0):
        state = dict(revision=1, human_revision=1, collaboration_epoch=1, collaborating=True, busy=False, total=1)
        clock = [0.0]
        applied, generated = [], []
        self.statuses = []

        def call(endpoint, name, **args):
            if name == "get_status":
                return {**state, "collaborating": state["collaborating"] and clock[0] < 8,
                        "busy": clock[0] < busy_until}
            if name == "add_items":
                self.assertEqual(args["revision"], state["revision"])
                self.assertEqual(args["collaboration_epoch"], state["collaboration_epoch"])
                applied.append(args); state["revision"] += 1
            if name == "_agent_status":
                self.statuses.append(args["status"])
            return dict(state)

        def respond(endpoint, snapshot, agent, executable, custom):
            generated.append(clock[0])
            if generate:
                return generate(state, len(generated))
            return [{"type": "text", "x0": 300, "y0": 200, "text": "Reply"}]

        with patch.object(bridge, "data", side_effect=call), patch.object(bridge, "generate_response", side_effect=respond), \
             patch.object(bridge.time, "monotonic", side_effect=lambda: clock[0]), \
             patch.object(bridge.time, "sleep", side_effect=lambda t: clock.__setitem__(0, clock[0] + t)):
            bridge.watch("socket")
        return applied, generated

    def test_pause_and_own_edits_do_not_retrigger(self):
        applied, generated = self.run_watcher()
        self.assertEqual(len(applied), 1)
        self.assertEqual(generated, [1.5])

    def test_waits_until_human_finishes_drawing(self):
        applied, generated = self.run_watcher(busy_until=2)
        self.assertEqual(len(applied), 1)
        self.assertGreaterEqual(generated[0], 3.25)

    def test_off_discards_an_inflight_response(self):
        def respond(state, count):
            state["collaborating"] = False
            state["collaboration_epoch"] += 1
            return [{"type": "text", "x0": 0, "y0": 0, "text": "Too late"}]
        applied, _ = self.run_watcher(respond)
        self.assertEqual(applied, [])

    def test_human_change_discards_stale_response_and_retries(self):
        def respond(state, count):
            if count == 1:
                state["human_revision"] += 1
                state["revision"] += 1
                raise bridge.StaleResponse()
            return [{"type": "text", "x0": 0, "y0": 0, "text": "Current"}]
        applied, generated = self.run_watcher(respond)
        self.assertEqual(len(generated), 2)
        self.assertEqual(len(applied), 1)
        self.assertEqual(applied[0]["revision"], 2)

    def test_prompted_response_cancels_auto_and_clears_thinking(self):
        def respond(state, count):
            state["revision"] += 1
            return [{"type": "text", "x0": 0, "y0": 0, "text": "Superseded"}]
        applied, generated = self.run_watcher(respond)
        self.assertEqual(applied, [])
        self.assertEqual(len(generated), 1)
        self.assertEqual(self.statuses[-1], "Watching your drawing")

    def test_codex_adapter_receives_image_and_validates_output(self):
        state = dict(revision=3, collaboration_epoch=2)
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "fake-codex"
            executable.write_text(f"#!{sys.executable}\n" + '''import json, pathlib, sys
args = sys.argv
assert args[1] == 'exec' and '--sandbox' in args
assert pathlib.Path(args[args.index('--image') + 1]).read_bytes() == b'image fixture'
assert 'world' in sys.stdin.read()
pathlib.Path(args[args.index('--output-last-message') + 1]).write_text(json.dumps({'annotations': [
 {'type': 'text', 'x0': 300, 'y0': 200, 'x1': 0, 'y1': 0, 'text': 'A useful response'}]}))
''')
            executable.chmod(0o700)
            preview = {"structuredContent": state, "content": [{"type": "image", "data": base64.b64encode(b"image fixture").decode()}]}
            with patch.dict(os.environ, {"PAINTBOARD_CODEX": str(executable)}), \
                 patch.object(bridge, "data", return_value={**state, "items": []}), patch.object(bridge, "tool", return_value=preview), \
                 patch.object(bridge, "still_current", return_value=True):
                items = bridge.generate_response("socket", state)
            self.assertEqual(items, [{"type": "text", "x0": 300, "y0": 200, "text": "A useful response", "color": 5, "width": 2.5}])

    def test_mcp_proxy_discovers_tools_before_a_window_is_open(self):
        messages = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        ]
        result = subprocess.run([sys.executable, str(BRIDGE), "--mcp"],
                                input="".join(json.dumps(m) + "\n" for m in messages), text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(replies), 2)
        self.assertIn("get_canvas", {t["name"] for t in replies[1]["result"]["tools"]})

    def test_discovery_checks_path_common_locations_and_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            (home / ".opencode/bin").mkdir(parents=True)
            for executable in (home / "codex", home / ".opencode/bin/opencode", home / "custom-claude"):
                executable.write_text("not executed during discovery")
                executable.chmod(0o700)
            with patch.dict(os.environ, {"HOME": directory, "PATH": directory,
                                        "PAINTBOARD_CLAUDE": str(home / "custom-claude")}, clear=True):
                found = bridge.discover_agents()
                self.assertEqual([a["id"] for a in found], ["codex", "claude", "opencode"])
                self.assertEqual(found[1]["executable"], str(home / "custom-claude"))
                (home / "codex").chmod(0o600)
                (home / ".opencode/bin/opencode").unlink()
                os.environ["PAINTBOARD_CLAUDE"] = str(home / "missing")
                self.assertEqual(bridge.discover_agents(), [])

    def test_claude_and_opencode_image_adapters(self):
        state = dict(revision=3, collaboration_epoch=2)
        preview = {"structuredContent": state, "content": [{"type": "image", "data": base64.b64encode(b"image fixture").decode()}]}
        for agent in ("claude", "opencode"):
            with self.subTest(agent=agent), tempfile.TemporaryDirectory() as directory:
                executable = Path(directory) / agent
                executable.write_text(f"#!{sys.executable}\n" + '''import base64, json, os, pathlib, sys
args = sys.argv
reply = {'annotations': [{'type':'text','x0':300,'y0':200,'x1':0,'y1':0,'text':'Reply'}]}
if '--print' in args:
    message = json.loads(sys.stdin.read())['message']['content']
    assert base64.b64decode(message[0]['source']['data']) == b'image fixture'
    assert 'world' in message[1]['text']
    assert args[args.index('--tools') + 1] == ''
    assert '--safe-mode' in args and '--json-schema' in args
    assert args[args.index('--output-format') + 1] == 'stream-json' and '--verbose' in args
    print(json.dumps({'type':'system', 'subtype':'init'}))
    print(json.dumps({'type':'result', 'is_error':False, 'structured_output':reply}))
else:
    assert args[1] == 'run' and '--pure' in args
    assert pathlib.Path(args[args.index('--file') + 1]).read_bytes() == b'image fixture'
    assert json.loads(os.environ['OPENCODE_CONFIG_CONTENT'])['permission'] == {'*':'deny'}
    assert 'annotations' in sys.stdin.read()
    print(json.dumps({'type':'step_start'}))
    print(json.dumps({'type':'text', 'part':{'text':json.dumps(reply)}}))
''')
                executable.chmod(0o700)
                with patch.object(bridge, "data", return_value={**state, "items": []}), \
                     patch.object(bridge, "tool", return_value=preview), patch.object(bridge, "still_current", return_value=True):
                    items = bridge.generate_response("socket", state, agent, str(executable))
                self.assertEqual(items[0]["text"], "Reply")
                self.assertEqual(items[0]["color"], 5)

    def test_provider_errors_cannot_be_applied_as_annotations(self):
        for agent, response in (("claude", {"type":"result", "is_error": True}), ("opencode", {"type": "error"})):
            with self.subTest(agent=agent), self.assertRaises(bridge.ToolError):
                bridge.parse_response(agent, json.dumps(response).encode())
        for response in (None, {"annotations": None}, {"annotations": [None]}, {"annotations": [{}] * 7}):
            with self.subTest(response=response), self.assertRaises(bridge.ToolError):
                bridge.annotations_to_items(response)


if __name__ == "__main__":
    unittest.main()
