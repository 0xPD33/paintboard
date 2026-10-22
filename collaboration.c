/* Local attachment to an existing window. Socket I/O stays off the GL thread. */
#include <sys/socket.h>
#include <sys/un.h>
#include <limits.h>
#include <fcntl.h>

#ifndef PAINTBOARD_PYTHON
#define PAINTBOARD_PYTHON "python3"
#endif
static int collaboration_fd = -1, collaboration_started;
static char collaboration_socket[108];
static pthread_t collaboration_thread;
static pthread_mutex_t collaboration_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t collaboration_cond = PTHREAD_COND_INITIALIZER;
static char *collaboration_request, *collaboration_reply;
static int collaboration_done;
static pid_t collaboration_worker;
static pid_t discovery_worker;
static unsigned discovery_generation;
static int automatic_requested;
static int discovery_again, custom_use_pending;

static void custom_load(void) {
    char filename[4096], content[16385];
    if (!config_path(filename, sizeof filename, "custom-agent.json")) return;
    FILE *f = fopen(filename, "r"); if (!f) return;
    size_t size = fread(content, 1, sizeof content - 1, f); int failed = ferror(f); fclose(f); content[size] = 0;
    cJSON *config = !failed && size < sizeof content - 1 ? cJSON_Parse(content) : NULL;
    const cJSON *command = field(config, "command"), *transport = field(config, "transport");
    int mode = -1;
    for (int i = 0; i < 3; i++) if (jstr(transport, custom_transports[i])) mode = i;
    if (mode >= 0 && cJSON_IsString(command) && strlen(command->valuestring) < sizeof custom_command) {
        strcpy(custom_command, command->valuestring); custom_transport = mode;
    } else strcpy(custom_error, "Invalid saved custom agent settings; enter a new command");
    cJSON_Delete(config);
}
static int custom_write(void) {
    cJSON *config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "command", custom_draft);
    cJSON_AddStringToObject(config, "transport", custom_transports[custom_draft_transport]);
    char *json = cJSON_PrintUnformatted(config); cJSON_Delete(config);
    int ok = json && config_write("custom-agent.json", json);
    free(json); return ok;
}

