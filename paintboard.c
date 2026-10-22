/* paintboard: offline Excalidraw-ish board. C23 + GLFW + legacy OpenGL + MCP. */
#include <GLFW/glfw3.h>
#include <EGL/egl.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

#ifndef __has_embed
#error "paintboard needs C23 #embed: build with gcc 15 or newer, or clang 19 or newer"
#endif

#define VERSION "0.3.0"
#define APP_ID "paintboard" /* must match the desktop entry file name, or launchers lose the icon */

static const unsigned char TTF_UI[] = {
#embed "vendor/Inter.ttf"
};
static const unsigned char TTF_CANVAS[] = {
#embed "vendor/Excalifont.ttf"
};

/* Item types are saved in board files: append new ones before SELECT, never reorder. */
enum { PEN, LINE, ARROW, RECT, ELLIPSE, TEXT, DIAMOND, TRIANGLE, STAR, HIGHLIGHT, SELECT, ERASER, NTOOLS };
enum { NONE, DRAW, MOVE, RESIZE, MARQUEE, ERASE };
#define NCOLORS 8
#define MAX_UNDO 256
#define PANEL_W 168
#define GRID 20.f
#define HANDLE 5.f /* half-size of a resize handle, px */
#define ACCENT 0x6965db
#define INK 0x1e1e1e
#define MUTED 0x868e96

static const char *TOOL_NAME[NTOOLS] = {"Pen", "Line", "Arrow", "Rect", "Ellipse", "Text", "Diamond", "Triangle", "Star", "Marker", "Select", "Eraser"};
static const char *TOOL_KEY[NTOOLS] = {"P", "L", "A", "R", "O", "T", "D", "I", "S", "H", "V", "E"};
static int is_pen(int t) { return t == PEN || t == HIGHLIGHT; }
static int fillable(int t) { return t == RECT || t == ELLIPSE || t == DIAMOND || t == TRIANGLE || t == STAR; }
static const unsigned STROKE[NCOLORS] = {0x1e1e1e, 0xe03131, 0x2f9e44, 0x1971c2, 0xf08c00, 0x9c36b5, 0x0c8599, 0x868e96};
static const unsigned FILLC[NCOLORS] = {0xe9ecef, 0xffc9c9, 0xb2f2bb, 0xa5d8ff, 0xffec99, 0xeebefa, 0x99e9f2, 0xdee2e6};

/* fill < 0 = none. sx/sy scale pen points. width doubles as text size (px = width * 7).
   from/to link a line or arrow end to the item at that index + 1; 0 is a free end. */
typedef struct { int type, color, fill, sel; float x0, y0, x1, y1, width, sx, sy; int p0, np, from, to; } Item;
typedef struct { Item *it; int n; } Snap;
typedef struct { GLuint tex; stbtt_bakedchar cd[224]; float px, ascent, em; } Font; /* em: CSS font-size per baked px */

/* Pen points (relative to x0,y0) and text bytes live in append-only pools, so an
   undo snapshot only copies the Item array and never goes stale. The only pool
   mutation is the text tail, and only for the item currently being typed into. */
static float *pool; static int npool, cappool;
static char *tpool; static int ntpool, captpool;
static Item *items; static int nitems, capitems;
static Item *clip; static int nclip;
static float *scratch; static int capscratch;
static Snap undo[MAX_UNDO], redo[MAX_UNDO]; static int nundo, nredo;
static Font ui_font, canvas_font;
static GLFWwindow *win;

static int tool = PEN, color, fillc = -1, snap, editing = -1, drag, handle, panning, winh;
static int editing_new, suppress_char;
static float width = 3, panx, pany, zoom = 1, pressx, pressy, sbase[4];
static float tool_width[NTOOLS] = { [0 ... NTOOLS - 1] = 3, [HIGHLIGHT] = 16 }; /* each tool remembers its own width or text size */
static Item *base; /* undo-top snapshot that a move/resize is re-derived from on every motion */
static double lastx, lasty;
static char *path; /* absolute, or NULL for an Untitled tab */
static int dirty; /* the active board differs from its file */
static unsigned long revision, human_revision, collaboration_epoch;
static int collaborating, agent_mutation;
static char collaboration_status[64] = "Off";
static void collaboration_toggle(void);
static void collaboration_select(void), collaboration_discover(void);
#define NAGENTS 4
#define CUSTOM_AGENT 3
static const char *agent_ids[NAGENTS] = {"codex", "claude", "opencode", "custom"};
static const char *agent_labels[NAGENTS] = {"Codex", "Claude Code", "OpenCode", "Custom"};
static char agent_paths[NAGENTS][4096];
static int selected_agent = -2, agent_scanning;
static const char *custom_transports[] = {"acp", "command", "json"};
static const char *custom_transport_labels[] = {"ACP (standard agent protocol)", "Command: prompt on stdin", "Command: JSON request on stdin"};
static char custom_command[4096], custom_draft[4096], custom_error[128];
static int custom_transport, custom_draft_transport, custom_dialog, custom_cursor, custom_select_all;
static void custom_save(void);
static void changed(void) { revision++; dirty = 1; if (!agent_mutation) human_revision++; }
static char notice[200]; static double notice_until; /* hint bar message */
static void notify(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(notice, sizeof notice, fmt, a); va_end(a);
    fprintf(stderr, "%s\n", notice); notice_until = win ? glfwGetTime() + 4 : 0;
}
static int ui_mode; static float ui_x, ui_y; /* 0 = draw the panel, 1 = hit-test a click at (ui_x, ui_y) */
/* Panel geometry is in logical px scaled by ui_s (monitor DPI, times PAINTBOARD_UI_SCALE). */
static float ui_s = 1, ui_user_scale = 1, panel_scroll, panel_h = 800; static int ui_hover;
static float panel_w(void) { return PANEL_W * ui_s; }
static float monitor_dpi_scale(void) { /* logical px per inch over 96; catches 4K monitors left at compositor scale 1 */
    GLFWmonitor *m = glfwGetPrimaryMonitor(); const GLFWvidmode *mode = m ? glfwGetVideoMode(m) : NULL;
    int mmw, mmh; float csx = 1, csy;
    if (!mode) return 1;
    glfwGetMonitorPhysicalSize(m, &mmw, &mmh); glfwGetMonitorContentScale(m, &csx, &csy);
    if (mmw <= 0) return 1;
    return fminf(fmaxf(mode->width / csx / (mmw / 25.4f) / 96, 1), 2.5f);
}

static void *grow(void *p, int *cap, int need, size_t sz) {
    if (need <= *cap) return p;
    do *cap = *cap ? *cap * 2 : 64; while (*cap < need);
    return realloc(p, *cap * sz);
}
static void add_pt(float x, float y) { pool = grow(pool, &cappool, npool + 2, sizeof *pool); pool[npool++] = x; pool[npool++] = y; }
static void add_item(Item it) { items = grow(items, &capitems, nitems + 1, sizeof *items); items[nitems++] = it; }
static void hex(unsigned c) { glColor3ub(c >> 16 & 255, c >> 8 & 255, c & 255); }
static void hexa(unsigned c, float a) { glColor4f((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, a); }
static float snapf(float v) { return snap ? roundf(v / GRID) * GRID : v; }

/* ---- undo: whole-array snapshots. ponytail: O(items) per action, fine below ~100k items ---- */
static Snap snapshot(void) {
    Snap s = { malloc((nitems + 1) * sizeof(Item)), nitems };
    memcpy(s.it, items, nitems * sizeof(Item));
    return s;
}
static void restore(Snap s) {
    items = grow(items, &capitems, s.n + 1, sizeof *items);
    memcpy(items, s.it, s.n * sizeof(Item));
    /* Undo is feedback, not a request to regenerate the annotation just removed. */
    nitems = s.n; free(s.it); revision++; dirty = 1;
}
static void checkpoint(void) { /* call before every mutation */
    changed();
    if (nundo == MAX_UNDO) { free(undo[0].it); memmove(undo, undo + 1, --nundo * sizeof *undo); }
    undo[nundo++] = snapshot();
    while (nredo) free(redo[--nredo].it);
}
static void do_undo(void) { if (nundo) { redo[nredo++] = snapshot(); restore(undo[--nundo]); } }
static void do_redo(void) { if (nredo) { undo[nundo++] = snapshot(); restore(redo[--nredo]); } }

/* ---- fonts: one baked atlas per font, Latin-1 range ---- */
static void font_bake(Font *f, const unsigned char *ttf, float px, unsigned char *bmp) {
    stbtt_fontinfo info; int a, d, g;
    stbtt_InitFont(&info, ttf, 0); stbtt_GetFontVMetrics(&info, &a, &d, &g);
    f->px = px; f->ascent = (float)a / (a - d);
    f->em = stbtt_ScaleForPixelHeight(&info, 1) / stbtt_ScaleForMappingEmToPixels(&info, 1);
    int r = stbtt_BakeFontBitmap(ttf, 0, px, bmp, 1024, 1024, 32, 224, f->cd);
    if (r <= 0) fprintf(stderr, "font atlas overflow (%d)\n", r);
}
static void font_upload(Font *f, const unsigned char *bmp) {
    glGenTextures(1, &f->tex); glBindTexture(GL_TEXTURE_2D, f->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 1024, 1024, 0, GL_ALPHA, GL_UNSIGNED_BYTE, bmp);
}
static unsigned char atlas[2][1024 * 1024];
static void fonts_bake(void) { font_bake(&ui_font, TTF_UI, 40, atlas[0]); font_bake(&canvas_font, TTF_CANVAS, 48, atlas[1]); }
static void fonts_upload(void) { font_upload(&ui_font, atlas[0]); font_upload(&canvas_font, atlas[1]); }
static int utf8_next(const char **s) { /* codepoint, or '?' outside the atlas; never runs past a NUL */
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++, n = c < 0xc0 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    if (n) c &= 0x3f >> n;
    while (n-- > 0 && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
    *s = (const char *)p;
    return c >= 32 && c < 256 ? c : '?';
}
static float text_run(const Font *f, float x, float y, float px, const char *s, int draw, float *h) {
    float sc = px / f->px, cx = 0, cy = 0, w = 0; int lines = 1;
    if (draw) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, f->tex); glBegin(GL_QUADS); }
    while (*s) {
        if (*s == '\n') { s++; w = fmaxf(w, cx); cx = 0; cy += px * 1.25f; lines++; continue; }
        int c = utf8_next(&s); float qx = 0, qy = 0; stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f->cd, 1024, 1024, c - 32, &qx, &qy, &q, 1);
        if (draw) {
            float bx = x + cx, by = y + cy + px * f->ascent;
            glTexCoord2f(q.s0, q.t0); glVertex2f(bx + q.x0 * sc, by + q.y0 * sc);
            glTexCoord2f(q.s1, q.t0); glVertex2f(bx + q.x1 * sc, by + q.y0 * sc);
            glTexCoord2f(q.s1, q.t1); glVertex2f(bx + q.x1 * sc, by + q.y1 * sc);
            glTexCoord2f(q.s0, q.t1); glVertex2f(bx + q.x0 * sc, by + q.y1 * sc);
        }
        cx += qx * sc;
    }
    if (draw) { glEnd(); glDisable(GL_TEXTURE_2D); }
    if (h) *h = lines * px * 1.25f;
    return fmaxf(w, cx);
}
static float text_w(const Font *f, float px, const char *s) { return text_run(f, 0, 0, px, s, 0, NULL); }
static void text_draw(const Font *f, float x, float y, float px, unsigned col, const char *s) { hex(col); text_run(f, x, y, px, s, 1, NULL); }

/* ---- geometry ---- */
static void pen_pt(const Item *it, int k, float *x, float *y) {
    *x = it->x0 + pool[it->p0 + 2 * k] * it->sx; *y = it->y0 + pool[it->p0 + 2 * k + 1] * it->sy;
}
static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    float t = l > 0 ? ((px - ax) * dx + (py - ay) * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypotf(ax + t * dx - px, ay + t * dy - py);
}
static void bbox(const Item *it, float *b) {
    b[0] = fminf(it->x0, it->x1); b[1] = fminf(it->y0, it->y1);
    b[2] = fmaxf(it->x0, it->x1); b[3] = fmaxf(it->y0, it->y1);
    for (int k = 0; is_pen(it->type) && k < it->np; k++) {
        float x, y; pen_pt(it, k, &x, &y);
        b[0] = fminf(b[0], x); b[1] = fminf(b[1], y); b[2] = fmaxf(b[2], x); b[3] = fmaxf(b[3], y);
    }
}
static int poly(const Item *it, float *p) { /* closed outline of a fillable shape, at most 64 points */
    float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2, rx = fabsf(it->x1 - it->x0) / 2, ry = fabsf(it->y1 - it->y0) / 2;
    switch (it->type) {
    case RECT: memcpy(p, (float[]){ it->x0, it->y0, it->x1, it->y0, it->x1, it->y1, it->x0, it->y1 }, 8 * sizeof *p); return 4;
    case DIAMOND: memcpy(p, (float[]){ cx, it->y0, it->x1, cy, cx, it->y1, it->x0, cy }, 8 * sizeof *p); return 4;
    case TRIANGLE: memcpy(p, (float[]){ cx, it->y0, it->x1, it->y1, it->x0, it->y1 }, 6 * sizeof *p); return 3;
    case STAR: /* a unit star spans x +-0.951 and y -1..0.809; stretch it to fill the box */
        for (int i = 0; i < 10; i++) {
            float a = -1.5708f + i * .62832f, r = i & 1 ? .4f : 1;
            p[2 * i] = cx + rx / .951f * r * cosf(a); p[2 * i + 1] = fminf(it->y0, it->y1) + ry * 2 / 1.809f * (1 + r * sinf(a));
        }
        return 10;
    default:
        for (int i = 0; i < 64; i++) { p[2 * i] = cx + rx * cosf(i * 0.09817f); p[2 * i + 1] = cy + ry * sinf(i * 0.09817f); }
        return 64;
    }
}
static int touches(const Item *it, float wx, float wy, int edges) { /* edges: unfilled shapes count only their outline */
    float tol = it->width / 2 + 6 * ui_s / zoom, b[4], ax, ay, bx, by, p[128];
    switch (it->type) {
    case PEN: case HIGHLIGHT:
        pen_pt(it, 0, &ax, &ay);
        if (it->np == 1 && hypotf(wx - ax, wy - ay) < tol) return 1;
        for (int k = 0; k + 1 < it->np; k++, ax = bx, ay = by) {
            pen_pt(it, k + 1, &bx, &by);
            if (seg_dist(wx, wy, ax, ay, bx, by) < tol) return 1;
        }
        return 0;
    case LINE: case ARROW:
        return seg_dist(wx, wy, it->x0, it->y0, it->x1, it->y1) < tol;
    case TEXT: break;
    default:
        if (!edges || it->fill >= 0) break;
        for (int n = poly(it, p), k = 0; k < n; k++)
            if (seg_dist(wx, wy, p[2 * k], p[2 * k + 1], p[2 * ((k + 1) % n)], p[2 * ((k + 1) % n) + 1]) < tol) return 1;
        return 0;
    }
    /* ponytail: selection hits shapes by bbox; the eraser uses outlines so it can reach inside a frame */
    bbox(it, b);
    return wx > b[0] - tol && wx < b[2] + tol && wy > b[1] - tol && wy < b[3] + tol;
}
static int hit(float wx, float wy) {
    for (int i = nitems - 1; i >= 0; i--) if (touches(&items[i], wx, wy, 0)) return i;
    return -1;
}
static int connector(int t) { return t == LINE || t == ARROW; }
static int link_target(float wx, float wy, int skip) { /* topmost item a connector end can link to, or -1 */
    for (int i = nitems - 1; i >= 0; i--) if (i != skip && !connector(items[i].type) && touches(&items[i], wx, wy, 0)) return i;
    return -1;
}
static float edge_t(const float *b, float dx, float dy) { /* fraction of (dx, dy) from the box center to its edge */
    float hw = (b[2] - b[0]) / 2, hh = (b[3] - b[1]) / 2;
    return fminf(dx ? hw / fabsf(dx) : 1e30f, dy ? hh / fabsf(dy) : 1e30f);
}
static void relink_one(Item *it) { /* a linked end sits just outside its item's box, on the line toward the other end */
    float a[4] = { it->x0, it->y0, it->x0, it->y0 }, b[4] = { it->x1, it->y1, it->x1, it->y1 };
    if (it->from) bbox(&items[it->from - 1], a);
    if (it->to) bbox(&items[it->to - 1], b);
    float ax = (a[0] + a[2]) / 2, ay = (a[1] + a[3]) / 2, bx = (b[0] + b[2]) / 2, by = (b[1] + b[3]) / 2;
    float dx = bx - ax, dy = by - ay, len = hypotf(dx, dy), ta = 0, tb = 0;
    if (len > 0 && it->from) ta = edge_t(a, dx, dy) + 8 / len;
    if (len > 0 && it->to) tb = edge_t(b, dx, dy) + 8 / len;
    if (ta + tb >= 1) ta = tb = 0; /* ponytail: overlapping boxes connect center to center */
    it->x0 = ax + dx * ta; it->y0 = ay + dy * ta; it->x1 = bx - dx * tb; it->y1 = by - dy * tb;
}
static void relink(void) { for (int i = 0; i < nitems; i++) if (items[i].from || items[i].to) relink_one(&items[i]); }
static void compact(const char *gone) { /* drop items with gone[i]; links follow the shifted indices or come free */
    int *at = malloc((nitems + 1) * sizeof *at), n = 0;
    for (int i = 0; i < nitems; i++) at[i] = gone[i] ? 0 : ++n;
    n = 0;
    for (int i = 0; i < nitems; i++) {
        if (gone[i]) continue;
        Item it = items[i];
        if (it.from) it.from = at[it.from - 1];
        if (it.to) it.to = at[it.to - 1];
        items[n++] = it;
    }
    nitems = n; free(at);
}
static void remove_item(int i) { char *gone = calloc(nitems + 1, 1); gone[i] = 1; compact(gone); free(gone); }
static void keep_links(Item *it, const Item *b) { /* a moved connector stays linked only to items moving with it */
    if (it->from && !b[it->from - 1].sel) it->from = 0;
    if (it->to && !b[it->to - 1].sel) it->to = 0;
}
static int nsel(void) { int n = 0; for (int i = 0; i < nitems; i++) n += items[i].sel; return n; }
static void select_none(void) { for (int i = 0; i < nitems; i++) items[i].sel = 0; }
static int sel_bbox(float *b) {
    int n = 0; b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f;
    for (int i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        float ib[4]; bbox(&items[i], ib); n++;
        b[0] = fminf(b[0], ib[0]); b[1] = fminf(b[1], ib[1]); b[2] = fmaxf(b[2], ib[2]); b[3] = fmaxf(b[3], ib[3]);
    }
    return n;
}
static int sel_frame(float *b) { /* selection box with room for stroke width and handles */
    int n = sel_bbox(b); float m = 6 * ui_s / zoom;
    for (int i = 0; i < nitems; i++) if (items[i].sel) m = fmaxf(m, items[i].width / 2 + 6 * ui_s / zoom);
    b[0] -= m; b[1] -= m; b[2] += m; b[3] += m;
    return n;
}

