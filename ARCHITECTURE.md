# Architecture

The board and GUI live in `paintboard.c`; `mcp.c` is included into the same
translation unit and exposes those operations to agents. `collaboration.c`
connects existing windows to clients; `paintboard-bridge` runs the local MCP
proxy and the automatic agent watcher. Tool schemas are
embedded from `mcp-tools.json`. The build uses a Makefile. This document
explains the parts that are not obvious from reading the source top to bottom.

## Shape of the program

`main` bakes the fonts, opens the tabs, opens a GLFW window at 85% of the
primary monitor's work area, and then blocks in `glfwWaitEvents`. The program
only redraws after an event arrives, so an idle board costs no CPU.

Rendering uses the OpenGL fixed function pipeline. There are no shaders and no
vertex buffers. Every frame walks the item array and emits quads. This is slow in
principle, but a board with a few thousand items still redraws in well under a
frame, and the code stays short.

## Data model

Everything on the board is an `Item`:

```c
typedef struct { int type, color, fill, sel; float x0, y0, x1, y1, width, sx, sy; int p0, np, from, to; } Item;
```

One flat array, `items`, holds them in paint order. Index 0 draws first and sits
at the back. `sel` marks the item as selected.

`x0, y0, x1, y1` mean different things per type. For a rectangle or an ellipse
they are two opposite corners. For a line or an arrow they are the two ends. For
text and for a pen stroke, `x0, y0` is the anchor and `x1, y1` is the measured
bottom right corner, which the code recomputes rather than stores as truth.

`width` is the stroke width in world units. For text it also sets the size, at
seven pixels per unit.

### Linked ends

`from` and `to` link the ends of a line or an arrow to other items. The value is
the item's index plus one, so 0 is a free end and a zeroed `Item` has no links.
Only lines and arrows have links.

A linked end is derived data. `relink()` puts it on the linked item's bounding
box, 8 units outside, on the line toward the other end. That other end is the
center of its linked item, or the free end's point. Every change to an item's
box calls `relink()`: a move, a resize, a text edit, a width change, an MCP add
or update, and a load. Overlapping boxes connect center to center.

Indices shift when items are removed, so every removal goes through
`compact()`. It moves the links to the new indices. An end whose item is removed
becomes free where it is. A move or resize that takes a connector but not its
linked item frees that end: the connector goes where the human drags it. Copy,
cut, paste, and duplicate keep the links between the copied items and drop the
others. Undo needs nothing extra, because a snapshot holds the links.

The human links an end in three ways. A line or arrow that starts on an item
links its start. One that ends on an item links its end. Dragging the end handle
of one selected connector onto an item links the end nearer the pointer.
`link_target()` finds the topmost item under the point that is not a connector.

### The two pools

Pen points and text bytes do not live in the `Item`. They live in two pools:

- `pool`, an array of floats, holds pen points as `x, y` pairs relative to the
  item anchor.
- `tpool`, an array of bytes, holds UTF-8 text with a NUL after each string.

An item points into a pool with `p0` and counts elements with `np`.

Both pools are append only. Nothing is ever removed from them, and no existing
entry is ever rewritten. That single rule is what makes undo cheap, so it is
worth stating why.

An undo step copies the `Item` array and nothing else. If the pools could shift,
a snapshot taken before an edit would point at the wrong bytes after it. Because
the pools only grow, every index in every snapshot stays correct forever. Deleting
an item leaks its pool entries, which is fine for a drawing session and saves the
whole problem of tracking references.

There are two mutations, and both touch only a pool tail that one new item owns.
While you type, `text_put` and `text_cut` change the tail of `tpool` at the
caret. When a pen or marker stroke ends, `smooth_stroke` averages the stroke's
points in place at the tail of `pool`. No snapshot refers to those entries yet.
When you click an existing text item to edit it, `start_edit` copies its bytes to
the end of the pool and repoints the item there, so the older snapshots keep
looking at the original copy.

