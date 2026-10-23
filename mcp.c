/* Included by paintboard.c: MCP stdio transport and tools share the board's
   mutation/undo functions. Only the main thread ever touches board or GL state. */
#include <cjson/cJSON.h>
#include <pthread.h>

#define MCP_MAX_LINE (1024 * 1024)
static const char MCP_SPEC[] = {
#embed "mcp-tools.json"
    , 0
};
static const char GUIDE[] = {
#embed "agent-guide.md"
    , 0
};
#define GUIDE_URI "paintboard://guide"
static const char *ITEM_TYPE[] = {"pen", "line", "arrow", "rect", "ellipse", "text", "diamond", "triangle", "star", "highlight", "table"};
static int mcp_mode, headless, render_mode, mcp_phase, mcp_output_failed;
static cJSON *mcp_spec;
static pthread_mutex_t mcp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mcp_cond = PTHREAD_COND_INITIALIZER;
static char *mcp_pending;
static int mcp_eof;
static pthread_t mcp_thread;
static FILE *mcp_output;

static cJSON *field(const cJSON *o, const char *name) { return cJSON_GetObjectItemCaseSensitive(o, name); }
static int jstr(const cJSON *j, const char *s) { return cJSON_IsString(j) && !strcmp(j->valuestring, s); }
static int number(const cJSON *j, double lo, double hi, int integer) {
    return cJSON_IsNumber(j) && isfinite(j->valuedouble) && j->valuedouble >= lo && j->valuedouble <= hi
        && (!integer || floor(j->valuedouble) == j->valuedouble);
}
static int keys(const cJSON *o, const char *allowed) {
    if (!cJSON_IsObject(o)) return 0;
    for (const cJSON *p = o->child; p; p = p->next) {
        char key[80];
        if (strlen(p->string) > 75 || strchr(p->string, '|')) return 0;
        snprintf(key, sizeof key, "|%s|", p->string);
        if (!strstr(allowed, key)) return 0;
        for (const cJSON *q = o->child; q != p; q = q->next) if (!strcmp(p->string, q->string)) return 0;
    }
    return 1;
}
static cJSON *tool_error(const char *message) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "content"), *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text"); cJSON_AddStringToObject(text, "text", message);
    cJSON_AddItemToArray(a, text); cJSON_AddBoolToObject(r, "isError", 1); return r;
}
static cJSON *tool_result(cJSON *data) {
    cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "content"), *text = cJSON_CreateObject();
    char *s = cJSON_PrintUnformatted(data);
    cJSON_AddStringToObject(text, "type", "text"); cJSON_AddStringToObject(text, "text", s ? s : "{}"); free(s);
    cJSON_AddItemToArray(a, text); cJSON_AddItemToObject(r, "structuredContent", data); return r;
}
static cJSON *board_status(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "revision", revision); cJSON_AddNumberToObject(r, "total", nitems);
    cJSON_AddNumberToObject(r, "human_revision", human_revision);
    cJSON_AddNumberToObject(r, "collaboration_epoch", collaboration_epoch);
    return r;
}
static void add_path(cJSON *r) { if (path) cJSON_AddStringToObject(r, "path", path); else cJSON_AddNullToObject(r, "path"); }
static cJSON *item_json(const Item *it) { /* the fields parse_item takes, so a read item can be sent back */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", ITEM_TYPE[it->type]);
    if (it->id) cJSON_AddStringToObject(r, "id", tstr(it->id));
    cJSON_AddNumberToObject(r, "color", it->color); cJSON_AddNumberToObject(r, "width", it->width);
    if (fillable(it->type) || it->type == TABLE) cJSON_AddNumberToObject(r, "fill", it->fill);
    if (has_points(it) && it->np) {
        cJSON *points = cJSON_AddArrayToObject(r, "points");
        for (int k = 0; k < it->np; k++) {
            float x, y; if (is_pen(it->type)) pen_pt(it, k, &x, &y); else conn_pt(it, k + 1, &x, &y);
            double pair[2] = {x, y}; cJSON_AddItemToArray(points, cJSON_CreateDoubleArray(pair, 2));
        }
    }
    if (!is_pen(it->type)) {
        cJSON_AddNumberToObject(r, "x0", it->x0); cJSON_AddNumberToObject(r, "y0", it->y0);
        if (it->type == TEXT) cJSON_AddStringToObject(r, "text", tpool + it->p0);
        else cJSON_AddNumberToObject(r, "x1", it->x1);
        if (it->type != TEXT && it->type != TABLE) cJSON_AddNumberToObject(r, "y1", it->y1); /* text and tables size themselves */
    }
    if (it->wrap) cJSON_AddNumberToObject(r, "wrap", it->wrap);
    if (it->type == TABLE) {
        cJSON *rows = cJSON_AddArrayToObject(r, "rows"), *row = NULL; const char *s = tpool + it->p0;
        for (int k = 0; k < it->np; k++, s += strlen(s) + 1) {
            if (k % it->cols == 0) cJSON_AddItemToArray(rows, row = cJSON_CreateArray());
            cJSON_AddItemToArray(row, cJSON_CreateString(s));
        }
    }
    if (it->from) cJSON_AddNumberToObject(r, "from", it->from - 1);
    if (it->to) cJSON_AddNumberToObject(r, "to", it->to - 1);
    if (it->parent) cJSON_AddNumberToObject(r, "parent", it->parent - 1);
    if (it->label) cJSON_AddStringToObject(r, "label", tstr(it->label));
    if (it->caption) cJSON_AddStringToObject(r, "caption", tstr(it->caption));
    if (it->label_size) cJSON_AddNumberToObject(r, "label_size", it->label_size);
    if (it->style & DASHED) cJSON_AddBoolToObject(r, "dashed", 1);
    if (it->style & ELBOW) cJSON_AddBoolToObject(r, "elbow", 1);
    return r;
}
static cJSON *get_board(const cJSON *args) {
    const cJSON *off = field(args, "offset"), *lim = field(args, "limit");
    if (!keys(args, "|offset|limit|") || (off && !number(off, 0, 1 << 20, 1)) || (lim && !number(lim, 1, 1000, 1)))
        return tool_error("Expected offset >= 0 and limit between 1 and 1000.");
    int offset = off ? off->valueint : 0, limit = lim ? lim->valueint : 200;
    cJSON *r = board_status(), *list = cJSON_AddArrayToObject(r, "items"), *view = cJSON_AddObjectToObject(r, "view");
    add_path(r); cJSON_AddBoolToObject(r, "headless", headless);
    cJSON_AddNumberToObject(view, "panx", panx); cJSON_AddNumberToObject(view, "pany", pany); cJSON_AddNumberToObject(view, "zoom", zoom);
    if (win) { int ww, wh; glfwGetWindowSize(win, &ww, &wh); cJSON_AddNumberToObject(view, "width", ww); cJSON_AddNumberToObject(view, "height", wh); }
    cJSON_AddNumberToObject(r, "undo_steps", nundo); cJSON_AddNumberToObject(r, "redo_steps", nredo);
    for (int i = offset; i < nitems && i < offset + limit; i++) {
        cJSON *entry = cJSON_CreateObject(); float b[4]; bbox(&items[i], b);
        cJSON_AddNumberToObject(entry, "index", i); cJSON_AddBoolToObject(entry, "selected", items[i].sel);
        cJSON_AddItemToObject(entry, "item", item_json(&items[i]));
        cJSON_AddItemToObject(entry, "bounds", cJSON_CreateFloatArray(b, 4)); cJSON_AddItemToArray(list, entry);
    }
    if (offset + limit < nitems) cJSON_AddNumberToObject(r, "next_offset", offset + limit);
    return tool_result(r);
}
static int text_ok(const cJSON *v) { /* 1 to 65536 bytes, newlines but no other control characters */
    if (!cJSON_IsString(v)) return 0;
    size_t n = strlen(v->valuestring);
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++) if (*p < 32 && *p != '\n') return 0;
    return n && n <= 65536;
}
static int name_ok(const cJSON *v) { /* an id: 1 to 64 bytes, no control characters */
    if (!cJSON_IsString(v) || !*v->valuestring || strlen(v->valuestring) > 64) return 0;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++) if (*p < 32) return 0;
    return 1;
}
static int put_text(const char *s) { /* s and its NUL onto the tpool tail; returns the offset, or -1 when tpool is full */
    int n = strlen(s) + 1;
    if (ntpool + n + 1 >= 1 << 26) return -1;
    tpool = grow(tpool, &captpool, ntpool + n + 1, 1); memcpy(tpool + ntpool, s, n); ntpool += n;
    return ntpool - n;
}
#define FULL "the board's text storage is full"
/* NULL when the item is valid, else the reason. Strings and points go onto the pool tails, which the
   caller rolls back when the batch fails. The caller resolves from, to, and parent. */
