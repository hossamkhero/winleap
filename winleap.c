/*
 * winleap.c - Mark-based window jump with explicit instance selection
 *
 * Usage:
 *   ./winleap [--config <path>] [--current-workspace] [--debug] <mark_number>
 *   ./winleap --help
 *   ./winleap --open-debug
 *
 * Config supports:
 *   <number>=<wm_class>
 *   instance_keys=<ordered selector chars>
 *   debug=<true|false|1|0|yes|no>
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_WINDOWS 256
#define MAX_CLASS_LEN 256
#define MAX_TITLE_LEN 512
#define MAX_LINE_LEN 512
#define MAX_MARKS 100
#define MAX_INSTANCE_KEYS 128
#define MAX_PATH_LEN 1024

#define DEFAULT_INSTANCE_KEYS "qwertyuiopasdfghjklzxcvbnm1234567890"

// Debug logging
static FILE *logfile = NULL;
static int debug_enabled = 0;

void log_msg(const char *fmt, ...) {
    if (!debug_enabled || !logfile) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(logfile, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(logfile, fmt, args);
    va_end(args);

    fprintf(logfile, "\n");
    fflush(logfile);
}

void log_section(const char *title) {
    if (!debug_enabled || !logfile) return;
    fprintf(logfile, "\n========================================\n");
    fprintf(logfile, "%s\n", title);
    fprintf(logfile, "========================================\n");
    fflush(logfile);
}

typedef struct {
    Window id;
    char wm_class[MAX_CLASS_LEN];
    char title[MAX_TITLE_LEN];
    long desktop;
} WindowInfo;

typedef struct {
    int number;
    char wm_class[MAX_CLASS_LEN];
} MarkMapping;

typedef struct {
    MarkMapping marks[MAX_MARKS];
    int num_marks;
    char instance_keys[MAX_INSTANCE_KEYS];
    int debug;
} Config;

// Global state
static Display *display;
static Window root;
static WindowInfo windows[MAX_WINDOWS];
static int num_windows = 0;

// X11 atoms
static Atom atom_wm_class;
static Atom atom_net_wm_name;
static Atom atom_utf8_string;
static Atom atom_net_client_list;
static Atom atom_net_active_window;
static Atom atom_net_wm_desktop;
static Atom atom_net_current_desktop;

void init_atoms(void) {
    atom_wm_class = XInternAtom(display, "WM_CLASS", False);
    atom_net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    atom_utf8_string = XInternAtom(display, "UTF8_STRING", False);
    atom_net_client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    atom_net_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_desktop = XInternAtom(display, "_NET_WM_DESKTOP", False);
    atom_net_current_desktop = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

int parse_bool(const char *value, int *out) {
    if (!value || !out) return 0;
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
        *out = 1;
        return 1;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

int parse_instance_keys(const char *raw_value, char *out, size_t out_size) {
    if (!raw_value || !out || out_size < 2) return 0;

    int seen[256] = {0};
    size_t j = 0;

    for (size_t i = 0; raw_value[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)raw_value[i];

        if (isspace(ch)) continue;
        if (!isprint(ch)) continue;

        char normalized = (char)tolower(ch);
        unsigned char key = (unsigned char)normalized;

        if (seen[key]) {
            fprintf(stderr, "Duplicate selector key in instance_keys: '%c'\n", normalized);
            return 0;
        }
        if (j >= out_size - 1) {
            fprintf(stderr, "instance_keys is too long\n");
            return 0;
        }

        seen[key] = 1;
        out[j++] = normalized;
    }

    if (j == 0) {
        fprintf(stderr, "instance_keys cannot be empty\n");
        return 0;
    }

    out[j] = '\0';
    return 1;
}

int read_config_file(const char *filepath, Config *config) {
    if (!config) return 0;

    memset(config, 0, sizeof(*config));
    config->debug = 0;
    strncpy(config->instance_keys, DEFAULT_INSTANCE_KEYS, sizeof(config->instance_keys) - 1);
    config->instance_keys[sizeof(config->instance_keys) - 1] = '\0';

    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "Failed to open config: %s\n", filepath);
        return 0;
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f) && config->num_marks < MAX_MARKS) {
        line[strcspn(line, "\n")] = '\0';

        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);

        if (*key == '\0') continue;

        if (strcasecmp(key, "instance_keys") == 0) {
            if (!parse_instance_keys(value, config->instance_keys, sizeof(config->instance_keys))) {
                fclose(f);
                return 0;
            }
            continue;
        }

        if (strcasecmp(key, "debug") == 0) {
            int parsed_debug = 0;
            if (!parse_bool(value, &parsed_debug)) {
                fprintf(stderr, "Invalid debug value: %s\n", value);
                fclose(f);
                return 0;
            }
            config->debug = parsed_debug;
            continue;
        }

        // Default format: number=wmclass
        char *endptr = NULL;
        long num = strtol(key, &endptr, 10);
        if (endptr == key || *endptr != '\0' || num <= 0 || num > INT_MAX) {
            continue;
        }
        if (*value == '\0') {
            continue;
        }

        config->marks[config->num_marks].number = (int)num;
        strncpy(config->marks[config->num_marks].wm_class, value, MAX_CLASS_LEN - 1);
        config->marks[config->num_marks].wm_class[MAX_CLASS_LEN - 1] = '\0';
        config->num_marks++;
    }

    fclose(f);
    return config->num_marks > 0;
}

const char *find_wmclass_for_mark(const Config *config, int mark_num) {
    if (!config) return NULL;
    for (int i = 0; i < config->num_marks; i++) {
        if (config->marks[i].number == mark_num) {
            return config->marks[i].wm_class;
        }
    }
    return NULL;
}

int file_exists_readable(const char *path) {
    if (!path || !path[0]) return 0;
    return access(path, R_OK) == 0;
}

void path_join(char *out, size_t out_size, const char *a, const char *b) {
    if (!out || out_size == 0) return;
    if (!a || !a[0]) {
        snprintf(out, out_size, "%s", b ? b : "");
        return;
    }
    if (!b || !b[0]) {
        snprintf(out, out_size, "%s", a);
        return;
    }

    size_t a_len = strlen(a);
    if (a[a_len - 1] == '/') {
        snprintf(out, out_size, "%s%s", a, b);
    } else {
        snprintf(out, out_size, "%s/%s", a, b);
    }
}

void build_path_near_executable(char *out, size_t out_size, const char *argv0, const char *filename) {
    if (!out || out_size == 0) return;

    const char *last_slash = strrchr(argv0, '/');
    if (last_slash) {
        int dir_len = (int)(last_slash - argv0);
        snprintf(out, out_size, "%.*s/%s", dir_len, argv0, filename);
    } else {
        snprintf(out, out_size, "%s", filename);
    }
}

void resolve_config_path(char *out, size_t out_size, const char *argv0, const char *override_path) {
    if (override_path && override_path[0]) {
        snprintf(out, out_size, "%s", override_path);
        return;
    }

    char candidate[MAX_PATH_LEN];

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home && xdg_config_home[0]) {
        path_join(candidate, sizeof(candidate), xdg_config_home, "winleap/winleap.conf");
        if (file_exists_readable(candidate)) {
            snprintf(out, out_size, "%s", candidate);
            return;
        }
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        path_join(candidate, sizeof(candidate), home, ".config/winleap/winleap.conf");
        if (file_exists_readable(candidate)) {
            snprintf(out, out_size, "%s", candidate);
            return;
        }
    }

    build_path_near_executable(candidate, sizeof(candidate), argv0, "winleap.conf");
    if (file_exists_readable(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return;
    }
    if (out_size > 0) {
        if (xdg_config_home && xdg_config_home[0]) {
            path_join(out, out_size, xdg_config_home, "winleap/winleap.conf");
        } else if (home && home[0]) {
            path_join(out, out_size, home, ".config/winleap/winleap.conf");
        } else {
            build_path_near_executable(out, out_size, argv0, "winleap.conf");
        }
    }
}

void resolve_debug_log_path(char *out, size_t out_size) {
    char base[MAX_PATH_LEN];

    const char *xdg_state_home = getenv("XDG_STATE_HOME");
    if (xdg_state_home && xdg_state_home[0]) {
        path_join(base, sizeof(base), xdg_state_home, "winleap");
    } else {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            path_join(base, sizeof(base), home, ".local/state/winleap");
        } else {
            snprintf(base, sizeof(base), ".");
        }
    }

    path_join(out, out_size, base, "debug.log");
}

int mkdir_p(const char *dir) {
    if (!dir || !dir[0]) return 0;

    char tmp[MAX_PATH_LEN];
    size_t len = strlen(dir);
    if (len >= sizeof(tmp)) return 0;

    snprintf(tmp, sizeof(tmp), "%s", dir);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (strlen(tmp) > 0 && mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return 0;
            }
            tmp[i] = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return 0;
    }
    return 1;
}

int ensure_parent_dir(const char *path) {
    if (!path || !path[0]) return 0;

    char parent[MAX_PATH_LEN];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) return 1;
    if (slash == parent) return 1;

    *slash = '\0';
    return mkdir_p(parent);
}

int print_debug_log(const char *debug_path) {
    if (!debug_path || !debug_path[0]) return 1;

    printf("Debug log path: %s\n", debug_path);

    FILE *f = fopen(debug_path, "r");
    if (!f) {
        if (errno == ENOENT) {
            printf("Debug log does not exist yet. Run with debug enabled to create it.\n");
            return 0;
        }
        perror("Failed to open debug log");
        return 1;
    }

    printf("\n----- begin debug log -----\n");
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    printf("\n----- end debug log -----\n");

    fclose(f);
    return 0;
}

int get_wm_class(Window win, char *buf, size_t bufsize) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(display, win, atom_wm_class, 0, 1024, False,
                           XA_STRING, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) != Success || !prop) {
        return 0;
    }

    // WM_CLASS format: "instance\0class\0"
    const char *instance = (const char *)prop;
    const char *class_name = instance + strlen(instance) + 1;

    if (class_name < (const char *)prop + nitems) {
        strncpy(buf, class_name, bufsize - 1);
        buf[bufsize - 1] = '\0';
    } else {
        strncpy(buf, instance, bufsize - 1);
        buf[bufsize - 1] = '\0';
    }

    XFree(prop);
    return strlen(buf) > 0;
}

int get_window_title(Window win, char *buf, size_t bufsize) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    // Try _NET_WM_NAME first (UTF-8)
    if (XGetWindowProperty(display, win, atom_net_wm_name, 0, 1024, False,
                           atom_utf8_string, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        strncpy(buf, (char *)prop, bufsize - 1);
        buf[bufsize - 1] = '\0';
        XFree(prop);
        return 1;
    }

    // Fall back to WM_NAME
    char *name = NULL;
    if (XFetchName(display, win, &name) && name) {
        strncpy(buf, name, bufsize - 1);
        buf[bufsize - 1] = '\0';
        XFree(name);
        return 1;
    }

    strcpy(buf, "(untitled)");
    return 0;
}

long get_window_desktop(Window win) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(display, win, atom_net_wm_desktop, 0, 1, False,
                           XA_CARDINAL, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        long desktop = *(long *)prop;
        XFree(prop);
        return desktop;
    }

    return -1;
}

long get_current_desktop(void) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(display, root, atom_net_current_desktop, 0, 1, False,
                           XA_CARDINAL, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        long desktop = *(long *)prop;
        XFree(prop);
        return desktop;
    }

    return -1;
}

int discover_windows(void) {
    log_section("DISCOVERING WINDOWS");

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(display, root, atom_net_client_list, 0, 1024, False,
                           XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) != Success || !prop) {
        log_msg("ERROR: Cannot get _NET_CLIENT_LIST");
        return 0;
    }

    Window *client_list = (Window *)prop;
    num_windows = 0;

    for (unsigned long i = 0; i < nitems && num_windows < MAX_WINDOWS; i++) {
        Window win = client_list[i];

        if (!get_wm_class(win, windows[num_windows].wm_class, MAX_CLASS_LEN)) {
            continue;
        }

        get_window_title(win, windows[num_windows].title, MAX_TITLE_LEN);
        windows[num_windows].id = win;
        windows[num_windows].desktop = get_window_desktop(win);

        log_msg("  Found: [%lu] desktop=%ld %s - %s",
                (unsigned long)win,
                windows[num_windows].desktop,
                windows[num_windows].wm_class,
                windows[num_windows].title);

        num_windows++;
    }

    XFree(prop);
    log_msg("Total windows: %d", num_windows);
    return num_windows;
}

int find_windows_by_class_and_scope(const char *target_class,
                                    int current_workspace_only,
                                    long current_desktop,
                                    int *indices,
                                    int max_indices) {
    int count = 0;

    for (int i = 0; i < num_windows && count < max_indices; i++) {
        if (strcasecmp(windows[i].wm_class, target_class) != 0) {
            continue;
        }

        if (current_workspace_only) {
            if (current_desktop < 0) {
                continue;
            }
            if (windows[i].desktop != current_desktop) {
                continue;
            }
        }

        indices[count++] = i;
    }

    return count;
}

void activate_window(int idx) {
    if (idx < 0 || idx >= num_windows) return;

    Window win = windows[idx].id;
    log_msg("ACTIVATING: [%lu] desktop=%ld %s - %s",
            (unsigned long)win,
            windows[idx].desktop,
            windows[idx].wm_class,
            windows[idx].title);

    long desktop = windows[idx].desktop;
    if (desktop >= 0) {
        XEvent switch_event = {0};
        switch_event.xclient.type = ClientMessage;
        switch_event.xclient.send_event = True;
        switch_event.xclient.message_type = atom_net_current_desktop;
        switch_event.xclient.window = root;
        switch_event.xclient.format = 32;
        switch_event.xclient.data.l[0] = desktop;
        switch_event.xclient.data.l[1] = CurrentTime;

        XSendEvent(display, root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &switch_event);
        XFlush(display);
        usleep(50000);
    }

    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.send_event = True;
    event.xclient.message_type = atom_net_active_window;
    event.xclient.window = win;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;

    XSendEvent(display, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);

    XMapRaised(display, win);
    XRaiseWindow(display, win);
    XSetInputFocus(display, win, RevertToPointerRoot, CurrentTime);

    XFlush(display);
}

int grab_keyboard(void) {
    int grab_result = GrabSuccess;
    int max_retries = 10;
    int retry_delay_us = 10000;

    for (int i = 0; i < max_retries; i++) {
        grab_result = XGrabKeyboard(display, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);

        if (grab_result == GrabSuccess) {
            return 1;
        }

        if (grab_result == AlreadyGrabbed && i < max_retries - 1) {
            usleep(retry_delay_us);
            retry_delay_us = retry_delay_us * 3 / 2;
            continue;
        }

        break;
    }

    log_msg("ERROR: Failed to grab keyboard (code %d)", grab_result);
    return 0;
}

int select_instance_interactively(const Config *config, const int *matching_indices, int match_count) {
    if (!config || !matching_indices || match_count <= 0) {
        return -2;
    }

    int key_count = (int)strlen(config->instance_keys);
    if (match_count > key_count) {
        fprintf(stderr, "Too many windows (%d) for instance_keys length (%d)\n", match_count, key_count);
        log_msg("ERROR: %d matches exceed %d instance keys", match_count, key_count);
        return -2;
    }

    log_section("INSTANCE SELECT MODE");
    for (int i = 0; i < match_count; i++) {
        int idx = matching_indices[i];
        char selector = config->instance_keys[i];
        log_msg("  '%c' -> [%lu] desktop=%ld %s - %s",
                selector,
                (unsigned long)windows[idx].id,
                windows[idx].desktop,
                windows[idx].wm_class,
                windows[idx].title);
    }

    if (!grab_keyboard()) {
        fprintf(stderr, "Failed to grab keyboard for instance selection\n");
        return -2;
    }

    XFlush(display);

    while (1) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type != KeyPress) {
            continue;
        }

        char key_buf[32];
        KeySym keysym;
        int len = XLookupString(&event.xkey, key_buf, sizeof(key_buf) - 1, &keysym, NULL);

        if (keysym == XK_Escape) {
            XUngrabKeyboard(display, CurrentTime);
            log_msg("CANCELLED by user (ESC)");
            return -1;
        }

        if (len <= 0) {
            continue;
        }

        key_buf[len] = '\0';
        char typed = (char)tolower((unsigned char)key_buf[0]);

        for (int i = 0; i < match_count; i++) {
            if (typed == config->instance_keys[i]) {
                int selected = matching_indices[i];
                XUngrabKeyboard(display, CurrentTime);
                log_msg("SELECTED selector '%c'", typed);
                return selected;
            }
        }

        log_msg("Ignored selector key '%c'", typed);
    }
}

void print_usage(const char *prog, const char *config_path, const char *debug_path) {
    printf("Usage:\n");
    printf("  %s [--config <path>] [--current-workspace] [--debug] <number>\n", prog);
    printf("  %s --open-debug\n", prog);
    printf("  %s --help\n\n", prog);

    printf("Options:\n");
    printf("  --current-workspace  Only consider windows in current workspace\n");
    printf("  --debug              Force debug logging on for this run\n");
    printf("  --open-debug         Print debug log path and contents\n");
    printf("  --config <path>      Use a specific config file\n");
    printf("  --help               Show this help\n\n");

    printf("Config resolution order:\n");
    printf("  1. --config <path>\n");
    printf("  2. $XDG_CONFIG_HOME/winleap/winleap.conf\n");
    printf("  3. ~/.config/winleap/winleap.conf\n");
    printf("  4. ./winleap.conf (next to executable)\n\n");

    if (config_path && config_path[0]) {
        printf("Resolved config path: %s\n", config_path);
    }
    if (debug_path && debug_path[0]) {
        printf("Debug log path: %s\n", debug_path);
    }
}

int main(int argc, char *argv[]) {
    int current_workspace_only = 0;
    int cli_debug = 0;
    int open_debug = 0;
    int show_help = 0;
    const char *config_override = NULL;
    const char *mark_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--current-workspace") == 0) {
            current_workspace_only = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            cli_debug = 1;
        } else if (strcmp(argv[i], "--open-debug") == 0) {
            open_debug = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            show_help = 1;
        } else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --config\n");
                return 1;
            }
            config_override = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        } else if (!mark_arg) {
            mark_arg = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }

    char config_path[MAX_PATH_LEN];
    resolve_config_path(config_path, sizeof(config_path), argv[0], config_override);

    char debug_path[MAX_PATH_LEN];
    resolve_debug_log_path(debug_path, sizeof(debug_path));

    if (show_help) {
        print_usage(argv[0], config_path, debug_path);
        return 0;
    }

    if (open_debug) {
        return print_debug_log(debug_path);
    }

    if (!mark_arg) {
        print_usage(argv[0], config_path, debug_path);
        return 1;
    }

    int mark_num = atoi(mark_arg);
    if (mark_num <= 0) {
        fprintf(stderr, "Invalid mark number: %s\n", mark_arg);
        return 1;
    }

    Config config;
    if (!read_config_file(config_path, &config)) {
        fprintf(stderr, "Failed to read config: %s\n", config_path);
        return 1;
    }

    debug_enabled = cli_debug || config.debug;

    if (debug_enabled) {
        if (!ensure_parent_dir(debug_path)) {
            fprintf(stderr, "Warning: cannot create debug log directory for: %s\n", debug_path);
        }
        logfile = fopen(debug_path, "a");
        if (!logfile) {
            fprintf(stderr, "Warning: cannot open debug log file: %s\n", debug_path);
        }
    }

    log_section("WINLEAP STARTED");
    log_msg("Mark requested: %d", mark_num);
    log_msg("Scope: %s", current_workspace_only ? "current workspace" : "global");
    log_msg("Debug source: %s", cli_debug ? "--debug" : (config.debug ? "config" : "disabled"));
    log_msg("Config path: %s", config_path);
    log_msg("Debug path: %s", debug_path);
    log_msg("Instance keys: %s", config.instance_keys);

    const char *target_class = find_wmclass_for_mark(&config, mark_num);
    if (!target_class) {
        fprintf(stderr, "No mapping found for mark %d\n", mark_num);
        log_msg("ERROR: No mapping found for mark %d", mark_num);
        if (logfile) fclose(logfile);
        return 1;
    }

    log_msg("Target WM_CLASS: %s", target_class);

    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        log_msg("ERROR: Cannot open display");
        if (logfile) fclose(logfile);
        return 2;
    }

    root = DefaultRootWindow(display);
    init_atoms();

    if (!discover_windows()) {
        fprintf(stderr, "Failed to discover windows\n");
        log_msg("ERROR: discover_windows failed");
        XCloseDisplay(display);
        if (logfile) fclose(logfile);
        return 2;
    }

    long current_desktop = -1;
    if (current_workspace_only) {
        current_desktop = get_current_desktop();
        log_msg("Current desktop: %ld", current_desktop);
    }

    int matching_indices[MAX_WINDOWS];
    int match_count = find_windows_by_class_and_scope(target_class,
                                                      current_workspace_only,
                                                      current_desktop,
                                                      matching_indices,
                                                      MAX_WINDOWS);

    if (match_count == 0) {
        fprintf(stderr, "No windows found for: %s%s\n",
                target_class,
                current_workspace_only ? " (current workspace)" : "");
        log_msg("No matches for class '%s' in scope", target_class);
        XCloseDisplay(display);
        if (logfile) fclose(logfile);
        return 1;
    }

    log_section("MATCHING WINDOWS");
    for (int i = 0; i < match_count; i++) {
        int idx = matching_indices[i];
        log_msg("  [%d] wid=%lu desktop=%ld class=%s title=%s",
                i,
                (unsigned long)windows[idx].id,
                windows[idx].desktop,
                windows[idx].wm_class,
                windows[idx].title);
    }

    int target_idx = -1;

    if (match_count == 1) {
        target_idx = matching_indices[0];
        log_msg("Single instance: immediate activation");
    } else {
        log_msg("Multiple instances (%d): entering instance-select mode", match_count);
        target_idx = select_instance_interactively(&config, matching_indices, match_count);

        if (target_idx == -1) {
            XCloseDisplay(display);
            if (logfile) fclose(logfile);
            return 1;
        }
        if (target_idx < 0) {
            XCloseDisplay(display);
            if (logfile) fclose(logfile);
            return 2;
        }
    }

    activate_window(target_idx);
    log_msg("SUCCESS: Window activated");

    XCloseDisplay(display);
    if (logfile) fclose(logfile);
    return 0;
}