/* ---- text items: tpool + p0 is a NUL-terminated UTF-8 string ---- */
static float tpx(const Item *it) { return it->width * 7; }
static void measure(Item *it) {
    float h, w = text_run(&canvas_font, 0, 0, tpx(it), tpool + it->p0, 0, &h);
    it->x1 = it->x0 + fmaxf(w, tpx(it) * .3f); it->y1 = it->y0 + h;
}
static int caret; /* byte offset of the caret in the text being edited */
static void text_put(const char *b, int n) { /* insert at the caret; the edited text is the pool tail */
    tpool = grow(tpool, &captpool, ntpool + n + 1, 1);
    char *s = tpool + items[editing].p0;
    memmove(s + caret + n, s + caret, items[editing].np - caret); memcpy(s + caret, b, n);
    ntpool += n; tpool[ntpool] = 0; caret += n;
    items[editing].np += n; measure(&items[editing]); relink();
}
static int text_step(int at, int dir) { /* neighbouring codepoint boundary */
    const char *s = tpool + items[editing].p0; int np = items[editing].np;
    if (dir < 0 && at > 0) do at--; while (at && (s[at] & 0xc0) == 0x80);
    if (dir > 0 && at < np) do at++; while (at < np && (s[at] & 0xc0) == 0x80);
    return at;
}
static void text_cut(int a, int b) { /* remove bytes [a, b) and leave the caret at a */
    Item *it = &items[editing]; char *s = tpool + it->p0;
    memmove(s + a, s + b, it->np - b); it->np -= b - a; ntpool -= b - a;
    tpool[ntpool] = 0; caret = a; measure(it); relink();
}
static void text_del(void) { text_cut(text_step(caret, -1), caret); } /* backspace */
static void start_edit(int i) { /* re-append the bytes at the tail so older snapshots keep their copy */
    Item *it = &items[i]; int old = it->p0;
    tpool = grow(tpool, &captpool, ntpool + it->np + 2, 1);
    memcpy(tpool + ntpool, tpool + old, it->np);
    it->p0 = ntpool; ntpool += it->np; tpool[ntpool] = 0; editing = i; caret = it->np;
    editing_new = nundo && i >= undo[nundo - 1].n; measure(it);
}
static void end_edit(void) {
    if (editing < 0) return;
    if (items[editing].np) ntpool++;   /* commit the NUL */
    else if (editing_new) restore(undo[--nundo]); /* cancel a new empty item */
    else remove_item(editing); /* erasing existing text is an undoable deletion */
    editing = -1;
}

/* ---- transforms ---- */
static void xform(Item *it, const Item *b, float ax, float ay, float fx, float fy) { /* scale about (ax,ay) */
    *it = *b;
    it->x0 = ax + (b->x0 - ax) * fx; it->x1 = ax + (b->x1 - ax) * fx;
    it->y0 = ay + (b->y0 - ay) * fy; it->y1 = ay + (b->y1 - ay) * fy;
    it->sx = b->sx * fx; it->sy = b->sy * fy;
    if (it->type == TEXT) { it->width = fminf(64, fmaxf(1, b->width * sqrtf(fabsf(fx * fy)))); measure(it); }
}
static void delete_selected(void) {
    if (!nsel()) return;
    checkpoint(); char *gone = malloc(nitems + 1);
    for (int i = 0; i < nitems; i++) gone[i] = items[i].sel;
    compact(gone); free(gone);
}
static int *copy_at; static int capcopy_at; /* original index -> copy position + 1, while copying a selection */
static void copy_links(Item *c) { /* a copied connector keeps only links to items copied with it */
    if (c->from) c->from = copy_at[c->from - 1];
    if (c->to) c->to = copy_at[c->to - 1];
}
static void map_selection(void) {
    copy_at = grow(copy_at, &capcopy_at, nitems + 1, sizeof *copy_at);
    for (int i = 0, k = 0; i < nitems; i++) copy_at[i] = items[i].sel ? ++k : 0;
}
static void duplicate(void) {
    int n = nitems;
    if (!nsel()) return;
    checkpoint(); map_selection();
    for (int i = 0; i < n; i++) if (copy_at[i]) copy_at[i] += n; /* copies append in selection order */
    for (int i = 0; i < n; i++) {
        if (!items[i].sel) continue;
        Item c = items[i]; items[i].sel = 0; copy_links(&c);
        c.x0 += GRID; c.x1 += GRID; c.y0 += GRID; c.y1 += GRID; add_item(c);
    }
}
static float *clip_pts; static int nclip_pts, capclip_pts;
static char *clip_txt; static int nclip_txt, capclip_txt;
static float clip_x, clip_y; /* top left of the copied selection; bbox() cannot read clip_pts */
static void copy_sel(void) { /* pools belong to one tab, so the clipboard keeps its own copy of points and text */
    float b[4]; sel_bbox(b); clip_x = b[0]; clip_y = b[1];
    clip = realloc(clip, (nsel() + 1) * sizeof(Item)); nclip = nclip_pts = nclip_txt = 0; map_selection();
    for (int i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        Item c = items[i]; copy_links(&c); /* links now count clipboard positions */
        if (is_pen(c.type)) {
            clip_pts = grow(clip_pts, &capclip_pts, nclip_pts + 2 * c.np, sizeof *clip_pts);
            memcpy(clip_pts + nclip_pts, pool + c.p0, 2 * c.np * sizeof *pool); c.p0 = nclip_pts; nclip_pts += 2 * c.np;
        } else if (c.type == TEXT) {
            clip_txt = grow(clip_txt, &capclip_txt, nclip_txt + c.np + 1, 1);
            memcpy(clip_txt + nclip_txt, tpool + c.p0, c.np + 1); c.p0 = nclip_txt; nclip_txt += c.np + 1;
        }
        clip[nclip++] = c;
    }
}
static void paste(float wx, float wy) {
    if (!nclip) return;
    checkpoint(); select_none();
    float dx = snapf(wx) - clip_x, dy = snapf(wy) - clip_y; int at = nitems;
    for (int k = 0; k < nclip; k++) {
        Item c = clip[k]; c.sel = 1;
        if (c.from) c.from += at;
        if (c.to) c.to += at;
        c.x0 += dx; c.x1 += dx; c.y0 += dy; c.y1 += dy;
        if (is_pen(c.type)) { int at = c.p0; c.p0 = npool; for (int j = 0; j < c.np; j++) add_pt(clip_pts[at + 2 * j], clip_pts[at + 2 * j + 1]); }
        else if (c.type == TEXT) {
            tpool = grow(tpool, &captpool, ntpool + c.np + 2, 1);
            memcpy(tpool + ntpool, clip_txt + c.p0, c.np + 1); c.p0 = ntpool; ntpool += c.np + 1;
        }
        add_item(c);
    }
}
static void set_color(int c) {
    color = c;
    if (!nsel()) return;
    checkpoint(); for (int i = 0; i < nitems; i++) if (items[i].sel) items[i].color = c;
}
static void set_fill(int c) {
    fillc = c;
    if (!nsel()) return;
    checkpoint(); for (int i = 0; i < nitems; i++) if (items[i].sel && fillable(items[i].type)) items[i].fill = c;
}
static void reset_board(void) { /* undoable, so a misclick costs one Ctrl+Z */
    end_edit();
    if (nitems) { checkpoint(); nitems = 0; }
    panx = pany = 0; zoom = 1; drag = NONE;
}
static int erased; /* items removed by the current eraser drag; the first removal checkpoints */
static void erase_at(float wx, float wy) {
    for (int i = nitems - 1; i >= 0; i--) {
        if (!touches(&items[i], wx, wy, 1)) continue;
        if (!erased++) checkpoint();
        remove_item(i);
    }
}
static void erase_path(float ax, float ay, float bx, float by) { /* sample so fast drags do not skip thin items */
    int n = fminf(ceilf(hypotf(bx - ax, by - ay) * zoom / 4), 1000);
    for (int k = 1; k <= n; k++) erase_at(ax + (bx - ax) * k / n, ay + (by - ay) * k / n);
}
static void smooth_stroke(Item *it) { /* a finished stroke owns the pool tail, so rewriting it keeps snapshots valid */
    float *p = pool + it->p0;
    for (int pass = 0; pass < 3; pass++) {
        float px = p[0], py = p[1];
        for (int k = 1; k + 1 < it->np; k++) { /* 1-2-1 average; ends stay put */
            float x = p[2 * k], y = p[2 * k + 1];
            p[2 * k] = (px + 2 * x + p[2 * k + 2]) / 4; p[2 * k + 1] = (py + 2 * y + p[2 * k + 3]) / 4;
            px = x; py = y;
        }
    }
}
static void set_tool(int t) { tool = t; width = tool_width[t]; }
static int width_kind(void) { /* TEXT, HIGHLIGHT, or PEN for any other stroke: picks the size label and presets */
    int k = -1;
    if (editing >= 0) return TEXT;
    for (int i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        int t = items[i].type == TEXT || items[i].type == HIGHLIGHT ? items[i].type : PEN;
        if (k >= 0 && k != t) return PEN;
        k = t;
    }
    return k >= 0 ? k : tool == TEXT || tool == HIGHLIGHT ? tool : PEN;
}
static float shown_width(void) { /* the edited or first selected item, else the tool default */
    if (editing >= 0) return items[editing].width;
    for (int i = 0; i < nitems; i++) if (items[i].sel) return items[i].width;
    return width;
}
static void set_width(float w) {
    width = tool_width[tool] = fminf(fmaxf(w, 1), 64);
    if (!nsel()) return;
    checkpoint();
    for (int i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        items[i].width = width;
        if (items[i].type == TEXT) measure(&items[i]);
    }
    relink();
}