static const char *parse_item(const cJSON *o, Item *it) {
    static char why[300];
    *it = (Item){.type = -1, .fill = -1, .width = 3, .sx = 1, .sy = 1};
    for (int i = 0; i < SELECT; i++) if (jstr(field(o, "type"), ITEM_TYPE[i])) it->type = i;
    if (it->type < 0) return "type must be pen, line, arrow, rect, ellipse, text, diamond, triangle, star, highlight, or table";
    int t = it->type, link = connector(t);
    const char *fields = is_pen(t) ? "|type|id|parent|color|width|points|"
        : t == TEXT ? "|type|id|parent|color|width|x0|y0|text|wrap|"
        : t == TABLE ? "|type|id|parent|color|width|fill|x0|y0|x1|y1|rows|label_size|"
        : link ? "|type|id|parent|color|width|x0|y0|x1|y1|from|to|points|elbow|dashed|label|label_size|"
        : "|type|id|parent|color|width|fill|x0|y0|x1|y1|dashed|label|caption|label_size|";
    if (!keys(o, fields)) { snprintf(why, sizeof why, "unknown or repeated field; %s takes %s", ITEM_TYPE[t], fields); return why; }
    const cJSON *v = field(o, "color");
    if (v) { if (!number(v, 0, 7, 1)) return "color must be an integer 0-7"; it->color = v->valueint; }
    v = field(o, "fill"); if (v) { if (!number(v, -1, 7, 1)) return "fill must be an integer -1-7"; it->fill = v->valueint; }
    v = field(o, "width"); if (v) { if (!number(v, 1, 64, 0)) return "width must be a number 1-64"; it->width = v->valuedouble; }
    v = field(o, "label_size"); if (v) { if (!number(v, 1, 64, 0)) return "label_size must be a number 1-64"; it->label_size = v->valuedouble; }
    v = field(o, "wrap"); if (v) { if (!number(v, 0, 1e6, 0)) return "wrap must be a width 0-1000000; 0 does not wrap"; it->wrap = v->valuedouble; }
    for (int k = 0; k < 2; k++) {
        v = field(o, k ? "elbow" : "dashed");
        if (v && !cJSON_IsBool(v)) return "dashed and elbow must be true or false";
        if (cJSON_IsTrue(v)) it->style |= k ? ELBOW : DASHED;
    }
    v = field(o, "id"); if (v && !name_ok(v)) return "id must be 1-64 bytes without control characters";
    if (v && (it->id = put_text(v->valuestring) + 1) <= 0) return FULL;
    v = field(o, "label"); if (v && !text_ok(v)) return "label must be 1-65536 bytes of text without control characters other than newline";
    if (v && (it->label = put_text(v->valuestring) + 1) <= 0) return FULL;
    v = field(o, "caption"); if (v && !text_ok(v)) return "caption must be 1-65536 bytes of text without control characters other than newline";
    if (v && (it->caption = put_text(v->valuestring) + 1) <= 0) return FULL;

    const cJSON *pts = field(o, "points");
    if (is_pen(t) || pts) { /* a pen's points are relative to its first one; waypoints stay absolute */
        int n = cJSON_GetArraySize(pts), max = link ? 100 : 10000;
        if (!cJSON_IsArray(pts) || n < 1 || n > max || npool + 2 * n >= 1 << 26)
            return link ? "points must be an array of 1-100 [x, y] waypoints" : "points must be an array of 1-10000 [x, y] pairs";
        it->p0 = npool; it->np = n;
        for (const cJSON *pt = pts->child; pt; pt = pt->next) {
            if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) != 2 || !number(pt->child, -1e6, 1e6, 0) || !number(pt->child->next, -1e6, 1e6, 0))
                return "each point must be [x, y] within +/-1000000";
            float x = pt->child->valuedouble, y = pt->child->next->valuedouble;
            if (is_pen(t) && pt == pts->child) { it->x0 = it->x1 = x; it->y0 = it->y1 = y; }
            if (is_pen(t)) add_pt(x - it->x0, y - it->y0); else add_pt(x, y);
        }
    }
    if (is_pen(t)) return NULL;
    const char *names[] = {"x0", "y0", "x1", "y1"}; float coords[4] = {0};
    for (int i = 0; i < (t == TEXT ? 2 : t == TABLE ? 3 : 4); i++) {
        v = field(o, names[i]);
        if (!v && link && field(o, i < 2 ? "from" : "to")) continue; /* relink places a linked end */
        if (!number(v, -1e6, 1e6, 0)) return t == TEXT ? "x0 and y0 must be numbers within +/-1000000"
            : t == TABLE ? "x0, y0, and x1 must be numbers within +/-1000000; a table sets its own height"
            : link ? "x0, y0 (or from) and x1, y1 (or to) must be numbers within +/-1000000"
            : "x0, y0, x1, y1 must be numbers within +/-1000000";
        coords[i] = v->valuedouble;
    }
    if (t == TABLE && field(o, "y1") && !number(field(o, "y1"), -1e6, 1e6, 0)) return "y1 must be a number; a table sets its own height";
    it->x0 = coords[0]; it->y0 = coords[1]; it->x1 = coords[2]; it->y1 = coords[3];
    if (t == TEXT) {
        v = field(o, "text");
        if (!text_ok(v)) return "text must be 1-65536 bytes without control characters other than newline";
        if ((it->p0 = put_text(v->valuestring)) < 0) return FULL;
        it->np = strlen(v->valuestring); measure(it);
    }
    if (t == TABLE) { /* the cells sit one after another on tpool */
        const cJSON *rows = field(o, "rows");
        int nr = cJSON_GetArraySize(rows), nc = cJSON_IsArray(rows) && rows->child ? cJSON_GetArraySize(rows->child) : 0;
        if (!cJSON_IsArray(rows) || nr < 1 || nr > MAX_ROWS || nc < 1 || nc > MAX_COLS) return "rows must hold 1-100 rows of 1-20 cells";
        if (it->x1 - it->x0 < 24 * nc) return "x1 must be at least 24 per column right of x0";
        it->p0 = ntpool; it->cols = nc; it->np = nr * nc;
        for (const cJSON *row = rows->child; row; row = row->next) {
            if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != nc) return "every row needs the same number of cells";
            for (const cJSON *cell = row->child; cell; cell = cell->next) {
                if (!cJSON_IsString(cell) || (*cell->valuestring && !text_ok(cell))) return "a cell must be text of at most 65536 bytes without control characters other than newline";
                if (put_text(cell->valuestring) < 0) return FULL;
            }
        }
        measure(it);
    }
    return NULL;
}
static int find_id(const char *s, int n) { /* among the first n items; ponytail: a linear scan, index the ids if boards reach ~100k items */
    for (int i = 0; i < n; i++) if (items[i].id && !strcmp(tstr(items[i].id), s)) return i;
    return -1;
}
/* A from, to, or parent value: board item i as i + 1, earlier batch item j as -(j + 1), 0 when absent,
   INT_MIN when invalid. board counts the items a batch may name; *indexed notes a numeric index. */