Saving or exporting while typing commits that copy and starts a new edit copy,
so continued typing cannot rewrite the saved/undo version. Finishing an empty
new item cancels its creation; erasing existing text deletes it as an undoable
edit.

### Pen scaling

`sx` and `sy` scale the pooled points when the stroke is drawn or tested:

```c
x = it->x0 + pool[it->p0 + 2 * k] * it->sx;
```

Resizing a pen stroke therefore changes two floats. It never rewrites the pool,
which would break the append only rule.

## Undo and redo

`checkpoint()` runs before every mutation and pushes a copy of the whole `Item`
array onto `undo`. `do_undo` swaps the current array onto `redo` and restores the
top of `undo`. Each tab has its own stacks, and each holds at most 256 entries.
The oldest entry is dropped when the stack fills.

A full copy per action is the deliberate ceiling. It costs `O(items)` per edit,
which is nothing below roughly a hundred thousand items. A command based system
would be smaller per step and much larger in code.

Drags need care, because a drag produces many motion events but must produce one
undo step. Move and resize therefore keep a pointer, `base`, to the top undo
snapshot, and recompute the result from `base` on every motion instead of adding
a delta to the current value. This gives one undo step per drag, and it also
avoids the rounding drift that repeated deltas cause. A click that does not move
never calls `checkpoint`, so selecting something does not fill the undo stack.

## Documents and tabs

One window holds several boards, one per tab. The board code works on globals:
`items`, the two pools, the undo and redo stacks, `path`, `dirty`, and the view
(`panx`, `pany`, `zoom`). A `Doc` struct holds a copy of that set for each tab.
Only the active tab's set is in the globals, and `docs[cur]` is stale until
`doc_park` refreshes it. Code that reads every tab, such as the tab bar, calls
`doc_park` first.

To switch tabs, `doc_park` copies the globals into the active `Doc`, and
`doc_enter` copies the new tab's `Doc` into the globals. The rest of the code
does not know that tabs exist. The other design, a `doc->` prefix on every board
access, touches almost every function for no change in behavior.

Before a switch, `doc_leave` ends any text edit and any drag, so no index or
`base` pointer can point into the board that leaves. The text caret and the drag
state therefore need no copy in `Doc`.

`doc_enter` increments `revision`. Every agent edit carries the revision it read,
so a reply computed for the old tab fails that check and cannot land on the new
tab. `revision` and `human_revision` stay global and monotonic, so two tabs never
share a revision number. A switch does not change `collaboration_epoch` or
`human_revision`. The watcher therefore keeps running and does not reply to a tab
only because the user opened it. It replies after the next human edit.

Some state belongs to the window, not to a board. The tool, the stroke and fill
colors, the tool widths, grid snap, the panel scroll, the agent selection, and
the clipboard stay global for all tabs.

The clipboard must work across tabs. Each pool belongs to one board, so a copied
item cannot keep its pool index. A copy therefore stores the items and a private
copy of their pen points and text bytes. A paste appends that data to the active
pools and moves each `p0` to the new location. `bbox()` cannot read the private
copy, so a copy also records the top left corner of the selection for the paste.

Each `Doc` has a `dirty` flag. `changed()` and `restore()` set it, and a
successful save clears it. An undo back to the saved state leaves the flag set.
That costs a needless save, which is harmless.

A tab with no `path` is "Untitled". Save on an Untitled tab opens Save As.
Export PNG and Export SVG on an Untitled tab show "Save the board first",
because the export path comes from the board path.

### Closing

When a tab or the window closes:

- A tab with a path asks "Save", "Discard", or "Cancel" when `dirty` is set.
  "Save" writes the file. A tab with no change closes without a question.
- An Untitled tab with items asks the same question. Its "Save" opens Save As.
  An Untitled tab with no items closes without a question.
- "Cancel" stops the close.

A failed save also stops the close, so the user does not lose the board.

`proceed()` runs these rules. `close_tab` and the window close set `pending`
and call it. It stops at a question and continues when the dialog is answered.
For the window, it walks the tabs in order and only marks `quit_done`. The tabs
stay open until the process exits, so a cancel leaves the window as it was. The
main loop clears `glfwWindowShouldClose` and runs its own loop until
`quit_done`.

