# Architecture

Paintboard is one C file of about 770 lines. It has no internal headers, no
build system beyond a Makefile, and no runtime configuration. This document
explains the parts that are not obvious from reading the source top to bottom.

## Shape of the program

`main` opens a GLFW window, bakes the fonts, loads the board, and then blocks in
`glfwWaitEvents`. The program only redraws after an event arrives, so an idle
board costs no CPU.

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
position drifting apart. A click below `PANEL_W` runs the hit test pass and
returns before the canvas ever sees the event.

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

`PB02` files, which had no fill, no selection flag, and no pen scale, still load.
The reader converts them through a small struct that matches the old layout and
fills the new fields with defaults. They are written back as `PB03`.

## PNG export

Export re-renders the canvas with decorations off, reads the canvas region back
with `glReadPixels`, and writes it with `stb_image_write`. It captures the visible
area only. There is no offscreen framebuffer and no rendering of items outside the
window.

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
