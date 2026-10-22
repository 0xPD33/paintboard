# Drawing on Paintboard

Paintboard is an offline whiteboard. You draw by adding items through MCP tools.
Every edit is one undo step for the human, so prefer one call per idea.

## Workflow

1. `get_status`: see the active tab's path, the tab count, and whether the window is busy.
2. `list_tabs`, `switch_tab`, `open_board`, or `new_board`: pick the board. Tools act on the active tab.
3. `get_board`: read the items and the `revision`. Read it again after a tab switch.
4. Draw. For a diagram of boxes and arrows, send `add_graph` and let Paintboard
   do the layout. Otherwise use `add_items`. Give items an `id`.
5. `get_canvas`: look at the result. Fix it with `update_items` or `delete_items`.
6. `save_board`: write the file. An Untitled tab needs `path`. `export_svg` then
   writes a vector copy next to it, with no OpenGL needed.

Ids stay in the file. `update_items` takes a complete item that carries its `id`,
or `{"id": ..., "item": {...}}`. It and `delete_items` need no `revision` when they
name items by id, so a script can change one item without reading the board first.
Board indices shift, so an edit that uses an index needs the current `revision`.
A stale revision means that the human or a tab switch changed the board: read it again and retry.
`replace_items` swaps the whole board for a new list in one step.
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
| `rect`, `ellipse`, `diamond`, `triangle`, `star` | `x0, y0, x1, y1` (two corners), `fill`, `label`, `caption`, `label_size`, `dashed` |
| `line`, `arrow` | `x0, y0, x1, y1` (start and end); `from` replaces `x0, y0`, `to` replaces `x1, y1`; `label`, `points`, `elbow`, `dashed` |
| `text` | `x0, y0` (top left), `text` (newlines allowed), `wrap` |
| `table` | `x0, y0, x1` (top left and right edge), `rows`, `fill` (the header row), `label_size` |
| `pen`, `highlight` | `points`: absolute `[x, y]` pairs |

All items take `id`, `parent`, `color`, and `width`.

- Colors 0-7: black, red, green, blue, orange, purple, teal, gray.
- Fills 0-7: light gray, pink, light green, light blue, light yellow, lavender, light teal, gray. -1 is no fill.
- `width` is the stroke width, default 3. For text it is the size: 7 px per unit,
  so 3 is 21 px, 4 is 28 px, and 6 is a heading.
- `highlight` is a translucent marker. Use width 16 or more.

## Labels, arrows, and containers

- A `label` belongs to its item and moves with it. A shape centers it in black
  and wraps it to fit. A line or an arrow shows it halfway along, in its color.
  A `caption` is a smaller gray line under a shape's label.
- `from`/`to` take a board index, an id on the board, or the id of an earlier
  item in the same batch. A linked end stops just outside the item's outline,
  also on a diamond or an ellipse, and follows the item when it moves.
- `points` on a line or an arrow are waypoints. With `elbow: true` the route
  turns at right angles. A loop back from the last step can go around the
  boxes: give two waypoints below them.
- `dashed: true` draws a dashed stroke, for example for an optional relation.
- `parent` puts an item in a container. Moving the container moves what it
  holds. A container shows its label at the top left, so leave 40 units there.
- `text` with `wrap` wraps its lines at that width instead of manual line breaks.
- A `table` sizes its rows to the wrapped cell text. Its first row is the header.
- `get_board` returns every field, with links as board indices. Send an item
  back in `update_items` to keep its links.

## Layout

- `add_graph` takes `nodes` (`id`, `label`, `caption`, `shape`, `fill`, `color`,
  `rank`, `group`) and `edges` (`from`, `to`, `label`, `type`, `dashed`, `elbow`).
  It ranks the nodes along the longest path, right or `down`, keeps a group in
  one dashed container, and routes an edge that closes a cycle around the graph.
  A node's `fill` also sets its stroke color, unless you give `color`.
- Steps that run in parallel need no `rank`: give them edges from the same node,
  and they sit side by side. `rank` pins a node's position along the flow,
  counted from 0, so two nodes with ranks 0 and 1 come one after the other.
- `measure_text` returns the width and height of text before you place it.
- A box for a one-line label at size 3: text width + 40 wide, 60 tall.
- Leave 60-80 units between boxes so arrows have room.

## Example: a three-step flow

```json
{"nodes": [
  {"id": "plan", "label": "Plan", "fill": 3},
  {"id": "build", "label": "Build", "caption": "two weeks", "fill": 2},
  {"id": "ship", "label": "Ship?", "shape": "diamond", "fill": 4}],
 "edges": [
  {"from": "plan", "to": "build"},
  {"from": "build", "to": "ship"},
  {"from": "ship", "to": "plan", "label": "no", "dashed": true}]}
```

The same flow with `add_items`, placed by hand:

```json
{"items": [
  {"type": "rect", "id": "a", "x0": 200, "y0": 100, "x1": 360, "y1": 160, "fill": 3, "color": 3, "label": "Plan"},
  {"type": "rect", "id": "b", "x0": 440, "y0": 100, "x1": 600, "y1": 160, "fill": 2, "color": 2, "label": "Build"},
  {"type": "diamond", "id": "c", "x0": 680, "y0": 80, "x1": 840, "y1": 180, "fill": 4, "color": 4, "label": "Ship?"},
  {"type": "arrow", "from": "a", "to": "b"},
  {"type": "arrow", "from": "b", "to": "c"},
  {"type": "arrow", "from": "c", "to": "a", "label": "no", "elbow": true, "points": [[760, 240], [280, 240]]},
  {"type": "text", "x0": 200, "y0": 40, "text": "Release flow", "width": 4}
]}
```

## Collaboration

In a live window, the human enables **Collaborate** in the panel. While it is
off, the window asks the human to allow each call, and the call waits up to 45
seconds. `get_status` and `measure_text` always work. Respect the human's
drawing: add next to it, and do not delete or move their items unless they ask.