Closing a tab that is not active switches to it first, so a question shows the
board it is about. After the close, the program goes back to the tab that was
active before.

### Opening

Open compares each open tab with the chosen file by device and inode, or by name
when a file does not exist yet. If a tab has the file, the program switches to
that tab. If the active tab is Untitled and untouched, the file loads into it.
Otherwise the file loads into a new tab. A failed load removes that new tab and
shows the error. It does not stop the program, and the other tabs stay as they
were. The open dialog refuses a name that does not exist.

Save As refuses a path that another open tab uses, because two tabs that write
one file overwrite each other.

### The tab bar

The tab bar is a strip across the top of the canvas, right of the panel. It draws
over the canvas, as the hint bar does at the bottom, so canvas coordinates and
PNG export do not change. It uses the same immediate mode walk as the panel. A
left click in the strip runs the hit test pass and returns before the canvas sees
the event.

Each tab shows the file name, a dot when `dirty` is set, and a close button. A
middle click on a tab also closes it. A `+` button after the last tab makes a new
board. Tabs share the width of the strip and shrink to fit, and long names are
cut at the end. The window title shows the active file path and the dot. The
main loop sets the title only when the text changes.

### Shortcuts

| Key | Action |
| --- | --- |
| `Ctrl+N` | New board in a new tab |
| `Ctrl+O` | Open |
| `Ctrl+S` | Save, or Save As for an Untitled tab |
| `Ctrl+Shift+S` | Save As |
| `Ctrl+W` | Close the tab |
| `Ctrl+Q` | Close the window |
| `Ctrl+Tab`, `Ctrl+PgDn` | Next tab |
| `Ctrl+Shift+Tab`, `Ctrl+PgUp` | Previous tab |
| `Ctrl+1` to `Ctrl+9` | Go to that tab |

`1` to `8` without `Ctrl` stay the stroke color keys. The panel has a FILE
section with New, Open, Save, Save As, Export PNG, and Export SVG. During a text edit, the
tab and file shortcuts end the edit first. `Ctrl+S` on a named tab keeps the
edit open, as before.

### The file dialog

Open and Save As use a dialog that the program draws itself. It uses no toolkit
and no helper program, so it works the same on every desktop and offline. It
follows the custom agent dialog: a modal window with its own draft state, so
keys go to the dialog and not to the canvas. Both dialogs edit their field with
`line_key` and draw it with `line_draw`. Opening the dialog ends any text edit,
because a save during an edit would write a text item without its NUL.

The dialog shows:

- The current folder, with an "Up" button.
- A list of the subfolders and the `.pb` files in that folder, sorted by name.
  Hidden entries are not shown. The wheel scrolls the list.
- A name field and the "Cancel" and "Open" or "Save" buttons.

A click on a folder opens it. A click on a file puts its name in the field. A
double click or `Enter` confirms, and `Esc` cancels. The name field also accepts
an absolute path or a path that starts with `~/`. Save As adds `.pb` when the
name has no extension. If the file exists, the button changes to "Replace", and a
second click confirms.

The dialog starts in the folder of the active tab. For an Untitled tab it starts
in the folder that the dialog used last, and else in `$HOME`.

### Messages

`notify()` shows a message in the hint bar for four seconds and also writes it
to stderr. Examples are a failed load, a failed save, a save, and an export.
While a message shows, the main loop waits with a timeout, so the message
disappears without a new event. Errors inside the file dialog, such as a
refused Save As, show in the dialog instead.

### Session

When the window starts to close, the program writes the paths of the named tabs
to `$XDG_CONFIG_HOME/paintboard/session`, next to `custom-agent.json`. The file
is plain text. The first line is the index of the active tab among the named
tabs, and each further line is one absolute path. `config_write` replaces the
file atomically, and the custom agent settings use the same function. `--mcp`
does not write the session, so an agent session cannot replace the user's tabs.

