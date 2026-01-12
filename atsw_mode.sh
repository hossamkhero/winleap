#!/usr/bin/env bash

#
# atsw_mode.sh - App-To-Switch-Window Mode
#
# This script implements vim-style marks for X11 windows.
# When activated:
# 1. Discovers all visible windows
# 2. Computes unique prefixes for each app/window
# 3. Grabs keyboard and waits for prefix input
# 4. Activates the matching window when prefix is unique
#
# Exit codes:
#   0 - Success (window activated)
#   1 - Cancelled (Escape pressed)
#   2 - Error

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GRABBER="$SCRIPT_DIR/grab_keys"
LOGFILE="$SCRIPT_DIR/debug_output.txt"

# ============================================================================
# DEBUG LOGGING
# ============================================================================
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOGFILE"
}

log_section() {
    echo "" >> "$LOGFILE"
    echo "========================================" >> "$LOGFILE"
    echo "$1" >> "$LOGFILE"
    echo "========================================" >> "$LOGFILE"
}

# ============================================================================
# WINDOW DISCOVERY
# ============================================================================

# Associative arrays to store window data
declare -A WINDOW_CLASS        # wid -> WM_CLASS
declare -A WINDOW_TITLE        # wid -> Window title
declare -A APP_WINDOWS         # app_name -> space-separated list of wids
declare -A PREFIX_TO_WINDOW    # prefix -> wid (final mapping)
declare -A WINDOW_PREFIX       # wid -> assigned prefix

discover_windows() {
    log_section "DISCOVERING WINDOWS"
    
    # Get all visible windows
    local wids
    wids=$(xdotool search --onlyvisible --name "" 2>/dev/null || true)
    
    if [[ -z "$wids" ]]; then
        log "ERROR: No windows found"
        return 1
    fi
    
    # Process each window
    while read -r wid; do
        [[ -z "$wid" ]] && continue
        
        # Get WM_CLASS and title
        local props
        props=$(xprop -id "$wid" WM_CLASS _NET_WM_NAME 2>/dev/null || true)
        
        # Extract WM_CLASS (second value is the class name)
        # Format: WM_CLASS(STRING) = "instance", "class"
        local wm_class
        if echo "$props" | grep -q 'WM_CLASS(STRING)'; then
            wm_class=$(echo "$props" | grep "WM_CLASS(STRING)" | sed 's/.*= "[^"]*", "\([^"]*\)"/\1/' || true)
        else
            continue  # Skip windows without proper WM_CLASS
        fi
        
        # Skip windows without WM_CLASS or with empty class
        [[ -z "$wm_class" ]] && continue
        [[ "$wm_class" == *"not found"* ]] && continue
        
        # Extract title
        local title
        title=$(echo "$props" | grep "_NET_WM_NAME" | sed 's/_NET_WM_NAME.*= "\(.*\)"/\1/' || true)
        [[ -z "$title" ]] && title="(untitled)"
        
        # Store window info
        WINDOW_CLASS[$wid]="$wm_class"
        WINDOW_TITLE[$wid]="$title"
        
        # Group by app name (lowercase for prefix matching)
        local app_name
        app_name=$(echo "$wm_class" | tr '[:upper:]' '[:lower:]')
        
        if [[ -v APP_WINDOWS[$app_name] ]]; then
            APP_WINDOWS[$app_name]="${APP_WINDOWS[$app_name]} $wid"
        else
            APP_WINDOWS[$app_name]="$wid"
        fi
        
        log "  Found: [$wid] $wm_class - $title"
    done <<< "$wids"
    
    log "Total apps: ${#APP_WINDOWS[@]}"
    log "Total windows: ${#WINDOW_CLASS[@]}"
    
    return 0
}

# ============================================================================
# PREFIX COMPUTATION
# Get shortest unique prefix for each app, then for windows within that app
# ============================================================================