static int ref_item(const cJSON *v, int k, const Item *batch, int self, int board, int *indexed) {
    if (!v) return 0;
    if (number(v, 0, board - 1, 1)) { *indexed = 1; return v->valueint == self ? INT_MIN : v->valueint + 1; }
    if (!cJSON_IsString(v)) return INT_MIN;
    for (int j = k - 1; batch && j >= 0; j--) if (batch[j].id && !strcmp(tstr(batch[j].id), v->valuestring)) return -(j + 1);
    int i = find_id(v->valuestring, board);
    return i >= 0 && i != self ? i + 1 : INT_MIN;
}
static int current_revision(const cJSON *args) {
    const cJSON *r = field(args, "revision");
    return number(r, 0, 9007199254740991., 1) && r->valuedouble == (double)revision;
}
enum { ADD, UPDATE, REPLACE };
/* add_items, update_items, and replace_items: stage and check the whole batch, then commit it as one undo step. */
static cJSON *edit_items(const cJSON *args, int mode) {
    if (!keys(args, mode == ADD ? "|items|revision|collaboration_epoch|" : "|items|revision|")) return tool_error("Unexpected arguments.");
    const cJSON *rev = field(args, "revision"), *epoch = field(args, "collaboration_epoch"), *list = field(args, "items");
    if (rev && !current_revision(args)) return tool_error("Stale revision; call get_board and retry with its revision.");
    if (epoch && (!number(epoch, 0, 9007199254740991., 1) || epoch->valuedouble != collaboration_epoch)) return tool_error("Collaboration session changed; discard this response.");
    int n = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || n < (mode == REPLACE ? 0 : 1) || n > 1000 || nitems + n >= 1 << 20)
        return tool_error(mode == REPLACE ? "Expected 0 to 1000 items." : "Expected 1 to 1000 items within the board capacity.");
    static const char *REF[3] = {"from", "to", "parent"};
    int board = mode == REPLACE ? 0 : nitems, start = mode == ADD ? nitems : 0, indexed = 0, k = 0, oldnp = npool, oldnt = ntpool;
    int indices[1000], refs[1000][3], *parent = NULL; char newid[1000], message[400]; const char *why = NULL;
    Item *staged = malloc((n + 1) * sizeof *staged);
    if (!staged) return tool_error("Out of memory.");
    for (const cJSON *entry = list->child; entry; entry = entry->next, k++) {
        const cJSON *o = entry; int target = -1;
        indices[k] = start + k;
        if (mode == UPDATE) {
            const cJSON *idx = field(entry, "index"), *id = field(entry, "id");
            o = field(entry, "item");
            if (!o && id && !idx) o = entry; /* the item's own fields beside its id, the way add_items takes it */
            else if (!keys(entry, "|index|id|item|") || !o || !idx == !id) { why = "expected {id, item}, {index, item}, or an item with its id"; goto invalid; }
            target = idx ? (number(idx, 0, nitems - 1, 1) ? idx->valueint : -1) : cJSON_IsString(id) ? find_id(id->valuestring, nitems) : -1;
            if (target < 0) { why = idx ? "the index is not on the board" : "no board item has that id"; goto invalid; }
            indexed |= idx != NULL;
            for (int j = 0; j < k; j++) if (indices[j] == target) { why = "the item repeats in this batch"; goto invalid; }
            indices[k] = target;
        }
        if ((why = parse_item(o, &staged[k]))) goto invalid;
        for (int e = 0; e < 3; e++) {
            refs[k][e] = ref_item(field(o, REF[e]), k, mode == UPDATE ? NULL : staged, target, board, &indexed);
            if (refs[k][e] == INT_MIN) {
                why = mode == REPLACE ? "from, to, and parent must be ids of earlier items in this batch"
                    : "from, to, and parent must name another item: a board index, an id on the board, or the id of an earlier item in this batch";
                goto invalid;
            }
        }
        newid[k] = staged[k].id != 0;
        if (newid[k]) {
            int owner = find_id(tstr(staged[k].id), board);
            if (owner >= 0 && owner != target) { why = "another board item has that id"; goto invalid; }
            for (int j = 0; j < k; j++) if (newid[j] && !strcmp(tstr(staged[j].id), tstr(staged[k].id))) { why = "the id repeats in this batch"; goto invalid; }
        } else if (target >= 0) staged[k].id = items[target].id; /* an update keeps the id it does not name */
    }
    k = -1;
    if (mode != ADD && indexed && !rev) { why = "Board indices need the revision from get_board. Pass revision, or name items by id"; goto invalid; }
    parent = malloc((start + n + board + 1) * sizeof *parent); /* the nesting after the commit */
    for (int i = 0; i < board; i++) parent[i] = items[i].parent;
    for (int j = 0; j < n; j++) parent[indices[j]] = refs[j][2] < 0 ? indices[-refs[j][2] - 1] + 1 : refs[j][2];
    for (int i = 0; i < (mode == UPDATE ? board : start + n); i++)
        if (depth(parent, i) > MAX_DEPTH) { why = "parent would make a loop or nest more than 64 containers deep"; goto invalid; }
    free(parent);
    checkpoint();
    if (mode == REPLACE) nitems = 0;
    char *in_batch = mode == UPDATE ? calloc(nitems + 1, 1) : NULL; float (*moved)[2] = mode == UPDATE ? calloc(nitems + 1, sizeof *moved) : NULL;
    for (k = 0; k < n; k++) { /* batch references resolve here: every earlier item has its final index */
        int r[3];
        for (int e = 0; e < 3; e++) r[e] = refs[k][e] < 0 ? indices[-refs[k][e] - 1] + 1 : refs[k][e];
        staged[k].from = r[0]; staged[k].to = r[1]; staged[k].parent = r[2];
        if (mode != UPDATE) { add_item(staged[k]); continue; }
        int t = indices[k]; float a[4], b[4]; plain_box(&items[t], a); plain_box(&staged[k], b);
        in_batch[t] = 1; moved[t][0] = b[0] - a[0]; moved[t][1] = b[1] - a[1];
        staged[k].sel = items[t].sel; items[t] = staged[k];
    }
    for (int j = 0; mode == UPDATE && j < nitems; j++) { /* a moved container takes what it holds, by its nearest moved container */
        if (in_batch[j]) continue;
        for (int a = items[j].parent - 1, d = 0; a >= 0 && d <= MAX_DEPTH; a = items[a].parent - 1, d++)
            if (in_batch[a] == 1) { moved[j][0] = moved[a][0]; moved[j][1] = moved[a][1]; shift(&items[j], moved[j][0], moved[j][1]); in_batch[j] = 2; break; }
    }
    for (int j = 0; mode == UPDATE && j < nitems; j++) { /* waypoints follow when both linked ends moved alike */
        Item *it = &items[j]; int a = it->from - 1, b = it->to - 1;
        if (in_batch[j] || !connector(it->type) || !it->np || a < 0 || b < 0 || !in_batch[a] || !in_batch[b]
            || moved[a][0] != moved[b][0] || moved[a][1] != moved[b][1]) continue;
        shift(it, moved[a][0], moved[a][1]);
    }
    free(staged); free(in_batch); free(moved); relink();
    cJSON *r = board_status(), *bounds = cJSON_AddArrayToObject(r, "bounds");
    cJSON_AddItemToObject(r, "indices", cJSON_CreateIntArray(indices, n));
    for (k = 0; k < n; k++) { float b[4]; bbox(&items[indices[k]], b); cJSON_AddItemToArray(bounds, cJSON_CreateFloatArray(b, 4)); }
    return tool_result(r);
invalid:
    npool = oldnp; ntpool = oldnt; free(staged); free(parent);
    if (k >= 0) snprintf(message, sizeof message, "items[%d]: %s. Nothing changed.", k, why);
    else snprintf(message, sizeof message, "%s. Nothing changed.", why);
    return tool_error(message);
}
static cJSON *remove_items(const cJSON *args) {
    const cJSON *list = field(args, "indices"), *names = field(args, "ids");
    int n = cJSON_GetArraySize(list), m = cJSON_GetArraySize(names);
    if (!keys(args, "|revision|indices|ids|") || (!list && !names) || (list && (!cJSON_IsArray(list) || n < 1 || n > 1000))
        || (names && (!cJSON_IsArray(names) || m < 1 || m > 1000))) return tool_error("Expected indices, ids, or both, with 1 to 1000 entries each.");
    if ((list || field(args, "revision")) && !current_revision(args))
        return tool_error("Missing or stale revision. Call get_board and retry with its revision, or delete by ids.");
    char *gone = calloc(nitems + 1, 1), message[200];
    for (const cJSON *v = list ? list->child : NULL; v; v = v->next) {
        if (!number(v, 0, nitems - 1, 1) || gone[v->valueint]) { free(gone); return tool_error("Each index must be on the board and appear once."); }
        gone[v->valueint] = 1;
    }
    for (const cJSON *v = names ? names->child : NULL; v; v = v->next) {
        int i = cJSON_IsString(v) ? find_id(v->valuestring, nitems) : -1;
        if (i < 0 || gone[i]) {
            snprintf(message, sizeof message, "No board item has the id %.64s, or it repeats. Nothing changed.", cJSON_IsString(v) ? v->valuestring : "given");
            free(gone); return tool_error(message);
        }
        gone[i] = 1;
    }
    checkpoint(); compact(gone); free(gone); return tool_result(board_status());
}

/* ---- add_graph: nodes and edges in, a layered layout out, drawn by edit_items as one undo step.
   Ranks follow the longest path from the sources, after a depth-first search drops the edges that close
   a cycle. Barycenter sweeps order each rank, and a group's nodes sort together. ---- */
