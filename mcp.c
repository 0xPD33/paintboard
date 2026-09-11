/* Included by paintboard.c: MCP stdio transport and tools share the board's
   mutation/undo functions. Only the main thread ever touches board or GL state. */
#include <cjson/cJSON.h>
#include <pthread.h>

#define MCP_MAX_LINE (1024 * 1024)
static const char MCP_SPEC[] = {
#embed "mcp-tools.json"
    , 0
};
static const char *ITEM_TYPE[] = {"pen", "line", "arrow", "rect", "ellipse", "text"};
static int mcp_mode, headless, mcp_phase, mcp_output_failed;
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
static cJSON *item_json(const Item *it) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", ITEM_TYPE[it->type]);
    cJSON_AddNumberToObject(r, "color", it->color); cJSON_AddNumberToObject(r, "width", it->width);
    if (it->type == RECT || it->type == ELLIPSE) cJSON_AddNumberToObject(r, "fill", it->fill);
    if (it->type == PEN) {
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
    return r;
}
static cJSON *get_board(const cJSON *args) {
    const cJSON *off = field(args, "offset"), *lim = field(args, "limit");
    if (!keys(args, "|offset|limit|") || (off && !number(off, 0, 1 << 20, 1)) || (lim && !number(lim, 1, 1000, 1)))
        return tool_error("Expected offset >= 0 and limit between 1 and 1000.");
    int offset = off ? off->valueint : 0, limit = lim ? lim->valueint : 200;
    cJSON *r = board_status(), *list = cJSON_AddArrayToObject(r, "items"), *view = cJSON_AddObjectToObject(r, "view");
    cJSON_AddStringToObject(r, "path", path); cJSON_AddBoolToObject(r, "headless", headless);
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
static int parse_item(const cJSON *o, Item *it) {
    *it = (Item){.type = -1, .fill = -1, .width = 3, .sx = 1, .sy = 1};
    for (int i = 0; i < SELECT; i++) if (jstr(field(o, "type"), ITEM_TYPE[i])) it->type = i;
    if (it->type < 0) return 0;
    const char *allowed = it->type == PEN ? "|type|color|width|points|"
        : it->type == TEXT ? "|type|color|width|x0|y0|text|"
        : it->type == RECT || it->type == ELLIPSE ? "|type|color|width|fill|x0|y0|x1|y1|" : "|type|color|width|x0|y0|x1|y1|";
    if (!keys(o, allowed)) return 0;
    const cJSON *v = field(o, "color");
    if (v) { if (!number(v, 0, 7, 1)) return 0; it->color = v->valueint; }
    v = field(o, "fill"); if (v) { if (!number(v, -1, 7, 1)) return 0; it->fill = v->valueint; }
    v = field(o, "width"); if (v) { if (!number(v, 1, 64, 0)) return 0; it->width = v->valuedouble; }
    if (it->type == PEN) {
        const cJSON *pts = field(o, "points"); int n = cJSON_GetArraySize(pts);
        if (!cJSON_IsArray(pts) || n < 1 || n > 10000 || npool + 2 * n >= 1 << 26) return 0;
        it->p0 = npool; it->np = n;
        for (const cJSON *pt = pts->child; pt; pt = pt->next) {
            if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) != 2 || !number(pt->child, -1e6, 1e6, 0) || !number(pt->child->next, -1e6, 1e6, 0)) return 0;
            float x = pt->child->valuedouble, y = pt->child->next->valuedouble;
            if (pt == pts->child) { it->x0 = it->x1 = x; it->y0 = it->y1 = y; }
            add_pt(x - it->x0, y - it->y0);
        }
    } else {
        const char *names[] = {"x0", "y0", "x1", "y1"}; float coords[4] = {0};
        for (int i = 0; i < (it->type == TEXT ? 2 : 4); i++) {
            v = field(o, names[i]); if (!number(v, -1e6, 1e6, 0)) return 0; coords[i] = v->valuedouble;
        }
        it->x0 = coords[0]; it->y0 = coords[1]; it->x1 = coords[2]; it->y1 = coords[3];
        if (it->type == TEXT) {
            v = field(o, "text"); if (!cJSON_IsString(v)) return 0;
            size_t n = strlen(v->valuestring); if (!n || n > 65536 || ntpool + n + 1 >= 1 << 26) return 0;
            for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++) if (*p < 32 && *p != '\n') return 0;
            it->p0 = ntpool; it->np = n; tpool = grow(tpool, &captpool, ntpool + n + 1, 1);
            memcpy(tpool + ntpool, v->valuestring, n + 1); ntpool += n + 1; measure(it);
        }
    }
    return 1;
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
    if (!cJSON_IsArray(list) || n < 1 || n > 1000 || (!replace && nitems + n >= 1 << 20)) return tool_error("Expected 1 to 1000 items within the board capacity.");
    Item *staged = malloc(n * sizeof *staged); int indices[1000], k = 0, oldnp = npool, oldnt = ntpool;
    if (!staged) return tool_error("Out of memory.");
    for (const cJSON *entry = list->child; entry; entry = entry->next, k++) {
        indices[k] = nitems + k;
        if (replace) {
            const cJSON *idx = field(entry, "index");
            if (!keys(entry, "|index|item|") || !number(idx, 0, nitems - 1, 1)) goto invalid;
            indices[k] = idx->valueint;
            for (int j = 0; j < k; j++) if (indices[j] == indices[k]) goto invalid;
        }
        if (!parse_item(replace ? field(entry, "item") : entry, &staged[k])) goto invalid;
    }
    checkpoint();
    for (k = 0; k < n; k++) {
        if (replace) { staged[k].sel = items[indices[k]].sel; items[indices[k]] = staged[k]; }
        else add_item(staged[k]);
    }
    free(staged);
    cJSON *r = board_status(); cJSON_AddItemToObject(r, "indices", cJSON_CreateIntArray(indices, n)); return tool_result(r);