static void *collaboration_listener(void *unused) {
    (void)unused;
    for (;;) {
        int fd = accept(collaboration_fd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; return NULL; }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        struct timeval timeout = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        FILE *f = fdopen(fd, "r+"); if (!f) { close(fd); continue; }
        char *line = mcp_read_stream(f);
        if (!line) { fclose(f); continue; }
        pthread_mutex_lock(&collaboration_mutex);
        collaboration_request = line; collaboration_done = 0;
        pthread_mutex_unlock(&collaboration_mutex); glfwPostEmptyEvent();
        pthread_mutex_lock(&collaboration_mutex);
        while (!collaboration_done) pthread_cond_wait(&collaboration_cond, &collaboration_mutex);
        char *reply = collaboration_reply; collaboration_reply = NULL;
        pthread_mutex_unlock(&collaboration_mutex);
        if (reply) {
            size_t n = strlen(reply), sent = 0;
            while (sent < n) { ssize_t r = send(fd, reply + sent, n - sent, MSG_NOSIGNAL); if (r <= 0) break; sent += r; }
            free(reply);
        }
        fclose(f);
    }
}
static void collaboration_cancel(void) {
    if (!collaboration_worker) return;
    kill(-collaboration_worker, SIGTERM);
    for (int i = 0; i < 10; i++) {
        if (waitpid(collaboration_worker, NULL, WNOHANG) != 0) { collaboration_worker = 0; return; }
        usleep(10000);
    }
    kill(-collaboration_worker, SIGKILL); waitpid(collaboration_worker, NULL, 0); collaboration_worker = 0;
}
static void collaboration_launch(void) {
    if (selected_agent < 0) { strcpy(collaboration_status, selected_agent == -2 ? "Detecting agents..." : "Chat tools connected"); return; }
    char exe[PATH_MAX + 16]; ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX - 1);
    if (n < 0) { strcpy(collaboration_status, "Cannot start agent"); return; }
    strcpy(exe + n, "-bridge");
    collaboration_worker = fork();
    if (!collaboration_worker) {
        setpgid(0, 0);
        execlp(PAINTBOARD_PYTHON, PAINTBOARD_PYTHON, exe, "--socket", collaboration_socket, "--watch",
               "--agent", agent_ids[selected_agent], "--executable", agent_paths[selected_agent],
               "--custom-command", custom_command, "--custom-transport", custom_transports[custom_transport], (char *)NULL);
        _exit(127);
    }
    if (collaboration_worker < 0) { collaboration_worker = 0; strcpy(collaboration_status, "Cannot start agent"); }
    else { setpgid(collaboration_worker, collaboration_worker); strcpy(collaboration_status, "Watching your drawing"); }
}
static void collaboration_toggle(void) {
    collaboration_epoch++; collaborating ^= 1; automatic_requested = collaborating;
    if (!collaborating) { collaboration_cancel(); strcpy(collaboration_status, "Off"); return; }
    if (!collaboration_started) { collaborating = 0; strcpy(collaboration_status, "Connection unavailable"); return; }
    collaboration_launch();
}
static void collaboration_select(void) {
    if (selected_agent == -2) return;
    int next = selected_agent;
    do { next = next == NAGENTS - 1 ? -1 : next + 1; } while (next >= 0 && !*agent_paths[next]);
    if (next == selected_agent) return;
    collaboration_epoch++; collaboration_cancel(); selected_agent = next;
    if (collaborating) { automatic_requested = 1; collaboration_launch(); }
}
static void collaboration_discover(void) {
    if (!collaboration_started) return;
    if (discovery_worker) { discovery_again = 1; return; }
    char exe[PATH_MAX + 16], generation[32]; ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX - 1);
    if (n < 0) return;
    strcpy(exe + n, "-bridge");
    snprintf(generation, sizeof generation, "%u", ++discovery_generation);
    discovery_worker = fork();
    if (!discovery_worker) {
        execlp(PAINTBOARD_PYTHON, PAINTBOARD_PYTHON, exe, "--socket", collaboration_socket,
               "--discover", "--generation", generation, "--custom-command", custom_command,
               "--custom-transport", custom_transports[custom_transport], (char *)NULL);
        _exit(127);
    }
    if (discovery_worker < 0) { discovery_worker = 0; strcpy(collaboration_status, "Agent discovery failed"); }
    else agent_scanning = 1;
}
static void custom_save(void) {
    if (!collaboration_started) { strcpy(custom_error, "Connection unavailable"); return; }
    if (!custom_write()) { strcpy(custom_error, "Cannot save custom agent settings"); return; }
    strcpy(custom_command, custom_draft); custom_transport = custom_draft_transport;
    *custom_error = 0; custom_use_pending = 1; discovery_generation++;
    collaboration_epoch++; collaboration_cancel();
    selected_agent = -1;
    if (collaborating) strcpy(collaboration_status, "Checking custom command...");
    collaboration_discover();
}
static cJSON *collaboration_discovered(const cJSON *params) {
    const cJSON *generation = field(params, "generation"), *agents = field(params, "agents");
    if (!number(generation, 0, 9007199254740991., 1) || generation->valuedouble != discovery_generation || !cJSON_IsArray(agents))
        return tool_error("Stale discovery.");
    char paths[NAGENTS][4096] = {{0}};
    const cJSON *entry;
    cJSON_ArrayForEach(entry, agents) {
        const cJSON *exe = field(entry, "executable");
        if (!cJSON_IsString(exe) || exe->valuestring[0] != '/' || strlen(exe->valuestring) >= sizeof paths[0]) continue;
        for (int i = 0; i < NAGENTS; i++) if (jstr(field(entry, "id"), agent_ids[i])) strcpy(paths[i], exe->valuestring);
    }
    int next = selected_agent;
    if (next == -2) { next = -1; for (int i = 0; i < NAGENTS; i++) if (*paths[i]) { next = i; break; } }
    else if (next >= 0 && !*paths[next]) next = -1;
    const cJSON *error = field(params, "custom_error");
    if (cJSON_IsString(error)) snprintf(custom_error, sizeof custom_error, "%.100s", error->valuestring);
    if (custom_use_pending) {
        next = *paths[CUSTOM_AGENT] ? CUSTOM_AGENT : -1;
        custom_use_pending = 0;
        if (!*custom_error) custom_dialog = 0;
        if (collaborating) automatic_requested = 1;
        strcpy(collaboration_status, collaborating ? "Chat tools connected" : "Off");
    }
    int changed_agent = next != selected_agent || (next >= 0 && strcmp(paths[next], agent_paths[next]));
    memcpy(agent_paths, paths, sizeof paths); selected_agent = next;
    if (changed_agent) {
        collaboration_epoch++; collaboration_cancel();
        if (collaborating && automatic_requested) collaboration_launch();
    }
    return tool_result(cJSON_CreateObject());
}
static void collaboration_start(void) {
    custom_load();
    char dir[80]; snprintf(dir, sizeof dir, "/tmp/paintboard-%lu", (unsigned long)getuid());
    struct stat st;
    if ((mkdir(dir, 0700) && errno != EEXIST) || lstat(dir, &st) || !S_ISDIR(st.st_mode)
        || st.st_uid != getuid() || (st.st_mode & 0077)) { strcpy(collaboration_status, "Private socket unavailable"); return; }
    snprintf(collaboration_socket, sizeof collaboration_socket, "%s/%ld.sock", dir, (long)getpid());
    collaboration_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX}; strcpy(addr.sun_path, collaboration_socket);
    if (collaboration_fd < 0 || bind(collaboration_fd, (struct sockaddr *)&addr, sizeof addr)
        || chmod(collaboration_socket, 0600) || listen(collaboration_fd, 8)) {
        if (collaboration_fd >= 0) close(collaboration_fd);
        collaboration_fd = -1; strcpy(collaboration_status, "Connection unavailable"); return;
    }
    signal(SIGPIPE, SIG_IGN);
    if (!mcp_spec) mcp_spec = cJSON_Parse(MCP_SPEC);
    if (!mcp_spec || pthread_create(&collaboration_thread, NULL, collaboration_listener, NULL)) {
        close(collaboration_fd); collaboration_fd = -1; unlink(collaboration_socket); return;
    }
    collaboration_started = 1;
    if (collaborating) strcpy(collaboration_status, "Agent tools connected");
    collaboration_discover();
}
static void collaboration_answer(char *line, const char *error) { /* reply to one queued request and free it; the listener sends the reply */
    char *reply = NULL; size_t len; FILE *output = open_memstream(&reply, &len);
    if (output) {
        int phase = mcp_phase, failed = mcp_output_failed; mcp_phase = 2; mcp_output = output;
        cJSON *req = cJSON_Parse(line);
        if (error) rpc_reply(field(req, "id"), tool_error(error), 0, NULL);
        else if (jstr(field(req, "method"), "_agent_discovery"))
            rpc_reply(field(req, "id"), collaboration_discovered(field(req, "params")), 0, NULL);
        else if (jstr(field(field(req, "params"), "name"), "_agent_status"))
            rpc_reply(field(req, "id"), call_tool("_agent_status", field(field(req, "params"), "arguments")), 0, NULL);
        else mcp_dispatch(line);
        cJSON_Delete(req); fclose(output); mcp_output = NULL; mcp_phase = phase; mcp_output_failed = failed;
    }
    free(line);
    pthread_mutex_lock(&collaboration_mutex);
    collaboration_reply = reply; collaboration_done = 1;
    pthread_cond_signal(&collaboration_cond); pthread_mutex_unlock(&collaboration_mutex);
}
/* An attached client (the bridge names it in params.client) that calls a board tool while Collaborate is
   off waits for the human: the request stays unanswered until Deny, Allow once, Allow for this session,
   or PERMIT_WAIT seconds. The listener serves one request at a time, so others queue behind it. */