/* ---- drawing: quads + round joins, no glLineWidth (driver max width is unreliable) ---- */
static void fill(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}
static void seg(float ax, float ay, float bx, float by, float w) {
    float dx = bx - ax, dy = by - ay, l = hypotf(dx, dy);
    if (l < 1e-6f) return;
    float nx = -dy / l * w / 2, ny = dx / l * w / 2;
    glBegin(GL_QUADS);
    glVertex2f(ax + nx, ay + ny); glVertex2f(bx + nx, by + ny);
    glVertex2f(bx - nx, by - ny); glVertex2f(ax - nx, ay - ny);
    glEnd();
}
static void dot(float x, float y, float r) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x, y);
    for (int i = 0; i <= 12; i++) glVertex2f(x + cosf(i * 0.5236f) * r, y + sinf(i * 0.5236f) * r);
    glEnd();
}
static void rrect(float x, float y, float w, float h, float r) {
    fill(x + r, y, w - 2 * r, h); fill(x, y + r, w, h - 2 * r);
    dot(x + r, y + r, r); dot(x + w - r, y + r, r); dot(x + w - r, y + h - r, r); dot(x + r, y + h - r, r);
}
static void stroke(const float *xy, int n, int closed, float w) {
    for (int i = 0; i < n; i++) {
        dot(xy[2 * i], xy[2 * i + 1], w / 2);
        if (i + 1 < n) seg(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], w);
    }
    if (closed && n > 1) seg(xy[2 * n - 2], xy[2 * n - 1], xy[0], xy[1], w);
}
static void outline(const float *b, float m, float w) {
    float r[8] = { b[0] - m, b[1] - m, b[2] + m, b[1] - m, b[2] + m, b[3] + m, b[0] - m, b[3] + m };
    stroke(r, 4, 1, w);
}
static void draw_item(const Item *it) {
    float w = it->width, buf[128];
    hex(STROKE[it->color]);
    switch (it->type) {
    case PEN: case HIGHLIGHT:
        scratch = grow(scratch, &capscratch, 2 * it->np + 2, sizeof *scratch);
        for (int k = 0; k < it->np; k++) pen_pt(it, k, &scratch[2 * k], &scratch[2 * k + 1]);
        if (it->type == HIGHLIGHT) { /* stencil paints each pixel once, so overlapping quads do not darken */
            hexa(STROKE[it->color], .35f); glClear(GL_STENCIL_BUFFER_BIT); glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 0, 0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        }
        stroke(scratch, it->np, 0, w);
        glDisable(GL_STENCIL_TEST);
        break;
    case LINE: case ARROW: {
        float l[4] = { it->x0, it->y0, it->x1, it->y1 };
        stroke(l, 2, 0, w);
        if (it->type == ARROW) {
            float a = atan2f(it->y1 - it->y0, it->x1 - it->x0), h = fmaxf(12, w * 4);
            float head[6] = { it->x1 - h * cosf(a - .5f), it->y1 - h * sinf(a - .5f), it->x1, it->y1,
                              it->x1 - h * cosf(a + .5f), it->y1 - h * sinf(a + .5f) };
            stroke(head, 3, 0, w);
        }
        break;
    }
    case RECT: case ELLIPSE: case DIAMOND: case TRIANGLE: case STAR: {
        int n = poly(it, buf);
        if (it->fill >= 0) { /* fan from the vertex mean: inside every shape here, since all are star-convex about it */
            float mx = 0, my = 0;
            for (int i = 0; i < n; i++) { mx += buf[2 * i] / n; my += buf[2 * i + 1] / n; }
            hex(FILLC[it->fill]);
            glBegin(GL_TRIANGLE_FAN); glVertex2f(mx, my); for (int i = 0; i <= n; i++) glVertex2fv(buf + 2 * (i % n)); glEnd();
            hex(STROKE[it->color]);
        }
        stroke(buf, n, 1, w);
        break;
    }
    case TEXT:
        text_draw(&canvas_font, it->x0, it->y0, tpx(it), STROKE[it->color], tpool + it->p0);
        break;
    }
}
static void draw_grid(int ww) {
    float g = GRID;
    while (g * zoom < 14) g *= 5;
    float x0 = floorf((panel_w() - panx) / zoom / g) * g, x1 = (ww - panx) / zoom;
    float y0 = floorf(-pany / zoom / g) * g, y1 = (winh - pany) / zoom;
    hex(0xd9d9d9); glPointSize(2);
    glBegin(GL_POINTS);
    for (float x = x0; x <= x1; x += g) for (float y = y0; y <= y1; y += g) glVertex2f(x, y);
    glEnd();
}
static void draw_selection(void) {
    float b[4]; int n = sel_frame(b);
    if (!n) return;
    hex(ACCENT);
    if (n > 1) for (int i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        float ib[4]; bbox(&items[i], ib); outline(ib, items[i].width / 2 + 3 / zoom, 1 / zoom);
    }
    outline(b, 0, 1.5f / zoom);
    float cx[4] = { b[0], b[2], b[2], b[0] }, cy[4] = { b[1], b[1], b[3], b[3] }, h = HANDLE * ui_s / zoom, e = 1 / zoom;
    for (int k = 0; k < 4; k++) {
        hex(ACCENT); fill(cx[k] - h - e, cy[k] - h - e, 2 * (h + e), 2 * (h + e));
        hex(0xffffff); fill(cx[k] - h, cy[k] - h, 2 * h, 2 * h);
    }
}
static void draw_caret(void) {
    if (editing < 0) return;
    const Item *it = &items[editing]; char *s = tpool + it->p0, keep = s[caret];
    s[caret] = 0; /* measure only the text before the caret */
    const char *nl = strrchr(s, '\n'); float px = tpx(it); int lines = 1;
    for (const char *p = s; *p; p++) lines += *p == '\n';
    float x = it->x0 + text_w(&canvas_font, px, nl ? nl + 1 : s) + 1, y = it->y0 + (lines - 1) * px * 1.25f;
    s[caret] = keep;
    float c[4] = { x, y + px * .1f, x, y + px * 1.1f };
    hex(ACCENT); stroke(c, 2, 0, 1.5f / zoom);
}
static void draw_marquee(void) {
    if (drag != MARQUEE) return;
    float wx = (lastx - panx) / zoom, wy = (lasty - pany) / zoom;
    float b[4] = { fminf(pressx, wx), fminf(pressy, wy), fmaxf(pressx, wx), fmaxf(pressy, wy) };
    hexa(ACCENT, .08f); fill(b[0], b[1], b[2] - b[0], b[3] - b[1]);
    hex(ACCENT); outline(b, 0, 1 / zoom);
}
static void render_canvas(int ww, int decorated) {
    glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, ww, winh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(panx, pany, 0); glScalef(zoom, zoom, 1);
    if (decorated) draw_grid(ww);
    for (int i = 0; i < nitems; i++) draw_item(&items[i]);
    if (decorated) { draw_selection(); draw_caret(); draw_marquee(); }
}

/* ---- file: "PB04" nitems npool ntpool items[] pool[] tpool[] ---- */
static int write_board(FILE *f) {
    return fwrite("PB04", 4, 1, f) == 1 && fwrite(&nitems, 4, 1, f) == 1
        && fwrite(&npool, 4, 1, f) == 1 && fwrite(&ntpool, 4, 1, f) == 1
        && fwrite(items, sizeof(Item), nitems, f) == (size_t)nitems
        && fwrite(pool, sizeof(float), npool, f) == (size_t)npool
        && fwrite(tpool, 1, ntpool, f) == (size_t)ntpool && fflush(f) == 0;
}
static int save(void) {
    struct stat st;
    int exists = stat(path, &st) == 0;
    if (!exists && errno != ENOENT) { perror(path); return -1; }
    /* Resolve existing symlinks so atomic replacement still saves their target. */
    char *target = exists ? realpath(path, NULL) : strdup(path);
    if (!target) { perror(path); return -1; }
    const char *slash = strrchr(target, '/');
    size_t dirlen = slash ? (size_t)(slash - target + 1) : 0;
    char *tmp = malloc(dirlen + sizeof ".paintboard-XXXXXX");
    if (!tmp) { free(target); perror(path); return -1; }
    memcpy(tmp, target, dirlen); memcpy(tmp + dirlen, ".paintboard-XXXXXX", sizeof ".paintboard-XXXXXX");
    int fd = mkstemp(tmp), ok = 0, err = errno;
    if (fd >= 0) {
        FILE *f = fdopen(fd, "wb");
        if (f) {
            ok = (!exists || fchmod(fd, st.st_mode & 0777) == 0) && write_board(f) && fsync(fd) == 0;
            err = errno;
            if (fclose(f) != 0) { ok = 0; err = errno; }
            if (ok && rename(tmp, target) != 0) { ok = 0; err = errno; }
        } else { err = errno; close(fd); }
        if (!ok) unlink(tmp);
    }
    free(tmp); free(target);
    if (!ok) { errno = err ? err : EIO; perror(path); return -1; }
    dirty = 0; return 0;
}
typedef struct { int type, color; float x0, y0, x1, y1, width; int p0, np; } ItemV2; /* PB02 layout */
typedef struct { int type, color, fill, sel; float x0, y0, x1, y1, width, sx, sy; int p0, np; } ItemV3; /* PB03: Item without links */
static int load(void) { /* 0 ok (missing file = empty board), -1 corrupt, -2 open error */
    FILE *f = fopen(path, "rb"); char magic[4] = {0}; int n = 0, np = 0, nt = 0;
    if (!f) { if (errno == ENOENT) return 0; perror(path); return -2; }
    int got = fread(magic, 4, 1, f) == 1, v2 = got && !memcmp(magic, "PB02", 4), v3 = got && !memcmp(magic, "PB03", 4);
    int ok = (v2 || v3 || !memcmp(magic, "PB04", 4))
             && fread(&n, 4, 1, f) == 1 && fread(&np, 4, 1, f) == 1 && fread(&nt, 4, 1, f) == 1
             && n >= 0 && n < 1 << 20 && np >= 0 && np < 1 << 26 && nt >= 0 && nt < 1 << 26;
    if (ok) {
        items = grow(items, &capitems, n + 1, sizeof *items);
        pool = grow(pool, &cappool, np + 2, sizeof *pool);
        tpool = grow(tpool, &captpool, nt + 2, 1);
        if (v2) {
            ItemV2 *o = malloc((n + 1) * sizeof *o);
            ok = fread(o, sizeof *o, n, f) == (size_t)n;
            for (int i = 0; ok && i < n; i++)
                items[i] = (Item){ .type = o[i].type, .color = o[i].color, .fill = -1, .x0 = o[i].x0, .y0 = o[i].y0,
                                   .x1 = o[i].x1, .y1 = o[i].y1, .width = o[i].width, .sx = 1, .sy = 1, .p0 = o[i].p0, .np = o[i].np };
            free(o);
        } else if (v3) {
            ItemV3 *o = malloc((n + 1) * sizeof *o);
            ok = fread(o, sizeof *o, n, f) == (size_t)n;
            for (int i = 0; ok && i < n; i++) { items[i] = (Item){0}; memcpy(&items[i], &o[i], sizeof *o); } /* same leading fields */
            free(o);
        } else ok = fread(items, sizeof(Item), n, f) == (size_t)n;
        ok = ok && fread(pool, sizeof(float), np, f) == (size_t)np && fread(tpool, 1, nt, f) == (size_t)nt;
    }
    fclose(f);
    for (int i = 0; ok && i < nt; i++) ok = !tpool[i] || tpool[i] == '\n' || (unsigned char)tpool[i] >= 32;
    for (int i = 0; ok && i < n; i++) {
        Item *it = &items[i]; it->sel = 0;
        ok = it->type >= 0 && it->type < SELECT && it->color >= 0 && it->color < NCOLORS && it->fill >= -1 && it->fill < NCOLORS
             && it->width >= 1 && it->width <= 64 && isfinite(it->x0) && isfinite(it->y0) && isfinite(it->x1) && isfinite(it->y1)
             && isfinite(it->sx) && isfinite(it->sy) && it->p0 >= 0 && it->np >= 0
             && (is_pen(it->type) ? it->p0 <= np && it->np <= (np - it->p0) / 2
               : it->type == TEXT ? it->p0 < nt && it->np < nt - it->p0 && !tpool[it->p0 + it->np] : 1)
             && it->from >= 0 && it->from <= n && it->to >= 0 && it->to <= n && it->from != i + 1 && it->to != i + 1
             && (connector(it->type) || (!it->from && !it->to));
    }
    if (!ok) return -1;
    nitems = n; npool = np; ntpool = nt;
    for (int i = 0; i < n; i++) if (items[i].type == TEXT) measure(&items[i]); /* font metrics may differ from the saving build */
    relink();
    return 0;
}
static char *export_path(const char *board, const char *type) { /* type is ".png" or ".svg" */
    const char *name = strrchr(board, '/'); name = name ? name + 1 : board;
    const char *ext = strrchr(name, '.');
    size_t n = ext && ext != name ? (size_t)(ext - board) : strlen(board);
    /* A board named *.png must not be overwritten by its own export. */
    if (ext && !strcmp(ext, type)) n = strlen(board);
    char *out = malloc(n + 5);
    if (out) { memcpy(out, board, n); memcpy(out + n, type, 5); }
    return out;
}
static int write_png(FILE *f, const unsigned char *pixels, int w, int h) {
    int len;
    unsigned char *png = stbi_write_png_to_mem(pixels, w * 3, w, h, 3, &len);
    int ok = png && fwrite(png, 1, len, f) == (size_t)len && fflush(f) == 0;
    free(png); return ok;
}
static int export_png(void) { /* ponytail: exports the visible canvas area; pan/zoom to frame it */
    if (!path) { notify("Save the board first; the export goes next to it"); return -1; }
    int fw, fh, ww, wh; glfwGetFramebufferSize(win, &fw, &fh); glfwGetWindowSize(win, &ww, &wh);
    if (ww <= panel_w() || wh <= 0 || fw <= 0 || fh <= 0) { fprintf(stderr, "no visible canvas to export\n"); return -1; }
    int x = panel_w() * fw / ww, W = fw - x;
    winh = wh;
    glViewport(0, 0, fw, fh); render_canvas(ww, 0);
    unsigned char *px = malloc((size_t)W * fh * 3);
    char *out = export_path(path, ".png");
    if (!px || !out) { free(px); free(out); fprintf(stderr, "export: out of memory\n"); return -1; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(x, 0, W, fh, GL_RGB, GL_UNSIGNED_BYTE, px);
    stbi_flip_vertically_on_write(1);
    FILE *f = fopen(out, "wb");
    int ok = f && write_png(f, px, W, fh);
    if (f && fclose(f) != 0) ok = 0;
    notify(ok ? "exported %s" : "export failed: %s", out);
    free(px); free(out); return ok ? 0 : -1;
}

/* ---- offscreen: an EGL pbuffer draws the whole board with no window or display server ---- */
static EGLDisplay egl_dpy = EGL_NO_DISPLAY; static EGLConfig egl_cfg; static EGLContext egl_ctx = EGL_NO_CONTEXT;
static EGLSurface offscreen_begin(int w, int h) {
    EGLint n, attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                         EGL_BLUE_SIZE, 8, EGL_STENCIL_SIZE, 8, EGL_SAMPLES, 4, EGL_NONE };
    int first = egl_ctx == EGL_NO_CONTEXT;
    if (first) {
        /* Mesa's surfaceless platform needs no display server; other drivers fall back to the default display. */
        egl_dpy = eglGetPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, EGL_DEFAULT_DISPLAY, NULL);
        if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, NULL, NULL)) {
            egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, NULL, NULL)) return EGL_NO_SURFACE;
        }
        if (!eglChooseConfig(egl_dpy, attr, &egl_cfg, 1, &n) || !n) {
            attr[13] = 0; /* no multisampling */
            if (!eglChooseConfig(egl_dpy, attr, &egl_cfg, 1, &n) || !n) return EGL_NO_SURFACE;
        }
        if (!eglBindAPI(EGL_OPENGL_API) || (egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, NULL)) == EGL_NO_CONTEXT)
            return EGL_NO_SURFACE;
    }
    EGLSurface s = eglCreatePbufferSurface(egl_dpy, egl_cfg, (EGLint[]){ EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE });
    if (s == EGL_NO_SURFACE) return s;
    if (!eglMakeCurrent(egl_dpy, s, s, egl_ctx)) { eglDestroySurface(egl_dpy, s); return EGL_NO_SURFACE; }
    if (first) { fonts_upload(); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    return s;
}
static void board_bounds(float *b) { /* every item with a 40-unit margin; an empty board is 400 x 300 */
    float ib[4]; b[0] = b[1] = 0; b[2] = 400; b[3] = 300;
    for (int i = 0; i < nitems; i++) {
        bbox(&items[i], ib);
        if (!i) memcpy(b, ib, sizeof ib);
        else { b[0] = fminf(b[0], ib[0]); b[1] = fminf(b[1], ib[1]); b[2] = fmaxf(b[2], ib[2]); b[3] = fmaxf(b[3], ib[3]); }
    }
    b[0] -= 40; b[1] -= 40; b[2] += 40; b[3] += 40;
}
/* Every item, fitted with a margin, at zoom 1 unless a side would pass 4096 px. Returns bottom-up RGB, or NULL. */
static unsigned char *render_fitted(int *pw, int *ph) {
    float b[4]; board_bounds(b);
    float scale = fminf(1, 4096 / fmaxf(b[2] - b[0], b[3] - b[1]));
    int w = fmaxf(1, ceilf((b[2] - b[0]) * scale)), h = fmaxf(1, ceilf((b[3] - b[1]) * scale));
    EGLSurface s = offscreen_begin(w, h);
    unsigned char *px = s == EGL_NO_SURFACE ? NULL : malloc((size_t)w * h * 3);
    if (px) {
        float view[4] = { panx, pany, zoom, winh };
        zoom = scale; panx = -b[0] * scale; pany = -b[1] * scale; winh = h;
        glViewport(0, 0, w, h); render_canvas(w, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
        panx = view[0]; pany = view[1]; zoom = view[2]; winh = view[3];
    }
    if (s != EGL_NO_SURFACE) { eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(egl_dpy, s); }
    *pw = w; *ph = h; return px;
}
static int write_fitted(FILE *f) { /* 1 ok */
    int w, h; unsigned char *px = render_fitted(&w, &h);
    if (!px) { fprintf(stderr, "offscreen rendering is unavailable: no EGL OpenGL driver\n"); return 0; }
    stbi_flip_vertically_on_write(1); int ok = write_png(f, px, w, h); free(px); return ok;
}

/* ---- SVG: the whole board as vectors; needs no OpenGL ---- */
static void svg_points(FILE *f, const float *xy, int n) { for (int i = 0; i < n; i++) fprintf(f, "%s%.1f,%.1f", i ? " " : "", xy[2 * i], xy[2 * i + 1]); }
static int write_svg(FILE *f) { /* 1 ok */
    float b[4], buf[128]; board_bounds(b); int text = 0;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"%.1f %.1f %.1f %.1f\">\n",
            ceilf(b[2] - b[0]), ceilf(b[3] - b[1]), b[0], b[1], b[2] - b[0], b[3] - b[1]);
    for (int i = 0; i < nitems; i++) text |= items[i].type == TEXT;
    if (text) { /* embed the canvas font so text keeps its look and width */
        static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        fputs("<style>@font-face{font-family:Excalifont;src:url(data:font/ttf;base64,", f);
        for (size_t i = 0, n = sizeof TTF_CANVAS; i < n; i += 3) {
            unsigned v = TTF_CANVAS[i] << 16 | (i + 1 < n ? TTF_CANVAS[i + 1] << 8 : 0) | (i + 2 < n ? TTF_CANVAS[i + 2] : 0);
            fprintf(f, "%c%c%c%c", B64[v >> 18], B64[v >> 12 & 63], i + 1 < n ? B64[v >> 6 & 63] : '=', i + 2 < n ? B64[v & 63] : '=');
        }
        fputs(")}text{font-family:Excalifont,sans-serif;white-space:pre}</style>\n", f);
    }
    fprintf(f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"#ffffff\"/>\n", b[0], b[1], b[2] - b[0], b[3] - b[1]);
    for (int i = 0; i < nitems; i++) {
        const Item *it = &items[i]; unsigned c = STROKE[it->color];
        if (it->type == TEXT) { /* one element per line, so blank lines keep their height */
            float px = tpx(it), y = it->y0 + px * canvas_font.ascent;
            fprintf(f, "<g font-size=\"%.2f\" fill=\"#%06x\">", px * canvas_font.em, c);
            for (const char *s = tpool + it->p0; ; s++, y += px * 1.25f) {
                fprintf(f, "<text x=\"%.1f\" y=\"%.1f\">", it->x0, y);
                for (; *s && *s != '\n'; s++)
                    if (*s == '&') fputs("&amp;", f); else if (*s == '<') fputs("&lt;", f); else if (*s == '>') fputs("&gt;", f); else fputc(*s, f);
                fputs("</text>", f);
                if (!*s) break;
            }
            fputs("</g>\n", f);
            continue;
        }
        fprintf(f, "<g fill=\"none\" stroke=\"#%06x\" stroke-width=\"%g\" stroke-linecap=\"round\" stroke-linejoin=\"round\"%s>",
                c, it->width, it->type == HIGHLIGHT ? " opacity=\".35\"" : "");
        switch (it->type) {
        case PEN: case HIGHLIGHT:
            if (it->np == 1) { float x, y; pen_pt(it, 0, &x, &y); fprintf(f, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"%g\" fill=\"#%06x\" stroke=\"none\"/>", x, y, it->width / 2, c); break; }
            fputs("<polyline points=\"", f);
            for (int k = 0; k < it->np; k++) { float x, y; pen_pt(it, k, &x, &y); fprintf(f, "%s%.1f,%.1f", k ? " " : "", x, y); }
            fputs("\"/>", f);
            break;
        case LINE: case ARROW:
            fprintf(f, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\"/>", it->x0, it->y0, it->x1, it->y1);
            if (it->type == ARROW) { /* the same head as draw_item */
                float a = atan2f(it->y1 - it->y0, it->x1 - it->x0), h = fmaxf(12, it->width * 4);
                float head[6] = { it->x1 - h * cosf(a - .5f), it->y1 - h * sinf(a - .5f), it->x1, it->y1,
                                  it->x1 - h * cosf(a + .5f), it->y1 - h * sinf(a + .5f) };
                fputs("<polyline points=\"", f); svg_points(f, head, 3); fputs("\"/>", f);
            }
            break;
        default: {
            char fillattr[24] = "";
            if (it->fill >= 0) snprintf(fillattr, sizeof fillattr, " fill=\"#%06x\"", FILLC[it->fill]);
            if (it->type == ELLIPSE)
                fprintf(f, "<ellipse cx=\"%.1f\" cy=\"%.1f\" rx=\"%.1f\" ry=\"%.1f\"%s/>", (it->x0 + it->x1) / 2, (it->y0 + it->y1) / 2,
                        fabsf(it->x1 - it->x0) / 2, fabsf(it->y1 - it->y0) / 2, fillattr);
            else { fputs("<polygon points=\"", f); svg_points(f, buf, poly(it, buf)); fprintf(f, "\"%s/>", fillattr); }
        }
        }
        fputs("</g>\n", f);
    }
    return fputs("</svg>\n", f) >= 0 && fflush(f) == 0;
}
static int export_svg(void) {
    if (!path) { notify("Save the board first; the export goes next to it"); return -1; }
    char *out = export_path(path, ".svg"); FILE *f = out ? fopen(out, "w") : NULL;
    int ok = f && write_svg(f);
    if (f && fclose(f) != 0) ok = 0;
    notify(ok ? "exported %s" : "export failed: %s", out ? out : "out of memory");
    free(out); return ok ? 0 : -1;
}


