/*
 * grab_keys.c - Grabs keyboard and outputs keypresses to stdout
 * 
 * When run, this program:
 * 1. Grabs the entire keyboard (no other app gets input)
 * 2. Prints each keypress as a single character to stdout
 * 3. Exits on Escape or when stdin is closed
 *
 * Compile: gcc -o grab_keys grab_keys.c -lX11
 * Usage:   ./grab_keys
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    Display *display;
    Window root;
    XEvent event;
    KeySym keysym;
    char buf[32];
    int len;
    
    // Open connection to X server
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "ERROR: Cannot open display\n");
        return 1;
    }
    
    root = DefaultRootWindow(display);
    
    // Grab the keyboard with retry logic
    // When launched via GNOME shortcut, GNOME may still have the keyboard grabbed
    // We retry with increasing delays to give it time to release
    int grab_result;
    int max_retries = 10;
    int retry_delay_us = 10000; // Start with 10ms
    
    for (int i = 0; i < max_retries; i++) {
        grab_result = XGrabKeyboard(
            display,
            root,
            True,                    // owner_events
            GrabModeAsync,           // pointer_mode
            GrabModeAsync,           // keyboard_mode  
            CurrentTime
        );
        
        if (grab_result == GrabSuccess) {
            break;  // Successfully grabbed!
        }
        
        // If this is AlreadyGrabbed (1), wait and retry
        if (grab_result == AlreadyGrabbed && i < max_retries - 1) {
            usleep(retry_delay_us);
            retry_delay_us *= 1.5;  // Exponential backoff
            continue;
        }
        
        // Other errors or final retry failed
        break;
    }
    
    if (grab_result != GrabSuccess) {
        fprintf(stderr, "ERROR: Failed to grab keyboard (code %d)\n", grab_result);
        XCloseDisplay(display);
        return 1;
    }
    
    // Flush to ensure grab is active
    XFlush(display);
    
    // Signal that we're ready (parent process can read this)
    fprintf(stdout, "READY\n");
    fflush(stdout);
    
    // Event loop - process keypresses
    while (1) {
        XNextEvent(display, &event);
        
        if (event.type == KeyPress) {
            // Get the keysym and string representation
            len = XLookupString(&event.xkey, buf, sizeof(buf) - 1, &keysym, NULL);
            
            // Check for Escape - exit signal
            if (keysym == XK_Escape) {
                fprintf(stdout, "ESCAPE\n");
                fflush(stdout);
                break;
            }
            
            // Check for Return/Enter
            if (keysym == XK_Return || keysym == XK_KP_Enter) {
                fprintf(stdout, "RETURN\n");
                fflush(stdout);
                continue;   
            }
            
            // Check for Backspace
            if (keysym == XK_BackSpace) {
                fprintf(stdout, "BACKSPACE\n");
                fflush(stdout);
                continue;
            }
            
            // Regular printable character
            if (len > 0) {
                buf[len] = '\0';
                fprintf(stdout, "KEY:%s\n", buf);
                fflush(stdout);
            } else {
                // Non-printable key - output the keysym name
                const char *keyname = XKeysymToString(keysym);
                if (keyname) {
                    fprintf(stdout, "SYM:%s\n", keyname);
                    fflush(stdout);
                }
            }
        }
    }
    
    // Release keyboard grab
    XUngrabKeyboard(display, CurrentTime);
    XFlush(display);
    XCloseDisplay(display);
    
    return 0;
}