`paintboard` with no file arguments restores the session. A file that no longer
exists or loads is skipped with a message. If no tab remains, the program keeps
one Untitled tab. `paintboard a.pb b.pb` opens each file in its own tab and does
not read the session. A missing file starts an empty board with that path. The
file appears on the first save, and not on a close with no change.

The program opens the tabs after it creates the window, so open errors show in
the hint bar.

### MCP and collaboration

MCP tools and the collaboration watcher act on the active tab. `get_status`
reports the active tab's path, or `null` for an Untitled tab, and the number of
tabs. `list_tabs`, `switch_tab`, `new_board`, `open_board`, and `close_tab` give
an agent the same tab operations as the human. `save_board` with a `path` is Save
As, and an Untitled tab needs one. `export_png` and `export_svg` on an Untitled
tab return a tool error. A tab switch changes the revision, so an agent sees a stale revision error
and must read the board again. A file dialog, a close question, or a pending
close makes the window busy: the watcher waits, and board tools return a busy
error.

`close_tab` refuses a tab with unsaved changes unless `discard` is true. An agent
has no dialog, so the refusal takes the place of the question.

`--mcp` takes at most one FILE and loads it before the window opens. A bad file
stops startup, because the client needs that board. On stdin EOF the program
saves every named tab without a question. `--mcp --headless` has tabs too, through
the same tools.

## Coordinates

Items store world coordinates. The screen transform is
`screen = world * zoom + pan`.

The render loop pushes that transform with `glTranslatef` and `glScalef`, so the
draw code works entirely in world units. Input goes the other way and converts by
hand:

```c
float wx = (sx - panx) / zoom, wy = (sy - pany) / zoom;
```

Zooming with the wheel keeps the point under the cursor fixed. The code solves for
the pan that holds that point still rather than zooming about the window center.

Sizes that should not shrink when you zoom out, such as the selection outline and
the resize handles, divide by `zoom` so they stay constant on screen.

## Drawing strokes

`glLineWidth` is capped at a small value by most drivers, and the cap is not
reported reliably, so Paintboard does not use it. `seg` builds a quad for each
segment and `dot` puts a filled circle at each join and each end. `stroke` walks a
point list and calls both. Round joins come free from the dots.

`poly()` gives the outline of every closed shape. Ellipses are 64 segment
polygons. Fills use a triangle fan from the mean of the vertices, which lies
inside each shape because each shape is star-convex about that point.

The marker draws its stroke at 35% opacity. The joins overlap, so a plain blend
would darken them. The marker therefore uses the stencil buffer, so each pixel
gets paint only once per stroke.

## Text

Both fonts are baked once at startup with `stbtt_BakeFontBitmap` into a 1024 by
1024 alpha texture. Each atlas covers 224 code points starting at 32, which is
ASCII plus Latin-1.

The panel font, Inter, bakes at 32 pixels. The canvas font, Excalifont, bakes at
48 pixels. Both are then scaled to the requested size when drawn. The texture uses
mipmaps and linear filtering, so text stays clean when you zoom out.

`text_run` does double duty. With `draw` set it emits the quads. With `draw`
clear it only measures, which is how `measure()` keeps `x1, y1` on a text item
correct. Code points outside the baked range render as a question mark.

Font metrics can change between builds, so `load()` re-measures every text item
instead of trusting the stored box.

The fonts are compiled into the binary with the C23 `#embed` directive. This is
the reason for the GCC 15 requirement. It removes any question of where the font
files are at runtime.

## Hit testing and selection

`hit()` walks the array backwards, so the item on top wins. The test depends on
the type:

- Pen, line, and arrow use point to segment distance against every segment.
- Closed shapes and text use the bounding box.

The eraser calls the same test with `edges` set. Then an unfilled shape counts
only its outline, so you can erase the items inside a frame and keep the frame.
A drag samples the path every four screen pixels, so a fast stroke does not skip
thin items. The first removal in a drag creates the one undo step.

The tolerance is half the stroke width plus six screen pixels, so thin lines stay
easy to grab at any zoom.