/* ---- tabs: the active board lives in the globals; docs[cur] is stale until doc_park ---- */
typedef struct {
    float *pool; int npool, cappool; char *tpool; int ntpool, captpool; Item *items; int nitems, capitems;
    Snap undo[MAX_UNDO], redo[MAX_UNDO]; int nundo, nredo; char *path; float panx, pany, zoom; int dirty;
} Doc;
#define DOC_FIELDS X(pool) X(npool) X(cappool) X(tpool) X(ntpool) X(captpool) X(items) X(nitems) X(capitems) \
                   X(nundo) X(nredo) X(path) X(panx) X(pany) X(zoom) X(dirty)
static Doc *docs; static int ndocs, capdocs, cur;
static void doc_park(void) {
#define X(f) docs[cur].f = f;
    DOC_FIELDS
#undef X
    memcpy(docs[cur].undo, undo, sizeof undo); memcpy(docs[cur].redo, redo, sizeof redo);
}
static void doc_enter(int i) {
    cur = i;
#define X(f) f = docs[i].f;
    DOC_FIELDS
#undef X
    memcpy(undo, docs[i].undo, sizeof undo); memcpy(redo, docs[i].redo, sizeof redo);
    revision++; /* a reply computed for the previous tab now fails its revision check */
}
static void doc_leave(void) { end_edit(); drag = NONE; panning = 0; doc_park(); } /* nothing may point into the board */
static void doc_add(char *owned_path) { /* a new empty tab, made active */
    if (ndocs) doc_leave();
    docs = grow(docs, &capdocs, ndocs + 1, sizeof *docs);
    memset(&docs[ndocs], 0, sizeof *docs); docs[ndocs].path = owned_path; docs[ndocs].zoom = 1;
    doc_enter(ndocs++);
    items = grow(items, &capitems, 1, sizeof *items); pool = grow(pool, &cappool, 2, sizeof *pool); tpool = grow(tpool, &captpool, 2, 1);
}
static void doc_switch(int i) {
    if (i == cur || i < 0 || i >= ndocs) return;
    doc_leave(); doc_enter(i);
}
static void doc_remove(int i) { /* drop a tab unsaved; when it was the last one no tab is active */
    doc_leave();
    Doc *d = &docs[i];
    for (int k = 0; k < d->nundo; k++) free(d->undo[k].it);
    for (int k = 0; k < d->nredo; k++) free(d->redo[k].it);
    free(d->items); free(d->pool); free(d->tpool); free(d->path);
    memmove(docs + i, docs + i + 1, (ndocs - i - 1) * sizeof *docs);
    if (--ndocs) doc_enter(i < cur ? cur - 1 : cur < ndocs ? cur : ndocs - 1);
}
static int same_file(const char *a, const char *b) {
    struct stat sa, sb;
    if (!stat(a, &sa) && !stat(b, &sb)) return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    return !strcmp(a, b);
}
static int find_tab(const char *file) {
    doc_park();
    for (int i = 0; i < ndocs; i++) if (docs[i].path && same_file(docs[i].path, file)) return i;
    return -1;
}
static char *absolute(const char *p) {
    if (p[0] == '/') return strdup(p);
    char *cwd = getcwd(NULL, 0), *r = malloc(strlen(cwd) + strlen(p) + 2);
    sprintf(r, "%s/%s", cwd, p); free(cwd); return r;
}
static const char *base_name(const char *p) { return p ? strrchr(p, '/') + 1 : "Untitled"; }
static int open_path(const char *file) { /* absolute path; switches to its tab when it is already open. 0 ok */
    int i = find_tab(file), prev = cur;
    if (i >= 0) { doc_switch(i); return 0; }
    int reuse = !path && !nitems && !nundo && !nredo; /* an untouched Untitled tab takes the file */
    if (reuse) { end_edit(); path = strdup(file); } else doc_add(strdup(file));
    int r = load();
    if (!r) { dirty = 0; return 0; }
    if (r == -1) notify("%s: not a paintboard file", file); else notify("Cannot open %s: %s", file, strerror(errno));
    if (reuse) { free(path); path = NULL; } else { doc_remove(cur); doc_switch(prev); }
    return -1;
}
static int save_now(void) { /* 0 ok */
    if (!save()) return 0;
    notify("Cannot save %s: %s", path, strerror(errno)); return -1;
}
static int save_named_tabs(void) { /* an MCP session ends without questions; 0 when every save worked */
    int failed = 0;
    for (int i = 0; i < ndocs; i++) { doc_switch(i); end_edit(); if (path && save_now()) failed = 1; }
    return failed;
}

/* Closing a tab or the window: a board with unsaved changes asks first. */
enum { CLOSE_TAB = 1, QUIT };
static int pending, quit_tab, close_back = -1, discard, asking, quit_done, fd_then_proceed;
static void proceed(void) { /* carry the pending close as far as it goes without a question */
    while (pending) {
        if (pending == QUIT) doc_switch(quit_tab);
        end_edit();
        if (!discard && (path ? dirty : nitems)) { asking = 1; return; }
        discard = 0;
        if (pending == QUIT) { if (++quit_tab == ndocs) { pending = 0; quit_done = 1; } continue; }
        int closed = cur, back = close_back; pending = 0;
        doc_remove(closed);
        if (!ndocs) doc_add(NULL); else if (back >= 0) doc_switch(back > closed ? back - 1 : back);
    }
}
static void close_tab(int i) {
    if (pending || i < 0 || i >= ndocs) return;
    close_back = i == cur ? -1 : cur;
    doc_switch(i); pending = CLOSE_TAB; proceed();
}

/* ---- config files in $XDG_CONFIG_HOME/paintboard ---- */
static int config_path(char *out, size_t size, const char *name) {
    const char *root = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int n;
    if (root && *root == '/') n = snprintf(out, size, "%s/paintboard/%s", root, name);
    else if (home && *home == '/') n = snprintf(out, size, "%s/.config/paintboard/%s", home, name);
    else return 0;
    return n > 0 && (size_t)n < size;
}
static int config_write(const char *name, const char *text) { /* atomic replace; 1 ok */
    char filename[4096], temporary[4120];
    if (!config_path(filename, sizeof filename, name)) return 0;
    for (char *p = filename + 1; *p; p++) if (*p == '/') {
        *p = 0; int ok = !mkdir(filename, 0700) || errno == EEXIST; *p = '/';
        if (!ok) return 0;
    }
    snprintf(temporary, sizeof temporary, "%s.XXXXXX", filename);
    int fd = mkstemp(temporary); if (fd < 0) return 0;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); return 0; }
    int ok = fputs(text, f) >= 0 && fflush(f) == 0;
    if (fclose(f)) ok = 0;
    if (ok && rename(temporary, filename)) ok = 0;
    if (!ok) unlink(temporary);
    return ok;
}
/* session: the active tab's index among the named tabs, then one absolute path per line */
static void session_write(void) {
    char *text = NULL; size_t size; FILE *f = open_memstream(&text, &size); int active = 0, k = 0;
    if (!f) return;
    doc_park();
    for (int i = 0; i < ndocs; i++) if (docs[i].path && !strchr(docs[i].path, '\n')) { if (i == cur) active = k; k++; }
    fprintf(f, "%d\n", active);
    for (int i = 0; i < ndocs; i++) if (docs[i].path && !strchr(docs[i].path, '\n')) fprintf(f, "%s\n", docs[i].path);
    fclose(f);
    if (!config_write("session", text)) fprintf(stderr, "cannot save the session\n");
    free(text);
}
static void session_restore(void) {
    char name[4096], line[PATH_MAX + 2]; int active = 0, want = -1;
    FILE *f = config_path(name, sizeof name, "session") ? fopen(name, "r") : NULL;
    if (!f) return;
    if (fgets(line, sizeof line, f)) active = atoi(line);
    for (int k = 0; fgets(line, sizeof line, f); k++) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] != '/') continue;
        if (access(line, F_OK)) { notify("%s no longer exists", line); continue; }
        if (!open_path(line) && k == active) want = cur;
    }
    fclose(f);
    if (want >= 0) doc_switch(want);
}

