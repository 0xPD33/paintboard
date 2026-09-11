/* paintboard: offline Excalidraw-ish board. C23 + GLFW + legacy OpenGL + MCP. */
#include <GLFW/glfw3.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
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

#define VERSION "0.2.0"
#define APP_ID "paintboard" /* must match the desktop entry file name, or launchers lose the icon */

static const unsigned char TTF_UI[] = {
#embed "vendor/Inter.ttf"
};
static const unsigned char TTF_CANVAS[] = {
#embed "vendor/Excalifont.ttf"
};

enum { PEN, LINE, ARROW, RECT, ELLIPSE, TEXT, SELECT, NTOOLS };
enum { NONE, DRAW, MOVE, RESIZE, MARQUEE };
#define NCOLORS 8
#define MAX_UNDO 256
#define PANEL_W 168
#define GRID 20.f
#define HANDLE 5.f /* half-size of a resize handle, px */
#define ACCENT 0x6965db
#define INK 0x1e1e1e
#define MUTED 0x868e96

static const char *TOOL_NAME[NTOOLS] = {"Pen", "Line", "Arrow", "Rectangle", "Ellipse", "Text", "Select"};
static const char *TOOL_KEY[NTOOLS] = {"P", "L", "A", "R", "O", "T", "V"};
static const unsigned STROKE[NCOLORS] = {0x1e1e1e, 0xe03131, 0x2f9e44, 0x1971c2, 0xf08c00, 0x9c36b5, 0x0c8599, 0x868e96};
static const unsigned FILLC[NCOLORS] = {0xe9ecef, 0xffc9c9, 0xb2f2bb, 0xa5d8ff, 0xffec99, 0xeebefa, 0x99e9f2, 0xdee2e6};