compute_prefixes() {
    log_section "COMPUTING PREFIXES"
    
    # Get all app names
    local app_names=("${!APP_WINDOWS[@]}")
    local num_apps=${#app_names[@]}
    
    if [[ $num_apps -eq 0 ]]; then
        log "ERROR: No apps to process"
        return 1
    fi
    
    # For each app, find the shortest unique prefix
    for app in "${app_names[@]}"; do
        local prefix=""
        local app_len=${#app}
        
        # Try increasing prefix lengths
        for ((i=1; i<=app_len; i++)); do
            prefix="${app:0:$i}"
            local is_unique=true
            
            # Check against all other apps
            for other_app in "${app_names[@]}"; do
                [[ "$other_app" == "$app" ]] && continue
                
                # If other app starts with same prefix, not unique
                if [[ "${other_app:0:$i}" == "$prefix" ]]; then
                    is_unique=false
                    break
                fi
            done
            
            if $is_unique; then
                break
            fi
        done
        
        # Now handle multiple windows of the same app
        local wids
        read -ra wids <<< "${APP_WINDOWS[$app]}"
        local num_windows=${#wids[@]}
        
        if [[ $num_windows -eq 1 ]]; then
            # Single window - just use the app prefix
            local wid="${wids[0]}"
            PREFIX_TO_WINDOW[$prefix]="$wid"
            WINDOW_PREFIX[$wid]="$prefix"
            log "  $prefix -> ${WINDOW_CLASS[$wid]} (${WINDOW_TITLE[$wid]})"
        else
            # Multiple windows - append numbers
            local idx=1
            for wid in "${wids[@]}"; do
                local full_prefix="${prefix}${idx}"
                PREFIX_TO_WINDOW[$full_prefix]="$wid"
                WINDOW_PREFIX[$wid]="$full_prefix"
                log "  $full_prefix -> ${WINDOW_CLASS[$wid]} - ${WINDOW_TITLE[$wid]}"
                ((idx++))
            done
        fi
    done
    
    log ""
    log "PREFIX TABLE:"
    for pfx in $(echo "${!PREFIX_TO_WINDOW[@]}" | tr ' ' '\n' | sort); do
        local wid="${PREFIX_TO_WINDOW[$pfx]}"
        log "  '$pfx' -> [${WINDOW_CLASS[$wid]}] ${WINDOW_TITLE[$wid]}"
    done
    
    return 0
}

# ============================================================================
# PREFIX MATCHING
# Given current buffer, return matching status
# ============================================================================

# Returns:
#   0 = exactly one match (sets MATCH_RESULT)
#   1 = multiple matches (sets MATCH_COUNT)
#   2 = no matches
check_prefix_match() {
    local buffer="$1"
    MATCH_RESULT=""
    MATCH_COUNT=0
    MATCHING_PREFIXES=""
    
    if [[ -z "$buffer" ]]; then
        MATCH_COUNT=${#PREFIX_TO_WINDOW[@]}
        return 1
    fi
    
    local matches=()
    for pfx in "${!PREFIX_TO_WINDOW[@]}"; do
        # Check if prefix starts with buffer (buffer is partial prefix)
        if [[ "$pfx" == "$buffer"* ]]; then
            matches+=("$pfx")
        fi
    done
    
    MATCH_COUNT=${#matches[@]}
    
    if [[ $MATCH_COUNT -eq 0 ]]; then
        return 2
    elif [[ $MATCH_COUNT -eq 1 ]]; then
        MATCH_RESULT="${PREFIX_TO_WINDOW[${matches[0]}]}"
        MATCHING_PREFIXES="${matches[0]}"
        return 0
    else
        MATCHING_PREFIXES="${matches[*]}"
        return 1
    fi
}

# ============================================================================
# WINDOW ACTIVATION
# ============================================================================

activate_window() {
    local wid="$1"
    log "ACTIVATING: [$wid] ${WINDOW_CLASS[$wid]} - ${WINDOW_TITLE[$wid]}"
    xdotool windowactivate "$wid" 2>/dev/null || true
}

# ============================================================================
# MAIN
# ============================================================================

main() {
    log_section "ATSW MODE STARTED"
    
    # Check if grabber exists
    if [[ ! -x "$GRABBER" ]]; then
        log "ERROR: grab_keys not found"
        echo "ERROR: grab_keys not found. Compile it first:" >&2
        echo "  gcc -o grab_keys grab_keys.c -lX11" >&2
        exit 2
    fi
    
    # Discover windows
    if ! discover_windows; then
        log "ERROR: Window discovery failed"
        exit 2
    fi
    
    # Compute prefixes
    if ! compute_prefixes; then
        log "ERROR: Prefix computation failed"
        exit 2
    fi
    
    # CRITICAL: Sleep is needed for hotkey to release
    sleep 0.15
    
    # Buffer to accumulate keystrokes
    buffer=""
    
    log ""
    log "WAITING FOR INPUT..."
    log "  (Press keys to match prefix, ESC to cancel)"
    log ""
    
    # Start the keyboard grabber and read its output
    while IFS= read -r line; do
        case "$line" in
            "READY")
                # Grabber is ready
                log "Keyboard grabbed, ready for input"
                ;;
            "ESCAPE")
                # User cancelled
                log "CANCELLED by user (ESC)"
                exit 1
                ;;
            "RETURN")
                # User pressed enter - check if we have exactly one match
                if [[ -n "$buffer" ]]; then
                    if check_prefix_match "$buffer"; then
                        activate_window "$MATCH_RESULT"
                        log "FINISHED: Activated window via ENTER"
                        exit 0
                    else
                        log "ENTER pressed but no unique match (buffer='$buffer', matches=$MATCH_COUNT)"
                    fi
                fi
                ;;
            "BACKSPACE")
                # Remove last character
                if [[ -n "$buffer" ]]; then
                    buffer="${buffer%?}"
                    log "BACKSPACE: buffer='$buffer'"
                fi
                ;;
            KEY:*)
                # Regular key - append to buffer
                key="${line#KEY:}"
                buffer="${buffer}${key}"
                log "KEY: '$key' -> buffer='$buffer'"
                
                # Check for matches
                if check_prefix_match "$buffer"; then
                    # Exactly one match - immediately activate
                    log "UNIQUE MATCH: prefix='$MATCHING_PREFIXES'"
                    activate_window "$MATCH_RESULT"
                    log "FINISHED: Activated window"
                    exit 0
                elif [[ $MATCH_COUNT -eq 0 ]]; then
                    # No matches - could beep or show error
                    log "NO MATCH for buffer='$buffer'"
                    # Optionally, we could remove the last char and continue
                    # For now, we'll keep waiting
                else
                    # Multiple matches - keep waiting
                    log "PARTIAL MATCH: $MATCH_COUNT possible ($MATCHING_PREFIXES)"
                fi
                ;;
            SYM:*)
                # Special key - ignore for now
                sym="${line#SYM:}"
                log "SYM: $sym (ignored)"
                ;;
        esac
    done < <("$GRABBER")
    
    # If we get here, grabber exited unexpectedly
    log "ERROR: Grabber exited unexpectedly"
    exit 2
}

main "$@"