/* ---- panel: immediate-mode; the same walk draws (ui_mode 0) or hit-tests a click (ui_mode 1) ---- */
static int inside(float x, float y, float w, float h, float px, float py) { return px >= x && px < x + w && py >= y && py < y + h; }
static int button(float x, float y, float w, float h, const char *name, const char *key, int active) {
    int hover = inside(x, y, w, h, ui_x, ui_y);
    if (ui_mode) return hover;
    ui_hover |= hover;
    hex(active ? ACCENT : 0xdee2e6); rrect(x - 1, y - 1, w + 2, h + 2, 7);
    hex(active ? 0xe0dfff : hover ? 0xf1f3f5 : 0xffffff); rrect(x, y, w, h, 6);
    float tx = key ? x + 12 : x + (w - text_w(&ui_font, 13, name)) / 2;
    text_draw(&ui_font, tx, y + (h - 13 * 1.25f) / 2, 13, INK, name);
    if (key) text_draw(&ui_font, x + w - 10 - text_w(&ui_font, 11, key), y + (h - 11 * 1.25f) / 2, 11, MUTED, key);
    return 0;
}
static int swatch(float x, float y, unsigned col, int active) {
    if (ui_mode) return inside(x, y, 24, 24, ui_x, ui_y);
    ui_hover |= inside(x, y, 24, 24, ui_x, ui_y);
    if (active) { hex(ACCENT); rrect(x - 4, y - 4, 32, 32, 9); hex(0xf8f9fa); rrect(x - 2, y - 2, 28, 28, 8); }
    else { hex(0xdee2e6); rrect(x - 1, y - 1, 26, 26, 7); }
    hex(col); rrect(x, y, 24, 24, 6);
    return 0;
}
static void section(float y, const char *s) { if (!ui_mode) text_draw(&ui_font, 12, y, 10, MUTED, s); }
static void fit(char *s, float px, float w) { /* cut UTF-8 text at the end until it fits w */
    while (*s && text_w(&ui_font, px, s) > w) {
        size_t k = strlen(s) - 1;
        while (k && (s[k] & 0xc0) == 0x80) k--;
        s[k] = 0;
    }
}
/* One-line UTF-8 text fields, shared by the custom agent and file dialogs. */
static int line_key(char *s, int *at, int key) { /* caret moves and deletes; 0 when the key is not for the field */
    int from = *at;
    if (key == GLFW_KEY_HOME) *at = 0;
    else if (key == GLFW_KEY_END) *at = strlen(s);
    else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_BACKSPACE) { if (*at) do --*at; while (*at && (s[*at] & 0xc0) == 0x80); }
    else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_DELETE) { if (s[*at]) do ++*at; while ((s[*at] & 0xc0) == 0x80); }
    else return 0;
    if (key == GLFW_KEY_BACKSPACE) memmove(s + *at, s + from, strlen(s + from) + 1);
    if (key == GLFW_KEY_DELETE) { memmove(s + from, s + *at, strlen(s + *at) + 1); *at = from; }
    return 1;
}
static void line_draw(float x, float y, float w, const char *s, int at, const char *placeholder, int selected) {
    hex(ACCENT); rrect(x - 1, y - 1, w + 2, 36, 5);
    hex(selected ? 0xe0dfff : 0xffffff); rrect(x, y, w, 34, 4);
    char visible[4096]; snprintf(visible, sizeof visible, "%s", s); visible[at] = 0;
    int start = 0;
    while (text_w(&ui_font, 13, visible + start) > w - 24 && start < at) {
        do start++; while ((visible[start] & 0xc0) == 0x80);
    }
    float caret = text_w(&ui_font, 13, visible + start);
    snprintf(visible, sizeof visible, "%s", s + start); fit(visible, 13, w - 20);
    text_draw(&ui_font, x + 8, y + 7, 13, *s ? INK : MUTED, *s ? visible : placeholder);
    hex(ACCENT); fill(x + 8 + caret, y + 6, 1, 20);
}
static void custom_open(void) {
    strcpy(custom_draft, custom_command); custom_draft_transport = custom_transport;
    custom_cursor = strlen(custom_draft); custom_select_all = 0; custom_dialog = 1;
}
static void custom_insert(const char *text, size_t size) {
    size_t length = custom_select_all ? 0 : strlen(custom_draft);
    if (length + size >= sizeof custom_draft) { strcpy(custom_error, "Command is too long"); return; }
    for (size_t i = 0; i < size; i++) if ((unsigned char)text[i] < 32) { strcpy(custom_error, "Enter a single-line command"); return; }
    if (custom_select_all) { *custom_draft = 0; custom_cursor = 0; custom_select_all = 0; }
    memmove(custom_draft + custom_cursor + size, custom_draft + custom_cursor, length - custom_cursor + 1);
    memcpy(custom_draft + custom_cursor, text, size); custom_cursor += size; *custom_error = 0;
}
static void custom_key(GLFWwindow *w, int key, int mods) {
    int control = mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER);
    if (key == GLFW_KEY_ESCAPE) { custom_dialog = 0; return; }
    if (key == GLFW_KEY_ENTER) { custom_save(); return; }
    if (control && key == GLFW_KEY_A) { custom_select_all = 1; return; }
    if (control && key == GLFW_KEY_C) { if (custom_select_all) glfwSetClipboardString(w, custom_draft); return; }
    if (control && key == GLFW_KEY_V) { const char *s = glfwGetClipboardString(w); if (s) custom_insert(s, strlen(s)); return; }
    if (key == GLFW_KEY_TAB) { custom_draft_transport = (custom_draft_transport + 1) % 3; return; }
    if (custom_select_all && (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE)) { *custom_draft = 0; custom_cursor = 0; }
    else if (!line_key(custom_draft, &custom_cursor, key)) return;
    custom_select_all = 0;
}
static void custom_ui(void) {
    int pw, ph; glfwGetWindowSize(win, &pw, &ph);
    float ww = pw / ui_s, wh = ph / ui_s;
    float w = fminf(680, ww - 24), x = (ww - w) / 2, y = fmaxf(12, (wh - 340) / 2);
    if (!ui_mode) {
        hexa(0x000000, .25f); fill(0, 0, ww, wh);
        hex(0xf8f9fa); rrect(x, y, w, 340, 12);
        text_draw(&ui_font, x + 20, y + 16, 20, INK, "Custom agent");
        text_draw(&ui_font, x + 20, y + 54, 12, MUTED, "Launch command (use the agent's non-interactive or ACP command)");
        line_draw(x + 20, y + 78, w - 40, custom_draft, custom_cursor, "e.g. my-agent acp", custom_select_all);
        const char *help = custom_draft_transport == 0
            ? "ACP v1: sends the canvas image and reads the streamed reply.\nThe agent must support images. Sign in with its CLI first."
            : "Return annotation JSON on stdout or write it to {output}.\nArguments: {image}, {prompt}, {prompt_file}, {schema}, {board}\nQuotes group arguments. Commands run without an implicit shell.";
        text_draw(&ui_font, x + 20, y + 170, 12, MUTED, help);
        text_draw(&ui_font, x + 20, y + 236, 12, *custom_error ? 0xe03131 : MUTED,
                  *custom_error ? custom_error : "Saved for future windows. Empty the command to remove it.");
    }
    if (button(x + 20, y + 126, w - 40, 30, custom_transport_labels[custom_draft_transport], ">", 0))
        custom_draft_transport = (custom_draft_transport + 1) % 3;
    if (button(x + w - 256, y + 284, 100, 32, "Cancel", NULL, 0)) custom_dialog = 0;
    if (button(x + w - 144, y + 284, 124, 32, "Save & use", NULL, 1)) custom_save();
}
/* ---- file dialog: drawn in-app so it needs no toolkit and works the same on every desktop ---- */
enum { FD_OPEN = 1, FD_SAVE };
static int fd_mode, fd_at, fd_replace, fd_scroll, fd_n, fd_last = -1;
static char fd_dir[PATH_MAX], fd_name[4096], fd_error[200], last_dir[PATH_MAX], **fd_entries; static double fd_click;
static int fd_order(const void *a, const void *b) { /* folders first, then by name */
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int dx = x[strlen(x) - 1] == '/', dy = y[strlen(y) - 1] == '/';
    return dx != dy ? dy - dx : strcmp(x, y);
}
static void fd_scan(void) {
    for (int i = 0; i < fd_n; i++) free(fd_entries[i]);
    fd_n = 0; fd_scroll = 0; fd_last = -1; *fd_error = 0;
    DIR *d = opendir(fd_dir); struct dirent *e; int cap = 0;
    if (!d) { snprintf(fd_error, sizeof fd_error, "%s", strerror(errno)); return; }
    while ((e = readdir(d))) {
        char full[PATH_MAX * 2]; struct stat st; size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;
        snprintf(full, sizeof full, "%s/%s", fd_dir, e->d_name);
        if (stat(full, &st)) continue;
        int folder = S_ISDIR(st.st_mode);
        if (!folder && !(S_ISREG(st.st_mode) && n > 3 && !strcmp(e->d_name + n - 3, ".pb"))) continue;
        fd_entries = grow(fd_entries, &cap, fd_n + 1, sizeof *fd_entries);
        fd_entries[fd_n] = malloc(n + 2); memcpy(fd_entries[fd_n], e->d_name, n); strcpy(fd_entries[fd_n++] + n, folder ? "/" : "");
    }
    closedir(d);
    qsort(fd_entries, fd_n, sizeof *fd_entries, fd_order);
}
static void fd_cd(const char *dir) { /* dir is absolute */
    snprintf(fd_dir, sizeof fd_dir, "%s", dir);
    for (size_t n = strlen(fd_dir); n > 1 && fd_dir[n - 1] == '/'; n--) fd_dir[n - 1] = 0;
    fd_scan();
}
static void fd_up(void) {
    char *slash = strrchr(fd_dir, '/');
    if (slash && slash != fd_dir) *slash = 0; else strcpy(fd_dir, "/");
    fd_scan();
}
static void fd_open(int mode) {
    const char *home = getenv("HOME"); char *cwd = getcwd(NULL, 0);
    end_edit(); drag = NONE;
    if (path) { snprintf(fd_dir, sizeof fd_dir, "%s", path); *strrchr(fd_dir, '/') = 0; if (!*fd_dir) strcpy(fd_dir, "/"); }
    else snprintf(fd_dir, sizeof fd_dir, "%s", *last_dir ? last_dir : home && *home == '/' ? home : cwd ? cwd : "/");
    free(cwd);
    snprintf(fd_name, sizeof fd_name, "%s", mode == FD_SAVE && path ? base_name(path) : "");
    fd_at = strlen(fd_name); fd_replace = 0; fd_mode = mode; fd_scan();
}
static void fd_cancel(void) {
    fd_mode = 0;
    if (fd_then_proceed) { fd_then_proceed = 0; pending = 0; }
}
static int fd_target(char *out, size_t size) { /* the name field resolved to an absolute path */
    const char *home = getenv("HOME"); int n;
    if (!*fd_name) return 0;
    if (fd_name[0] == '/') n = snprintf(out, size, "%s", fd_name);
    else if (fd_name[0] == '~' && (!fd_name[1] || fd_name[1] == '/') && home) n = snprintf(out, size, "%s%s", home, fd_name + 1);
    else n = snprintf(out, size, "%s/%s", strcmp(fd_dir, "/") ? fd_dir : "", fd_name);
    if (n <= 0 || (size_t)n >= size - 3) return 0;
    struct stat st;
    if (fd_mode == FD_SAVE && !strchr(strrchr(out, '/'), '.') && (stat(out, &st) || !S_ISDIR(st.st_mode))) strcat(out, ".pb");
    return 1;
}
static void fd_confirm(void) {
    char target[PATH_MAX + 8]; struct stat st;
    if (!fd_target(target, sizeof target)) { strcpy(fd_error, "Enter a file name"); return; }
    int exists = !stat(target, &st);
    if (exists && S_ISDIR(st.st_mode)) { fd_cd(target); *fd_name = 0; fd_at = 0; return; }
    if (fd_mode == FD_OPEN) {
        if (!exists) { snprintf(fd_error, sizeof fd_error, "%s does not exist", base_name(target)); return; }
        snprintf(last_dir, sizeof last_dir, "%s", fd_dir); fd_mode = 0; open_path(target); return;
    }
    int other = find_tab(target);
    if (other >= 0 && other != cur) { strcpy(fd_error, "That file is open in another tab"); return; }
    if (exists && other != cur && !fd_replace) { fd_replace = 1; return; }
    char *old = path; path = strdup(target);
    if (save()) { snprintf(fd_error, sizeof fd_error, "Cannot save: %s", strerror(errno)); free(path); path = old; return; }
    free(old); snprintf(last_dir, sizeof last_dir, "%s", fd_dir); notify("Saved %s", base_name(path));
    int go = fd_then_proceed; fd_then_proceed = 0; fd_mode = 0;
    if (go) proceed();
}
static void fd_pick(int i) { /* a click on a list row */
    const char *e = fd_entries[i]; size_t n = strlen(e);
    if (e[n - 1] == '/') {
        char dir[PATH_MAX * 2]; snprintf(dir, sizeof dir, "%s/%.*s", strcmp(fd_dir, "/") ? fd_dir : "", (int)n - 1, e);
        fd_cd(dir); return;
    }
    int twice = i == fd_last && glfwGetTime() - fd_click < .4;
    fd_last = i; fd_click = glfwGetTime();
    snprintf(fd_name, sizeof fd_name, "%s", e); fd_at = n; fd_replace = 0; *fd_error = 0;
    if (twice) fd_confirm();
}
static void fd_insert(const char *t, size_t n) {
    size_t length = strlen(fd_name);
    if (length + n >= sizeof fd_name) return;
    for (size_t i = 0; i < n; i++) if ((unsigned char)t[i] < 32) return;
    memmove(fd_name + fd_at + n, fd_name + fd_at, length - fd_at + 1); memcpy(fd_name + fd_at, t, n);
    fd_at += n; fd_replace = 0; *fd_error = 0;
}
static void fd_key(GLFWwindow *w, int key, int mods) {
    if (key == GLFW_KEY_ESCAPE) fd_cancel();
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) fd_confirm();
    else if ((mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) && key == GLFW_KEY_V) {
        const char *s = glfwGetClipboardString(w); if (s) fd_insert(s, strlen(s));
    } else if (line_key(fd_name, &fd_at, key)) { fd_replace = 0; *fd_error = 0; }
}
static void dialog_box(float h, float *x, float *y, float *w) { /* dims the window, returns the box in logical px */
    int pw, ph; glfwGetWindowSize(win, &pw, &ph);
    float ww = pw / ui_s, wh = ph / ui_s;
    *w = fminf(560, ww - 24); *x = (ww - *w) / 2; *y = fmaxf(12, (wh - h) / 2);
    if (!ui_mode) { hexa(0x000000, .25f); fill(0, 0, ww, wh); hex(0xf8f9fa); rrect(*x, *y, *w, h, 12); }
}
static void fd_ui(void) {
    int pw, ph; glfwGetWindowSize(win, &pw, &ph);
    float x, y, w, h = fminf(520, ph / ui_s - 24), row = 26; dialog_box(h, &x, &y, &w);
    float list_y = y + 88, list_h = h - 88 - 120; int rows = fmaxf(1, (list_h - 8) / row);
    fd_scroll = fmaxf(0, fminf(fd_scroll, fd_n - rows));
    if (!ui_mode) {
        text_draw(&ui_font, x + 20, y + 16, 20, INK, fd_mode == FD_OPEN ? "Open board" : "Save board as");
        const char *shown = fd_dir;
        while (shown[1] && text_w(&ui_font, 12, shown) > w - 120) shown++;
        text_draw(&ui_font, x + 20, y + 58, 12, MUTED, shown);
        hex(0xdee2e6); rrect(x + 19, list_y - 1, w - 38, list_h + 2, 7); hex(0xffffff); rrect(x + 20, list_y, w - 40, list_h, 6);
        if (!fd_n) text_draw(&ui_font, x + 32, list_y + 9, 13, MUTED, "No folders or boards here");
    }
    if (button(x + w - 88, y + 50, 68, 28, "Up", NULL, 0)) { fd_up(); return; }
    for (int k = 0; k < rows && fd_scroll + k < fd_n; k++) {
        int i = fd_scroll + k; float ry = list_y + 4 + k * row;
        int over = inside(x + 24, ry, w - 48, row, ui_x, ui_y);
        if (ui_mode) { if (over) { fd_pick(i); return; } continue; }
        const char *e = fd_entries[i]; int folder = e[strlen(e) - 1] == '/';
        if (!folder && !strcmp(e, fd_name)) { hex(0xe0dfff); rrect(x + 24, ry, w - 48, row, 4); }
        else if (over) { hex(0xf1f3f5); rrect(x + 24, ry, w - 48, row, 4); }
        ui_hover |= over;
        text_draw(&ui_font, x + 34, ry + 5, 13, folder ? ACCENT : INK, e);
    }
    if (!ui_mode) {
        text_draw(&ui_font, x + 20, y + h - 112, 12, MUTED, "Name");
        line_draw(x + 20, y + h - 92, w - 40, fd_name, fd_at, fd_mode == FD_OPEN ? "Pick a board or type a path" : "board.pb", 0);
        if (*fd_error) text_draw(&ui_font, x + 20, y + h - 40, 12, 0xe03131, fd_error);
        else if (fd_replace) text_draw(&ui_font, x + 20, y + h - 40, 12, MUTED, "The file exists.");
    }
    if (button(x + w - 232, y + h - 48, 100, 32, "Cancel", NULL, 0)) { fd_cancel(); return; }
    if (button(x + w - 124, y + h - 48, 104, 32, fd_mode == FD_OPEN ? "Open" : fd_replace ? "Replace" : "Save", NULL, 1)) fd_confirm();
}
static void cmd_save(void) { if (!path) fd_open(FD_SAVE); else if (!save_now()) notify("Saved %s", base_name(path)); }