/* fill < 0 = none. sx/sy scale pen points. width doubles as text size (px = width * 7). */
typedef struct { int type, color, fill, sel; float x0, y0, x1, y1, width, sx, sy; int p0, np; } Item;
typedef struct { Item *it; int n; } Snap;
typedef struct { GLuint tex; stbtt_bakedchar cd[224]; float px, ascent; } Font;

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
static Item *base; /* undo-top snapshot that a move/resize is re-derived from on every motion */
static double lastx, lasty;
static const char *path;
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
static void changed(void) { revision++; if (!agent_mutation) human_revision++; }
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
    nitems = s.n; free(s.it); revision++;
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
    for (int k = 0; it->type == PEN && k < it->np; k++) {
        float x, y; pen_pt(it, k, &x, &y);
        b[0] = fminf(b[0], x); b[1] = fminf(b[1], y); b[2] = fmaxf(b[2], x); b[3] = fmaxf(b[3], y);
    }
}
static int hit(float wx, float wy) {
    for (int i = nitems - 1; i >= 0; i--) {
        const Item *it = &items[i];
        float tol = it->width / 2 + 6 * ui_s / zoom, b[4], ax, ay, bx, by;
        switch (it->type) {
        case PEN:
            pen_pt(it, 0, &ax, &ay);
            if (it->np == 1 && hypotf(wx - ax, wy - ay) < tol) return i;
            for (int k = 0; k + 1 < it->np; k++, ax = bx, ay = by) {
                pen_pt(it, k + 1, &bx, &by);
                if (seg_dist(wx, wy, ax, ay, bx, by) < tol) return i;
            }
            break;
        case LINE: case ARROW:
            if (seg_dist(wx, wy, it->x0, it->y0, it->x1, it->y1) < tol) return i;
            break;
        default: /* ponytail: bbox hit for rect/ellipse/text; switch to outline hit if nested shapes annoy */
            bbox(it, b);
            if (wx > b[0] - tol && wx < b[2] + tol && wy > b[1] - tol && wy < b[3] + tol) return i;
        }
    }
    return -1;
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
static void text_put(const char *b, int n) {
    tpool = grow(tpool, &captpool, ntpool + n + 1, 1);
    memcpy(tpool + ntpool, b, n); ntpool += n; tpool[ntpool] = 0;
    items[editing].np += n; measure(&items[editing]);
}
static void text_del(void) { /* drop one codepoint */
    Item *it = &items[editing];
    if (!it->np) return;
    do { ntpool--; it->np--; } while (it->np && (tpool[ntpool] & 0xc0) == 0x80);
    tpool[ntpool] = 0; measure(it);
}
static void start_edit(int i) { /* re-append the bytes at the tail so older snapshots keep their copy */
    Item *it = &items[i]; int old = it->p0;
    tpool = grow(tpool, &captpool, ntpool + it->np + 2, 1);
    memcpy(tpool + ntpool, tpool + old, it->np);
    it->p0 = ntpool; ntpool += it->np; tpool[ntpool] = 0; editing = i;
    editing_new = nundo && i >= undo[nundo - 1].n; measure(it);
}
static void end_edit(void) {
    if (editing < 0) return;
    if (items[editing].np) ntpool++;   /* commit the NUL */
    else if (editing_new) restore(undo[--nundo]); /* cancel a new empty item */
    else { /* erasing existing text is an undoable deletion */
        memmove(items + editing, items + editing + 1, (nitems - editing - 1) * sizeof *items);
        nitems--;
    }
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
    checkpoint(); int n = 0;
    for (int i = 0; i < nitems; i++) if (!items[i].sel) items[n++] = items[i];
    nitems = n;
}
static void duplicate(void) {
    int n = nitems;
    if (!nsel()) return;
    checkpoint();
    for (int i = 0; i < n; i++) {
        if (!items[i].sel) continue;
        Item c = items[i]; items[i].sel = 0;
        c.x0 += GRID; c.x1 += GRID; c.y0 += GRID; c.y1 += GRID; add_item(c);
    }
}
static void copy_sel(void) { /* copies share the immutable pools, so a struct copy is enough */
    clip = realloc(clip, (nsel() + 1) * sizeof(Item)); nclip = 0;
    for (int i = 0; i < nitems; i++) if (items[i].sel) clip[nclip++] = items[i];
}
static void paste(float wx, float wy) {
    if (!nclip) return;
    checkpoint(); select_none();
    float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
    for (int k = 0; k < nclip; k++) { float ib[4]; bbox(&clip[k], ib); b[0] = fminf(b[0], ib[0]); b[1] = fminf(b[1], ib[1]); }
    float dx = snapf(wx) - b[0], dy = snapf(wy) - b[1];
    for (int k = 0; k < nclip; k++) {
        Item c = clip[k]; c.sel = 1;
        c.x0 += dx; c.x1 += dx; c.y0 += dy; c.y1 += dy; add_item(c);
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
    checkpoint(); for (int i = 0; i < nitems; i++) if (items[i].sel && (items[i].type == RECT || items[i].type == ELLIPSE)) items[i].fill = c;
}
static void set_width(float w) {
    width = fminf(fmaxf(w, 1), 32);
    if (!nsel()) return;
    checkpoint();
    for (int i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        items[i].width = width;
        if (items[i].type == TEXT) measure(&items[i]);
    }
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
    case PEN:
        scratch = grow(scratch, &capscratch, 2 * it->np + 2, sizeof *scratch);
        for (int k = 0; k < it->np; k++) pen_pt(it, k, &scratch[2 * k], &scratch[2 * k + 1]);
        stroke(scratch, it->np, 0, w);
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
    case RECT: {
        float r[8] = { it->x0, it->y0, it->x1, it->y0, it->x1, it->y1, it->x0, it->y1 };
        if (it->fill >= 0) { hex(FILLC[it->fill]); fill(it->x0, it->y0, it->x1 - it->x0, it->y1 - it->y0); hex(STROKE[it->color]); }
        stroke(r, 4, 1, w);
        break;
    }
    case ELLIPSE: {
        float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2;
        float rx = fabsf(it->x1 - it->x0) / 2, ry = fabsf(it->y1 - it->y0) / 2;
        for (int i = 0; i < 64; i++) { buf[2 * i] = cx + rx * cosf(i * 0.09817f); buf[2 * i + 1] = cy + ry * sinf(i * 0.09817f); }
        if (it->fill >= 0) {
            hex(FILLC[it->fill]);
            glBegin(GL_TRIANGLE_FAN); for (int i = 0; i < 64; i++) glVertex2fv(buf + 2 * i); glEnd();
            hex(STROKE[it->color]);
        }
        stroke(buf, 64, 1, w);
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
    const Item *it = &items[editing]; const char *s = tpool + it->p0, *nl = strrchr(s, '\n');
    float px = tpx(it); int lines = 1;
    for (const char *p = s; *p; p++) lines += *p == '\n';
    float x = it->x0 + text_w(&canvas_font, px, nl ? nl + 1 : s) + 1, y = it->y0 + (lines - 1) * px * 1.25f;
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

/* ---- file: "PB03" nitems npool ntpool items[] pool[] tpool[] ---- */
static int write_board(FILE *f) {
    return fwrite("PB03", 4, 1, f) == 1 && fwrite(&nitems, 4, 1, f) == 1
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
    return 0;
}
typedef struct { int type, color; float x0, y0, x1, y1, width; int p0, np; } ItemV2; /* PB02 layout */
static int load(void) { /* 0 ok (missing file = empty board), -1 corrupt, -2 open error */
    FILE *f = fopen(path, "rb"); char magic[4] = {0}; int n = 0, np = 0, nt = 0;
    if (!f) { if (errno == ENOENT) return 0; perror(path); return -2; }
    int v2 = fread(magic, 4, 1, f) == 1 && !memcmp(magic, "PB02", 4);
    int ok = (v2 || !memcmp(magic, "PB03", 4))
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
             && (it->type == PEN  ? it->p0 <= np && it->np <= (np - it->p0) / 2
               : it->type == TEXT ? it->p0 < nt && it->np < nt - it->p0 && !tpool[it->p0 + it->np] : 1);
    }
    if (!ok) return -1;
    nitems = n; npool = np; ntpool = nt;
    for (int i = 0; i < n; i++) if (items[i].type == TEXT) measure(&items[i]); /* font metrics may differ from the saving build */
    return 0;
}
static char *png_path(const char *board) {
    const char *name = strrchr(board, '/'); name = name ? name + 1 : board;
    const char *ext = strrchr(name, '.');
    size_t n = ext && ext != name ? (size_t)(ext - board) : strlen(board);
    /* A board named *.png must not be overwritten by its own export. */
    if (ext && !strcmp(ext, ".png")) n = strlen(board);
    char *out = malloc(n + 5);
    if (out) { memcpy(out, board, n); memcpy(out + n, ".png", 5); }
    return out;
}
static int write_png(FILE *f, const unsigned char *pixels, int w, int h) {
    int len;
    unsigned char *png = stbi_write_png_to_mem(pixels, w * 3, w, h, 3, &len);
    int ok = png && fwrite(png, 1, len, f) == (size_t)len && fflush(f) == 0;
    free(png); return ok;
}
static int export_png(void) { /* ponytail: exports the visible canvas area; pan/zoom to frame it */
    int fw, fh, ww, wh; glfwGetFramebufferSize(win, &fw, &fh); glfwGetWindowSize(win, &ww, &wh);
    if (ww <= panel_w() || wh <= 0 || fw <= 0 || fh <= 0) { fprintf(stderr, "no visible canvas to export\n"); return -1; }
    int x = panel_w() * fw / ww, W = fw - x;
    winh = wh;
    glViewport(0, 0, fw, fh); render_canvas(ww, 0);
    unsigned char *px = malloc((size_t)W * fh * 3);
    char *out = png_path(path);
    if (!px || !out) { free(px); free(out); fprintf(stderr, "export: out of memory\n"); return -1; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(x, 0, W, fh, GL_RGB, GL_UNSIGNED_BYTE, px);
    stbi_flip_vertically_on_write(1);
    FILE *f = fopen(out, "wb");
    int ok = f && write_png(f, px, W, fh);
    if (f && fclose(f) != 0) ok = 0;
    fprintf(stderr, ok ? "exported %s\n" : "export failed: %s\n", out);
    free(px); free(out); return ok ? 0 : -1;
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
    if (key == GLFW_KEY_HOME) custom_cursor = 0;
    else if (key == GLFW_KEY_END) custom_cursor = strlen(custom_draft);
    else if (key == GLFW_KEY_LEFT && custom_cursor) {
        do custom_cursor--; while (custom_cursor && (custom_draft[custom_cursor] & 0xc0) == 0x80);
    } else if (key == GLFW_KEY_RIGHT && custom_draft[custom_cursor]) {
        do custom_cursor++; while ((custom_draft[custom_cursor] & 0xc0) == 0x80);
    } else if (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE) {
        if (custom_select_all) { *custom_draft = 0; custom_cursor = 0; }
        else if (key == GLFW_KEY_BACKSPACE && custom_cursor) {
            int end = custom_cursor;
            do custom_cursor--; while (custom_cursor && (custom_draft[custom_cursor] & 0xc0) == 0x80);
            memmove(custom_draft + custom_cursor, custom_draft + end, strlen(custom_draft + end) + 1);
        } else if (key == GLFW_KEY_DELETE && custom_draft[custom_cursor]) {
            int end = custom_cursor;
            do end++; while ((custom_draft[end] & 0xc0) == 0x80);
            memmove(custom_draft + custom_cursor, custom_draft + end, strlen(custom_draft + end) + 1);
        }
    } else return;
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
        hex(ACCENT); rrect(x + 19, y + 77, w - 38, 36, 5);
        hex(custom_select_all ? 0xe0dfff : 0xffffff); rrect(x + 20, y + 78, w - 40, 34, 4);
        char visible[4096]; strcpy(visible, custom_draft); visible[custom_cursor] = 0;
        int start = 0;
        while (text_w(&ui_font, 13, visible + start) > w - 64 && start < custom_cursor) {
            do start++; while ((visible[start] & 0xc0) == 0x80);
        }
        float caret = text_w(&ui_font, 13, visible + start);
        strcpy(visible, custom_draft + start);
        while (*visible && text_w(&ui_font, 13, visible) > w - 60) {
            size_t n = strlen(visible) - 1;
            while (n && (visible[n] & 0xc0) == 0x80) n--;
            visible[n] = 0;
        }
        text_draw(&ui_font, x + 28, y + 85, 13, *custom_draft ? INK : MUTED,
                  *custom_draft ? visible : "e.g. my-agent acp");
        hex(ACCENT); fill(x + 28 + caret, y + 84, 1, 20);
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
static void ui(void) {
    float y = 12, vis = winh / ui_s; char buf[24];
    panel_scroll = fminf(fmaxf(panel_scroll, 0), fmaxf(0, panel_h - vis));
    ui_y += panel_scroll;
    if (!ui_mode) {
        glPushMatrix(); glTranslatef(0, -panel_scroll, 0);
        hex(0xf8f9fa); fill(0, panel_scroll, PANEL_W, vis); hex(0xe9ecef); fill(PANEL_W - 1, panel_scroll, 1, vis);
    }
    for (int t = 0; t < NTOOLS; t++, y += 32) if (button(8, y, 152, 28, TOOL_NAME[t], TOOL_KEY[t], tool == t)) tool = t;
    y += 4; section(y, "STROKE"); y += 18;
    for (int c = 0; c < NCOLORS; c++) if (swatch(24 + c % 4 * 32, y + c / 4 * 32, STROKE[c], c == color)) set_color(c);
    y += 70; section(y, "FILL"); y += 18;
    if (swatch(8, y, 0xffffff, fillc < 0)) set_fill(-1);
    if (!ui_mode) { hex(0xe03131); seg(14, y + 18, 26, y + 6, 2); }
    for (int c = 0; c < NCOLORS; c++) if (swatch(8 + (c + 1) % 5 * 32, y + (c + 1) / 5 * 32, FILLC[c], c == fillc)) set_fill(c);
    y += 70; section(y, "STROKE WIDTH / TEXT SIZE"); y += 18;
    if (button(8, y, 28, 28, "-", NULL, 0)) set_width(width - 1);
    if (!ui_mode) {
        hex(STROKE[color]); seg(46, y + 14, 94, y + 14, fminf(width, 16));
        snprintf(buf, sizeof buf, "%g", width);
        text_draw(&ui_font, 112 - text_w(&ui_font, 12, buf) / 2, y + 6, 12, INK, buf);
    }
    if (button(132, y, 28, 28, "+", NULL, 0)) set_width(width + 1);
    y += 36;
    if (button(8, y, 152, 28, "Snap to grid", "G", snap)) snap ^= 1;
    y += 36;
    if (button(8, y, 72, 28, "Undo", NULL, 0)) do_undo();
    if (button(88, y, 72, 28, "Redo", NULL, 0)) do_redo();
    y += 32;
    if (button(8, y, 72, 28, "Delete", NULL, 0)) delete_selected();
    if (button(88, y, 72, 28, "Clear", NULL, 0)) { checkpoint(); nitems = 0; }
    y += 32;
    if (button(8, y, 152, 28, "Export PNG", "Ctrl+E", 0)) export_png();
    y += 32;
    if (button(8, y, 152, 28, "Save", "Ctrl+S", 0)) save();
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
        "Click to type, click text to edit it", "Click or drag a box to select" };
    char s[160]; int n = nsel();
    if (editing >= 0) snprintf(s, sizeof s, "Enter newline  ·  Esc done  ·  [ ] text size");
    else if (tool == SELECT && n) snprintf(s, sizeof s, "%d selected  ·  drag to move, corners resize  ·  Del  ·  Ctrl+D duplicate", n);
    else snprintf(s, sizeof s, "%s%s  ·  right drag pan  ·  wheel zoom", tool_hint[tool], snap ? "  ·  grid snap on" : "");
    float w = text_w(&ui_font, 11, s) + 20, x = PANEL_W + 12, y = winh / ui_s - 30;
    if (x + w > ww / ui_s) return;
    hexa(0xffffff, .85f); rrect(x, y, w, 22, 6);
    text_draw(&ui_font, x + 10, y + 4, 11, MUTED, s);
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
    float wx = (sx - panx) / zoom, wy = (sy - pany) / zoom, b[4]; int shift = mods & GLFW_MOD_SHIFT;
    if (button != GLFW_MOUSE_BUTTON_LEFT) { panning = action == GLFW_PRESS; return; }
    if (action == GLFW_RELEASE) {
        if (drag == DRAW) {
            const Item *it = &items[nitems - 1];
            if (it->type != PEN && it->x0 == it->x1 && it->y0 == it->y1) restore(undo[--nundo]); /* drop zero-size shape */
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
    end_edit();
    if (sx < panel_w()) { ui_mode = 1; ui_x = sx / ui_s; ui_y = sy / ui_s; ui(); ui_mode = 0; return; }
    pressx = wx; pressy = wy; base = NULL;
    if (tool == SELECT) {
        if (sel_frame(b)) {
            float cx[4] = { b[0], b[2], b[2], b[0] }, cy[4] = { b[1], b[1], b[3], b[3] };
            for (int k = 0; k < 4; k++) {
                float grab = HANDLE * ui_s + 3;
                if (fabsf(cx[k] * zoom + panx - sx) > grab || fabsf(cy[k] * zoom + pany - sy) > grab) continue;
                checkpoint(); base = undo[nundo - 1].it; sel_bbox(sbase); handle = k; drag = RESIZE;
                return;
            }
        }
        int h = hit(wx, wy);
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
    int h = tool == TEXT ? hit(wx, wy) : -1;
    if (tool != PEN) { wx = snapf(wx); wy = snapf(wy); }
    checkpoint();
    Item it = { .type = tool, .color = color, .fill = tool == RECT || tool == ELLIPSE ? fillc : -1,
                .x0 = wx, .y0 = wy, .x1 = wx, .y1 = wy, .width = width, .sx = 1, .sy = 1, .p0 = tool == TEXT ? ntpool : npool };
    if (tool == TEXT) {
        if (h >= 0 && items[h].type == TEXT) start_edit(h);
        else { add_item(it); start_edit(nitems - 1); }
        return;
    }
    if (tool == PEN) { add_pt(0, 0); it.np = 1; }
    add_item(it); drag = DRAW;
}
static void on_cursor(GLFWwindow *w, double sx, double sy) {
    float dx = sx - lastx, dy = sy - lasty; lastx = sx; lasty = sy;
    if (custom_dialog) return;
    float wx = (sx - panx) / zoom, wy = (sy - pany) / zoom;
    if (panning) { panx += dx; pany += dy; }
    switch (drag) {
    case DRAW: {
        Item *it = &items[nitems - 1];
        /* ponytail: drop points closer than 2px; immediate mode is the ceiling, VBOs if boards get huge */
        if (it->type == PEN) {
            if (hypotf(wx - it->x0 - pool[npool - 2], wy - it->y0 - pool[npool - 1]) > 2 / zoom) { add_pt(wx - it->x0, wy - it->y0); it->np++; }
        } else { it->x1 = snapf(wx); it->y1 = snapf(wy); }
        break;
    }
    case MOVE: {
        if (!base) { checkpoint(); base = undo[nundo - 1].it; sel_bbox(sbase); } /* no undo entry for a plain click */
        float mx = wx - pressx, my = wy - pressy;
        if (snap) { mx = snapf(sbase[0] + mx) - sbase[0]; my = snapf(sbase[1] + my) - sbase[1]; }
        for (int i = 0; i < nitems; i++) {
            if (!base[i].sel) continue;
            items[i] = base[i]; items[i].x0 += mx; items[i].x1 += mx; items[i].y0 += my; items[i].y1 += my;
        }
        break;
    }
    case RESIZE: {
        int l = handle == 0 || handle == 3, t = handle < 2;
        float ax = sbase[l ? 2 : 0], ay = sbase[t ? 3 : 1], cx = sbase[l ? 0 : 2], cy = sbase[t ? 1 : 3];
        float nx = snapf(cx + wx - pressx), ny = snapf(cy + wy - pressy);
        float fx = fabsf(cx - ax) < 1e-3f ? 1 : (nx - ax) / (cx - ax), fy = fabsf(cy - ay) < 1e-3f ? 1 : (ny - ay) / (cy - ay);
        for (int i = 0; i < nitems; i++) if (base[i].sel) xform(&items[i], &base[i], ax, ay, fx, fy);
        break;
    }
    }
}
static void zoom_at(float sx, float sy, float by) { /* keep the point under (sx, sy) fixed */
    float z = fminf(fmaxf(zoom * by, .05f), 50), f = z / zoom;
    panx = sx - (sx - panx) * f; pany = sy - (sy - pany) * f; zoom = z;
}
static void on_scroll(GLFWwindow *w, double dx, double dy) {
    if (custom_dialog) return;
    double sx, sy; glfwGetCursorPos(w, &sx, &sy);
    if (sx < panel_w()) { panel_scroll -= dy * 40; return; }
    zoom_at(sx, sy, dy > 0 ? 1.1f : 1 / 1.1f);
}
static void on_char(GLFWwindow *w, unsigned c) {
    int suppressed = suppress_char; suppress_char = 0;
    if (suppressed && (c == '[' || c == ']')) return;
    if ((!custom_dialog && editing < 0) || c < 32) return;
    char b[4]; int n = 0;
    if (c < 0x80) b[n++] = c;
    else if (c < 0x800) { b[n++] = 0xc0 | c >> 6; b[n++] = 0x80 | (c & 0x3f); }
    else if (c < 0x10000) { b[n++] = 0xe0 | c >> 12; b[n++] = 0x80 | (c >> 6 & 0x3f); b[n++] = 0x80 | (c & 0x3f); }
    else { b[n++] = 0xf0 | c >> 18; b[n++] = 0x80 | (c >> 12 & 0x3f); b[n++] = 0x80 | (c >> 6 & 0x3f); b[n++] = 0x80 | (c & 0x3f); }
    if (custom_dialog) custom_insert(b, n); else text_put(b, n);
}
static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    suppress_char = 0;
    if (action == GLFW_RELEASE) return;
    if (custom_dialog) { custom_key(w, key, mods); return; }
    if (editing >= 0) {
        if ((mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) && (key == GLFW_KEY_S || key == GLFW_KEY_E)) {
            int i = items[editing].np ? editing : -1;
            end_edit();
            if (key == GLFW_KEY_S) save(); else export_png();
            if (i >= 0) { checkpoint(); start_edit(i); }
        }
        else if (!(mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_ALT | GLFW_MOD_SHIFT))
                 && (key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET)) {
            Item *it = &items[editing];
            width = it->width = fminf(64, fmaxf(1, it->width + (key == GLFW_KEY_LEFT_BRACKET ? -1 : 1)));
            measure(it); suppress_char = 1;
        }
        else if (key == GLFW_KEY_ESCAPE) end_edit();
        else if (key == GLFW_KEY_ENTER) text_put("\n", 1);
        else if (key == GLFW_KEY_BACKSPACE) text_del();
        return;
    }
    if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) return;
    drag = NONE; /* undo/delete mid-drag would leave a dangling item index */
    float wx = (lastx - panx) / zoom, wy = (lasty - pany) / zoom;
    if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) {
        switch (key) {
        case GLFW_KEY_Z: if (mods & GLFW_MOD_SHIFT) do_redo(); else do_undo(); break;
        case GLFW_KEY_Y: do_redo(); break;
        case GLFW_KEY_S: save(); break;
        case GLFW_KEY_E: export_png(); break;
        case GLFW_KEY_A: for (int i = 0; i < nitems; i++) items[i].sel = 1; tool = SELECT; break;
        case GLFW_KEY_D: duplicate(); break;
        case GLFW_KEY_C: copy_sel(); break;
        case GLFW_KEY_X: copy_sel(); delete_selected(); break;
        case GLFW_KEY_V: paste(wx, wy); break;
        case GLFW_KEY_EQUAL: case GLFW_KEY_MINUS: {
            int ww; glfwGetWindowSize(w, &ww, &winh);
            zoom_at((panel_w() + ww) / 2, winh / 2.f, key == GLFW_KEY_EQUAL ? 1.25f : .8f); break;
        }
        }
        return;
    }
    switch (key) {
    case GLFW_KEY_P: tool = PEN; break;
    case GLFW_KEY_L: tool = LINE; break;
    case GLFW_KEY_A: tool = ARROW; break;
    case GLFW_KEY_R: tool = RECT; break;
    case GLFW_KEY_O: tool = ELLIPSE; break;
    case GLFW_KEY_T: tool = TEXT; break;
    case GLFW_KEY_V: tool = SELECT; break;
    case GLFW_KEY_G: snap ^= 1; break;
    case GLFW_KEY_ESCAPE: select_none(); break;
    case GLFW_KEY_0: panx = pany = 0; zoom = 1; break;
    case GLFW_KEY_DELETE: case GLFW_KEY_BACKSPACE: delete_selected(); break;
    case GLFW_KEY_LEFT_BRACKET: case GLFW_KEY_MINUS: set_width(width - 1); break;
    case GLFW_KEY_RIGHT_BRACKET: case GLFW_KEY_EQUAL: set_width(width + 1); break;
    default: if (key >= GLFW_KEY_1 && key <= GLFW_KEY_8) set_color(key - GLFW_KEY_1);
    }
}

static int selftest(void) {
    char dir[] = "/tmp/paintboard-test-XXXXXX", file[128];
    assert(mkdtemp(dir)); snprintf(file, sizeof file, "%s/board.pb", dir);
    fonts_bake(); path = file;
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
    assert(load() == 0 && nitems == 3 && npool == 4 && ntpool == 6 && !strcmp(tpool + items[2].p0, "h") && hit(125, 101) == 1);
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
    char *out = png_path("/tmp/project.v1/notes"); assert(!strcmp(out, "/tmp/project.v1/notes.png")); free(out);
    out = png_path("/tmp/.notes"); assert(!strcmp(out, "/tmp/.notes.png")); free(out);
    out = png_path("board.png"); assert(!strcmp(out, "board.png.png")); free(out);
    char longpath[1024]; memset(longpath, 'a', 1020); memcpy(longpath + 1020, ".pb", 4);
    out = png_path(longpath); assert(strlen(out) == 1024 && !strcmp(out + 1020, ".png")); free(out);
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
    assert(remove(path) == 0 && rmdir(dir) == 0); puts("ok");
    return 0;
}

static void usage(FILE *f) {
    fprintf(f,
        "usage: paintboard [--mcp [--headless]] [FILE]\n"
        "  FILE defaults to board.pb and is created on exit if it does not exist.\n\n"
        "  --mcp       expose this canvas to an MCP client over stdin/stdout\n"
        "  --headless  run MCP without a window (PNG export unavailable)\n\n"
        "tools:  P pen  L line  A arrow  R rect  O ellipse  T text  V select\n"
        "select: click, shift+click, drag a box; drag to move, corner handles to resize\n"
        "edit:   Del delete  Ctrl+Z/Y undo/redo  Ctrl+A all  Ctrl+D duplicate  Ctrl+C/X/V copy/cut/paste\n"
        "style:  1-8 stroke color  [ ] width / text size  G snap to grid\n"
        "text:   click to type, Enter newline, Esc done; click existing text with T to edit\n"
        "view:   right/middle drag pan  wheel zoom  Ctrl+= / Ctrl+- zoom  0 reset\n"
        "file:   Ctrl+S save (also on exit)  Ctrl+E export png\n"
        "env:    PAINTBOARD_UI_SCALE=1.5 scales the panel and handles on top of the monitor DPI\n");
}

#include "mcp.c"
#include "collaboration.c"

int main(int argc, char **argv) {
    items = grow(items, &capitems, 1, sizeof *items); pool = grow(pool, &cappool, 2, sizeof *pool); tpool = grow(tpool, &captpool, 2, 1);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mcp")) { mcp_mode = 1; continue; }
        if (!strcmp(argv[i], "--headless")) { headless = 1; continue; }
        if (!strcmp(argv[i], "--test")) return selftest();
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        if (!strcmp(argv[i], "--version")) { puts("paintboard " VERSION); return 0; }
        if (argv[i][0] == '-') { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(stderr); return 1; }
        path = argv[i];
    }
    if (headless && !mcp_mode) { fprintf(stderr, "--headless requires --mcp\n"); return 1; }
    fonts_bake();
    if (!path) path = "board.pb";
    if (path[0] != '/') {
        char *cwd = getcwd(NULL, 0), *absolute = malloc(strlen(cwd) + strlen(path) + 2);
        sprintf(absolute, "%s/%s", cwd, path); free(cwd); path = absolute;
    }
    collaborating = mcp_mode; /* Explicit stdio sessions retain their existing opt-in. */
    if (collaborating) strcpy(collaboration_status, "Agent tools connected");
    int loaded = load();
    if (loaded) { if (loaded == -1) fprintf(stderr, "%s: not a paintboard file\n", path); return 1; }
    if (headless) {
        if (mcp_start()) return 1;
        char *line;
        while (!mcp_output_failed && (line = mcp_read_line())) { mcp_dispatch(line); free(line); }
        int saved = save(); cJSON_Delete(mcp_spec); return saved || mcp_output_failed ? 1 : 0;
    }
    if (!mcp_mode) usage(stdout);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_SAMPLES, 4);
    /* Wayland compositors and X11 taskbars match this against the desktop entry to find the icon. */
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, APP_ID);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, APP_ID);
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, APP_ID);
    char title[300]; snprintf(title, sizeof title, "paintboard - %s", path);
    const char *env_scale = getenv("PAINTBOARD_UI_SCALE");
    if (env_scale) ui_user_scale = fminf(fmaxf(atof(env_scale), .5f), 4);
    /* Open at 85% of the work area so the window fits laptops and does not dwarf small monitors. */
    int wx, wy, ww0 = 1280, wh0 = 800; GLFWmonitor *mon = glfwGetPrimaryMonitor();
    if (mon) { glfwGetMonitorWorkarea(mon, &wx, &wy, &ww0, &wh0); ww0 = ww0 * .85f; wh0 = wh0 * .85f; }
    if (!(win = glfwCreateWindow(ww0 > 400 ? ww0 : 400, wh0 > 300 ? wh0 : 300, title, NULL, NULL))) return 1;
    glfwSetWindowSizeLimits(win, 400, 300, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(win); glfwSwapInterval(1);
    glfwSetMouseButtonCallback(win, on_button); glfwSetCursorPosCallback(win, on_cursor);
    glfwSetScrollCallback(win, on_scroll); glfwSetKeyCallback(win, on_key); glfwSetCharCallback(win, on_char);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_POINT_SMOOTH);
    fonts_upload();
    GLFWcursor *arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR), *cross = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR),
               *ibeam = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR), *hand = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
    if (mcp_mode && mcp_start()) { glfwTerminate(); return 1; }
    collaboration_start();
    float dpi_scale = monitor_dpi_scale();
    while (!glfwWindowShouldClose(win)) {
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
        ui(); hint_bar(ww);
        if (custom_dialog) { ui_hover = 0; custom_ui(); }
        glfwSetCursor(win, ui_hover ? hand : custom_dialog || lastx < panel_w() || tool == SELECT ? arrow : tool == TEXT ? ibeam : cross);
        glfwSwapBuffers(win);
        if (agent_scanning) glfwWaitEventsTimeout(.1); else glfwWaitEvents();
    }
    end_edit(); int saved = save();
    collaboration_stop();
    if (mcp_mode) mcp_stop();
    glfwTerminate();
    return saved || mcp_output_failed ? 1 : 0;
}
