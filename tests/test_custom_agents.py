"""Custom command transport and ACP interoperability without model/network calls."""
import base64
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
sys.dont_write_bytecode = True
from test_collaboration import bridge


class CustomAgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="paintboard-custom-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.image = {"data": base64.b64encode(b"image fixture").decode()}
        self.state = dict(revision=3, collaboration_epoch=2)

    def executable(self, name, source):
        path = self.directory / name
        path.write_text(f"#!{sys.executable}\n" + source)
        path.chmod(0o700)
        return path

    def test_command_quotes_placeholders_and_json_stdin(self):
        script = self.executable("agent with spaces", '''import base64, json, os, pathlib, sys
request = json.loads(sys.stdin.read())
assert request['version'] == 1
assert base64.b64decode(request['image']['data']) == b'image fixture'
assert 'world' in request['prompt']
assert pathlib.Path(sys.argv[1]).read_bytes() == b'image fixture'
assert pathlib.Path(sys.argv[2]).read_text() == request['prompt']
assert sys.argv[3] == request['prompt']
assert sys.argv[4] == '; touch SHOULD_NOT_EXIST'
assert json.loads(pathlib.Path(os.environ['PAINTBOARD_REQUEST']).read_text()) == request
reply = {'annotations':[{'type':'text','x0':300,'y0':200,'text':'Custom reply'}]}
pathlib.Path(request['paths']['output']).write_text(json.dumps(reply))
print('diagnostic stdout is ignored when output file is used')
''')
        command = shlex.join([str(script), "{image}", "{prompt_file}", "{prompt}", "; touch SHOULD_NOT_EXIST"])
        preview = {"structuredContent": self.state, "content": [{"type": "image", **self.image}]}
        with patch.object(bridge, "data", return_value={**self.state, "items": []}), \
             patch.object(bridge, "tool", return_value=preview), patch.object(bridge, "still_current", return_value=True):
            items = bridge.generate_response("socket", self.state, "custom", custom={"command": command, "transport": "json"})
        self.assertEqual(items[0]["text"], "Custom reply")
        self.assertFalse((self.directory / "SHOULD_NOT_EXIST").exists())

    def test_prompt_stdin_and_stdout_json(self):
        script = self.executable("custom", '''import json, sys
assert 'at most 6' in sys.stdin.read()
print('```json\\n' + json.dumps({'annotations': []}) + '\\n```')
''')
        preview = {"structuredContent": self.state, "content": [{"type": "image", **self.image}]}
        with patch.object(bridge, "data", return_value={**self.state, "items": []}), \
             patch.object(bridge, "tool", return_value=preview), patch.object(bridge, "still_current", return_value=True):
            self.assertEqual(bridge.generate_response("socket", self.state, "custom",
                             custom={"command": str(script), "transport": "command"}), [])

    def test_invalid_commands_and_saved_config(self):
        for command in ("", "'unclosed", "/definitely/missing/agent"):
            with self.subTest(command=command), self.assertRaises(bridge.ToolError):
                bridge.custom_argv(command)
        script = self.executable("configured", "raise RuntimeError('Discovery must not execute agents')\n")
        config = self.directory / "paintboard/custom-agent.json"
        config.parent.mkdir()
        saved = {"command": shlex.quote(str(script)), "transport": "acp"}
        config.write_text(json.dumps(saved))
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.directory)}):
            self.assertEqual(bridge.custom_config(), saved)
            self.assertEqual(bridge.discover_agents(saved)[-1]["executable"], str(script))
            config.write_text('{"transport":"unknown", "command":"agent"}')
            with self.assertRaises(bridge.ToolError):
                bridge.custom_config()

    def acp_script(self):
        return self.executable("acp-agent", '''import json, pathlib, sys, time
mode = sys.argv[1]
def send(value):
    print(json.dumps({'jsonrpc':'2.0', **value}), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    method = request.get('method')
    if method == 'initialize':
        assert request['params']['clientCapabilities'] == {}
        if mode == 'malformed':
            print('not json', flush=True)
            continue
        if mode == 'hang':
            time.sleep(10)
        if mode == 'oversize':
            print('x' * (1024 * 1024 + 1), flush=True)
            continue
        send({'id':request['id'], 'result':{'protocolVersion':2 if mode == 'v2' else 1,
             'agentCapabilities':{'promptCapabilities':{'image':mode != 'no-image'}}}})
    elif method == 'session/new':
        assert pathlib.Path(request['params']['cwd']).is_absolute()
        assert request['params']['mcpServers'] == []
        if mode == 'auth':
            send({'id':request['id'], 'error':{'code':-32000,'message':'Authentication required'}})
        else:
            send({'id':request['id'], 'result':{'sessionId':'test-session'}})
    elif method == 'session/prompt':
        if mode == 'stale':
            pathlib.Path(sys.argv[2]).write_text(str(__import__('os').getpid()))
            time.sleep(10)
        content = request['params']['prompt']
        assert content[0]['type'] == 'image' and content[0]['mimeType'] == 'image/png'
        send({'id':99, 'method':'session/request_permission', 'params':{'sessionId':'test-session','options':[]}})
        permission = json.loads(sys.stdin.readline())
        assert permission['id'] == 99 and permission['result']['outcome']['outcome'] == 'cancelled'
        send({'id':100, 'method':'fs/read_text_file', 'params':{}})
        assert json.loads(sys.stdin.readline())['error']['code'] == -32601
        def text(value, session='test-session', kind='agent_message_chunk', message='reply'):
            send({'method':'session/update','params':{'sessionId':session,'update':{
                'sessionUpdate':kind,'messageId':message,'content':{'type':'text','text':value}}}})
        text('ignore another session', session='other-session')
        text('ignore thoughts', kind='agent_thought_chunk')
        text('ignore earlier message', message='preamble')
        text('{"annotations":')
        text('[]}')
        send({'id':request['id'], 'result':{'stopReason':'max_tokens' if mode == 'partial' else 'end_turn'}})
''')

    def test_acp_images_streaming_messages_and_permission_denial(self):
        script = self.acp_script()
        self.assertEqual(bridge.run_acp([str(script), "ok"], self.directory, "prompt", self.image, lambda: True), {"annotations": []})

    def test_acp_capability_auth_protocol_and_incomplete_errors(self):
        script = self.acp_script()
        for mode, message in (("no-image", "does not support canvas images"), ("v2", "does not support ACP v1"),
                              ("auth", "needs sign-in"), ("malformed", "expected JSON-RPC"),
                              ("partial", "reply incomplete"), ("oversize", "too large"), ("hang", "timed out")):
            with self.subTest(mode=mode), self.assertRaisesRegex(bridge.ToolError, message):
                bridge.run_acp([str(script), mode], self.directory, "prompt", self.image, lambda: True, timeout=.5)

    def test_acp_stale_canvas_stops_process(self):
        script = self.acp_script()
        marker = self.directory / "prompt-started"
        start = time.monotonic()
        with self.assertRaises(bridge.StaleResponse):
            bridge.run_acp([str(script), "stale", str(marker)], self.directory, "prompt", self.image, lambda: not marker.exists())
        self.assertLess(time.monotonic() - start, 2)
        self.assertFalse(Path("/proc", marker.read_text()).exists())


if __name__ == "__main__":
    unittest.main()