/* ---- Save / Discard / Cancel for a closing board with unsaved changes ---- */
static void ask_answer(int answer) { /* 0 cancel, 1 discard, 2 save */
    asking = 0;
    if (!answer) pending = 0;
    else if (answer == 1) { discard = 1; proceed(); }
    else if (path) { if (save_now()) pending = 0; else proceed(); } /* a failed save keeps the board open */
    else { fd_then_proceed = 1; fd_open(FD_SAVE); }
}
static void ask_ui(void) {
    float x, y, w; dialog_box(170, &x, &y, &w); char title[300];
    if (!ui_mode) {
        snprintf(title, sizeof title, "Save changes to %s?", base_name(path)); fit(title, 20, w - 40);
        text_draw(&ui_font, x + 20, y + 16, 20, INK, title);
        text_draw(&ui_font, x + 20, y + 56, 13, MUTED, path ? "Your changes are lost if you discard them."
                                                            : "This Untitled board has no file. Its drawing is lost if you discard it.");
    }
    if (button(x + 20, y + 118, 100, 32, "Discard", NULL, 0)) { ask_answer(1); return; }
    if (button(x + w - 232, y + 118, 100, 32, "Cancel", NULL, 0)) { ask_answer(0); return; }
    if (button(x + w - 124, y + 118, 104, 32, path ? "Save" : "Save...", NULL, 1)) ask_answer(2);
}

/* ---- tab bar: a strip over the top of the canvas, the same draw / hit-test walk as the panel ---- */
#define TAB_H 32
static int ui_middle; /* the hit-test click is a middle click */
static void tabs_ui(int ww) {
    float x0 = PANEL_W, w = ww / ui_s - x0, tw = fminf(200, (w - 44) / ndocs);
    doc_park();
    if (!ui_mode) { hex(0xf1f3f5); fill(x0, 0, w, TAB_H); hex(0xdee2e6); fill(x0, TAB_H - 1, w, 1); }
    for (int i = 0; i < ndocs; i++) {
        float x = x0 + 4 + i * tw, cx = x + tw - 24;
        int over = inside(x, 4, tw - 2, TAB_H - 5, ui_x, ui_y), on_close = over && inside(cx, 8, 18, 18, ui_x, ui_y);
        if (ui_mode) {
            if (!over) continue;
            if (on_close || ui_middle) close_tab(i); else doc_switch(i);
            return;
        }
        ui_hover |= over;
        hex(i == cur ? 0xffffff : over ? 0xe9ecef : 0xf1f3f5); rrect(x, 4, tw - 2, TAB_H - 5, 6);
        if (i == cur) { hex(ACCENT); fill(x + 8, TAB_H - 3, tw - 18, 2); }
        char label[256]; snprintf(label, sizeof label, "%s", base_name(docs[i].path));
        fit(label, 12, tw - 40);
        text_draw(&ui_font, x + 10, 11, 12, i == cur ? INK : MUTED, label);
        if (on_close) { hex(0xdee2e6); rrect(cx, 8, 18, 18, 4); }
        if (docs[i].dirty && !on_close) { hex(MUTED); dot(cx + 9, 17, 3.5f); }
        else if (over || i == cur) text_draw(&ui_font, cx + 5, 9, 13, MUTED, "\xc3\x97");
    }
    if (button(x0 + 6 + ndocs * tw, 5, 26, 22, "+", NULL, 0)) doc_add(NULL);
}

static void ui(void) {
    float y = 12, vis = winh / ui_s; char buf[24];
    panel_scroll = fminf(fmaxf(panel_scroll, 0), fmaxf(0, panel_h - vis));
    ui_y += panel_scroll;
    if (!ui_mode) {
        glPushMatrix(); glTranslatef(0, -panel_scroll, 0);
        hex(0xf8f9fa); fill(0, panel_scroll, PANEL_W, vis); hex(0xe9ecef); fill(PANEL_W - 1, panel_scroll, 1, vis);
    }
    static const int order[NTOOLS] = { SELECT, ERASER, PEN, HIGHLIGHT, LINE, ARROW, RECT, ELLIPSE, DIAMOND, TRIANGLE, STAR, TEXT };
    for (int k = 0; k < NTOOLS; k++) if (button(8 + k % 2 * 80, y + k / 2 * 32, 72, 28, TOOL_NAME[order[k]], NULL, tool == order[k])) set_tool(order[k]);
    y += (NTOOLS + 1) / 2 * 32 + 4; section(y, "STROKE"); y += 18;
    for (int c = 0; c < NCOLORS; c++) if (swatch(24 + c % 4 * 32, y + c / 4 * 32, STROKE[c], c == color)) set_color(c);
    y += 70; section(y, "FILL"); y += 18;
    if (swatch(8, y, 0xffffff, fillc < 0)) set_fill(-1);
    if (!ui_mode) { hex(0xe03131); seg(14, y + 18, 26, y + 6, 2); }
    for (int c = 0; c < NCOLORS; c++) if (swatch(8 + (c + 1) % 5 * 32, y + (c + 1) / 5 * 32, FILLC[c], c == fillc)) set_fill(c);
    static const float PRESET[3][4] = { { 2, 3, 5, 8 }, { 1, 3, 6, 12 }, { 8, 16, 24, 32 } }; /* text, stroke, marker */
    static const char *PRESET_NAME[4] = { "S", "M", "L", "XL" };
    int kind = width_kind(); float shown = shown_width();
    y += 70; section(y, kind == TEXT ? "TEXT SIZE" : kind == HIGHLIGHT ? "MARKER WIDTH" : "STROKE WIDTH"); y += 18;
    if (button(8, y, 28, 28, "-", NULL, 0)) set_width(shown - 1);
    if (!ui_mode) {
        if (kind == TEXT) {
            float px = fminf(shown * 7, 26);
            text_draw(&canvas_font, 66 - text_w(&canvas_font, px, "Aa") / 2, y + 14 - px * .6f, px, STROKE[color], "Aa");
            snprintf(buf, sizeof buf, "%gpx", shown * 7);
        } else {
            if (kind == HIGHLIGHT) hexa(STROKE[color], .35f); else hex(STROKE[color]);
            seg(46, y + 14, 86, y + 14, fminf(shown, 24));
            snprintf(buf, sizeof buf, "%g", shown);
        }
        text_draw(&ui_font, 108 - text_w(&ui_font, 12, buf) / 2, y + 6, 12, INK, buf);
    }
    if (button(132, y, 28, 28, "+", NULL, 0)) set_width(shown + 1);
    y += 32;
    const float *preset = PRESET[kind == TEXT ? 0 : kind == HIGHLIGHT ? 2 : 1];
    for (int k = 0; k < 4; k++) if (button(8 + k * 39, y, 35, 26, PRESET_NAME[k], NULL, shown == preset[k])) set_width(preset[k]);
    y += 34;
    if (button(8, y, 152, 28, "Snap to grid", "G", snap)) snap ^= 1;
    y += 36;
    if (button(8, y, 72, 28, "Undo", NULL, 0)) do_undo();
    if (button(88, y, 72, 28, "Redo", NULL, 0)) do_redo();
    y += 32;
    if (button(8, y, 72, 28, "Delete", NULL, 0)) delete_selected();
    if (button(88, y, 72, 28, "Reset", NULL, 0)) reset_board();
    y += 36; section(y, "FILE"); y += 18;
    if (button(8, y, 72, 28, "New", NULL, 0)) doc_add(NULL);
    if (button(88, y, 72, 28, "Open", NULL, 0)) fd_open(FD_OPEN);
    y += 32;
    if (button(8, y, 72, 28, "Save", NULL, 0)) cmd_save();
    if (button(88, y, 72, 28, "Save As", NULL, 0)) fd_open(FD_SAVE);
    y += 32;
    if (button(8, y, 152, 28, "Export PNG", "Ctrl+E", 0)) export_png();
    y += 32;
    if (button(8, y, 152, 28, "Export SVG", "Ctrl+Shift+E", 0)) export_svg();
    y += 36;
    snprintf(buf, sizeof buf, "Zoom %d%%", (int)(zoom * 100 + .5f));
    if (button(8, y, 152, 28, buf, "0", 0)) { panx = pany = 0; zoom = 1; }
    y += 36;
    if (button(8, y, 152, 28, collaborating ? "Collaborate: On" : "Collaborate: Off", NULL, collaborating)) collaboration_toggle();
    y += 36; section(y, "AUTO REPLY"); y += 18;
    const char *label = selected_agent == -2 ? "Detecting..." : selected_agent < 0 ? "Chat only" : agent_labels[selected_agent];
    if (button(8, y, 92, 28, label, NULL, 0)) collaboration_select();
    if (button(104, y, 24, 28, "+", NULL, custom_dialog)) custom_open();
    if (button(132, y, 28, 28, agent_scanning ? "..." : "R", NULL, 0)) collaboration_discover();
    if (!ui_mode) {
        char status[64]; snprintf(status, sizeof status, "%s", collaboration_status);
        /* Keep status text inside the sidebar; full errors remain in get_status. */
        while (*status && text_w(&ui_font, 10, status) > 152) status[strlen(status) - 1] = 0;
        text_draw(&ui_font, 8, y + 34, 10, MUTED, status);
        if (panel_h > vis) { /* scroll thumb */
            hex(0xced4da); rrect(PANEL_W - 5, panel_scroll + panel_scroll / panel_h * vis, 3, vis / panel_h * vis, 1.5f);
        }
        glPopMatrix();
    }
    panel_h = y + 52;
    ui_y -= panel_scroll;
}
static void hint_bar(int ww) { /* one-line context help along the bottom of the canvas */
    static const char *tool_hint[NTOOLS] = {
        "Drag to draw", "Drag to draw a line", "Drag to draw an arrow", "Drag to draw a rectangle", "Drag to draw an ellipse",
        "Click to type, click text to edit it (or double-click it with Select)", "Drag to draw a diamond", "Drag to draw a triangle", "Drag to draw a star",
        "Drag to highlight", "Click or drag a box to select", "Drag over items to erase  ·  Ctrl+Shift+Del resets the board" };
    char s[200]; int n = nsel();
    if (*notice && glfwGetTime() < notice_until) snprintf(s, sizeof s, "%s", notice);
    else if (editing >= 0) snprintf(s, sizeof s, "Enter newline  ·  Esc done  ·  arrows, Home, End move  ·  [ ] text size  ·  Ctrl+V paste");
    else if (tool == SELECT && n) snprintf(s, sizeof s, "%d selected  ·  drag to move, corners resize  ·  Del  ·  Ctrl+D duplicate", n);
    else snprintf(s, sizeof s, "%s (%s)%s  ·  right drag pan  ·  wheel zoom", tool_hint[tool], TOOL_KEY[tool], snap ? "  ·  grid snap on" : "");
    float x = PANEL_W + 12, y = winh / ui_s - 30;
    fit(s, 11, ww / ui_s - x - 20);
    float w = text_w(&ui_font, 11, s) + 20;
    if (!*s) return;
    hexa(0xffffff, .85f); rrect(x, y, w, 22, 6);
    text_draw(&ui_font, x + 10, y + 4, 11, *notice && glfwGetTime() < notice_until ? INK : MUTED, s);
}

