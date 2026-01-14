/*
 * atsw_manual.c - Manual mode for number-based window switching
 * 
 * Usage: ./atsw_manual <number>
 * 
 * Reads marks.conf to map numbers to WM_CLASS names.
 * If the target app is already focused with multiple instances, cycles to next.
 * Otherwise, switches to the first instance of the app.
 *
 * Compile: gcc -O2 -o atsw_manual atsw_manual.c -lX11
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>

#define MAX_WINDOWS 256
#define MAX_CLASS_LEN 256
#define MAX_TITLE_LEN 512
#define MAX_LINE_LEN 512
#define MAX_MARKS 100

// Debug logging
static FILE *logfile = NULL;

void log_msg(const char *fmt, ...) {
    if (!logfile) return;
    
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
    if (!logfile) return;
    fprintf(logfile, "\n========================================\n");
    fprintf(logfile, "%s\n", title);
    fprintf(logfile, "========================================\n");
    fflush(logfile);
}

// Window info structure
typedef struct {
    Window id;
    char wm_class[MAX_CLASS_LEN];
    char title[MAX_TITLE_LEN];
} WindowInfo;

// Mark mapping structure
typedef struct {
    int number;
    char wm_class[MAX_CLASS_LEN];
} MarkMapping;

// Global state
static Display *display;
static Window root;
static WindowInfo windows[MAX_WINDOWS];
static int num_windows = 0;
static MarkMapping marks[MAX_MARKS];
static int num_marks = 0;

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

// Get WM_CLASS for a window
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
    // We want the class (second string)
    const char *instance = (const char *)prop;
    const char *class = instance + strlen(instance) + 1;
    
    if (class < (const char *)prop + nitems) {
        strncpy(buf, class, bufsize - 1);
        buf[bufsize - 1] = '\0';
    } else {
        strncpy(buf, instance, bufsize - 1);
        buf[bufsize - 1] = '\0';
    }
    
    XFree(prop);
    return strlen(buf) > 0;
}

// Get window title
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

// Get currently active window
Window get_active_window(void) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    
    if (XGetWindowProperty(display, root, atom_net_active_window, 0, 1, False,
                           XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        Window active = *(Window *)prop;
        XFree(prop);
        return active;
    }
    
    return None;
}

// Discover all windows
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
        
        log_msg("  Found: [%lu] %s - %s", 
                (unsigned long)win, 
                windows[num_windows].wm_class, 
                windows[num_windows].title);
        
        num_windows++;
    }
    
    XFree(prop);
    log_msg("Total windows: %d", num_windows);
    return num_windows;
}

// Read marks.conf file
int read_marks_file(const char *filepath) {
    log_section("READING MARKS FILE");
    log_msg("Opening: %s", filepath);
    
    FILE *f = fopen(filepath, "r");
    if (!f) {
        log_msg("ERROR: Cannot open marks.conf");
        return 0;
    }
    
    char line[MAX_LINE_LEN];
    num_marks = 0;
    
    while (fgets(line, sizeof(line), f) && num_marks < MAX_MARKS) {
        // Remove newline
        line[strcspn(line, "\n")] = '\0';
        
        // Skip empty lines and comments
        char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        
        // Parse: number=wmclass
        char *eq = strchr(p, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *number_str = p;
        char *wmclass = eq + 1;
        
        // Trim whitespace
        while (*number_str && isspace(*number_str)) number_str++;
        while (*wmclass && isspace(*wmclass)) wmclass++;
        
        int num = atoi(number_str);
        if (num <= 0) continue;
        
        marks[num_marks].number = num;
        strncpy(marks[num_marks].wm_class, wmclass, MAX_CLASS_LEN - 1);
        marks[num_marks].wm_class[MAX_CLASS_LEN - 1] = '\0';
        
        log_msg("  Mark %d -> %s", marks[num_marks].number, marks[num_marks].wm_class);
        num_marks++;
    }
    
    fclose(f);
    log_msg("Loaded %d marks", num_marks);
    return num_marks;
}

// Find WM_CLASS for a given mark number
const char* find_wmclass_for_mark(int mark_num) {
    for (int i = 0; i < num_marks; i++) {
        if (marks[i].number == mark_num) {
            return marks[i].wm_class;
        }
    }
    return NULL;
}

// Find all windows matching a WM_CLASS (case-insensitive)
int find_windows_by_class(const char *target_class, int *indices, int max_indices) {
    int count = 0;
    
    for (int i = 0; i < num_windows && count < max_indices; i++) {
        if (strcasecmp(windows[i].wm_class, target_class) == 0) {
            indices[count++] = i;
        }
    }
    
    return count;
}

// Activate a window
void activate_window(int idx) {
    if (idx < 0 || idx >= num_windows) return;
    
    Window win = windows[idx].id;
    log_msg("ACTIVATING: [%lu] %s - %s", 
            (unsigned long)win, 
            windows[idx].wm_class, 
            windows[idx].title);
    
    // Get the desktop/workspace number of the window
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    
    if (XGetWindowProperty(display, win, atom_net_wm_desktop, 0, 1, False,
                           XA_CARDINAL, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success && prop) {
        long desktop = *(long *)prop;
        XFree(prop);
        
        log_msg("Window is on desktop %ld, switching...", desktop);
        
        // Switch to that desktop
        XEvent switch_event = {0};
        switch_event.xclient.type = ClientMessage;
        switch_event.xclient.serial = 0;
        switch_event.xclient.send_event = True;
        switch_event.xclient.message_type = atom_net_current_desktop;
        switch_event.xclient.window = root;
        switch_event.xclient.format = 32;
        switch_event.xclient.data.l[0] = desktop;
        switch_event.xclient.data.l[1] = CurrentTime;
        
        XSendEvent(display, root, False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &switch_event);
        XFlush(display);
        
        usleep(50000);  // 50ms delay for workspace switch
    }
    
    // Send _NET_ACTIVE_WINDOW message
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.serial = 0;
    event.xclient.send_event = True;
    event.xclient.message_type = atom_net_active_window;
    event.xclient.window = win;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;  // Source: pager
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;
    
    XSendEvent(display, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    
    // Direct focus
    XMapRaised(display, win);
    XRaiseWindow(display, win);
    XSetInputFocus(display, win, RevertToPointerRoot, CurrentTime);
    
    XFlush(display);
}

int main(int argc, char *argv[]) {
    // Open log file
    char logpath[512];
    const char *script_dir = argv[0];
    char *last_slash = strrchr(script_dir, '/');
    if (last_slash) {
        int dir_len = last_slash - script_dir;
        snprintf(logpath, sizeof(logpath), "%.*s/debug_output.txt", dir_len, script_dir);
    } else {
        strcpy(logpath, "debug_output.txt");
    }
    logfile = fopen(logpath, "a");
    
    log_section("ATSW MANUAL MODE STARTED");
    
    // Check arguments
    if (argc != 2) {
        log_msg("ERROR: Usage: %s <number>", argv[0]);
        fprintf(stderr, "Usage: %s <number>\n", argv[0]);
        if (logfile) fclose(logfile);
        return 1;
    }
    
    int mark_num = atoi(argv[1]);
    if (mark_num <= 0) {
        log_msg("ERROR: Invalid mark number: %s", argv[1]);
        fprintf(stderr, "Invalid mark number: %s\n", argv[1]);
        if (logfile) fclose(logfile);
        return 1;
    }
    
    log_msg("Mark number requested: %d", mark_num);
    
    // Open display
    display = XOpenDisplay(NULL);
    if (!display) {
        log_msg("ERROR: Cannot open display");
        if (logfile) fclose(logfile);
        return 2;
    }
    
    root = DefaultRootWindow(display);
    init_atoms();
    
    // Read marks file
    char marks_path[512];
    if (last_slash) {
        int dir_len = last_slash - script_dir;
        snprintf(marks_path, sizeof(marks_path), "%.*s/marks.conf", dir_len, script_dir);
    } else {
        strcpy(marks_path, "marks.conf");
    }
    
    if (!read_marks_file(marks_path)) {
        log_msg("ERROR: Failed to read marks file");
        fprintf(stderr, "Failed to read marks.conf\n");
        XCloseDisplay(display);
        if (logfile) fclose(logfile);
        return 1;
    }
    
    // Find WM_CLASS for this mark
    const char *target_class = find_wmclass_for_mark(mark_num);
    if (!target_class) {
        log_msg("ERROR: No mapping found for mark %d", mark_num);
        fprintf(stderr, "No mapping found for mark %d\n", mark_num);
        XCloseDisplay(display);
        if (logfile) fclose(logfile);
        return 1;
    }
    
    log_msg("Target WM_CLASS: %s", target_class);
    
    // Discover windows
    if (!discover_windows()) {
        log_msg("ERROR: Failed to discover windows");
        XCloseDisplay(display);
        if (logfile) fclose(logfile);
        return 2;
    }
    
    // Find all windows matching the target class
    int matching_indices[MAX_WINDOWS];
    int match_count = find_windows_by_class(target_class, matching_indices, MAX_WINDOWS);
    
    if (match_count == 0) {
        log_msg("No windows found for class: %s", target_class);
        fprintf(stderr, "No windows found for: %s\n", target_class);
        XCloseDisplay(display);
        if (logfile) fclose(logfile);
        return 1;
    }
    
    log_section("MATCHING WINDOWS");
    for (int i = 0; i < match_count; i++) {
        int idx = matching_indices[i];
        log_msg("  [%d] %s - %s", i, windows[idx].wm_class, windows[idx].title);
    }
    
    // Get currently active window
    Window active = get_active_window();
    log_msg("Active window: %lu", (unsigned long)active);
    
    // Determine which window to activate
    int target_idx = -1;
    
    if (match_count == 1) {
        // Only one window - just activate it
        target_idx = matching_indices[0];
        log_msg("SINGLE INSTANCE: Activating only window");
    } else {
        // Multiple windows - check if active window is one of them
        int active_idx = -1;
        for (int i = 0; i < match_count; i++) {
            if (windows[matching_indices[i]].id == active) {
                active_idx = i;
                break;
            }
        }
        
        if (active_idx >= 0) {
            // Active window is one of the matches - cycle to next
            int next_idx = (active_idx + 1) % match_count;
            target_idx = matching_indices[next_idx];
            log_msg("CYCLING: From index %d to %d", active_idx, next_idx);
        } else {
            // Active window is NOT one of the matches - go to first
            target_idx = matching_indices[0];
            log_msg("SWITCHING: To first instance (not currently focused)");
        }
    }
    
    // Activate the chosen window
    if (target_idx >= 0) {
        activate_window(target_idx);
        log_msg("SUCCESS: Window activated");
    }
    
    XCloseDisplay(display);
    if (logfile) fclose(logfile);
    return 0;
}
