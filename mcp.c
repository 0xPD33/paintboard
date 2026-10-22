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
static const char *ITEM_TYPE[] = {"pen", "line", "arrow", "rect", "ellipse", "text", "diamond", "triangle", "star", "highlight"};
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
static cJSON *item_json(const Item *it) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", ITEM_TYPE[it->type]);
    cJSON_AddNumberToObject(r, "color", it->color); cJSON_AddNumberToObject(r, "width", it->width);
    if (fillable(it->type)) cJSON_AddNumberToObject(r, "fill", it->fill);
    if (is_pen(it->type)) {
        cJSON *points = cJSON_AddArrayToObject(r, "points");
        for (int k = 0; k < it->np; k++) {
            float x, y; pen_pt(it, k, &x, &y); double pair[2] = {x, y};
            cJSON_AddItemToArray(points, cJSON_CreateDoubleArray(pair, 2));
        }
    } else {
        cJSON_AddNumberToObject(r, "x0", it->x0); cJSON_AddNumberToObject(r, "y0", it->y0);
        if (it->type == TEXT) cJSON_AddStringToObject(r, "text", tpool + it->p0);
        else { cJSON_AddNumberToObject(r, "x1", it->x1); cJSON_AddNumberToObject(r, "y1", it->y1); }
    }
    if (it->from) cJSON_AddNumberToObject(r, "from", it->from - 1);
    if (it->to) cJSON_AddNumberToObject(r, "to", it->to - 1);

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
/* NULL when the item is valid, else the reason. add allows the add_items extras: id and label. */
static const char *parse_item(const cJSON *o, Item *it, int add) {
    static char why[200];
    *it = (Item){.type = -1, .fill = -1, .width = 3, .sx = 1, .sy = 1};
    for (int i = 0; i < SELECT; i++) if (jstr(field(o, "type"), ITEM_TYPE[i])) it->type = i;
    if (it->type < 0) return "type must be pen, line, arrow, rect, ellipse, text, diamond, triangle, star, or highlight";
    int linkable = connector(it->type);
    const char *allowed = is_pen(it->type) ? "|type|color|width|points|"
        : it->type == TEXT ? "|type|color|width|x0|y0|text|"
        : fillable(it->type) ? "|type|color|width|fill|x0|y0|x1|y1|" : "|type|color|width|x0|y0|x1|y1|";
    const char *extra = !add ? (linkable ? "from|to|" : "") : fillable(it->type) ? "id|label|label_size|" : linkable ? "id|from|to|" : "id|";
    char fields[120]; snprintf(fields, sizeof fields, "%s%s", allowed, extra);
    if (!keys(o, fields)) { snprintf(why, sizeof why, "unknown or repeated field; %s takes %s", ITEM_TYPE[it->type], fields); return why; }
    const cJSON *v = field(o, "color");
    if (v) { if (!number(v, 0, 7, 1)) return "color must be an integer 0-7"; it->color = v->valueint; }
    v = field(o, "fill"); if (v) { if (!number(v, -1, 7, 1)) return "fill must be an integer -1-7"; it->fill = v->valueint; }
    v = field(o, "width"); if (v) { if (!number(v, 1, 64, 0)) return "width must be a number 1-64"; it->width = v->valuedouble; }
    v = field(o, "id"); if (v && (!cJSON_IsString(v) || !*v->valuestring || strlen(v->valuestring) > 64)) return "id must be a string of 1-64 bytes";
    if (field(o, "label") && !text_ok(field(o, "label"))) return "label must be 1-65536 bytes of text without control characters other than newline";
    v = field(o, "label_size"); if (v && (!field(o, "label") || !number(v, 1, 64, 0))) return "label_size needs a label and must be a number 1-64";

    if (is_pen(it->type)) {
        const cJSON *pts = field(o, "points"); int n = cJSON_GetArraySize(pts);
        if (!cJSON_IsArray(pts) || n < 1 || n > 10000 || npool + 2 * n >= 1 << 26) return "points must be an array of 1-10000 [x, y] pairs";
        it->p0 = npool; it->np = n;
        for (const cJSON *pt = pts->child; pt; pt = pt->next) {
            if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) != 2 || !number(pt->child, -1e6, 1e6, 0) || !number(pt->child->next, -1e6, 1e6, 0))
                return "each point must be [x, y] within +/-1000000";
            float x = pt->child->valuedouble, y = pt->child->next->valuedouble;
            if (pt == pts->child) { it->x0 = it->x1 = x; it->y0 = it->y1 = y; }
            add_pt(x - it->x0, y - it->y0);
        }
    } else {
        const char *names[] = {"x0", "y0", "x1", "y1"}; float coords[4] = {0};
        for (int i = 0; i < (it->type == TEXT ? 2 : 4); i++) {
            v = field(o, names[i]);
            if (!v && linkable && field(o, i < 2 ? "from" : "to")) continue; /* relink places a linked end */
            if (!number(v, -1e6, 1e6, 0)) return it->type == TEXT ? "x0 and y0 must be numbers within +/-1000000"
                : linkable ? "x0, y0 (or from) and x1, y1 (or to) must be numbers within +/-1000000"
                : "x0, y0, x1, y1 must be numbers within +/-1000000";
            coords[i] = v->valuedouble;
        }
        it->x0 = coords[0]; it->y0 = coords[1]; it->x1 = coords[2]; it->y1 = coords[3];
        if (it->type == TEXT) {
            v = field(o, "text");
            if (!text_ok(v)) return "text must be 1-65536 bytes without control characters other than newline";
            size_t n = strlen(v->valuestring); if (ntpool + n + 1 >= 1 << 26) return "the board's text storage is full";
            it->p0 = ntpool; it->np = n; tpool = grow(tpool, &captpool, ntpool + n + 1, 1);
            memcpy(tpool + ntpool, v->valuestring, n + 1); ntpool += n + 1; measure(it);
        }
    }
    return NULL;
}
/* A from/to value: board index i as i + 1, earlier batch item j as -(j + 1), 0 when absent, INT_MIN when invalid. */
static int ref_link(const cJSON *v, int k, const char **ids, int self) {
    if (!v) return 0;
    if (number(v, 0, nitems - 1, 1) && v->valueint != self) return v->valueint + 1;
    if (ids && cJSON_IsString(v)) for (int j = k - 1; j >= 0; j--) if (ids[j] && !strcmp(ids[j], v->valuestring)) return -(j + 1);
    return INT_MIN;
}
static void add_label(const Item *shape, const char *text, float size) { /* black text centered on the shape; every fill is light */
    size_t n = strlen(text);
    Item t = { .type = TEXT, .fill = -1, .width = size, .sx = 1, .sy = 1, .p0 = ntpool, .np = n };
    tpool = grow(tpool, &captpool, ntpool + n + 2, 1); memcpy(tpool + ntpool, text, n + 1); ntpool += n + 1;
    measure(&t); float w = t.x1 - t.x0, h = t.y1 - t.y0;
    t.x0 = (shape->x0 + shape->x1 - w) / 2; t.y0 = (shape->y0 + shape->y1 - h) / 2; measure(&t);
    add_item(t);
}
static int current_revision(const cJSON *args) {
    const cJSON *r = field(args, "revision");
    return number(r, 0, 9007199254740991., 1) && r->valuedouble == (double)revision;
}
static cJSON *edit_items(const cJSON *args, int replace) {
    if (!keys(args, replace ? "|revision|items|" : "|items|revision|collaboration_epoch|")) return tool_error("Unexpected arguments.");
    if (!replace && field(args, "revision") && !current_revision(args)) return tool_error("Stale revision; read the canvas again.");
    const cJSON *epoch = field(args, "collaboration_epoch");
    if (epoch && (!number(epoch, 0, 9007199254740991., 1) || epoch->valuedouble != collaboration_epoch)) return tool_error("Collaboration session changed; discard this response.");
    if (replace && !current_revision(args)) return tool_error("Missing or stale revision. Call get_board and retry with its revision.");
    const cJSON *list = field(args, "items"); int n = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || n < 1 || n > 1000 || (!replace && nitems + 2 * n >= 1 << 20)) return tool_error("Expected 1 to 1000 items within the board capacity.");
    Item *staged = malloc(n * sizeof *staged); int indices[1000], links[1000][2], k = 0, oldnp = npool, oldnt = ntpool;
    const char *ids[1000], *labels[1000], *why = NULL; float label_size[1000]; size_t label_bytes = 0; char message[300];
    if (!staged) return tool_error("Out of memory.");
    for (const cJSON *entry = list->child; entry; entry = entry->next, k++) {
        indices[k] = nitems + k;
        if (replace) {
            const cJSON *idx = field(entry, "index");
            if (!keys(entry, "|index|item|") || !number(idx, 0, nitems - 1, 1)) { why = "expected {index, item} with an existing index"; goto invalid; }
            indices[k] = idx->valueint;
            for (int j = 0; j < k; j++) if (indices[j] == indices[k]) { why = "the index repeats in this batch"; goto invalid; }
        }
        const cJSON *o = replace ? field(entry, "item") : entry;
        if ((why = parse_item(o, &staged[k], !replace))) goto invalid;
        links[k][0] = ref_link(field(o, "from"), k, replace ? NULL : ids, replace ? indices[k] : -1);
        links[k][1] = ref_link(field(o, "to"), k, replace ? NULL : ids, replace ? indices[k] : -1);
        if (links[k][0] == INT_MIN || links[k][1] == INT_MIN) {
            why = replace ? "from and to must be indices of other board items"
                          : "from and to must be existing board indices or ids of earlier items in this batch";
            goto invalid;
        }
        if (replace) continue;
        const cJSON *id = field(o, "id"), *label = field(o, "label"), *size = field(o, "label_size");
        ids[k] = id ? id->valuestring : NULL; labels[k] = label ? label->valuestring : NULL; label_size[k] = size ? size->valuedouble : 3;
        for (int j = 0; ids[k] && j < k; j++) if (ids[j] && !strcmp(ids[j], ids[k])) { why = "the id repeats in this batch"; goto invalid; }
        if (labels[k]) label_bytes += strlen(labels[k]) + 1;
    }
    if (ntpool + label_bytes >= 1 << 26) { k = n - 1; why = "the board's text storage is full"; goto invalid; }
    checkpoint();
    for (k = 0; k < n; k++) { /* batch links resolve here: every earlier item already has its final index */
        for (int e = 0; e < 2; e++) if (links[k][e] < 0) links[k][e] = indices[-links[k][e] - 1] + 1;
        staged[k].from = links[k][0]; staged[k].to = links[k][1];
        if (replace) { staged[k].sel = items[indices[k]].sel; items[indices[k]] = staged[k]; continue; }
        add_item(staged[k]); indices[k] = nitems - 1;
        if (labels[k]) add_label(&staged[k], labels[k], label_size[k]);
    }
    free(staged); relink();
    cJSON *r = board_status(), *bounds = cJSON_AddArrayToObject(r, "bounds");
    cJSON_AddItemToObject(r, "indices", cJSON_CreateIntArray(indices, n));
    for (k = 0; k < n; k++) { float b[4]; bbox(&items[indices[k]], b); cJSON_AddItemToArray(bounds, cJSON_CreateFloatArray(b, 4)); }
    return tool_result(r);