/* ---- input ---- */
static void on_button(GLFWwindow *w, int button, int action, int mods) {
    double sx, sy; glfwGetCursorPos(w, &sx, &sy);
    if (custom_dialog) {
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
            ui_mode = 1; ui_x = sx / ui_s; ui_y = sy / ui_s; custom_ui(); ui_mode = 0;
        }
        return;
    }
    if (asking || fd_mode) {
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
            ui_mode = 1; ui_x = sx / ui_s; ui_y = sy / ui_s;
            if (asking) ask_ui(); else fd_ui();
            ui_mode = 0;
        }
        return;
    }
    if (action == GLFW_PRESS && sx >= panel_w() && sy < TAB_H * ui_s && button != GLFW_MOUSE_BUTTON_RIGHT) {
        int ww; glfwGetWindowSize(w, &ww, &winh);
        ui_mode = 1; ui_x = sx / ui_s; ui_y = sy / ui_s; ui_middle = button == GLFW_MOUSE_BUTTON_MIDDLE;
        tabs_ui(ww); ui_mode = 0; return;
    }
    float wx = (sx - panx) / zoom, wy = (sy - pany) / zoom, b[4]; int shift = mods & GLFW_MOD_SHIFT;
    if (button != GLFW_MOUSE_BUTTON_LEFT) { panning = action == GLFW_PRESS; return; }
    if (action == GLFW_RELEASE) {
        if (drag == DRAW) {
            Item *it = &items[nitems - 1];
            /* the press point, since a linked start no longer sits there */
            if (!is_pen(it->type) && it->x1 == snapf(pressx) && it->y1 == snapf(pressy)) restore(undo[--nundo]); /* drop zero-size shape */
            else if (is_pen(it->type)) smooth_stroke(it);
            else if (connector(it->type)) {
                int h = link_target(wx, wy, nitems - 1);
                if (h >= 0 && h + 1 != it->from) { it->to = h + 1; relink_one(it); }
            }
        }
        for (int i = 0; drag == RESIZE && nsel() == 1 && i < nitems; i++) { /* a connector end dropped on an item links to it */
            Item *it = &items[i]; int h = link_target(wx, wy, i);
            if (!it->sel || !connector(it->type) || h < 0) continue;
            if (hypotf(wx - it->x0, wy - it->y0) < hypotf(wx - it->x1, wy - it->y1)) { if (h + 1 != it->to) it->from = h + 1; }
            else if (h + 1 != it->from) it->to = h + 1;
            relink_one(it);
        }
        if (drag == MARQUEE) {
            float m[4] = { fminf(pressx, wx), fminf(pressy, wy), fmaxf(pressx, wx), fmaxf(pressy, wy) };
            for (int i = 0; i < nitems; i++) {
                bbox(&items[i], b);
                if (b[0] >= m[0] && b[1] >= m[1] && b[2] <= m[2] && b[3] <= m[3]) items[i].sel = 1;
            }
        }
        drag = NONE;
        return;
    }
    int edited = editing >= 0 && items[editing].np ? editing : -1;
    end_edit();
    if (sx < panel_w()) {
        if (edited >= 0) { select_none(); items[edited].sel = 1; } /* panel styles then apply to the text just typed */
        ui_mode = 1; ui_x = sx / ui_s; ui_y = sy / ui_s; ui(); ui_mode = 0; return;
    }
    pressx = wx; pressy = wy; base = NULL;
    if (tool == SELECT) {
        static double last_click; static int last_hit = -1;
        int h = hit(wx, wy), twice = h >= 0 && h == last_hit && glfwGetTime() - last_click < .4;
        last_hit = h; last_click = glfwGetTime();
        if (twice && !shift && items[h].type == TEXT) { select_none(); checkpoint(); start_edit(h); last_hit = -1; return; }
        if (sel_frame(b)) {
            float cx[4] = { b[0], b[2], b[2], b[0] }, cy[4] = { b[1], b[1], b[3], b[3] };
            for (int k = 0; k < 4; k++) {
                float grab = HANDLE * ui_s + 3;
                if (fabsf(cx[k] * zoom + panx - sx) > grab || fabsf(cy[k] * zoom + pany - sy) > grab) continue;
                checkpoint(); base = undo[nundo - 1].it; sel_bbox(sbase); handle = k; drag = RESIZE;
                return;
            }
        }
        if (h >= 0) {
            if (shift) items[h].sel ^= 1;
            else if (!items[h].sel) { select_none(); items[h].sel = 1; }
            if (items[h].sel) drag = MOVE;
            return;
        }
        if (!shift && sel_bbox(b) && wx >= b[0] && wx <= b[2] && wy >= b[1] && wy <= b[3]) { drag = MOVE; return; }
        if (!shift) select_none();
        drag = MARQUEE;
        return;
    }
    select_none();
    if (tool == ERASER) { erased = 0; erase_at(wx, wy); drag = ERASE; return; }
    int h = tool == TEXT ? hit(wx, wy) : connector(tool) ? link_target(wx, wy, -1) : -1;
    if (!is_pen(tool)) { wx = snapf(wx); wy = snapf(wy); }
    checkpoint();
    Item it = { .type = tool, .color = color, .fill = fillable(tool) ? fillc : -1, .x0 = wx, .y0 = wy, .x1 = wx, .y1 = wy,
                .width = width, .sx = 1, .sy = 1, .p0 = tool == TEXT ? ntpool : npool };
    if (tool == TEXT) {
        if (h >= 0 && items[h].type == TEXT) start_edit(h);
        else { add_item(it); start_edit(nitems - 1); }
        return;
    }
    if (is_pen(tool)) { add_pt(0, 0); it.np = 1; }
    if (connector(tool)) it.from = h + 1; /* starting on an item links the start to it */
    add_item(it); drag = DRAW;
}
static void on_cursor(GLFWwindow *w, double sx, double sy) {
    float dx = sx - lastx, dy = sy - lasty; lastx = sx; lasty = sy;
    if (custom_dialog || fd_mode || asking) return;
    float wx = (sx - panx) / zoom, wy = (sy - pany) / zoom;
    if (panning) { panx += dx; pany += dy; }
    switch (drag) {
    case ERASE: erase_path((sx - dx - panx) / zoom, (sy - dy - pany) / zoom, wx, wy); break;
    case DRAW: {
        Item *it = &items[nitems - 1];
        /* ponytail: drop points closer than 2px; immediate mode is the ceiling, VBOs if boards get huge */
        if (is_pen(it->type)) {
            if (hypotf(wx - it->x0 - pool[npool - 2], wy - it->y0 - pool[npool - 1]) > 2 / zoom) { add_pt(wx - it->x0, wy - it->y0); it->np++; }
        } else { it->x1 = snapf(wx); it->y1 = snapf(wy); if (it->from) relink_one(it); }
        break;
    }
    case MOVE: {
        if (!base) { checkpoint(); base = undo[nundo - 1].it; sel_bbox(sbase); } /* no undo entry for a plain click */
        float mx = wx - pressx, my = wy - pressy;
        if (snap) { mx = snapf(sbase[0] + mx) - sbase[0]; my = snapf(sbase[1] + my) - sbase[1]; }
        for (int i = 0; i < nitems; i++) {
            if (!base[i].sel) continue;
            items[i] = base[i]; keep_links(&items[i], base);
            items[i].x0 += mx; items[i].x1 += mx; items[i].y0 += my; items[i].y1 += my;
        }
        relink();
        break;
    }
    case RESIZE: {
        int l = handle == 0 || handle == 3, t = handle < 2;
        float ax = sbase[l ? 2 : 0], ay = sbase[t ? 3 : 1], cx = sbase[l ? 0 : 2], cy = sbase[t ? 1 : 3];
        float nx = snapf(cx + wx - pressx), ny = snapf(cy + wy - pressy);
        float fx = fabsf(cx - ax) < 1e-3f ? 1 : (nx - ax) / (cx - ax), fy = fabsf(cy - ay) < 1e-3f ? 1 : (ny - ay) / (cy - ay);
        for (int i = 0; i < nitems; i++) if (base[i].sel) { xform(&items[i], &base[i], ax, ay, fx, fy); keep_links(&items[i], base); }
        relink();
        break;
    }
    }
}
static void zoom_at(float sx, float sy, float by) { /* keep the point under (sx, sy) fixed */
    float z = fminf(fmaxf(zoom * by, .05f), 50), f = z / zoom;
    panx = sx - (sx - panx) * f; pany = sy - (sy - pany) * f; zoom = z;
}
static void on_scroll(GLFWwindow *w, double dx, double dy) {
    if (fd_mode) { fd_scroll -= dy * 3; return; }
    if (custom_dialog || asking) return;
    double sx, sy; glfwGetCursorPos(w, &sx, &sy);
    if (sx < panel_w()) { panel_scroll -= dy * 40; return; }
    zoom_at(sx, sy, dy > 0 ? 1.1f : 1 / 1.1f);
}
static void on_char(GLFWwindow *w, unsigned c) {
    int suppressed = suppress_char; suppress_char = 0;
    if (suppressed && (c == '[' || c == ']')) return;
    if (asking || (!custom_dialog && !fd_mode && editing < 0) || c < 32) return;
    char b[4]; int n = 0;
    if (c < 0x80) b[n++] = c;
    else if (c < 0x800) { b[n++] = 0xc0 | c >> 6; b[n++] = 0x80 | (c & 0x3f); }
    else if (c < 0x10000) { b[n++] = 0xe0 | c >> 12; b[n++] = 0x80 | (c >> 6 & 0x3f); b[n++] = 0x80 | (c & 0x3f); }
    else { b[n++] = 0xf0 | c >> 18; b[n++] = 0x80 | (c >> 12 & 0x3f); b[n++] = 0x80 | (c >> 6 & 0x3f); b[n++] = 0x80 | (c & 0x3f); }
    if (custom_dialog) custom_insert(b, n); else if (fd_mode) fd_insert(b, n); else text_put(b, n);
}
static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    suppress_char = 0;
    if (action == GLFW_RELEASE) return;
    if (custom_dialog) { custom_key(w, key, mods); return; }
    if (asking) {
        if (key == GLFW_KEY_ESCAPE) ask_answer(0);
        else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) ask_answer(2);
        return;
    }
    if (fd_mode) { fd_key(w, key, mods); return; }
    int control = mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER);
    if (editing >= 0 && control && (key == GLFW_KEY_N || key == GLFW_KEY_O || key == GLFW_KEY_W || key == GLFW_KEY_Q || key == GLFW_KEY_TAB
        || key == GLFW_KEY_PAGE_UP || key == GLFW_KEY_PAGE_DOWN || (key >= GLFW_KEY_1 && key <= GLFW_KEY_9)
        || (key == GLFW_KEY_S && ((mods & GLFW_MOD_SHIFT) || !path))))
        end_edit(); /* tab and file commands finish the text first */
    if (editing >= 0) {
        if (control && (key == GLFW_KEY_S || key == GLFW_KEY_E)) {
            int i = items[editing].np ? editing : -1, at = caret;
            end_edit();
            if (key == GLFW_KEY_S) save_now(); else if (mods & GLFW_MOD_SHIFT) export_svg(); else export_png();
            if (i >= 0) { checkpoint(); start_edit(i); caret = at; }
        }
        else if ((mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) && key == GLFW_KEY_V) {
            const char *clip = glfwGetClipboardString(w); char *b = clip ? malloc(strlen(clip) + 1) : NULL; int n = 0;
            for (const char *p = clip; b && *p; p++) if ((unsigned char)*p >= 32 || *p == '\n') b[n++] = *p;
            if (n && items[editing].np + n <= 65536) text_put(b, n); /* same cap as MCP text */
            free(b);
        }
        else if (!(mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_ALT | GLFW_MOD_SHIFT))
                 && (key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET)) {
            Item *it = &items[editing];
            width = tool_width[TEXT] = it->width = fminf(64, fmaxf(1, it->width + (key == GLFW_KEY_LEFT_BRACKET ? -1 : 1)));
            measure(it); suppress_char = 1;
        }
        else if (key == GLFW_KEY_ESCAPE) end_edit();
        else if (key == GLFW_KEY_ENTER) text_put("\n", 1);
        else if (key == GLFW_KEY_BACKSPACE) text_del();
        else if (key == GLFW_KEY_DELETE) text_cut(caret, text_step(caret, 1));
        else if (key == GLFW_KEY_LEFT) caret = text_step(caret, -1);
        else if (key == GLFW_KEY_RIGHT) caret = text_step(caret, 1);
        else if (key == GLFW_KEY_HOME) while (caret && tpool[items[editing].p0 + caret - 1] != '\n') caret--;
        else if (key == GLFW_KEY_END) while (caret < items[editing].np && tpool[items[editing].p0 + caret] != '\n') caret++;
        return;
    }
    if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) return;
    drag = NONE; /* undo/delete mid-drag would leave a dangling item index */
    float wx = (lastx - panx) / zoom, wy = (lasty - pany) / zoom;
    if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
        switch (key) {
        case GLFW_KEY_Z: if (mods & GLFW_MOD_SHIFT) do_redo(); else do_undo(); break;
        case GLFW_KEY_Y: do_redo(); break;
        case GLFW_KEY_S: if (mods & GLFW_MOD_SHIFT) fd_open(FD_SAVE); else cmd_save(); break;
        case GLFW_KEY_E: if (mods & GLFW_MOD_SHIFT) export_svg(); else export_png(); break;
        case GLFW_KEY_N: doc_add(NULL); break;
        case GLFW_KEY_O: fd_open(FD_OPEN); break;
        case GLFW_KEY_W: close_tab(cur); break;
        case GLFW_KEY_Q: glfwSetWindowShouldClose(w, 1); break;
        case GLFW_KEY_TAB: doc_switch((cur + (mods & GLFW_MOD_SHIFT ? ndocs - 1 : 1)) % ndocs); break;
        case GLFW_KEY_PAGE_DOWN: doc_switch((cur + 1) % ndocs); break;
        case GLFW_KEY_PAGE_UP: doc_switch((cur + ndocs - 1) % ndocs); break;
        case GLFW_KEY_A: for (int i = 0; i < nitems; i++) items[i].sel = 1; set_tool(SELECT); break;
        case GLFW_KEY_D: duplicate(); break;
        case GLFW_KEY_C: copy_sel(); break;
        case GLFW_KEY_X: copy_sel(); delete_selected(); break;
        case GLFW_KEY_V: paste(wx, wy); break;
        case GLFW_KEY_DELETE: case GLFW_KEY_BACKSPACE: if (mods & GLFW_MOD_SHIFT) reset_board(); break;
        case GLFW_KEY_EQUAL: case GLFW_KEY_MINUS: {
            int ww; glfwGetWindowSize(w, &ww, &winh);
            zoom_at((panel_w() + ww) / 2, winh / 2.f, key == GLFW_KEY_EQUAL ? 1.25f : .8f); break;
        }
        default: if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) doc_switch(key - GLFW_KEY_1);
        }
        return;
    }
    switch (key) {
    case GLFW_KEY_P: set_tool(PEN); break;
    case GLFW_KEY_L: set_tool(LINE); break;
    case GLFW_KEY_A: set_tool(ARROW); break;
    case GLFW_KEY_R: set_tool(RECT); break;
    case GLFW_KEY_O: set_tool(ELLIPSE); break;
    case GLFW_KEY_T: set_tool(TEXT); break;
    case GLFW_KEY_V: set_tool(SELECT); break;
    case GLFW_KEY_D: set_tool(DIAMOND); break;
    case GLFW_KEY_I: set_tool(TRIANGLE); break;
    case GLFW_KEY_S: set_tool(STAR); break;
    case GLFW_KEY_H: set_tool(HIGHLIGHT); break;
    case GLFW_KEY_E: set_tool(ERASER); break;
    case GLFW_KEY_G: snap ^= 1; break;
    case GLFW_KEY_ESCAPE: select_none(); break;
    case GLFW_KEY_0: panx = pany = 0; zoom = 1; break;
    case GLFW_KEY_DELETE: case GLFW_KEY_BACKSPACE: delete_selected(); break;
    case GLFW_KEY_LEFT_BRACKET: case GLFW_KEY_MINUS: set_width(shown_width() - 1); break;
    case GLFW_KEY_RIGHT_BRACKET: case GLFW_KEY_EQUAL: set_width(shown_width() + 1); break;
    default: if (key >= GLFW_KEY_1 && key <= GLFW_KEY_8) set_color(key - GLFW_KEY_1);
    }
}