#define GRAPH_MAX 300
static const char *SHAPES[] = {"rect", "ellipse", "diamond", "triangle", "star"};
static const int SHAPE_TYPE[] = {RECT, ELLIPSE, DIAMOND, TRIANGLE, STAR};
static const float SHAPE_SHARE[] = {1, .72f, .5f, .5f, .4f}; /* the share of a shape's box that holds text, as in label_layout */
typedef struct { const cJSON *o; const char *id, *label, *caption; int shape, color, fill, pin, rank, g; float w, h, pos, key, main, cross; } Node;
static Node *gnodes;
static int node_order(const void *a, const void *b) { /* by rank, then key, then group, then the input order */
    const Node *x = &gnodes[*(const int *)a], *y = &gnodes[*(const int *)b];
    if (x->rank != y->rank) return x->rank - y->rank;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->g != y->g) return x->g - y->g;
    return *(const int *)a - *(const int *)b;
}
static cJSON *graph_error(Node *v, int (*e)[3], const char *what, int k, const char *why) {
    char message[300]; snprintf(message, sizeof message, "%s[%d]: %s. Nothing changed.", what, k, why);
    free(v); free(e); return tool_error(message);
}
static cJSON *add_graph(const cJSON *args) {
    const cJSON *nodes = field(args, "nodes"), *edges = field(args, "edges"), *dir = field(args, "direction");
    const cJSON *ox = field(args, "x"), *oy = field(args, "y"), *elbows = field(args, "elbow");
    int n = cJSON_GetArraySize(nodes), m = cJSON_GetArraySize(edges), down = jstr(dir, "down"), ng = 0, k = 0;
    if (!keys(args, "|nodes|edges|direction|x|y|elbow|") || !cJSON_IsArray(nodes) || n < 1 || n > GRAPH_MAX
        || (edges && (!cJSON_IsArray(edges) || m > GRAPH_MAX)) || (dir && !down && !jstr(dir, "right"))
        || (ox && !number(ox, -1e6, 1e6, 0)) || (oy && !number(oy, -1e6, 1e6, 0)) || (elbows && !cJSON_IsBool(elbows)))
        return tool_error("Expected nodes (1-300), and optional edges (0-300), direction (right or down), x, y, and elbow.");
    Node *v = calloc(n, sizeof *v); int (*e)[3] = calloc(m + 1, sizeof *e); /* e: from node, to node (-1 for a board item), closes a cycle */
    const char *groups[50];
    for (const cJSON *o = nodes->child; o; o = o->next, k++) {
        const cJSON *id = field(o, "id"), *shape = field(o, "shape"), *pin = field(o, "rank"), *group = field(o, "group");
        const cJSON *c = field(o, "color"), *f = field(o, "fill"), *label = field(o, "label"), *caption = field(o, "caption");
        Node *d = &v[k]; d->o = o; d->shape = -1; d->g = -1;
        for (int s = 0; s < 5; s++) if (jstr(shape, SHAPES[s])) d->shape = s;
        if (!keys(o, "|id|label|caption|shape|color|fill|rank|group|dashed|"))
            return graph_error(v, e, "nodes", k, "a node takes id, label, caption, shape, color, fill, rank, group, and dashed");
        if (!name_ok(id)) return graph_error(v, e, "nodes", k, "id must be 1-64 bytes without control characters");
        if (find_id(id->valuestring, nitems) >= 0) return graph_error(v, e, "nodes", k, "a board item already has that id");
        if ((label && !text_ok(label)) || (caption && !text_ok(caption))) return graph_error(v, e, "nodes", k, "label and caption must be 1-65536 bytes of text");
        if (shape && d->shape < 0) return graph_error(v, e, "nodes", k, "shape must be rect, ellipse, diamond, triangle, or star");
        if ((c && !number(c, 0, 7, 1)) || (f && !number(f, -1, 7, 1))) return graph_error(v, e, "nodes", k, "color must be 0-7 and fill -1-7");
        if (pin && !number(pin, 0, 999, 1)) return graph_error(v, e, "nodes", k, "rank must be an integer 0-999");
        if (group && !name_ok(group)) return graph_error(v, e, "nodes", k, "group must be 1-64 bytes without control characters");
        if (field(o, "dashed") && !cJSON_IsBool(field(o, "dashed"))) return graph_error(v, e, "nodes", k, "dashed must be true or false");
        for (int j = 0; j < k; j++) if (!strcmp(v[j].id, id->valuestring)) return graph_error(v, e, "nodes", k, "the id repeats");
        d->id = id->valuestring; d->label = label ? label->valuestring : d->id; d->caption = caption ? caption->valuestring : NULL;
        d->shape = d->shape < 0 ? 0 : d->shape; d->fill = f ? f->valueint : -1;
        d->color = c ? c->valueint : d->fill >= 0 ? d->fill : 0; /* stroke and fill palettes share their hues */
        d->pin = pin ? pin->valueint : -1;
        if (group) {
            for (int g = 0; g < ng; g++) if (!strcmp(groups[g], group->valuestring)) d->g = g;
            if (d->g < 0 && ng == 50) return graph_error(v, e, "nodes", k, "a graph takes at most 50 groups");
            if (d->g < 0) { groups[ng] = group->valuestring; d->g = ng++; }
        }
    }
    for (int g = 0; g < ng; g++) { /* a group's frame takes the group name as its id */
        int clash = find_id(groups[g], nitems) >= 0;
        for (int i = 0; i < n; i++) clash |= !strcmp(v[i].id, groups[g]);
        if (clash) { int first = 0; while (v[first].g != g) first++; return graph_error(v, e, "nodes", first, "the group name is already a node or board id"); }
    }
    k = 0;
    for (const cJSON *o = edges ? edges->child : NULL; o; o = o->next, k++) {
        const cJSON *ends[2] = { field(o, "from"), field(o, "to") }, *t = field(o, "type"), *c = field(o, "color");
        if (!keys(o, "|from|to|label|type|color|dashed|elbow|"))
            return graph_error(v, e, "edges", k, "an edge takes from, to, label, type, color, dashed, and elbow");
        for (int s = 0; s < 2; s++) {
            e[k][s] = -1;
            for (int i = 0; cJSON_IsString(ends[s]) && i < n; i++) if (!strcmp(v[i].id, ends[s]->valuestring)) e[k][s] = i;
            if (e[k][s] < 0 && (!cJSON_IsString(ends[s]) || find_id(ends[s]->valuestring, nitems) < 0))
                return graph_error(v, e, "edges", k, "from and to must name a node or an item id on the board");
        }
        if (!strcmp(ends[0]->valuestring, ends[1]->valuestring)) return graph_error(v, e, "edges", k, "from and to must differ");
        if ((t && !jstr(t, "arrow") && !jstr(t, "line")) || (c && !number(c, 0, 7, 1)) || (field(o, "label") && !text_ok(field(o, "label")))
            || (field(o, "dashed") && !cJSON_IsBool(field(o, "dashed"))) || (field(o, "elbow") && !cJSON_IsBool(field(o, "elbow"))))
            return graph_error(v, e, "edges", k, "type must be arrow or line, color 0-7, label text, and dashed and elbow true or false");
    }
    /* Box sizes: the label fits the share of the box that holds text, then snaps to the grid. */
    for (int i = 0; i < n; i++) {
        Node *d = &v[i]; float share = SHAPE_SHARE[d->shape], tw = text_w(&canvas_font, 21, d->label);
        if (d->caption) tw = fmaxf(tw, text_w(&canvas_font, 21 * .75f, d->caption));
        d->w = ceilf(fminf(fmaxf((tw + 16) / share + 24, 120), 320) / GRID) * GRID;
        float wrap = fmaxf(d->w * share - 16, 21), th = text_lines(d->label, 0, 0, 21, wrap, 1, 0, NULL);
        if (d->caption) th += text_lines(d->caption, 0, 0, 21 * .75f, wrap, 1, 0, NULL) + 2;
        d->h = ceilf(fmaxf(th / (share < 1 ? share + .05f : 1) + 24, 60) / GRID) * GRID;
    }
    /* Ranks: an iterative depth-first search marks the edges that close a cycle, then the longest path ranks the rest. */
    int *state = calloc(n, sizeof *state), *stack = malloc(n * sizeof *stack), *next = calloc(n, sizeof *next), *indeg = calloc(n, sizeof *indeg);
    for (int s = 0; s < n; s++) {
        if (state[s]) continue;
        int sp = 0; stack[sp++] = s; state[s] = 1;
        while (sp) {
            int u = stack[sp - 1];
            if (next[u] == m) { state[u] = 2; sp--; continue; }
            int j = next[u]++, w = e[j][1];
            if (e[j][0] != u || w < 0) continue;
            if (state[w] == 1) e[j][2] = 1;
            else if (!state[w]) { state[w] = 1; stack[sp++] = w; }
        }
    }
    for (int j = 0; j < m; j++) if (e[j][0] >= 0 && e[j][1] >= 0 && !e[j][2]) indeg[e[j][1]]++;
    int head = 0, tail = 0;
    for (int i = 0; i < n; i++) { v[i].rank = v[i].pin >= 0 ? v[i].pin : 0; if (!indeg[i]) stack[tail++] = i; }
    while (head < tail) {
        int u = stack[head++];
        for (int j = 0; j < m; j++) {
            int w = e[j][1];
            if (e[j][0] != u || w < 0 || e[j][2]) continue;
            if (v[w].pin < 0) v[w].rank = fmaxf(v[w].rank, v[u].rank + 1);
            if (!--indeg[w]) stack[tail++] = w;
        }
    }
    int used[1400] = {0}, nranks = 0; /* close the gaps that pinned ranks leave */
    for (int i = 0; i < n; i++) used[v[i].rank] = 1;
    for (int r = 0; r < 1400; r++) used[r] = used[r] ? ++nranks : 0;
    for (int i = 0; i < n; i++) v[i].rank = used[v[i].rank] - 1;
    free(state); free(next); free(indeg);
    /* Order inside ranks: barycenters of the neighbors above, then below; a group sorts by its mean. */
    int *ord = stack; gnodes = v;
    for (int i = 0; i < n; i++) { ord[i] = i; v[i].key = i; }
    for (int sweep = 0; sweep <= 8; sweep++) {
        qsort(ord, n, sizeof *ord, node_order);
        for (int a = 0, b; a < n; a = b) { /* position within the rank, from 0 to 1 */
            for (b = a; b < n && v[ord[b]].rank == v[ord[a]].rank; b++) {}
            for (int i = a; i < b; i++) v[ord[i]].pos = (i - a + .5f) / (b - a);
        }
        if (sweep == 8) break;
        float gsum[50] = {0}, gcount[50] = {0};
        for (int i = 0; i < n; i++) {
            float sum = 0; int count = 0;
            for (int j = 0; j < m; j++) {
                int other = e[j][2] ? -1 : e[j][0] == i ? e[j][1] : e[j][1] == i ? e[j][0] : -1; /* edges that close a cycle do not pull */
                if (other >= 0 && (sweep & 1 ? v[other].rank > v[i].rank : v[other].rank < v[i].rank)) { sum += v[other].pos; count++; }
            }
            v[i].key = count ? sum / count : v[i].pos;
            if (v[i].g >= 0) { gsum[v[i].g] += v[i].key; gcount[v[i].g]++; }
        }
        for (int i = 0; i < n; i++) if (v[i].g >= 0) v[i].key = gsum[v[i].g] / gcount[v[i].g];
    }
    /* Coordinates: ranks along the main axis; along the cross axis, each node moves toward its neighbors'
       mean, kept in order and apart by a forward and a backward pass, averaged. */
    float extent[1000] = {0}, gap = 100, lw = 0, lh = 0, at = 0, *want = malloc(n * sizeof *want), *lo = malloc(n * sizeof *lo), *hi = malloc(n * sizeof *hi);
    k = 0;
    for (const cJSON *o = edges ? edges->child : NULL; o; o = o->next, k++)
        if (field(o, "label")) { float h, w = text_run(&canvas_font, 0, 0, 21, field(o, "label")->valuestring, 0, &h); lw = fmaxf(lw, w); lh = fmaxf(lh, h); }
    gap = fmaxf(gap, (down ? lh : lw) + 60) + (ng ? (down ? 80 : 40) : 0); /* room for edge labels and group frames */
    for (int i = 0; i < n; i++) extent[v[i].rank] = fmaxf(extent[v[i].rank], down ? v[i].h : v[i].w);
    for (int r = 0; r < nranks; r++) { float size = extent[r]; extent[r] = at + size / 2; at += size + gap; } /* now the rank's center */
    for (int i = 0; i < n; i++) v[i].main = extent[v[i].rank];
#define CROSS(i) (down ? v[i].w : v[i].h)
#define APART(a, b) ((CROSS(a) + CROSS(b)) / 2 + (v[a].g == v[b].g ? 40 : 80 + (!down && v[b].g >= 0 ? 40 : 0)))
    for (int a = 0, b; a < n; a = b) {
        for (b = a; b < n && v[ord[b]].rank == v[ord[a]].rank; b++) if (b > a) v[ord[b]].cross = v[ord[b - 1]].cross + APART(ord[b - 1], ord[b]);
        float half = v[ord[b - 1]].cross / 2;
        for (int i = a; i < b; i++) v[ord[i]].cross -= half;
    }
    int *first = malloc((nranks + 1) * sizeof *first); /* ord index where each rank starts */
    for (int r = 0, i = 0; r <= nranks; r++) { while (i < n && v[ord[i]].rank < r) i++; first[r] = i; }
    for (int pass = 0; pass < 9; pass++) { /* down, up, ..., down: a node with one parent ends in line with it */
        for (int s = 0; s < nranks; s++) {
            int r = pass & 1 ? nranks - 1 - s : s, a = first[r], b = first[r + 1];
            for (int i = a; i < b; i++) {
                float sum = 0; int count = 0, u = ord[i];
                for (int j = 0; j < m; j++) {
                    int other = e[j][2] ? -1 : e[j][0] == u ? e[j][1] : e[j][1] == u ? e[j][0] : -1;
                    if (other >= 0 && (pass & 1 ? v[other].rank > r : v[other].rank < r)) { sum += v[other].cross; count++; }
                }
                want[u] = count ? sum / count : v[u].cross;
            }
            for (int i = a; i < b; i++) lo[i] = i > a ? fmaxf(want[ord[i]], lo[i - 1] + APART(ord[i - 1], ord[i])) : want[ord[i]];
            for (int i = b - 1; i >= a; i--) hi[i] = i < b - 1 ? fminf(want[ord[i]], hi[i + 1] - APART(ord[i], ord[i + 1])) : want[ord[i]];
            for (int i = a; i < b; i++) v[ord[i]].cross = (lo[i] + hi[i]) / 2;
        }
    }
    free(first);
#undef APART
#undef CROSS
    free(want); free(lo); free(hi); free(stack);
    /* Boxes on the grid, frames around groups, and the whole graph moved to the origin. */
    float box[GRAPH_MAX][4], frame[50][4], min[2] = {1e30f, 1e30f}, max[2] = {-1e30f, -1e30f}, origin[2] = {200, 80};
    for (int i = 0; i < n; i++) {
        float cx = down ? v[i].cross : v[i].main, cy = down ? v[i].main : v[i].cross;
        /* Centers on the grid keep chains straight; whole units first, so nearly equal centers round alike. */
        box[i][0] = roundf(roundf(cx) / GRID) * GRID - v[i].w / 2; box[i][1] = roundf(roundf(cy) / GRID) * GRID - v[i].h / 2;
        box[i][2] = box[i][0] + v[i].w; box[i][3] = box[i][1] + v[i].h;
    }
    for (int g = 0; g < ng; g++) {
        frame[g][0] = frame[g][1] = 1e30f; frame[g][2] = frame[g][3] = -1e30f;
        for (int i = 0; i < n; i++) if (v[i].g == g) { extend(frame[g], box[i][0], box[i][1]); extend(frame[g], box[i][2], box[i][3]); }
        frame[g][0] -= 20; frame[g][1] -= 60; frame[g][2] += 20; frame[g][3] += 20; /* room for the group name on top */
    }
    for (int i = 0; i < n + ng; i++) {
        const float *b = i < n ? box[i] : frame[i - n];
        min[0] = fminf(min[0], b[0]); min[1] = fminf(min[1], b[1]); max[0] = fmaxf(max[0], b[2]); max[1] = fmaxf(max[1], b[3]);
    }
    if (nitems) { /* below what is on the board already */
        float b[4], lowest = -1e30f, left = 1e30f;
        for (int i = 0; i < nitems; i++) { bbox(&items[i], b); lowest = fmaxf(lowest, b[3]); left = fminf(left, b[0]); }
        origin[0] = fmaxf(200, roundf(left / GRID) * GRID); origin[1] = ceilf((lowest + 80) / GRID) * GRID;
    }
    if (ox) origin[0] = ox->valuedouble;
    if (oy) origin[1] = oy->valuedouble;
    float dx = origin[0] - min[0], dy = origin[1] - min[1];
    for (int i = 0; i < n + ng; i++) { float *b = i < n ? box[i] : frame[i - n]; b[0] += dx; b[2] += dx; b[1] += dy; b[3] += dy; }
    max[0] += dx; max[1] += dy;
    /* The items: frames first, so they paint behind their nodes; edges that close a cycle go around the graph. */
    cJSON *req = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(req, "items"), *it;
    for (int g = 0; g < ng; g++) {
        cJSON_AddItemToArray(list, it = cJSON_CreateObject());
        cJSON_AddStringToObject(it, "type", "rect"); cJSON_AddStringToObject(it, "id", groups[g]);
        cJSON_AddNumberToObject(it, "x0", frame[g][0]); cJSON_AddNumberToObject(it, "y0", frame[g][1]);
        cJSON_AddNumberToObject(it, "x1", frame[g][2]); cJSON_AddNumberToObject(it, "y1", frame[g][3]);
        cJSON_AddNumberToObject(it, "color", 7); cJSON_AddNumberToObject(it, "width", 2);
        cJSON_AddBoolToObject(it, "dashed", 1); cJSON_AddStringToObject(it, "label", groups[g]);
    }
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(list, it = cJSON_CreateObject());
        cJSON_AddStringToObject(it, "type", SHAPES[v[i].shape]); cJSON_AddStringToObject(it, "id", v[i].id);
        cJSON_AddNumberToObject(it, "x0", box[i][0]); cJSON_AddNumberToObject(it, "y0", box[i][1]);
        cJSON_AddNumberToObject(it, "x1", box[i][2]); cJSON_AddNumberToObject(it, "y1", box[i][3]);
        cJSON_AddNumberToObject(it, "color", v[i].color); cJSON_AddNumberToObject(it, "fill", v[i].fill);
        cJSON_AddStringToObject(it, "label", v[i].label);
        if (v[i].caption) cJSON_AddStringToObject(it, "caption", v[i].caption);
        if (v[i].g >= 0) cJSON_AddStringToObject(it, "parent", groups[v[i].g]);
        if (cJSON_IsTrue(field(v[i].o, "dashed"))) cJSON_AddBoolToObject(it, "dashed", 1);
    }
    k = 0; float around = (down ? max[0] : max[1]) + 40; /* where the next edge around the graph may run */
    for (const cJSON *o = edges ? edges->child : NULL; o; o = o->next, k++) {
        const cJSON *t = field(o, "type"), *c = field(o, "color"), *label = field(o, "label"), *elbow = field(o, "elbow");
        cJSON_AddItemToArray(list, it = cJSON_CreateObject());
        cJSON_AddStringToObject(it, "type", t ? t->valuestring : "arrow");
        cJSON_AddStringToObject(it, "from", field(o, "from")->valuestring); cJSON_AddStringToObject(it, "to", field(o, "to")->valuestring);
        if (c) cJSON_AddNumberToObject(it, "color", c->valueint);
        if (label) cJSON_AddStringToObject(it, "label", label->valuestring);
        if (cJSON_IsTrue(field(o, "dashed"))) cJSON_AddBoolToObject(it, "dashed", 1);
        if (e[k][2]) { /* below the graph (rightward) or right of it (downward), clear of its own label and the edges before */
            float h, w = label ? text_run(&canvas_font, 0, 0, 21, label->valuestring, 0, &h) : 0, half = label ? (down ? w : h) / 2 + 6 : 0;
            float out = around + half, pt[2][2]; around = out + half + 20;
            for (int s = 0; s < 2; s++) {
                const float *b = box[e[k][s]];
                pt[s][0] = down ? out : (b[0] + b[2]) / 2; pt[s][1] = down ? (b[1] + b[3]) / 2 : out;
            }
            cJSON *points = cJSON_AddArrayToObject(it, "points");
            for (int s = 0; s < 2; s++) { double p[2] = { pt[s][0], pt[s][1] }; cJSON_AddItemToArray(points, cJSON_CreateDoubleArray(p, 2)); }
        }
        if (elbow ? cJSON_IsTrue(elbow) : e[k][2] || cJSON_IsTrue(elbows)) cJSON_AddBoolToObject(it, "elbow", 1);
    }
    free(v); free(e);
    cJSON *r = edit_items(req, ADD); cJSON_Delete(req); return r;
}
static cJSON *png_stream(FILE *f) {
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size <= 0 || st.st_size > 32 * 1024 * 1024) { fclose(f); return NULL; }
    size_t n = st.st_size; unsigned char *bytes = malloc(n); char *encoded = malloc(4 * ((n + 2) / 3) + 1);
    if (!bytes || !encoded || fread(bytes, 1, n, f) != n) { fclose(f); free(bytes); free(encoded); return NULL; }
    fclose(f);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)bytes[i] << 16;
        if (i + 1 < n) v |= (unsigned)bytes[i + 1] << 8;
        if (i + 2 < n) v |= bytes[i + 2];
        encoded[j++] = alphabet[v >> 18]; encoded[j++] = alphabet[v >> 12 & 63];
        encoded[j++] = i + 1 < n ? alphabet[v >> 6 & 63] : '='; encoded[j++] = i + 2 < n ? alphabet[v & 63] : '=';
    }
    encoded[j] = 0; free(bytes);
    cJSON *image = cJSON_CreateObject();
    cJSON_AddStringToObject(image, "type", "image"); cJSON_AddStringToObject(image, "mimeType", "image/png");
    cJSON_AddStringToObject(image, "data", encoded); free(encoded); return image;
}
static cJSON *png_image(const char *filename) { return png_stream(fopen(filename, "rb")); }
#define NO_EGL "Headless rendering needs an EGL OpenGL driver, such as Mesa. See server stderr."
static cJSON *canvas_image(void) {
    if (!win) { /* headless: the whole board, fitted */
        FILE *f = tmpfile();
        if (!f) return tool_error("Cannot capture canvas.");
        if (!write_fitted(f)) { fclose(f); return tool_error(NO_EGL); }
        rewind(f); cJSON *image = png_stream(f);
        if (!image) return tool_error("Canvas preview exceeds 32 MiB or could not be read.");
        cJSON *r = tool_result(board_status()); cJSON_AddItemToArray(field(r, "content"), image); return r;
    }
    int fw, fh, ww, wh; glfwGetFramebufferSize(win, &fw, &fh); glfwGetWindowSize(win, &ww, &wh);
    if (ww <= panel_w() || wh <= 0 || fw <= 0 || fh <= 0) return tool_error("No visible canvas.");
    int x = panel_w() * fw / ww, w = fw - x; winh = wh;
    unsigned char *pixels = malloc((size_t)w * fh * 3); FILE *f = tmpfile();
    if (!pixels || !f) { free(pixels); if (f) fclose(f); return tool_error("Cannot capture canvas."); }
    glViewport(0, 0, fw, fh); render_canvas(ww, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(x, 0, w, fh, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    stbi_flip_vertically_on_write(1); int ok = write_png(f, pixels, w, fh); free(pixels);
    if (!ok) { fclose(f); return tool_error("Cannot encode canvas."); }
    rewind(f); cJSON *image = png_stream(f);
    if (!image) return tool_error("Canvas preview exceeds 32 MiB or could not be read.");
    cJSON *r = tool_result(board_status()); cJSON_AddItemToArray(field(r, "content"), image); return r;
}
static cJSON *call_tool(const char *name, const cJSON *args) {
    if (!strcmp(name, "get_status")) {
        cJSON *r = board_status();
        add_path(r); cJSON_AddNumberToObject(r, "tabs", ndocs);
        cJSON_AddBoolToObject(r, "collaborating", collaborating);
        cJSON_AddBoolToObject(r, "busy", editing >= 0 || drag != NONE || panning || custom_dialog || fd_mode || asking || disk_asking || permit_shown);
        cJSON_AddStringToObject(r, "status", collaboration_status);
        cJSON_AddStringToObject(r, "automatic_agent", selected_agent < 0 ? "none" : agent_ids[selected_agent]);
        cJSON_AddBoolToObject(r, "agent_scanning", agent_scanning);
        cJSON *agents = cJSON_AddArrayToObject(r, "available_agents");
        for (int i = 0; i < NAGENTS; i++) if (*agent_paths[i]) cJSON_AddItemToArray(agents, cJSON_CreateString(agent_ids[i]));
        cJSON_AddBoolToObject(r, "custom_agent_configured", *custom_command != 0);
        cJSON_AddStringToObject(r, "custom_transport", custom_transports[custom_transport]);
        cJSON_AddStringToObject(r, "custom_agent_error", custom_error);
        return tool_result(r);
    }
    if (!strcmp(name, "measure_text")) { /* reads no board, so it works while collaboration is off */
        const cJSON *text = field(args, "text"), *size = field(args, "width");
        if (!keys(args, "|text|width|") || !cJSON_IsString(text) || !*text->valuestring || strlen(text->valuestring) > 65536
            || (size && !number(size, 1, 64, 0))) return tool_error("Expected text and an optional width between 1 and 64.");
        float px = (size ? size->valuedouble : 3) * 7, h, w = text_run(&canvas_font, 0, 0, px, text->valuestring, 0, &h);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "width", fmaxf(w, px * .3f)); cJSON_AddNumberToObject(r, "height", h);
        return tool_result(r);
    }
    if (!collaborating && !permit_once) return tool_error("Collaboration is off. Enable Collaborate in the Paintboard window.");
    if (!strcmp(name, "_agent_status")) {
        const cJSON *epoch = field(args, "collaboration_epoch"), *message = field(args, "status");
        if (!number(epoch, 0, 9007199254740991., 1) || epoch->valuedouble != collaboration_epoch || !cJSON_IsString(message)) return tool_error("Stale status.");
        snprintf(collaboration_status, sizeof collaboration_status, "%.60s", message->valuestring);
        return tool_result(board_status());
    }
    if (editing >= 0 || drag != NONE || panning || fd_mode || asking || disk_asking || pending)
        return tool_error("Canvas is busy with a human edit or dialog. Retry after it finishes.");
    if (!strcmp(name, "get_canvas")) return canvas_image();
    if (!strcmp(name, "list_tabs")) {
        if (!keys(args, "")) return tool_error("This tool takes no arguments.");
        cJSON *r = board_status(), *tabs = cJSON_AddArrayToObject(r, "tabs");
        doc_park();
        for (int i = 0; i < ndocs; i++) {
            cJSON *tab = cJSON_CreateObject();
            cJSON_AddNumberToObject(tab, "index", i);
            if (docs[i].path) cJSON_AddStringToObject(tab, "path", docs[i].path); else cJSON_AddNullToObject(tab, "path");
            cJSON_AddBoolToObject(tab, "active", i == cur); cJSON_AddBoolToObject(tab, "unsaved", docs[i].dirty);
            cJSON_AddNumberToObject(tab, "total", docs[i].nitems); cJSON_AddItemToArray(tabs, tab);
        }
        cJSON_AddNumberToObject(r, "active", cur); return tool_result(r);
    }
    if (!strcmp(name, "switch_tab") || !strcmp(name, "close_tab")) {
        const cJSON *index = field(args, "index"), *discard = field(args, "discard"); int closing = !strcmp(name, "close_tab");
        if (!keys(args, closing ? "|index|discard|" : "|index|") || !number(index, 0, ndocs - 1, 1) || (discard && !cJSON_IsBool(discard)))
            return tool_error("Expected index of an open tab; see list_tabs.");
        int i = index->valueint;
        if (!closing) doc_switch(i);
        else {
            doc_park();
            if (!cJSON_IsTrue(discard) && (docs[i].dirty || (!docs[i].path && docs[i].nitems)))
                return tool_error("The tab has unsaved changes. Call save_board on it first, or pass discard: true.");
            doc_remove(i);
            if (!ndocs) doc_add(NULL);
        }
        cJSON *r = board_status(); add_path(r); cJSON_AddNumberToObject(r, "tabs", ndocs); cJSON_AddNumberToObject(r, "active", cur);
        return tool_result(r);
    }
    if (!strcmp(name, "new_board") || !strcmp(name, "open_board")) {
        const cJSON *file = field(args, "path"), *again = field(args, "reload"); int opening = !strcmp(name, "open_board");
        if (!keys(args, opening ? "|path|reload|" : "") || (opening && (!cJSON_IsString(file) || !*file->valuestring)) || (again && !cJSON_IsBool(again)))
            return tool_error(opening ? "Expected path, absolute or relative to ~/Documents/Paintboard, and an optional reload." : "This tool takes no arguments.");
        if (!opening) doc_add(NULL);
        else {
            char *target = board_file(file->valuestring); int open = find_tab(target) >= 0; *notice = 0;
            int failed = open_path(target) || (open && cJSON_IsTrue(again) && reload()); free(target); /* a new tab is fresh already */
            if (failed) return tool_error(notice);
        }
        cJSON *r = board_status(); add_path(r); cJSON_AddNumberToObject(r, "tabs", ndocs); cJSON_AddNumberToObject(r, "active", cur);
        return tool_result(r);
    }
    if (!strcmp(name, "get_board")) return get_board(args);
    if (!strcmp(name, "add_items")) return edit_items(args, ADD);
    if (!strcmp(name, "replace_items")) return edit_items(args, REPLACE);
    if (!strcmp(name, "add_graph")) return add_graph(args);
    if (!strcmp(name, "update_items")) return edit_items(args, UPDATE);
    if (!strcmp(name, "delete_items")) return remove_items(args);
    if (!strcmp(name, "undo") || !strcmp(name, "redo")) {
        if (!keys(args, "|revision|") || !current_revision(args)) return tool_error("Missing or stale revision. Call get_board and retry with its revision.");
        int is_undo = !strcmp(name, "undo");
        if (is_undo ? !nundo : !nredo) return tool_error("No history available.");
        if (is_undo) do_undo(); else do_redo();
        return tool_result(board_status());
    }
    if (!strcmp(name, "set_view")) {
        const cJSON *x = field(args, "panx"), *y = field(args, "pany"), *z = field(args, "zoom");
        if (!keys(args, "|panx|pany|zoom|") || !number(x, -1e6, 1e6, 0) || !number(y, -1e6, 1e6, 0) || !number(z, .05, 50, 0))
            return tool_error("Expected finite panx/pany within +/-1000000 and zoom between 0.05 and 50.");
        panx = x->valuedouble; pany = y->valuedouble; zoom = z->valuedouble; return tool_result(board_status());
    }
    if (!strcmp(name, "export_svg")) {
        if (!keys(args, "")) return tool_error("This tool takes no arguments.");
        if (!path) return tool_error("The active tab is Untitled; the SVG goes next to the board file. Call save_board with a path first.");
        *notice = 0;
        if (export_svg()) return tool_error(notice);
        cJSON *r = board_status(); char *out = export_path(path, ".svg");
        cJSON_AddStringToObject(r, "path", out); free(out); return tool_result(r);
    }
    cJSON *r;
    if (!strcmp(name, "save_board")) {
        const cJSON *file = field(args, "path"), *overwrite = field(args, "overwrite");
        if (!keys(args, "|path|overwrite|") || (file && (!cJSON_IsString(file) || !*file->valuestring)) || (overwrite && !cJSON_IsBool(overwrite)))
            return tool_error("Expected an optional path and an optional overwrite.");
        if (!file && !path) return tool_error("The active tab is Untitled. Pass path to choose its file.");
        char *old = path; Stamp was = disk; int saved;
        if (file) {
            char *target = board_file(file->valuestring); int other = find_tab(target);
            if (other >= 0 && other != cur) { free(target); return tool_error("That file is open in another tab."); }
            path = target; disk = (Stamp){0}; /* the stamp belonged to the old file */
        }
        if ((saved = save(cJSON_IsTrue(overwrite))) != 0) {
            if (file) { free(path); path = old; disk = was; }
            return tool_error(saved == -3 ? "The file changed on disk since this tab read or saved it, so nothing was written. "
                                            "Call open_board with its path and reload: true to load that version, or save_board with overwrite: true to replace it."
                                          : "Save failed; the previous board file was preserved. See server stderr for details.");
        }
        if (file) free(old);
        r = board_status(); cJSON_AddStringToObject(r, "path", path);
    } else {
        if (!keys(args, "")) return tool_error("This tool takes no arguments.");
        if (!path) return tool_error("The active tab is Untitled; the PNG goes next to the board file. Call save_board with a path first.");
        char *out = export_path(path, ".png");
        if (!win) { /* headless: the whole board, fitted */
            FILE *f = fopen(out, "wb"); int ok = f && write_fitted(f);
            if (f && fclose(f)) ok = 0;
            if (!ok) { if (f) unlink(out); free(out); return tool_error(NO_EGL); }
        } else if (export_png()) { free(out); return tool_error("PNG export failed. See server stderr for details."); }
        r = board_status(); cJSON_AddStringToObject(r, "path", out);
        cJSON *image = png_image(out); free(out); r = tool_result(r);
        if (image) cJSON_AddItemToArray(field(r, "content"), image);
        return r;
    }
    return tool_result(r);
}
static void rpc_reply(const cJSON *id, cJSON *result, int code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0"); cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    if (code) { cJSON *err = cJSON_AddObjectToObject(r, "error"); cJSON_AddNumberToObject(err, "code", code); cJSON_AddStringToObject(err, "message", message); }
    else cJSON_AddItemToObject(r, "result", result);
    char *s = cJSON_PrintUnformatted(r);
    FILE *out = mcp_output ? mcp_output : stdout;
    if (!s || fprintf(out, "%s\n", s) < 0 || fflush(out) != 0) mcp_output_failed = 1;
    free(s); cJSON_Delete(r);
}
static void mcp_dispatch(const char *line) {
    /* cJSON strings cannot represent embedded NULs. Reject rather than truncate. */
    for (const char *p = line; *p; p++) if (*p == '\\' && p[1]) {
        if (!strncmp(p + 1, "u0000", 5)) { rpc_reply(NULL, NULL, -32700, "NUL characters are not supported"); return; }
        p++;
    }
    cJSON *req = cJSON_ParseWithOpts(line, NULL, 1);
    if (!req) { rpc_reply(NULL, NULL, -32700, "Parse error (maximum message size is 1 MiB)"); return; }
    const cJSON *id = field(req, "id"), *method = field(req, "method"), *params = field(req, "params");
    if (!cJSON_IsObject(req) || !jstr(field(req, "jsonrpc"), "2.0") || !cJSON_IsString(method)
        || (id && !cJSON_IsString(id) && !number(id, -9007199254740991., 9007199254740991., 1)) || (params && !cJSON_IsObject(params))) {
        rpc_reply(NULL, NULL, -32600, "Invalid request"); goto done;
    }
    if (!id) { if (jstr(method, "notifications/initialized") && mcp_phase == 1) mcp_phase = 2; goto done; }
    if (jstr(method, "ping")) { rpc_reply(id, cJSON_CreateObject(), 0, NULL); goto done; }
    if (jstr(method, "initialize")) {
        const cJSON *version = field(params, "protocolVersion"), *client = field(params, "clientInfo");
        if (mcp_phase || !cJSON_IsString(version) || !cJSON_IsObject(field(params, "capabilities"))
            || !cJSON_IsString(field(client, "name")) || !cJSON_IsString(field(client, "version"))) {
            rpc_reply(id, NULL, -32602, "Invalid initialize parameters or session already initialized"); goto done;
        }
        const char *v = "2025-11-25";
        if (jstr(version, "2024-11-05") || jstr(version, "2025-03-26") || jstr(version, "2025-06-18")) v = version->valuestring;
        cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, "protocolVersion", v);
        cJSON *caps = cJSON_AddObjectToObject(r, "capabilities"); cJSON_AddObjectToObject(caps, "tools");
        cJSON_AddObjectToObject(caps, "resources"); cJSON_AddObjectToObject(caps, "prompts");
        cJSON *info = cJSON_AddObjectToObject(r, "serverInfo"); cJSON_AddStringToObject(info, "name", "paintboard"); cJSON_AddStringToObject(info, "version", VERSION);
        cJSON_AddStringToObject(r, "instructions", "Paintboard is a whiteboard you draw on. The " GUIDE_URI " resource and the paintboard_guide prompt explain coordinates, styles, and layout with an example. Use get_board first. For boxes and arrows, send add_graph with nodes and edges; it does the layout. In add_items, give items an id and a label, and connect them with arrows using from/to, so you need no coordinate math; check the result with get_canvas. Ids stay in the file: update_items and delete_items take them without a revision, and replace_items swaps the whole board. Batch edits share undo history with the human. Edits that use board indices, and undo/redo, require the latest revision. Retry busy errors after the human finishes. Tools act on the active tab; list_tabs, switch_tab, open_board, and new_board change it, and a switch changes the revision. Save explicitly with save_board; it refuses to replace a file that another program saved meanwhile. This session owns the board file; do not open a second process on it.");
        mcp_phase = 1; rpc_reply(id, r, 0, NULL); goto done;
    }
    if (mcp_phase != 2) { rpc_reply(id, NULL, -32600, "Initialize the session first"); goto done; }
    if (jstr(method, "tools/list")) {
        if (field(params, "cursor")) { rpc_reply(id, NULL, -32602, "No pagination cursor is supported"); goto done; }
        cJSON *r = cJSON_CreateObject(), *list = cJSON_Duplicate(field(mcp_spec, "tools"), 1);
        for (cJSON *t = list->child; t; t = t->next) {
            if (jstr(field(t, "name"), "add_items") || jstr(field(t, "name"), "update_items") || jstr(field(t, "name"), "replace_items")) {
                cJSON *defs = cJSON_AddObjectToObject(field(t, "inputSchema"), "$defs");
                cJSON_AddItemToObject(defs, "item", cJSON_Duplicate(field(mcp_spec, "itemSchema"), 1));
            }
        }
        cJSON_AddItemToObject(r, "tools", list); rpc_reply(id, r, 0, NULL);
    } else if (jstr(method, "resources/list") || jstr(method, "prompts/list")) {
        int prompts = jstr(method, "prompts/list");
        cJSON *r = cJSON_CreateObject(), *entry = cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_AddArrayToObject(r, prompts ? "prompts" : "resources"), entry);
        if (prompts) cJSON_AddStringToObject(entry, "name", "paintboard_guide");
        else { cJSON_AddStringToObject(entry, "uri", GUIDE_URI); cJSON_AddStringToObject(entry, "name", "guide"); cJSON_AddStringToObject(entry, "mimeType", "text/markdown"); }
        cJSON_AddStringToObject(entry, "title", "Drawing on Paintboard");
        cJSON_AddStringToObject(entry, "description", "Workflow, coordinates, item types, palette, layout tips, and an example diagram.");
        rpc_reply(id, r, 0, NULL);
    } else if (jstr(method, "resources/read") || jstr(method, "prompts/get")) {
        int prompt = jstr(method, "prompts/get");
        if (!(prompt ? jstr(field(params, "name"), "paintboard_guide") : jstr(field(params, "uri"), GUIDE_URI))) {
            rpc_reply(id, NULL, -32602, prompt ? "Unknown prompt" : "Unknown resource"); goto done;
        }
        cJSON *r = cJSON_CreateObject(), *entry = cJSON_CreateObject();
        if (prompt) {
            cJSON *content = cJSON_AddObjectToObject(entry, "content");
            cJSON_AddStringToObject(entry, "role", "user");
            cJSON_AddStringToObject(content, "type", "text"); cJSON_AddStringToObject(content, "text", GUIDE);
            cJSON_AddItemToArray(cJSON_AddArrayToObject(r, "messages"), entry);
        } else {
            cJSON_AddStringToObject(entry, "uri", GUIDE_URI); cJSON_AddStringToObject(entry, "mimeType", "text/markdown");
            cJSON_AddStringToObject(entry, "text", GUIDE); cJSON_AddItemToArray(cJSON_AddArrayToObject(r, "contents"), entry);
        }
        rpc_reply(id, r, 0, NULL);
    } else if (jstr(method, "tools/call")) {
        const cJSON *name = field(params, "name"), *args = field(params, "arguments"); int found = 0;
        for (const cJSON *t = field(mcp_spec, "tools")->child; t; t = t->next) if (jstr(name, field(t, "name")->valuestring)) found = 1;
        if (!found || (args && !cJSON_IsObject(args))) { rpc_reply(id, NULL, -32602, "Unknown tool or invalid arguments"); goto done; }
        cJSON *empty = args ? NULL : cJSON_CreateObject();
        agent_mutation = 1;
        rpc_reply(id, call_tool(name->valuestring, args ? args : empty), 0, NULL); cJSON_Delete(empty);
        agent_mutation = 0;
    } else rpc_reply(id, NULL, -32601, "Method not found");
