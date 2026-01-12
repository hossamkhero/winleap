#!/usr/bin/env bash

#
# atsw_debug.sh - Debug helper that shows window discovery without grabbing keyboard
#

set -euo pipefail

echo "========================================"
echo "  ATSW WINDOW DISCOVERY DEBUG"
echo "========================================"
echo ""

# Associative arrays
declare -A WINDOW_CLASS
declare -A WINDOW_TITLE
declare -A APP_WINDOWS

# Get all visible windows
wids=$(xdotool search --onlyvisible --name "" 2>/dev/null || true)

echo "DISCOVERED WINDOWS:"
echo "-------------------"

while read -r wid; do
    [[ -z "$wid" ]] && continue
    
    props=$(xprop -id "$wid" WM_CLASS _NET_WM_NAME 2>/dev/null || true)
    
    # Skip windows without proper WM_CLASS
    if ! echo "$props" | grep -q 'WM_CLASS(STRING)'; then
        continue
    fi
    
    wm_class=$(echo "$props" | grep "WM_CLASS(STRING)" | sed 's/.*= "[^"]*", "\([^"]*\)"/\1/' || true)
    [[ -z "$wm_class" ]] && continue
    
    title=$(echo "$props" | grep "_NET_WM_NAME" | sed 's/_NET_WM_NAME.*= "\(.*\)"/\1/' || true)
    [[ -z "$title" ]] && title="(untitled)"
    
    WINDOW_CLASS[$wid]="$wm_class"
    WINDOW_TITLE[$wid]="$title"
    
    app_name=$(echo "$wm_class" | tr '[:upper:]' '[:lower:]')
    
    if [[ -v APP_WINDOWS[$app_name] ]]; then
        APP_WINDOWS[$app_name]="${APP_WINDOWS[$app_name]} $wid"
    else
        APP_WINDOWS[$app_name]="$wid"
    fi
    
    printf "  [%10s] %-25s %s\n" "$wid" "$wm_class" "${title:0:50}"
done <<< "$wids"

echo ""
echo "GROUPED BY APP:"
echo "---------------"
for app in "${!APP_WINDOWS[@]}"; do
    read -ra wids <<< "${APP_WINDOWS[$app]}"
    echo "  $app (${#wids[@]} windows)"
done

echo ""
echo "PREFIX TABLE:"
echo "-------------"

# Compute prefixes
app_names=("${!APP_WINDOWS[@]}")

declare -A PREFIX_TO_WINDOW
declare -A WINDOW_PREFIX

for app in "${app_names[@]}"; do
    prefix=""
    app_len=${#app}
    
    for ((i=1; i<=app_len; i++)); do
        prefix="${app:0:$i}"
        is_unique=true
        
        for other_app in "${app_names[@]}"; do
            [[ "$other_app" == "$app" ]] && continue
            if [[ "${other_app:0:$i}" == "$prefix" ]]; then
                is_unique=false
                break
            fi
        done
        
        if $is_unique; then
            break
        fi
    done
    
    read -ra wids <<< "${APP_WINDOWS[$app]}"
    num_windows=${#wids[@]}
    
    if [[ $num_windows -eq 1 ]]; then
        wid="${wids[0]}"
        PREFIX_TO_WINDOW[$prefix]="$wid"
        WINDOW_PREFIX[$wid]="$prefix"
    else
        idx=1
        for wid in "${wids[@]}"; do
            full_prefix="${prefix}${idx}"
            PREFIX_TO_WINDOW[$full_prefix]="$wid"
            WINDOW_PREFIX[$wid]="$full_prefix"
            ((idx++))
        done
    fi
done

# Print sorted prefix table
printf "\n  %-10s %-25s %s\n" "PREFIX" "APP" "WINDOW TITLE"
printf "  %-10s %-25s %s\n" "------" "---" "------------"

for pfx in $(echo "${!PREFIX_TO_WINDOW[@]}" | tr ' ' '\n' | sort); do
    wid="${PREFIX_TO_WINDOW[$pfx]}"
    printf "  %-10s %-25s %s\n" "'$pfx'" "${WINDOW_CLASS[$wid]}" "${WINDOW_TITLE[$wid]:0:40}"
done

echo ""
echo "========================================"
echo "To switch to a window, type its prefix!"
echo "========================================"