invalid:
    npool = oldnp; ntpool = oldnt; free(staged);
    snprintf(message, sizeof message, "items[%d]: %s. Nothing changed.", k, why);
    return tool_error(message);
}
static cJSON *remove_items(const cJSON *args) {
    if (!keys(args, "|revision|indices|")) return tool_error("Unexpected arguments.");
    if (!current_revision(args)) return tool_error("Missing or stale revision. Call get_board and retry with its revision.");
    const cJSON *list = field(args, "indices"); int n = cJSON_GetArraySize(list), indices[1000], k = 0;
    if (!cJSON_IsArray(list) || n < 1 || n > 1000) return tool_error("Expected 1 to 1000 indices.");
    for (const cJSON *v = list->child; v; v = v->next, k++) {
        if (!number(v, 0, nitems - 1, 1)) return tool_error("Item index out of range.");
        indices[k] = v->valueint;
        for (int j = 0; j < k; j++) if (indices[j] == indices[k]) return tool_error("Duplicate item index.");
    }
    checkpoint(); char *gone = calloc(nitems + 1, 1);
    for (k = 0; k < n; k++) gone[indices[k]] = 1;
    compact(gone); free(gone); return tool_result(board_status());
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
        cJSON_AddBoolToObject(r, "busy", editing >= 0 || drag != NONE || panning || custom_dialog || fd_mode || asking);
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
    if (!collaborating) return tool_error("Collaboration is off. Enable Collaborate in the Paintboard window.");
    if (!strcmp(name, "_agent_status")) {
        const cJSON *epoch = field(args, "collaboration_epoch"), *message = field(args, "status");
        if (!number(epoch, 0, 9007199254740991., 1) || epoch->valuedouble != collaboration_epoch || !cJSON_IsString(message)) return tool_error("Stale status.");
        snprintf(collaboration_status, sizeof collaboration_status, "%.60s", message->valuestring);
        return tool_result(board_status());
    }
    if (editing >= 0 || drag != NONE || panning || fd_mode || asking || pending)
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
        const cJSON *file = field(args, "path"); int opening = !strcmp(name, "open_board");
        if (!keys(args, opening ? "|path|" : "") || (opening && (!cJSON_IsString(file) || !*file->valuestring)))
            return tool_error(opening ? "Expected path, absolute or relative to the server's directory." : "This tool takes no arguments.");
        if (!opening) doc_add(NULL);
        else {
            char *target = absolute(file->valuestring); *notice = 0; int failed = open_path(target); free(target);
            if (failed) return tool_error(notice);
        }
        cJSON *r = board_status(); add_path(r); cJSON_AddNumberToObject(r, "tabs", ndocs); cJSON_AddNumberToObject(r, "active", cur);
        return tool_result(r);
    }
    if (!strcmp(name, "get_board")) return get_board(args);
    if (!strcmp(name, "add_items")) return edit_items(args, 0);
    if (!strcmp(name, "update_items")) return edit_items(args, 1);
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

        const cJSON *file = field(args, "path");
        if (!keys(args, "|path|") || (file && (!cJSON_IsString(file) || !*file->valuestring))) return tool_error("Expected an optional path.");
        if (!file && !path) return tool_error("The active tab is Untitled. Pass path to choose its file.");
        char *old = path;
        if (file) {
            char *target = absolute(file->valuestring); int other = find_tab(target);
            if (other >= 0 && other != cur) { free(target); return tool_error("That file is open in another tab."); }
            path = target;
        }
        if (save()) {
            if (file) { free(path); path = old; }
            return tool_error("Save failed; the previous board file was preserved. See server stderr for details.");
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
        cJSON_AddStringToObject(r, "instructions", "Paintboard is a whiteboard you draw on with add_items. The " GUIDE_URI " resource and the paintboard_guide prompt explain coordinates, styles, and layout with an example. Use get_board first. In add_items, give shapes an id and a label, and connect them with arrows using from/to, so you need no coordinate math; check the result with get_canvas. Batch edits share undo history with the human. Update/delete/undo/redo require the latest revision. Retry busy errors after the human finishes. Tools act on the active tab; list_tabs, switch_tab, open_board, and new_board change it, and a switch changes the revision. Save explicitly with save_board. This session owns the board file; do not open a second process on it.");
        mcp_phase = 1; rpc_reply(id, r, 0, NULL); goto done;
    }
    if (mcp_phase != 2) { rpc_reply(id, NULL, -32600, "Initialize the session first"); goto done; }
    if (jstr(method, "tools/list")) {
        if (field(params, "cursor")) { rpc_reply(id, NULL, -32602, "No pagination cursor is supported"); goto done; }
        cJSON *r = cJSON_CreateObject(), *list = cJSON_Duplicate(field(mcp_spec, "tools"), 1);
        for (cJSON *t = list->child; t; t = t->next) {
            if (jstr(field(t, "name"), "add_items") || jstr(field(t, "name"), "update_items")) {
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
