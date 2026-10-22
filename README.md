<div align="center">

<img src="assets/paintboard.svg" width="96" alt="Paintboard logo">

# Paintboard

**An offline whiteboard in one small C program. You draw, and your agents draw with you.**

[![build](https://github.com/0xPD33/paintboard/actions/workflows/release.yml/badge.svg)](https://github.com/0xPD33/paintboard/actions/workflows/release.yml)
[![release](https://img.shields.io/github/v/release/0xPD33/paintboard)](https://github.com/0xPD33/paintboard/releases/latest)
[![license](https://img.shields.io/github/license/0xPD33/paintboard)](LICENSE)

[Install](#install) · [Use](#use) · [Agents and MCP](#agents-and-mcp) · [Architecture](ARCHITECTURE.md)

</div>

![Paintboard with a launch plan board: a flow chart in a dashed container, a decision diamond with a loop back, a table, a sticky note, a red hand drawn circle, and the tool panel on the left](assets/hero.png)

Paintboard is a local drawing board written in C. Draw freehand, place shapes,
and type text on an infinite canvas. Each board is one file on disk, and
drawing needs no network and no account. Agents such as Claude Code and Codex
connect through MCP: they read the canvas, lay out diagrams, and ask you before
they change a board.

Excalidraw is the reference. This is the small offline version of it.

## Features

**Draw**

- Pen, marker, line, arrow, rectangle, ellipse, diamond, triangle, star, text, and eraser.
- Arrows that stay linked to the shapes at their ends, also on a diamond or an ellipse.
- Labels and captions that belong to a shape or an arrow. Double-click to type one.
- Dashed strokes, right-angle arrows with waypoints, wrapped text, and tables.
- Containers that carry the items inside them.
- Select, move, resize, copy, paste, and undo up to 256 steps.
- Eight stroke colors, eight fills, a dot grid with snapping, and pan and zoom.
- Several boards in tabs, a built-in file dialog, and the last session on restart.
- Export the view to PNG, or the whole board to SVG.

**Work with agents**

- An MCP server with 20 tools to read the canvas, edit items, lay out graphs, and manage tabs.
- `add_graph` takes nodes and edges and does the layout.
- The window asks you before a chat agent changes a board.
- Pause while you draw, and Codex, Claude Code, OpenCode, or a custom agent replies on the canvas.
- `paintboard-bridge --file` scripts a board with no window, and `--render` draws a board to PNG or SVG.
- A board that another program saves reloads in its tab. A save never replaces that version without a question.

<p align="center">
  <img src="assets/agent.gif" width="100%" alt="You write a question on the board. Claude Code asks to call add_graph. You allow it for this session, and an architecture diagram appears under the question.">
</p>

<p align="center"><sub>You write a question. Claude Code asks to draw. You allow it, and the diagram appears.</sub></p>

**Small by design**

| Part | Footprint |
| --- | --- |
| Program | One 1.2 MB binary. The two fonts and the agent guide are built in. |
| Source | About 3,600 lines of C and a 700-line Python bridge. |
| Libraries | OpenGL, EGL, and libc. The release binaries link GLFW and cJSON statically. |
| Memory | About 130 MB resident for a window, most of it the OpenGL driver. A headless MCP session starts at 7 MB. |
| Idle CPU | 0%. The window draws only after an input or a change. |
| Board files | A board of 5,000 items is about 1.1 MB on disk. |

## Install

### Prebuilt binaries

Each [release](https://github.com/0xPD33/paintboard/releases) has archives for
x86_64 and aarch64 Linux. They need:

- glibc 2.35 or newer: Ubuntu 22.04, Debian 12, or a newer distribution.
- An OpenGL driver, such as Mesa or the NVIDIA driver, and X11 or Wayland.
- Python 3 at `/usr/bin/python3` for the MCP bridge and the agent replies.

To install for one user, unpack the archive into `~/.local`:

```sh
version=0.5.0 arch=$(uname -m)
curl -fL https://github.com/0xPD33/paintboard/releases/download/v$version/paintboard-$version-linux-$arch.tar.gz \
  | tar -xz -C ~/.local --strip-components=1
```

This puts `paintboard` and `paintboard-bridge` in `~/.local/bin`, and the desktop
entry and the icons in `~/.local/share`. Use `sudo tar ... -C /usr/local` to
install for all users. Each archive has a `.sha256` file beside it.

### Nix flake

```sh
nix run github:0xPD33/paintboard          # run it once
nix profile install github:0xPD33/paintboard
```

### NixOS or home-manager

Add the flake as an input:

```nix
inputs.paintboard.url = "github:0xPD33/paintboard";
inputs.paintboard.inputs.nixpkgs.follows = "nixpkgs";
```

Then install the package. The flake also exports `overlays.default`, which adds
`pkgs.paintboard`.

```nix
home.packages = [ inputs.paintboard.packages.${pkgs.system}.default ];
# or, on NixOS:
environment.systemPackages = [ inputs.paintboard.packages.${pkgs.system}.default ];
```

### From source

You need GLFW 3.4 or newer, an OpenGL library, cJSON, Python 3, pkg-config, and a compiler with
C23 `#embed`. GCC 15 or Clang 19 are new enough.

```sh
make
sudo make install            # installs to /usr/local
make install PREFIX=~/.local # or install for one user
```

`make install` also writes the desktop entry and the icons, so Paintboard shows
up in your application launcher.

`nix develop` gives you a shell with the build tools and with `resvg`, which
`make icon` uses to redraw `assets/paintboard.png` after you edit the logo.
Run `make test` to exercise the board operations and MCP server; the protocol
tests use Python 3's standard library, included in the development shell.

The `build` workflow makes the release archives. It builds in an Ubuntu 22.04
container with Clang 19, static GLFW, and static cJSON, and runs `make test`.
A pushed `v*` tag attaches the archives to the GitHub release. To build an
archive on your machine, run `.github/build.sh` as root in an `ubuntu:22.04`
container.

## Use

```sh
paintboard                   # reopens the tabs of the last session
paintboard notes.pb          # opens or creates notes.pb
paintboard plan.pb map.pb    # opens each board in its own tab
paintboard --help
```

The window opens at 85% of the monitor work area. Each board opens in a tab, and
a dot on a tab marks unsaved changes. When you close a tab or the window, a board
with unsaved changes asks you to save it, discard the changes, or cancel. With no
file arguments, Paintboard reopens the boards that were open when you last closed
it, or one Untitled board.

Pick a tool with its key or in the panel, then drag on the canvas. Right drag
pans, and the wheel zooms. The sections below list every key.

<details>
<summary><b>Files and tabs</b></summary>

| Key or action | Result |
| --- | --- |
| `Ctrl+N` or `+` on the tab bar | New Untitled board in a new tab |
| `Ctrl+O` | Open a board |
| `Ctrl+S` | Save, or Save As for an Untitled board |
| `Ctrl+Shift+S` | Save As |
| `Ctrl+W`, the `×` on a tab, or a middle click on a tab | Close the tab |
| `Ctrl+Tab` or `Ctrl+PgDn` | Next tab |
| `Ctrl+Shift+Tab` or `Ctrl+PgUp` | Previous tab |
| `Ctrl+1` to `Ctrl+9` | Go to that tab |
| `Ctrl+Q` | Close the window |

Open and Save As use a built-in file dialog. It lists the folders and `.pb`
files in a folder. Click a folder to open it, or click **Up**. Click a file to
pick it, then double-click it or press `Enter`. You can also type a name, an
absolute path, or a path that starts with `~/`. Save As adds `.pb` to a name
with no extension, and asks before it replaces a file. If a board is already
open, Open switches to its tab. Copy and paste work between tabs.

Another program, such as a headless agent or a script, can save a board that is
open in a tab. Paintboard looks at the file when the window gets the focus and
when you switch to the tab:

- A tab with no unsaved changes loads the new file. Undo brings back the version before it.
- A tab with unsaved changes asks: **Reload**, **Overwrite**, or **Keep mine**.

Save asks the same question when the file changed since the tab read or saved it.

</details>

<details>
<summary><b>Tools, labels, and containers</b></summary>

| Key | Tool |
| --- | --- |
| `P` | Pen |
| `H` | Marker (translucent highlighter) |
| `L` | Line |
| `A` | Arrow |
| `R` | Rectangle |
| `O` | Ellipse |
| `D` | Diamond |
| `I` | Triangle |
| `S` | Star |
| `T` | Text |
| `V` | Select |
| `E` | Eraser |

The eraser removes every item it touches. An empty shape erases only at its outline.

Start a line or an arrow on an item, or end it on one, to link that end. A
linked end stays on the item's outline when you move or resize the item. To link an
existing end, select the line or arrow and drag its corner handle onto an item.
Moving the line or arrow without its item unlinks that end.

Double-click a shape, a line, or an arrow with the select tool to type its
label. A shape centers its label and wraps it to fit. A line or an arrow shows
its label halfway along. The label moves with its item. `[` and `]` change the
label size while you type, and erasing all the text removes the label.

A shape that holds other items is a container. It shows its label at the top
left, and moving it moves what it holds. Drop an item fully inside a container
to add it, and drag an item out to take it out. `Ctrl+G` puts the selection in a
new dashed container, just behind it, with room on top for a label. Agents also
create containers, and tables, through MCP.

</details>

<details>
<summary><b>Select and edit</b></summary>

| Key or action | Result |
| --- | --- |
| Click | Select one item |
| Shift click | Add an item to the selection, or remove it |
| Drag empty space | Select every item inside the box |
| Drag a selected item | Move the whole selection |
| Drag a corner handle | Resize the whole selection |
| `Ctrl+A` | Select all |
| `Esc` | Clear the selection |
| `Del` | Delete the selection |
| `Ctrl+Z` | Undo |
| `Ctrl+Y` or `Ctrl+Shift+Z` | Redo |
| `Ctrl+D` | Duplicate |
| `Ctrl+G` | Put the selection in a dashed container |
| `Ctrl+C`, `Ctrl+X`, `Ctrl+V` | Copy, cut, paste at the pointer |
| `Ctrl+Shift+Del` or the Reset button | Clear the board and reset the view. `Ctrl+Z` brings the items back |

</details>

<details>
<summary><b>Style</b></summary>

| Key or action | Result |
| --- | --- |
| `1` to `8` | Set the stroke color |
| `[` and `]` | Change the stroke width, or the text size for text |
| Panel `S`, `M`, `L`, `XL` | Pick a preset size for the current tool or the selection |
| `G` or panel **Snap** | Turn grid snapping on or off |
| Panel **Dash** | Dashed strokes for new lines, arrows, and shapes, or for the selection |
| Panel **Elbow** | Right-angle routes for new lines and arrows, or for the selected ones |
| Panel swatches | Set the stroke color and the fill color |

Each tool keeps its own size, so the pen, the marker, and text do not share one
width. A style key changes the current tool setting. If something is selected, it
also changes the selection. A panel control that would change nothing for the
current tool and the selection is greyed out: with the pen, the fill, Snap,
Dash, and Elbow; with the eraser, also the colors and the width. Pick the tool
first, then its style.

</details>

<details>
<summary><b>Text</b></summary>

Pick the text tool and click the canvas to start typing. Press `Enter` for a new
line and `Esc` when you finish. Click an existing text item with the text tool,
or double-click it with the select tool, to edit it again. The arrow keys, `Home`,
and `End` move the caret, `Del` deletes forward, and `Ctrl+V` pastes. `[` and
`]` resize the text while you type.

A click on the panel while you type ends the edit and selects the text, so a
color or size click applies to it. `Ctrl+S` saves and `Ctrl+E` exports without
leaving text editing. Erasing all the text removes the item when you finish;
undo restores it. Starting an empty item and finishing cancels it.

</details>

<details>
<summary><b>View and export</b></summary>

| Key or action | Result |
| --- | --- |
| Right or middle drag | Pan |
| Wheel | Zoom at the pointer, or scroll the panel when over it |
| `Ctrl+=` / `Ctrl+-` | Zoom in / out |
| `0` | Reset pan and zoom |
| `Ctrl+E` | Export a PNG next to the board file |
| `Ctrl+Shift+E` | Export an SVG next to the board file |

The PNG export writes what the canvas shows right now, without the grid and
without the tool panel. Pan and zoom to frame the picture before you export it.
The SVG export always holds the whole board as vectors. A board with text embeds
the canvas font in the SVG, which adds about 80 KB.

The panel scales with the monitor DPI and scrolls when the window is short. Set
`PAINTBOARD_UI_SCALE=1.5` to scale the panel and the selection handles by hand.

</details>

## Agents and MCP

Open Paintboard normally, then click **Collaborate: Off** to turn collaboration
on. The toggle controls both prompted agent access and automatic replies.

- Draw, pause for about 1.5 seconds, and the selected agent will inspect the canvas and add a
  short response in purple text, arrows, or shapes. The panel shows when it is
  thinking. Finish text editing with Esc before expecting a response.
- Ask your connected agent in chat to look at the drawing or add something.
  Its tools attach to this same window.
- Turn Collaborate off to stop the watcher. Responses based on an older drawing
  or a previous toggle session are discarded.
- While Collaborate is off, a chat agent's call opens a question in the window,
  such as "claude-code wants to call add_items". **Allow once** answers this
  call. **Allow for this session** turns Collaborate on without automatic
  replies. **Deny** or no answer within 45 seconds refuses the call.
- Each response is one undo step. Agent edits and Undo/Redo do not trigger
  another automatic reply; the watcher waits for your next drawing edit.

### Connect a chat agent

Connect Codex to the live attachment bridge:

```sh
codex mcp add paintboard -- paintboard-bridge --mcp
```

For clients that use an `mcpServers` JSON configuration, such as Claude Code:

```json
{
  "mcpServers": {
    "paintboard": {
      "command": "/absolute/path/to/paintboard-bridge",
      "args": ["--mcp"]
    }
  }
}
```

For a source checkout, use `python3 /absolute/path/to/paintboard-bridge` as the
server command. The bridge can start before a canvas is open and does not own the
window: disconnecting a chat leaves the drawing open. It connects over a private
local Unix socket, with no listening TCP port. If several windows are open, add
`--board /absolute/path/to/notes.pb` to select one explicitly.

The server also serves a guide for agents, `agent-guide.md`, as the resource
`paintboard://guide` and the prompt `paintboard_guide`. It covers the workflow,
coordinates, palette, layout, and an example diagram.

### Scripts and headless use

Scripts do not need their own MCP client. `--call` repeats, and each `--args`
belongs to the `--call` before it. `--args @FILE` reads the arguments from a
file, and `--args @-` from stdin. `--file` runs the calls on a board file in a
headless `paintboard`, with no window:

```sh
paintboard-bridge --file board.pb \
  --call add_graph --args @graph.json \
  --call update_items --args '{"items": [{"id": "ship", "item": {"type": "diamond", "x0": 680, "y0": 80, "x1": 840, "y1": 180, "fill": 1}}]}' \
  --call export_svg
```

One call prints indented JSON. Several calls print one line of JSON each. With
`--file`, the board saves when every call succeeds. A failed call stops the run,
names the call, and leaves the file as it was. A window that has the file open
reloads it, as described in [Files and tabs](#use).

Without `--file`, the calls go to the open window, which works without restarting a chat:

```sh
paintboard-bridge --list
paintboard-bridge --call get_board
paintboard-bridge --call get_canvas --image-out /tmp/canvas.png
```

To get a picture of a board without an agent or a window:

```sh
paintboard --render notes.pb             # writes notes.png
paintboard --render notes.pb view.png
paintboard --render notes.pb view.svg    # vectors; needs no EGL driver
```

<details>
<summary><b>Automatic replies and agent settings</b></summary>

The **AUTO REPLY** control cycles through installed **Codex**, **Claude Code**,
**OpenCode**, and **Chat only** (prompted MCP access without automatic replies).
Paintboard checks PATH and common user install directories on startup. Click
**R** beside the selector to refresh after installing an agent. Switching agents
cancels any pending reply. Codex is the initial choice when available; selection
lasts for the current window. Discovery checks executables, not sign-in readiness.

Automatic replies use the selected CLI's existing sign-in and model configuration
in separate sessions, without your chat's conversation history. Only the current
canvas image and board data are supplied. Use `PAINTBOARD_CODEX_MODEL`,
`PAINTBOARD_CLAUDE_MODEL`, or `PAINTBOARD_OPENCODE_MODEL` to override the model
(OpenCode uses `provider/model`). `PAINTBOARD_MODEL` remains a Codex-only alias.
`PAINTBOARD_CODEX`, `PAINTBOARD_CLAUDE`, and `PAINTBOARD_OPENCODE` override executable
locations. `paintboard-bridge --discover` prints detected paths. Drawing remains
available if an agent is unavailable; failures appear in the panel and stderr.

</details>

<details>
<summary><b>Custom agents</b></summary>

Click **+** beside AUTO REPLY, enter a launch command, choose a connection mode,
then **Save & use**. The command is checked for an installed executable and becomes
the **Custom** choice. Settings survive restarts; clearing the command removes it.
The setup currently stores one custom command, which you can replace at any time.
Saving a command cancels any pending reply. It runs when collaboration is on.

- **ACP** connects to agents implementing [Agent Client Protocol v1](https://agentclientprotocol.com/protocol/v1/initialization)
  over stdin/stdout. For example, `opencode acp --pure` uses the generic connection.
  The agent must advertise image support and already be signed in. Paintboard
  creates a session, sends the canvas and board data, and converts its final JSON
  response into annotations. Incomplete replies are discarded. Client file and
  terminal services are unavailable, and permission requests are declined.
- **Command: prompt on stdin** runs a CLI once per reply. The prompt includes
  board data and the annotation schema. Pass the image through an argument such
  as `my-agent --image {image} --prompt-file {prompt_file}`. This is a template;
  use the flags supported by your agent. It must return annotation JSON on stdout
  or write it to `{output}`. JSON code fences are accepted.
- **Command: JSON request on stdin** is for wrapper programs. It receives a JSON
  object with `version: 1`, `prompt`, `board`, `image` (`mimeType` and base64 `data`),
  `paths`, and `output_schema`. Its output uses the same annotation format.

Command arguments support `{image}`, `{prompt}` (the actual prompt text),
`{prompt_file}`, `{board}`, `{schema}`, `{request}`, and `{output}`. All other
placeholders name temporary files. `{request}` and the `PAINTBOARD_REQUEST`
environment variable point to the complete JSON request. These files last only
for the reply. Quote arguments containing spaces; expansion happens after argument
splitting, without an implicit shell. Prefer `{prompt_file}` over `{prompt}` for
large boards to avoid operating-system argument size limits.

For example, a wrapper can return:

```json
{"annotations":[{"type":"text","x0":300,"y0":200,"x1":0,"y1":0,"text":"Check this connection"}]}
```

At most six annotations may be returned, using `text`, `arrow`, `rect`, or `ellipse`
in world coordinates. A supplied output file takes precedence over stdout;
otherwise keep diagnostics on stderr. Custom programs use their own authentication
and tool settings; choose their non-interactive mode and appropriate permissions.
Paintboard cannot infer an arbitrary CLI's image flags or output format from its
executable name. Agents without ACP or suitable CLI output need a wrapper, or can
connect through MCP for prompted use.

The saved configuration is `$XDG_CONFIG_HOME/paintboard/custom-agent.json`, falling
back to `~/.config/paintboard/custom-agent.json`. It stores `command` and `transport`
(`acp`, `command`, or `json`), with owner-only file permissions. Sign in using the
agent's own CLI rather than putting credentials in the command.

</details>

<details>
<summary><b>MCP tools</b></summary>

| Tool | Purpose |
| --- | --- |
| `get_status` | Discover the active tab, tab count, toggle state, human revision, and whether editing is in progress |
| `get_canvas` | Read a canvas image without writing an export file |
| `get_board` | Read items, bounds, viewport, and the current revision; supports pagination |
| `add_items` | Add a batch of items, with ids, labels, captions, and connecting arrows |
| `add_graph` | Lay out nodes, edges, and groups, and draw them |
| `update_items` | Replace complete items, named by id or by index |
| `replace_items` | Swap the whole board for a new list of items |
| `delete_items` | Delete items by id or by index |
| `undo`, `redo` | Use the shared human/agent history |
| `set_view` | Set pan and zoom to frame the drawing |
| `save_board` | Save the active tab, or Save As with `path` |
| `export_png` | Save a PNG next to the board and return an image preview |
| `export_svg` | Save an SVG of the whole board next to the board file |
| `list_tabs`, `switch_tab` | See the open tabs and change the active one |
| `new_board`, `open_board`, `close_tab` | Open and close tabs |
| `measure_text` | Measure text before placing it |

Start with `get_board` and `get_canvas`. When adding a response to a snapshot,
pass its `revision` and `collaboration_epoch` to `add_items` so late responses are
rejected. Items keep their `id` in the file, so a script can name them later:
`update_items` and `delete_items` take ids without a `revision`. An edit that
uses a board index, and `undo` and `redo`, need the latest `revision`, returned
by reads and edits. A stale revision is rejected; read the board again before
retrying. Indices follow paint order and can shift after deletion or undo.
Edits are rejected while a human is dragging or typing. Each successful batch is
one undo step, and an invalid batch changes nothing.

Tools act on the active tab, and a tab switch changes the revision. Call
`save_board` to persist edits explicitly; an Untitled tab has no file to save.
Closing the window asks about unsaved changes. Disconnecting the attachment
bridge does not close or save the window. The MCP implementation follows the
[stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
and [tool protocol](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).

</details>

<details>
<summary><b>Graph layout and items</b></summary>

For boxes and arrows, `add_graph` does the layout. It takes `nodes` and `edges`,
and optional `direction` (`right` or `down`), `x`, `y`, and `elbow`:

```json
{
  "nodes": [
    {"id": "plan", "label": "Plan", "fill": 3, "group": "Team"},
    {"id": "build", "label": "Build", "caption": "two weeks", "fill": 2, "group": "Team"},
    {"id": "ship", "label": "Ship?", "shape": "diamond", "fill": 4}
  ],
  "edges": [
    {"from": "plan", "to": "build"},
    {"from": "build", "to": "ship"},
    {"from": "ship", "to": "plan", "label": "no", "dashed": true}
  ]
}
```

Nodes rank along the longest path from a source. `rank` pins a node's position
along the flow: its column when the graph runs right, its row when it runs down. The
nodes of a `group` sit together in a dashed container that has the group name as
its id. An edge that closes a cycle routes around the graph with right angles.
Node ids become item ids. The graph goes below what is already on the board.

To place items by hand, the arguments to `add_items` can be:

```json
{
  "items": [
    {"type": "rect", "id": "plan", "x0": 220, "y0": 80, "x1": 380, "y1": 140, "fill": 3, "color": 3, "label": "Plan"},
    {"type": "rect", "id": "build", "x0": 460, "y0": 80, "x1": 620, "y1": 140, "fill": 2, "color": 2, "label": "Build", "caption": "two weeks"},
    {"type": "arrow", "from": "plan", "to": "build", "label": "then"}
  ]
}
```

A `label` belongs to its shape, line, or arrow and moves with it. A `caption`
adds a smaller gray line under a shape's label. `from`, `to`, and `parent` take
a board index, an id on the board, or the id of an earlier item in the batch.
The arrow's ends stop at the outlines of the two items and stay linked: when the
human or `update_items` moves an item, its arrows follow. An invalid item names
its position and the reason. Other fields:

- `points` on a line or an arrow are waypoints, and `elbow: true` turns the route at right angles.
- `dashed: true` draws a dashed stroke on a line, an arrow, or a shape.
- `parent` puts an item in a container; moving the container moves what it holds.
- `wrap` on a text item wraps its lines at that width.
- A `table` takes `x0`, `y0`, `x1`, and `rows` of cells, and sizes its rows to the text.

Coordinates are world units. At the default view, the tool panel covers the left
168 logical pixels times the UI scale; `get_board` reports the window size.
Pen and highlight items use absolute `[x, y]` pairs in `points`. Text uses
`x0`, `y0`, and `text`; the other shapes use two corners/endpoints. Stroke width
defaults to 3 and text size is seven times the width. Color indices `0`–`7`
follow the panel palette, and fill `-1` means transparent. Full schemas are
available through `tools/list`.

</details>

<details>
<summary><b>Stdio sessions owned by a client</b></summary>

The binary also supports client-owned stdio sessions and headless automation:

```sh
paintboard --mcp /absolute/path/to/notes.pb
paintboard --mcp --headless /absolute/path/to/notes.pb
```

These explicitly opt in to agent access and own their board session. Use one
process per board file to avoid competing saves. In headless mode, `get_canvas`
and `export_png` draw every item, fitted, with no window. That needs an EGL
OpenGL driver such as Mesa, but no display server. The stdio server writes only
protocol messages to stdout.

Closing the client-owned `paintboard --mcp` stdin saves every tab that has a
file and unsaved changes, without a question. It does not replace a file that
another program saved meanwhile; it reports that on stderr and exits with
status 1. `save_board` refuses the same case unless you pass `overwrite: true`,
and `open_board` with `reload: true` loads the new file as one undo step.

</details>

<details>
<summary><b>Tests</b></summary>

`make test` runs the board operations and the MCP stdio tests with no window.
The protocol tests can also exercise a live display with
`PAINTBOARD_TEST_LIVE=1 python3 tests/test_mcp.py ./paintboard`.
`tests/test_collaboration.py` verifies scheduling, cancellation, discovery, and
the three built-in CLI adapters without making model calls.
`tests/test_custom_agents.py` verifies command expansion, ACP capabilities,
streaming, permission denial, timeouts, and stale-response cancellation.

`tests/test_collaboration_live.py` uses X11 and XTest on a private test display
to exercise the selector, custom setup, saved configuration, and replies. It also
drives the tabs, the file dialog, the close question, session restore, the
question before an agent call, reloads after another program saves, labels, and
containers. It saves screenshots to `PAINTBOARD_TEST_ARTIFACTS` (default
`$TMPDIR/paintboard-e2e`). The window must open at 1088 by 870, which a
1280 by 1024 Xvfb screen gives. In a Wayland session, unset `WAYLAND_DISPLAY`,
or GLFW opens the test windows on your desktop instead. The real Codex
smoke test is opt-in with `PAINTBOARD_TEST_REAL_CODEX=1`.

</details>

## Files

A board is one binary file. The format is described in
[ARCHITECTURE.md](ARCHITECTURE.md). Paintboard also reads the older `PB02`, `PB03`,
and `PB04` files and writes them back in the current format, `PB05`.

## Design notes

Read [ARCHITECTURE.md](ARCHITECTURE.md) for the data model, the render loop, the
undo system, and the file format.

## Third party

The `vendor/` directory holds the stb single file libraries and the two fonts,
Inter and Excalifont. See [vendor/NOTICE](vendor/NOTICE) for the credits and the
licenses.

## License

MIT. See [LICENSE](LICENSE).