Bounding box testing for a rectangle means you can select it by clicking its
middle. That is convenient more often than it is wrong, so it stays.

Selection lives in the `sel` flag on each item rather than in a separate list.
That way it survives array edits with no bookkeeping, and `sel_bbox` and
`delete_selected` are simple loops.

## The tool panel

The panel is immediate mode, and one function serves both jobs. `ui()` walks the
same widgets in the same order every time. The global `ui_mode` decides what the
walk does:

- `ui_mode == 0` draws the widgets.
- `ui_mode == 1` compares each widget rectangle against the click at `ui_x, ui_y`
  and returns true for the one that contains it.

So `if (button(...)) do_undo();` reads the same in both passes, and there is no
widget list, no layout tree, and no chance of the drawn position and the clickable
position drifting apart. A click left of `panel_w()` runs the hit test pass and
returns before the canvas ever sees the event.

The walk works in logical pixels. One factor, `ui_s`, scales the whole panel,
the custom agent dialog, the resize handles, and the hit tolerance. Each frame
computes it from the GLFW content scale times the window-to-framebuffer ratio,
which is what X11 with a DPI setting needs and what Wayland and macOS already
apply, times the primary monitor's physical DPI over 96, clamped to 1x to 2.5x,
which catches a 4K monitor left at compositor scale 1. `PAINTBOARD_UI_SCALE`
multiplies on top. The draw pass runs under `glScalef(ui_s, ui_s, 1)`; the hit
test pass divides the click by `ui_s` instead.

When the panel is taller than the window, the wheel over the panel scrolls it.
The draw pass translates by `-panel_scroll` and the hit test pass adds
`panel_scroll` to the click, so the two passes stay aligned. `ui()` records its
final `y` as the panel height, so a new widget needs no extra bookkeeping.

## File format

A board file is a header and three arrays:

```
"PB04"            4 bytes
nitems            int32
npool             int32
ntpool            int32
items[nitems]     raw Item structs
pool[npool]       raw floats
tpool[ntpool]     raw bytes
```

The item structs are written as they sit in memory. This is not portable across
compilers or architectures, and that is an accepted trade for a personal format.
It also means the format version must change whenever `Item` changes.

`load()` validates before it commits. It checks the counts against sane limits,
then checks every item: the type and the color are in range, the numbers are
finite, and the pool slice fits inside the pool. A text item must also end at a
NUL. A link must name another item in the file, and only a line or an arrow can
have one. A file that fails any check is rejected whole, and the open fails with a
message rather than opening a half read board.