#define PERMIT_WAIT 45
static char *permit_line, permit_text[160];
static int asks_permission(const char *line) {
    cJSON *req = cJSON_Parse(line); const cJSON *params = field(req, "params"), *client = field(params, "client"), *name = field(params, "name");
    int ask = !collaborating && jstr(field(req, "method"), "tools/call") && cJSON_IsString(client) && cJSON_IsString(name)
              && !jstr(name, "get_status") && !jstr(name, "measure_text"), known = 0;
    for (const cJSON *t = ask ? field(mcp_spec, "tools")->child : NULL; t; t = t->next) known |= jstr(name, field(t, "name")->valuestring);
    if (ask && known) snprintf(permit_text, sizeof permit_text, "%.60s wants to call %.40s", client->valuestring, name->valuestring);
    cJSON_Delete(req); return ask && known;
}
static void permit_answer(int answer) { /* 0 deny, 1 allow once, 2 allow for this session */
    char *line = permit_line;
    if (!line) return;
    permit_line = NULL; permit_shown = 0; permit_until = 0;
    if (answer == 2) { /* the toggle's On state, but without starting automatic replies */
        collaboration_epoch++; collaborating = 1; automatic_requested = 0; strcpy(collaboration_status, "Chat tools connected");
    }
    permit_once = answer == 1;
    collaboration_answer(line, answer ? NULL : "The human declined access in the Paintboard window.");
    permit_once = 0;
}
static void permit_ui(void) {
    float x, y, w; dialog_box(170, &x, &y, &w); char title[200], body[300];
    if (!ui_mode) {
        snprintf(title, sizeof title, "%s", permit_text); fit(title, 20, w - 40);
        text_draw(&ui_font, x + 20, y + 16, 20, INK, title);
        snprintf(body, sizeof body, "It acts on %s, and Collaborate is off. Allow for this session\nturns Collaborate on, without automatic replies.", base_name(path));
        text_draw(&ui_font, x + 20, y + 56, 13, MUTED, body);
    }
    if (button(x + 20, y + 118, 90, 32, "Deny", NULL, 0)) { permit_answer(0); return; }
    if (button(x + w - 292, y + 118, 110, 32, "Allow once", NULL, 1)) { permit_answer(1); return; }
    if (button(x + w - 174, y + 118, 154, 32, "Allow for this session", NULL, 0)) permit_answer(2);
}
static void collaboration_pump(void) {
    if (!collaboration_started) return;
    int scan_status;
    if (discovery_worker && waitpid(discovery_worker, &scan_status, WNOHANG) == discovery_worker) {
        discovery_worker = 0; agent_scanning = 0;
        if (!WIFEXITED(scan_status) || WEXITSTATUS(scan_status)) {
            if (selected_agent == -2) selected_agent = -1;
            strcpy(collaboration_status, "Agent discovery failed; press R");
        }
        if (discovery_again) { discovery_again = 0; collaboration_discover(); }
    }
    if (collaboration_worker && waitpid(collaboration_worker, NULL, WNOHANG) == collaboration_worker) {
        collaboration_worker = 0; strcpy(collaboration_status, "Agent stopped; toggle to retry");
    }
    if (permit_line && (collaborating || glfwGetTime() >= permit_until)) { /* turned on meanwhile, or nobody answered */
        char *line = permit_line; permit_line = NULL; permit_shown = 0; permit_until = 0;
        collaboration_answer(line, collaborating ? NULL : "Nobody answered in the Paintboard window. Ask the human to allow access, then retry.");
    }
    pthread_mutex_lock(&collaboration_mutex);
    char *line = permit_line ? NULL : collaboration_request; /* a held request blocks the listener anyway */
    if (line) collaboration_request = NULL;
    pthread_mutex_unlock(&collaboration_mutex);
    if (line && asks_permission(line)) { permit_line = line; permit_until = glfwGetTime() + PERMIT_WAIT; line = NULL; }
    if (permit_line && !custom_dialog && !fd_mode && !asking && !disk_asking && !pending && editing < 0 && drag == NONE && !panning)
        permit_shown = 1; /* not in the middle of the human's stroke or typing */
    if (line) collaboration_answer(line, NULL);
}
static void collaboration_stop(void) {
    collaboration_cancel();
    if (discovery_worker) { kill(discovery_worker, SIGTERM); waitpid(discovery_worker, NULL, 0); }
    if (!collaboration_started) return;
    pthread_cancel(collaboration_thread); pthread_join(collaboration_thread, NULL);
    close(collaboration_fd); unlink(collaboration_socket);
    free(collaboration_request); free(collaboration_reply); free(permit_line);
    if (!mcp_mode) cJSON_Delete(mcp_spec);
}
