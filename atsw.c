/*
 * atsw.c - App-To-Switch-Window (all-in-one C implementation)
 * 
 * Fast vim-style window switching using prefix matching.
 * All logic in C using direct X11 calls - no subprocess spawning.
 *
 * Compile: gcc -O2 -o atsw atsw.c -lX11
 * Usage:   ./atsw
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>

#define MAX_WINDOWS 256
#define MAX_APPS 128
#define MAX_PREFIX_LEN 32
#define MAX_CLASS_LEN 256
#define MAX_TITLE_LEN 512
#define MAX_BUFFER_LEN 64

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
    char wm_class_lower[MAX_CLASS_LEN];
    char title[MAX_TITLE_LEN];
    char prefix[MAX_PREFIX_LEN];
} WindowInfo;

// Global state
static Display *display;
static Window root;
static WindowInfo windows[MAX_WINDOWS];
static int num_windows = 0;

// X11 atoms (cached for speed)
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

// Get WM_CLASS for a window (returns the class name, not instance)
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

// Get window title (_NET_WM_NAME or WM_NAME)
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

// Get list of client windows from window manager
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
        
        // Get WM_CLASS
        if (!get_wm_class(win, windows[num_windows].wm_class, MAX_CLASS_LEN)) {
            continue;  // Skip windows without WM_CLASS
        }
        
        // Store lowercase version for prefix matching
        strncpy(windows[num_windows].wm_class_lower, windows[num_windows].wm_class, MAX_CLASS_LEN);
        for (char *p = windows[num_windows].wm_class_lower; *p; p++) {
            *p = tolower(*p);
        }
        
        // Get title
        get_window_title(win, windows[num_windows].title, MAX_TITLE_LEN);
        
        windows[num_windows].id = win;
        windows[num_windows].prefix[0] = '\0';
        
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

// Compute unique prefixes for all windows
void compute_prefixes(void) {
    log_section("COMPUTING PREFIXES");
    
    // Group windows by app class
    typedef struct {
        char class_lower[MAX_CLASS_LEN];
        int window_indices[MAX_WINDOWS];
        int count;
    } AppGroup;
    
    static AppGroup apps[MAX_APPS];
    int num_apps = 0;
    
    // Group windows by class
    for (int i = 0; i < num_windows; i++) {
        int found = -1;
        for (int j = 0; j < num_apps; j++) {
            if (strcmp(apps[j].class_lower, windows[i].wm_class_lower) == 0) {
                found = j;
                break;
            }
        }
        
        if (found >= 0) {
            apps[found].window_indices[apps[found].count++] = i;
        } else if (num_apps < MAX_APPS) {
            strncpy(apps[num_apps].class_lower, windows[i].wm_class_lower, MAX_CLASS_LEN);
            apps[num_apps].window_indices[0] = i;
            apps[num_apps].count = 1;
            num_apps++;
        }
    }
    
    // For each app, find shortest unique prefix
    for (int a = 0; a < num_apps; a++) {
        const char *app_class = apps[a].class_lower;
        int app_len = strlen(app_class);
        int prefix_len = 1;
        
        // Find shortest prefix that doesn't conflict with other apps
        for (prefix_len = 1; prefix_len <= app_len; prefix_len++) {
            int is_unique = 1;
            
            for (int other = 0; other < num_apps; other++) {
                if (other == a) continue;
                
                if (strncasecmp(app_class, apps[other].class_lower, prefix_len) == 0) {
                    is_unique = 0;
                    break;
                }
            }
            
            if (is_unique) break;
        }
        
        // Assign prefixes to windows
        if (apps[a].count == 1) {
            // Single window - just use app prefix
            int idx = apps[a].window_indices[0];
            strncpy(windows[idx].prefix, app_class, prefix_len);
            windows[idx].prefix[prefix_len] = '\0';
            log_msg("  %s -> %s (%s)", windows[idx].prefix, windows[idx].wm_class, windows[idx].title);
        } else {
            // Multiple windows - append number
            for (int w = 0; w < apps[a].count; w++) {
                int idx = apps[a].window_indices[w];
                snprintf(windows[idx].prefix, MAX_PREFIX_LEN, "%.*s%d", prefix_len, app_class, w + 1);
                log_msg("  %s -> %s - %s", windows[idx].prefix, windows[idx].wm_class, windows[idx].title);
            }
        }
    }
    
    log_msg("");
    log_msg("PREFIX TABLE:");
    for (int i = 0; i < num_windows; i++) {
        log_msg("  '%s' -> [%s] %s", windows[i].prefix, windows[i].wm_class, windows[i].title);
    }
}

// Find matching window for given buffer
// Returns: index of unique match, -1 if no match, -2 if multiple matches
int find_match(const char *buffer, int *match_count) {
    if (!buffer || !buffer[0]) {
        *match_count = num_windows;
        return -2;
    }
    
    int buf_len = strlen(buffer);
    int matches[MAX_WINDOWS];
    int count = 0;
    
    for (int i = 0; i < num_windows; i++) {
        if (strncasecmp(windows[i].prefix, buffer, buf_len) == 0) {
            matches[count++] = i;
        }
    }
    
    *match_count = count;
    
    if (count == 1) {
        return matches[0];
    } else if (count == 0) {
        return -1;
    } else {
        return -2;
    }
}

// Activate a window (robust - multiple methods like xdotool)
void activate_window(int idx) {
    if (idx < 0 || idx >= num_windows) return;
    
    Window win = windows[idx].id;
    log_msg("ACTIVATING: [%lu] %s - %s", (unsigned long)win, windows[idx].wm_class, windows[idx].title);
    
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
        
        // Small delay to let workspace switch complete
        usleep(50000);  // 50ms
    }
    
    // Method 1: Send _NET_ACTIVE_WINDOW message (for EWMH compliant WMs)
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
    
    // Method 2: Direct focus (what xdotool does)
    XMapRaised(display, win);
    XRaiseWindow(display, win);
    XSetInputFocus(display, win, RevertToPointerRoot, CurrentTime);
    
    XFlush(display);
}

// Grab keyboard with retry
int grab_keyboard(void) {
    int grab_result;
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
    
    log_section("ATSW STARTED (C VERSION)");
    
    // Open display
    display = XOpenDisplay(NULL);
    if (!display) {
        log_msg("ERROR: Cannot open display");
        return 2;
    }
    
    root = DefaultRootWindow(display);
    init_atoms();
    
    // CRITICAL: Grab keyboard FIRST
    log_msg("Grabbing keyboard...");
    if (!grab_keyboard()) {
        XCloseDisplay(display);
        return 2;
    }
    log_msg("Keyboard grabbed successfully!");
    XFlush(display);
    
    // Now discover windows and compute prefixes (keyboard already grabbed!)
    if (!discover_windows()) {
        XUngrabKeyboard(display, CurrentTime);
        XCloseDisplay(display);
        return 2;
    }
    
    compute_prefixes();
    
    // Input buffer
    char buffer[MAX_BUFFER_LEN] = {0};
    int buf_pos = 0;
    
    log_msg("");
    log_msg("WAITING FOR INPUT...");
    log_msg("  (Press keys to match prefix, ESC to cancel)");
    log_msg("");
    
    // Event loop
    XEvent event;
    while (1) {
        XNextEvent(display, &event);
        
        if (event.type != KeyPress) continue;
        
        char key_buf[32];
        KeySym keysym;
        int len = XLookupString(&event.xkey, key_buf, sizeof(key_buf) - 1, &keysym, NULL);
        
        // Handle Escape
        if (keysym == XK_Escape) {
            log_msg("CANCELLED by user (ESC)");
            XUngrabKeyboard(display, CurrentTime);
            XCloseDisplay(display);
            if (logfile) fclose(logfile);
            return 1;
        }
        
        // Handle Backspace
        if (keysym == XK_BackSpace) {
            if (buf_pos > 0) {
                buffer[--buf_pos] = '\0';
                log_msg("BACKSPACE: buffer='%s'", buffer);
            }
            continue;
        }
        
        // Handle Enter
        if (keysym == XK_Return || keysym == XK_KP_Enter) {
            if (buf_pos > 0) {
                int match_count;
                int match = find_match(buffer, &match_count);
                if (match >= 0) {
                    activate_window(match);
                    log_msg("FINISHED: Activated window via ENTER");
                    XUngrabKeyboard(display, CurrentTime);
                    XCloseDisplay(display);
                    if (logfile) fclose(logfile);
                    return 0;
                }
            }
            continue;
        }
        
        // Regular printable character
        if (len > 0 && buf_pos < MAX_BUFFER_LEN - 1) {
            key_buf[len] = '\0';
            
            // Append to buffer (lowercase for matching)
            for (int i = 0; i < len && buf_pos < MAX_BUFFER_LEN - 1; i++) {
                buffer[buf_pos++] = tolower(key_buf[i]);
            }
            buffer[buf_pos] = '\0';
            
            log_msg("KEY: '%s' -> buffer='%s'", key_buf, buffer);
            
            // Check for matches
            int match_count;
            int match = find_match(buffer, &match_count);
            
            if (match >= 0) {
                // Unique match - activate immediately
                log_msg("UNIQUE MATCH: prefix='%s'", windows[match].prefix);
                activate_window(match);
                log_msg("FINISHED: Activated window");
                XUngrabKeyboard(display, CurrentTime);
                XCloseDisplay(display);
                if (logfile) fclose(logfile);
                return 0;
            } else if (match_count == 0) {
                log_msg("NO MATCH for buffer='%s'", buffer);
            } else {
                log_msg("PARTIAL MATCH: %d possible", match_count);
            }
        }
    }
    
    XUngrabKeyboard(display, CurrentTime);
    XCloseDisplay(display);
    if (logfile) fclose(logfile);
    return 0;
}