done:
    cJSON_Delete(req);
}
static char *mcp_read_stream(FILE *input) {
    char *line = malloc(MCP_MAX_LINE + 1); if (!line) return NULL;
    size_t n = 0; int c, invalid = 0;
    while ((c = fgetc(input)) != EOF && c != '\n') {
        if (!c || n == MCP_MAX_LINE) invalid = 1;
        else line[n++] = c;
    }
    if (c == EOF && !n && !invalid) { free(line); return NULL; }
    line[invalid ? 0 : n] = 0; return line;
}
static char *mcp_read_line(void) { return mcp_read_stream(stdin); }
static void *mcp_reader(void *unused) {
    (void)unused;
    for (;;) {
        char *line = mcp_read_line();
        pthread_mutex_lock(&mcp_mutex);
        while (mcp_pending) pthread_cond_wait(&mcp_cond, &mcp_mutex);
        if (line) mcp_pending = line; else mcp_eof = 1;
        pthread_mutex_unlock(&mcp_mutex); glfwPostEmptyEvent();
        if (!line) return NULL;
    }
}
static int mcp_pump(void) {
    pthread_mutex_lock(&mcp_mutex);
    char *line = mcp_pending; mcp_pending = NULL; int eof = mcp_eof;
    pthread_cond_signal(&mcp_cond); pthread_mutex_unlock(&mcp_mutex);
    if (line) { mcp_dispatch(line); free(line); }
    return eof || mcp_output_failed;
}
static int mcp_start(void) {
    signal(SIGPIPE, SIG_IGN);
    mcp_spec = cJSON_Parse(MCP_SPEC);
    if (!mcp_spec) { fprintf(stderr, "invalid embedded MCP schema\n"); return -1; }
    if (!headless) {
        int err = pthread_create(&mcp_thread, NULL, mcp_reader, NULL);
        if (err) { fprintf(stderr, "MCP reader: %s\n", strerror(err)); return -1; }
    }
    return 0;
}
static void mcp_stop(void) {
    /* Stop the input thread before tearing down GLFW, including when stdin is
       still open and the human closes the window. fgetc/cond_wait are cancellable. */
    pthread_cancel(mcp_thread); pthread_join(mcp_thread, NULL);
    free(mcp_pending); cJSON_Delete(mcp_spec);
}