Only a missing file starts an empty board. Other open errors fail that open, as
described in [Opening](#opening).
Saving writes to a unique temporary file in the destination directory, checks
every write and flush, syncs the file, and renames it over the destination only
after success. Existing file permissions are retained. A failed save reports an
error and leaves the previous file intact.

Older files still load. `PB02` had no fill, no selection flag, and no pen scale.
`PB03` had no links. The reader converts each through a small struct that
matches the old layout and fills the new fields with defaults. They are written
back as `PB04`.

## PNG export

Export re-renders the canvas with decorations off, reads the canvas region back
with `glReadPixels`, and writes it with `stb_image_write`. In a window it captures
the visible area only.

### Offscreen rendering

`paintboard --render FILE [OUT.png]`, and `get_canvas` and `export_png` in
headless MCP, have no window. They draw into an EGL pbuffer instead. The same
`render_canvas` code draws, with a view that fits every item plus a 40 unit
margin at zoom 1. A side longer than 4096 pixels scales the whole image down.

EGL first asks for Mesa's surfaceless platform, which needs no display server,
so it works over SSH and in a container with Mesa. Other drivers fall back to
the default display. The context is made once and keeps the font textures. Each
render makes a pbuffer of the exact size and destroys it afterwards. A config
with 4x multisampling is preferred, and one without it is the fallback. The
stencil buffer is required, because the marker uses it.

With no EGL OpenGL driver, as in the Nix build sandbox, these calls fail with a
message. The tests accept that failure.

Export derives the extension from the basename, preserving dotted directory
names and allocating enough space for the full path. A board already named
`*.png` exports as `*.png.png` so the export cannot replace that board directly.
The same rule applies to `*.svg`.

## SVG export

`write_svg()` writes every item as SVG elements, fitted with the same 40 unit
margin as `--render`, at one SVG unit per world unit. It needs no OpenGL, so
`export_svg` and `paintboard --render FILE OUT.svg` work headless with no EGL
driver. The panel button and `Ctrl+Shift+E` write it next to the board file.

Each item maps to the shape the canvas draws. Strokes use round caps and joins.
The marker is one element at 35% opacity, so its overlaps do not darken, as with
the stencil. Arrow heads use the same geometry as `draw_item`. Text is one
`<text>` element per line, so blank lines keep their height.

A board with text embeds Excalifont as a base64 `@font-face`, about 80 KB, so
the text keeps its look and width in a browser. The font size is the canvas
pixel size times `Font.em`, because stb_truetype sizes by ascent to descent and
CSS sizes by the em square. A renderer without `@font-face`, such as resvg,
falls back to a sans serif font.

## MCP server

`--mcp` reserves stdout for newline-delimited JSON-RPC messages and exposes the
live canvas through MCP tools. cJSON handles JSON parsing and serialization;
`mcp-tools.json` supplies the discoverable input schemas. The server negotiates
the protocol during initialization, waits for the initialized notification, and
then accepts tool requests. Unknown requests get protocol errors; invalid tool
arguments, stale revisions, busy edits, and I/O failures return tool errors.

The input thread reads bounded messages and hands them to the main thread through
a mutex-protected queue, using `glfwPostEmptyEvent` to wake the existing event
loop. Only the main thread touches items, undo snapshots, pools, or OpenGL.
Requests arriving during a human drag or text edit return a busy error. On stdin
EOF the app saves and exits; when the window closes, the reader thread is stopped
before GLFW is terminated. The stdio mode does not listen on a TCP port.

Items keep the file layout. Agents address them by array index paired with a
monotonic session revision, which changes on checkpoints and history restores.
This prevents stale edits after human actions, deletions, or undo. Add/update
batches stage all items first and create one checkpoint only after validation;
failed batches roll back unused pool tails. Agent edits share normal GUI undo.
Reads paginate through the item array, and exports return a PNG path plus an
inline image preview when the file is at most 32 MiB.

`--mcp --headless` runs the same dispatcher synchronously without GLFW startup.
It bakes fonts for text measurement. Its images come from offscreen rendering.

### Help for agents

The server makes diagrams cheap to describe, so an agent does not need to do
coordinate math:

- In `add_items`, an item can take an `id`. A line or an arrow can take `from`,
  `to`, or both instead of the coordinates of that end: a board index, or the
  `id` of an earlier item in the same batch. The ends become
  [linked ends](#linked-ends), so they follow their items when an agent or the
  human moves them. `update_items` takes `from` and `to` as board indices, and
  `get_board` reports them, so a connector read and sent back stays linked.
  Coordinates sent for a linked end are recomputed.
- A closed shape can take a `label`. `add_label` adds a black text item centered
  on the shape, right after it. Black is readable on every fill, because every
  fill color is light. The label is an ordinary item, so the format does not
  change, and the human can edit it.
- `add_items` returns the `bounds` of each item it was sent.
- `measure_text` measures text with the canvas font before the agent places it.
  It reads no board, so it works while collaboration is off.
- `parse_item` returns the reason an item is invalid, and the error names the
  item's position in the batch. The batch still changes nothing.

`agent-guide.md` explains the workflow, the coordinates, the palette, the layout
rules, and an example. The binary embeds it with `#embed` and serves it as the
resource `paintboard://guide` and as the prompt `paintboard_guide`. The bridge
reads the installed copy from `share/paintboard` and serves it the same way. The
server instructions repeat the main points, because many clients show only the
instructions and the tools to the model.
`make test` runs the board regression tests and Python stdio integration tests;
the same protocol suite can target a live display with `PAINTBOARD_TEST_LIVE=1`.

## Live collaboration

Each normal window starts with collaboration off. A local socket in the
owner-only `/tmp/paintboard-<uid>/` directory lets clients attach to the existing
window. A socket reader queues requests for the main thread and wakes GLFW;
it never changes board or GL state. The proxy handles its own MCP lifecycle, so
several clients can use the same window without sharing initialization state.
Disconnecting a client does not end the drawing session.

The UI toggle gates board reads and mutations at the C tool dispatcher. Only
status/discovery is available while off. Turning on launches a Python watcher for the selected agent;
turning off increments a collaboration epoch and terminates the watcher process
group, including a running CLI child. Changing agents also advances the epoch and
cancels pending work; Chat only retains manual tool access without a watcher.
Discovery runs in a separate Python process at startup and on refresh, checking
PATH, common user install directories, and explicit executable overrides. Its
results return through the local socket and are applied on the main thread.
Every automatic response supplies both
its snapshot revision and epoch for an atomic check before adding its items.

Human checkpoints advance a separate `human_revision`; MCP mutations do not.
History restores change the main revision but do not request another response.
The watcher waits for a 1.5-second pause after editing ends, takes board data and
a canvas image, then invokes the selected CLI in an isolated temporary directory.
Codex uses `exec` with a read-only sandbox, a JSON schema, and user configuration
disabled. Claude Code uses streaming image input and schema-constrained output,
safe mode, and no built-in tools or MCP servers. OpenCode receives the image as
a file attachment, JSON event output, and a configuration overlay denying tools.
All adapters use saved authentication and convert replies to the same annotations.
The automatic session is not given the chat's conversation history.

The Custom slot stores one command and transport in the user's XDG config
directory. The dialog uses separate draft state so typing does not edit the
canvas; saving replaces the config atomically and rescans the executable.
Commands are parsed with `shlex`, resolved through the same executable search,
and launched as an argument array. File/prompt placeholders expand after parsing.
Command mode feeds either the prompt or a versioned JSON request through stdin,
and reads annotation JSON from stdout or a bounded output file.

ACP mode implements v1 JSON-RPC over stdio: initialize, check image capability,
create a temporary session, send image and prompt, and collect text message
chunks until `end_turn`. It advertises no file or terminal services, declines
permission requests, bounds protocol messages and output, and checks drawing
revision and timeout while reading and writing the pipes. Stale work sends
`session/cancel` and stops the process. ACP transport does not sandbox an agent's
own tools; custom agents retain their CLI's authentication and permission setup.

The watcher checks for new edits while waiting and discards stale work. It
validates the returned annotations and adds them as one normal undo step. Own
responses do not advance `human_revision`, preventing response loops. A failed
request is shown in the panel and retried only after another human edit or a
toggle. Image capture uses an anonymous temporary file and does not overwrite
the user's PNG export. Collaboration invokes a networked model; drawing itself
remains offline.

## Deliberate limits

The source marks its shortcuts with `ponytail:` comments. The ones that matter:

- Immediate mode rendering. Replace with vertex buffers if a board ever gets big
  enough to drop frames.
- Whole array undo snapshots. Fine below roughly a hundred thousand items.
- Bounding box hit testing for closed shapes. Outline testing would be better if
  nested shapes become annoying.
- Pen points closer than two screen pixels are dropped while drawing. A finished
  stroke gets three passes of a 1-2-1 average. There is no curve fitting.
- The text caret moves only with the keyboard. A click does not place it, and
  there is no selection inside a text item.
- PNG export from a window is limited to the visible area. `--render` draws the
  whole board.
- A linked end stops at the item's bounding box, not at its outline. On an
  ellipse, a diamond, or a star, an end that meets the box near a corner leaves
  a gap to the outline.

- The UI scale reads the primary monitor's DPI once at startup. A window moved
  to a monitor with a different DPI keeps that scale; `PAINTBOARD_UI_SCALE` is
  the override.
