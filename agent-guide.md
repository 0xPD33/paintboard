# Drawing on Paintboard

Paintboard is an offline whiteboard. You draw by adding items through MCP tools.
Every edit is one undo step for the human, so prefer one `add_items` call per idea.

## Workflow

1. `get_status`: see the active tab's path, the tab count, and whether the window is busy.
2. `list_tabs`, `switch_tab`, `open_board`, or `new_board`: pick the board. Tools act on the active tab.
3. `get_board`: read the items and the `revision`. Read it again after a tab switch.
4. `add_items`: draw. Use `id`, `label`, and `from`/`to` instead of computing coordinates.
5. `get_canvas`: look at the result. Fix overlaps with `update_items` or `delete_items`.
6. `save_board`: write the file. An Untitled tab needs `path`. `export_svg` then
   writes a vector copy next to it, with no OpenGL needed.

`update_items`, `delete_items`, `undo`, and `redo` need the current `revision`. A stale
revision means the human or a tab switch changed the board: read it again and retry.
A busy error means the human is drawing or a dialog is open: wait a moment and retry.

## Coordinates

World units, x to the right, y down. At zoom 1 one unit is one screen pixel.
In a window, `screen = world * zoom + pan`. The tool panel covers the left 168 px
and the tab bar the top 32 px, so with the default view put content at x >= 200
and y >= 60. `get_board` returns the view. Headless, `get_canvas` shows every item
fitted, so position does not matter.

The grid is 20 units. Snap to multiples of 20 for tidy layouts.

## Items

| type | fields |
| --- | --- |
| `rect`, `ellipse`, `diamond`, `triangle`, `star` | `x0, y0, x1, y1` (two corners), `fill`, `label`, `label_size` |
| `line`, `arrow` | `x0, y0, x1, y1` (start and end); `from` replaces `x0, y0`, `to` replaces `x1, y1` |
| `text` | `x0, y0` (top left), `text` (newlines allowed) |
| `pen`, `highlight` | `points`: absolute `[x, y]` pairs |

All items take `color` and `width`. `add_items` items also take `id`.

- Colors 0-7: black, red, green, blue, orange, purple, teal, gray.
- Fills 0-7: light gray, pink, light green, light blue, light yellow, lavender, light teal, gray. -1 is no fill.
- `width` is the stroke width, default 3. For text it is the size: 7 px per unit,
  so 3 is 21 px, 4 is 28 px, and 6 is a heading.
- `highlight` is a translucent marker. Use width 16 or more.

## Layout

- `measure_text` returns the width and height of text before you place it.
- A box for a one-line label at size 3: text width + 40 wide, 60 tall.
- Leave 60-80 units between boxes so arrows have room.
- A `label` centers black text on its shape. Every fill is light, so labels stay
  readable. Give the shape a matching stroke `color` and `fill`, such as 3 and 3.
  For colored text, add a `text` item instead.
- `from`/`to` take a board index or the `id` of an earlier item in the same batch.
  The ends stop just outside the two items' bounding boxes.
- Linked ends follow their items. To move a box and its arrows, update the box
  only. `get_board` shows the links as `from`/`to` indices. Send them back in
  `update_items` to keep a connector linked; without them the ends come free.

- `add_items` returns `indices` and `bounds` for each item you sent. A label is a
  separate text item right after its shape.

## Example: a three-step flow

```json
{"items": [
  {"type": "rect", "id": "a", "x0": 200, "y0": 100, "x1": 360, "y1": 160, "fill": 3, "color": 3, "label": "Plan"},
  {"type": "rect", "id": "b", "x0": 440, "y0": 100, "x1": 600, "y1": 160, "fill": 2, "color": 2, "label": "Build"},
  {"type": "diamond", "id": "c", "x0": 680, "y0": 80, "x1": 840, "y1": 180, "fill": 4, "color": 4, "label": "Ship?"},
  {"type": "arrow", "from": "a", "to": "b"},
  {"type": "arrow", "from": "b", "to": "c"},
  {"type": "text", "x0": 200, "y0": 40, "text": "Release flow", "width": 4}
]}
```

## Collaboration

In a live window, the human enables **Collaborate** in the panel. Until then only
`get_status` and `measure_text` work. Respect the human's drawing: add next to it,
and do not delete or move their items unless they ask.