invalid:
    npool = oldnp; ntpool = oldnt; free(staged);
    return tool_error("Invalid item or index; nothing changed. Use complete item definitions, valid coordinates/styles, and unique existing indices.");
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
    checkpoint(); int dst = 0;
    for (int i = 0; i < nitems; i++) {
        int remove = 0; for (k = 0; k < n; k++) if (indices[k] == i) { remove = 1; break; }
        if (!remove) items[dst++] = items[i];
    }
    nitems = dst; return tool_result(board_status());
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
static cJSON *canvas_image(void) {
    if (!win) return tool_error("Canvas image requires a live window.");
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
        cJSON_AddStringToObject(r, "path", path);
        cJSON_AddBoolToObject(r, "collaborating", collaborating);
        cJSON_AddBoolToObject(r, "busy", editing >= 0 || drag != NONE || panning || custom_dialog);
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
    if (!collaborating) return tool_error("Collaboration is off. Enable Collaborate in the Paintboard window.");
    if (!strcmp(name, "_agent_status")) {
        const cJSON *epoch = field(args, "collaboration_epoch"), *message = field(args, "status");
        if (!number(epoch, 0, 9007199254740991., 1) || epoch->valuedouble != collaboration_epoch || !cJSON_IsString(message)) return tool_error("Stale status.");
        snprintf(collaboration_status, sizeof collaboration_status, "%.60s", message->valuestring);
        return tool_result(board_status());
    }
    if (editing >= 0 || drag != NONE || panning) return tool_error("Canvas is busy with a human edit. Retry after the drag or text edit finishes.");
    if (!strcmp(name, "get_canvas")) return canvas_image();
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
    if (!keys(args, "")) return tool_error("This tool takes no arguments.");
    cJSON *r;
    if (!strcmp(name, "save_board")) {
        if (save()) return tool_error("Save failed; the previous board file was preserved. See server stderr for details.");
        r = board_status(); cJSON_AddStringToObject(r, "path", path);
    } else {
        if (!win) return tool_error("PNG export requires a live window. Start without --headless.");
        if (export_png()) return tool_error("PNG export failed. See server stderr for details.");
        char *out = png_path(path); r = board_status(); cJSON_AddStringToObject(r, "path", out);
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
        cJSON *info = cJSON_AddObjectToObject(r, "serverInfo"); cJSON_AddStringToObject(info, "name", "paintboard"); cJSON_AddStringToObject(info, "version", VERSION);
        cJSON_AddStringToObject(r, "instructions", "Use get_board first. Batch edits share undo history with the human. Update/delete/undo/redo require the latest revision. Retry busy errors after human editing finishes. Save explicitly with save_board. This session owns the board file; do not open a second process on it.");
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
