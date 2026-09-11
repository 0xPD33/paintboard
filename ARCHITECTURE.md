# Architecture

The board and GUI live in `paintboard.c`; `mcp.c` is included into the same
translation unit and exposes those operations to agents. `collaboration.c`
connects existing windows to clients; `paintboard-bridge` runs the local MCP
proxy and the automatic agent watcher. Tool schemas are
embedded from `mcp-tools.json`. The build uses a Makefile. This document
explains the parts that are not obvious from reading the source top to bottom.

## Shape of the program

`main` bakes the fonts, loads the board, opens a GLFW window at 85% of the
primary monitor's work area, and then blocks in `glfwWaitEvents`. The program
only redraws after an event arrives, so an idle board costs no CPU.

Rendering uses the OpenGL fixed function pipeline. There are no shaders and no
vertex buffers. Every frame walks the item array and emits quads. This is slow in
principle, but a board with a few thousand items still redraws in well under a
frame, and the code stays short.

## Data model

Everything on the board is an `Item`:

```c
typedef struct { int type, color, fill, sel; float x0, y0, x1, y1, width, sx, sy; int p0, np; } Item;
```

One flat array, `items`, holds them in paint order. Index 0 draws first and sits
at the back. `sel` marks the item as selected.

`x0, y0, x1, y1` mean different things per type. For a rectangle or an ellipse
they are two opposite corners. For a line or an arrow they are the two ends. For
text and for a pen stroke, `x0, y0` is the anchor and `x1, y1` is the measured
bottom right corner, which the code recomputes rather than stores as truth.

`width` is the stroke width in world units. For text it also sets the size, at
seven pixels per unit.

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

There is one mutation: while you type, `text_put` and `text_del` change the tail
of `tpool`. This is safe because only the item being edited can own the tail.
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
top of `undo`. Both stacks hold at most 256 entries. The oldest entry is dropped
when the stack fills.

A full copy per action is the deliberate ceiling. It costs `O(items)` per edit,
which is nothing below roughly a hundred thousand items. A command based system
would be smaller per step and much larger in code.

Drags need care, because a drag produces many motion events but must produce one
undo step. Move and resize therefore keep a pointer, `base`, to the top undo
snapshot, and recompute the result from `base` on every motion instead of adding
a delta to the current value. This gives one undo step per drag, and it also
avoids the rounding drift that repeated deltas cause. A click that does not move
never calls `checkpoint`, so selecting something does not fill the undo stack.

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

Ellipses are 64 segment polygons. Fills use a triangle fan for the ellipse and a
single quad for the rectangle.

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
- Rectangle, ellipse, and text use the bounding box.

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
"PB03"            4 bytes
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
NUL. A file that fails any check is rejected whole, and the program exits with a
message rather than opening a half read board.

Only a missing file starts an empty board. Other open errors abort startup.
Saving writes to a unique temporary file in the destination directory, checks
every write and flush, syncs the file, and renames it over the destination only
after success. Existing file permissions are retained. A failed save reports an
error and leaves the previous file intact.

`PB02` files, which had no fill, no selection flag, and no pen scale, still load.
The reader converts them through a small struct that matches the old layout and
fills the new fields with defaults. They are written back as `PB03`.

## PNG export

Export re-renders the canvas with decorations off, reads the canvas region back
with `glReadPixels`, and writes it with `stb_image_write`. It captures the visible
area only. There is no offscreen framebuffer and no rendering of items outside the
window.

Export derives the extension from the basename, preserving dotted directory
names and allocating enough space for the full path. A board already named
`*.png` exports as `*.png.png` so the export cannot replace that board directly.

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

Items retain the PB03 layout. Agents address them by array index paired with a
monotonic session revision, which changes on checkpoints and history restores.
This prevents stale edits after human actions, deletions, or undo. Add/update
batches stage all items first and create one checkpoint only after validation;
failed batches roll back unused pool tails. Agent edits share normal GUI undo.
Reads paginate through the item array, and exports return a PNG path plus an
inline image preview when the file is at most 32 MiB.

`--mcp --headless` runs the same dispatcher synchronously without GLFW startup.
It bakes fonts for text measurement and supports all tools except canvas images and PNG export.
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
- Pen points closer than two screen pixels are dropped while drawing. There is no
  curve fitting or smoothing.
- Text editing appends at the end only. There is no caret movement and no
  selection inside a text item.
- PNG export is limited to the visible area.
- The UI scale reads the primary monitor's DPI once at startup. A window moved
  to a monitor with a different DPI keeps that scale; `PAINTBOARD_UI_SCALE` is
  the override.