static int selftest(void) {
    char dir[] = "/tmp/paintboard-test-XXXXXX", file[128];
    assert(mkdtemp(dir)); snprintf(file, sizeof file, "%s/board.pb", dir);
    fonts_bake(); doc_add(NULL); path = file;
    assert(ui_font.cd['A' - 32].xadvance > 0 && canvas_font.cd['A' - 32].xadvance > 0);
    Item r = { .type = RECT, .color = 1, .fill = 2, .x0 = 10, .y0 = 10, .x1 = 50, .y1 = 40, .width = 3, .sx = 1, .sy = 1 };
    Item p = { .type = PEN, .fill = -1, .x0 = 100, .y0 = 100, .x1 = 100, .y1 = 100, .width = 3, .sx = 1, .sy = 1, .np = 2 };
    Item t = { .type = TEXT, .color = 2, .fill = -1, .x0 = 200, .y0 = 200, .width = 3, .sx = 1, .sy = 1 };
    checkpoint(); add_item(r);
    assert(hit(30, 20) == 0 && hit(200, 200) == -1);
    checkpoint(); add_pt(0, 0); add_pt(50, 0); add_item(p);
    assert(hit(125, 101) == 1 && hit(125, 110) == -1);
    checkpoint(); t.p0 = ntpool; add_item(t); start_edit(2); text_put("h\xc3\xa9", 3); end_edit();
    assert(nitems == 3 && !strcmp(tpool + items[2].p0, "h\xc3\xa9") && items[2].x1 > 200 && hit(202, 205) == 2);
    checkpoint(); start_edit(2); text_del(); end_edit();
    assert(!strcmp(tpool + items[2].p0, "h") && items[2].p0 == 4 && ntpool == 6);
    do_undo(); assert(!strcmp(tpool + items[2].p0, "h\xc3\xa9"));
    do_undo(); assert(nitems == 2);
    do_redo(); do_redo(); assert(nitems == 3 && !strcmp(tpool + items[2].p0, "h"));
    Item s; xform(&s, &items[0], 10, 10, 2, 2); assert(s.x1 == 90 && s.y1 == 70);
    float x, y; items[1].sx = 2; pen_pt(&items[1], 1, &x, &y); assert(x == 200 && y == 100); items[1].sx = 1;
    items[0].sel = items[1].sel = 1; duplicate();
    assert(nitems == 5 && items[3].x0 == 10 + GRID && items[3].sel && !items[0].sel);
    copy_sel(); delete_selected(); assert(nitems == 3 && nclip == 2);
    paste(300, 300); assert(nitems == 5 && items[3].x0 == 300 && items[4].x0 == 390);
    delete_selected(); assert(nitems == 3);
    assert(save() == 0); nitems = npool = ntpool = 0;
    assert(load() == 0 && nitems == 3 && npool == 8 && ntpool == 6 && !strcmp(tpool + items[2].p0, "h") && hit(125, 101) == 1);
    /* A failed write must leave the last successfully saved board intact. */
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit = {1, 1}; assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        items[0].color = 7; _exit(save() == -1 ? 0 : 1);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(load() == 0 && items[0].color == 1);
    char bad[160]; snprintf(bad, sizeof bad, "%s/child", file); path = bad;
    assert(load() == -2); /* ENOTDIR is not a new board */
    snprintf(bad, sizeof bad, "%s/missing.pb", dir); assert(load() == 0); path = file;
    char *out = export_path("/tmp/project.v1/notes", ".png"); assert(!strcmp(out, "/tmp/project.v1/notes.png")); free(out);
    out = export_path("/tmp/.notes", ".png"); assert(!strcmp(out, "/tmp/.notes.png")); free(out);
    out = export_path("board.png", ".png"); assert(!strcmp(out, "board.png.png")); free(out);
    char longpath[1024]; memset(longpath, 'a', 1020); memcpy(longpath + 1020, ".pb", 4);
    out = export_path(longpath, ".png"); assert(strlen(out) == 1024 && !strcmp(out + 1020, ".png")); free(out);
    FILE *full = fopen("/dev/full", "wb"); unsigned char pixel[3] = {255, 255, 255};
    assert(full && !write_png(full, pixel, 1, 1)); fclose(full);
    checkpoint(); start_edit(2); text_del(); end_edit(); assert(nitems == 2);
    unsigned long before_undo = human_revision;
    do_undo(); assert(nitems == 3 && !strcmp(tpool + items[2].p0, "h") && human_revision == before_undo);
    int before = nundo;
    checkpoint(); t.p0 = ntpool; add_item(t); start_edit(3); end_edit();
    assert(nitems == 3 && nundo == before);
    checkpoint(); start_edit(2);
    float oldwidth = items[2].width;
    on_key(NULL, GLFW_KEY_RIGHT_BRACKET, 0, GLFW_PRESS, 0); on_char(NULL, ']');
    assert(items[2].width == oldwidth + 1 && !strcmp(tpool + items[2].p0, "h"));
    on_key(NULL, GLFW_KEY_S, 0, GLFW_PRESS, GLFW_MOD_CONTROL);
    assert(editing == 2); text_put("!", 1); end_edit();
    do_undo(); assert(!strcmp(tpool + items[2].p0, "h"));
    assert(load() == 0 && !strcmp(tpool + items[2].p0, "h") && items[2].width == oldwidth + 1);
    /* Tabs: each board keeps its own items, pools, and history; a paste carries its pool data across. */
    char other[160], cfg[160], line[200]; snprintf(other, sizeof other, "%s/other.pb", dir);
    setenv("XDG_CONFIG_HOME", dir, 1);
    select_none(); items[1].sel = items[2].sel = 1; copy_sel(); select_none();
    int first = nitems, first_undo = nundo;
    doc_add(NULL); assert(ndocs == 2 && cur == 1 && !path && !nitems && !nundo && !dirty);
    paste(0, 0); assert(nitems == 2 && npool == 4 && ntpool == 2 && !strcmp(tpool + items[1].p0, "h") && dirty);
    float px, py; pen_pt(&items[0], 1, &px, &py); assert(items[0].x0 == 0 && items[0].y0 == 0 && px == 50 && items[1].x0 == 100);
    doc_switch(0); assert(nitems == first && nundo == first_undo && path == file);
    doc_switch(1); path = strdup(other); assert(save() == 0 && !dirty);
    assert(open_path(file) == 0 && cur == 0 && ndocs == 2 && open_path(other) == 0 && cur == 1);
    session_write(); snprintf(cfg, sizeof cfg, "%s/paintboard/session", dir);
    FILE *sf = fopen(cfg, "r"); assert(sf && fgets(line, sizeof line, sf) && atoi(line) == 1);
    assert(fgets(line, sizeof line, sf) && !strncmp(line, file, strlen(file)) && fgets(line, sizeof line, sf) && !strncmp(line, other, strlen(other)));
    fclose(sf);
    checkpoint(); items[0].color = 3; close_tab(1); assert(asking && ndocs == 2); /* unsaved changes ask first */
    ask_answer(2); assert(!asking && ndocs == 1 && cur == 0 && path == file); /* Save writes the file, then closes */
    assert(open_path(other) == 0 && cur == 1 && items[0].color == 3);
    doc_add(NULL); paste(0, 0); close_tab(2); assert(asking && ndocs == 3); /* Untitled with items asks */
    ask_answer(1); assert(!asking && ndocs == 2 && cur == 1);
    assert(remove(other) == 0 && remove(cfg) == 0);
    snprintf(cfg, sizeof cfg, "%s/paintboard", dir); assert(rmdir(cfg) == 0);
    assert(remove(file) == 0 && rmdir(dir) == 0); puts("ok");
    return 0;
}

static void usage(FILE *f) {
    fprintf(f,
        "usage: paintboard [FILE...]\n"
        "       paintboard --mcp [--headless] [FILE]\n"
        "       paintboard --render FILE [OUT.png|OUT.svg]\n"
        "  Each FILE opens in a tab; a missing FILE starts an empty board. With no FILE the\n"
        "  tabs of the last session come back. --mcp uses board.pb when FILE is omitted.\n\n"
        "  --mcp       expose this canvas to an MCP client over stdin/stdout\n"
        "  --headless  run MCP without a window; images show every item, fitted (needs an EGL driver)\n"
        "  --render    write every item on the board, fitted, with no window: a PNG (needs an EGL driver) or an SVG\n\n"
        "tools:  P pen  H marker  L line  A arrow  R rect  O ellipse  D diamond  I triangle  S star\n"
        "        T text  V select  E eraser\n"
        "select: click, shift+click, drag a box; drag to move, corner handles to resize\n"
        "link:   start or end a line or arrow on an item, or drag its end onto one; the end then follows the item\n"
        "edit:   Del delete  Ctrl+Z/Y undo/redo  Ctrl+A all  Ctrl+D duplicate  Ctrl+C/X/V copy/cut/paste\n"
        "        Ctrl+Shift+Del reset the board (undoable)\n"
        "style:  1-8 stroke color  [ ] width / text size  G snap to grid\n"
        "text:   click to type, Enter newline, Esc done, arrows/Home/End move, Ctrl+V paste;\n"
        "        click existing text with T, or double-click it with V, to edit\n"
        "view:   right/middle drag pan  wheel zoom  Ctrl+= / Ctrl+- zoom  0 reset\n"
        "file:   Ctrl+N new  Ctrl+O open  Ctrl+S save  Ctrl+Shift+S save as  Ctrl+E export png  Ctrl+Shift+E export svg\n"
        "tabs:   Ctrl+W close  Ctrl+Tab / Ctrl+PgDn next  Ctrl+Shift+Tab / Ctrl+PgUp previous  Ctrl+1-9 go to  Ctrl+Q quit\n"
        "env:    PAINTBOARD_UI_SCALE=1.5 scales the panel and handles on top of the monitor DPI\n");
}

#include "mcp.c"
#include "collaboration.c"

int main(int argc, char **argv) {
    const char *files[argc]; int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mcp")) { mcp_mode = 1; continue; }
        if (!strcmp(argv[i], "--headless")) { headless = 1; continue; }
        if (!strcmp(argv[i], "--render")) { render_mode = 1; continue; }
        if (!strcmp(argv[i], "--test")) return selftest();
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        if (!strcmp(argv[i], "--version")) { puts("paintboard " VERSION); return 0; }
        if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(stderr); return 1; }
        files[nfiles++] = argv[i];
    }
    if (headless && !mcp_mode) { fprintf(stderr, "--headless requires --mcp\n"); return 1; }
    if (mcp_mode && nfiles > 1) { fprintf(stderr, "--mcp takes one FILE\n"); return 1; }
    if (render_mode) { /* paintboard --render FILE [OUT.png|OUT.svg]: the whole board, no window */
        if (mcp_mode || nfiles < 1 || nfiles > 2) { fprintf(stderr, "usage: paintboard --render FILE [OUT.png|OUT.svg]\n"); return 1; }
        fonts_bake(); doc_add(absolute(files[0]));
        struct stat st; int loaded = stat(path, &st) ? -2 : load();
        if (loaded) { fprintf(stderr, loaded == -1 ? "%s: not a paintboard file\n" : "%s: cannot open\n", path); return 1; }
        char *out = nfiles > 1 ? strdup(files[1]) : export_path(path, ".png"); FILE *f = fopen(out, "wb");
        size_t len = strlen(out); int svg = len > 4 && !strcasecmp(out + len - 4, ".svg");
        int ok = f && (svg ? write_svg(f) : write_fitted(f));

        if (f && fclose(f)) ok = 0;
        if (ok) puts(out); else { if (f) unlink(out); fprintf(stderr, "cannot render %s\n", out); }
        free(out); return !ok;
    }
    fonts_bake();
    collaborating = mcp_mode; /* Explicit stdio sessions retain their existing opt-in. */
    if (collaborating) strcpy(collaboration_status, "Agent tools connected");
    if (mcp_mode) { /* an MCP client needs its board, so a bad file stops startup */
        doc_add(absolute(nfiles ? files[0] : "board.pb"));
        int loaded = load();
        if (loaded) { if (loaded == -1) fprintf(stderr, "%s: not a paintboard file\n", path); return 1; }
    }
    if (headless) {
        if (mcp_start()) return 1;
        char *line;
        while (!mcp_output_failed && (line = mcp_read_line())) { mcp_dispatch(line); free(line); }
        int failed = save_named_tabs(); cJSON_Delete(mcp_spec); return failed || mcp_output_failed ? 1 : 0;
    }
    if (!mcp_mode) usage(stdout);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_SAMPLES, 4);
    /* Wayland compositors and X11 taskbars match this against the desktop entry to find the icon. */
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, APP_ID);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, APP_ID);
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, APP_ID);
    const char *env_scale = getenv("PAINTBOARD_UI_SCALE");
    if (env_scale) ui_user_scale = fminf(fmaxf(atof(env_scale), .5f), 4);
    /* Open at 85% of the work area so the window fits laptops and does not dwarf small monitors. */
    int wx, wy, ww0 = 1280, wh0 = 800; GLFWmonitor *mon = glfwGetPrimaryMonitor();
    if (mon) { glfwGetMonitorWorkarea(mon, &wx, &wy, &ww0, &wh0); ww0 = ww0 * .85f; wh0 = wh0 * .85f; }
    if (!(win = glfwCreateWindow(ww0 > 400 ? ww0 : 400, wh0 > 300 ? wh0 : 300, "paintboard", NULL, NULL))) return 1;
    glfwSetWindowSizeLimits(win, 400, 300, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(win); glfwSwapInterval(1);
    glfwSetMouseButtonCallback(win, on_button); glfwSetCursorPosCallback(win, on_cursor);
    glfwSetScrollCallback(win, on_scroll); glfwSetKeyCallback(win, on_key); glfwSetCharCallback(win, on_char);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_POINT_SMOOTH);
    fonts_upload();
    GLFWcursor *arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR), *cross = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR),
               *ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR), *hand = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
    if (!mcp_mode) { /* after the window exists, so open errors show in the hint bar */
        doc_add(NULL);
        for (int i = 0; i < nfiles; i++) { char *p = absolute(files[i]); open_path(p); free(p); }
        if (!nfiles) session_restore();
    }
    if (mcp_mode && mcp_start()) { glfwTerminate(); return 1; }
    collaboration_start();
    float dpi_scale = monitor_dpi_scale();
    char shown_title[PATH_MAX + 32] = "";
    while (!quit_done) {
        if (glfwWindowShouldClose(win)) {
            glfwSetWindowShouldClose(win, 0);
            if (!asking) {
                custom_dialog = 0;
                if (fd_mode) fd_cancel();
                if (!pending) { if (!mcp_mode) session_write(); pending = QUIT; quit_tab = 0; proceed(); }
            }
            if (quit_done) break;
        }
        collaboration_pump();
        if (mcp_mode && mcp_pump()) break;
        int fw, fh, ww;
        glfwGetFramebufferSize(win, &fw, &fh); glfwGetWindowSize(win, &ww, &winh);
        /* X11 reports DPI but sizes windows in physical px; Wayland and macOS already size in logical px. */
        float csx, csy; glfwGetWindowContentScale(win, &csx, &csy);
        ui_s = csx * ww / fw * dpi_scale * ui_user_scale;
        glViewport(0, 0, fw, fh);
        render_canvas(ww, 1);
        glLoadIdentity(); glScalef(ui_s, ui_s, 1);
        ui_x = lastx / ui_s; ui_y = lasty / ui_s; ui_hover = 0;
        ui(); tabs_ui(ww); hint_bar(ww);
        if (custom_dialog) { ui_hover = 0; custom_ui(); }
        if (fd_mode) { ui_hover = 0; fd_ui(); }
        if (asking) { ui_hover = 0; ask_ui(); }
        int modal = custom_dialog || fd_mode || asking;
        glfwSetCursor(win, ui_hover ? hand : modal || lastx < panel_w() || lasty < TAB_H * ui_s || tool == SELECT ? arrow : tool == TEXT ? ibeam : cross);
        char title[PATH_MAX + 32];
        snprintf(title, sizeof title, "paintboard - %s%s", path ? path : "Untitled", dirty ? " \xe2\x80\xa2" : "");
        if (strcmp(title, shown_title)) { glfwSetWindowTitle(win, title); strcpy(shown_title, title); }
        glfwSwapBuffers(win);
        double notice_left = notice_until - glfwGetTime();
        if (agent_scanning) glfwWaitEventsTimeout(.1);
        else if (notice_left > 0) glfwWaitEventsTimeout(notice_left);
        else glfwWaitEvents();
    }
    int failed = 0; /* stdin EOF ends an MCP session without questions: save the named tabs */
    if (!quit_done) failed = save_named_tabs();
    collaboration_stop();
    if (mcp_mode) mcp_stop();
    glfwTerminate();
    return failed || mcp_output_failed ? 1 : 0;
}
