# Winleap

Window marks for direct switching that handles multi-instances of the same window.

### Usage

```bash
./winleap [--config <path>] [--current-workspace] [--debug] <number>
./winleap --open-debug
./winleap --help
```

### Config

Resolution order:

1. `--config <path>`
2. `$XDG_CONFIG_HOME/winleap/winleap.conf`
3. `~/.config/winleap/winleap.conf`
4. `./winleap.conf` (next to executable)

Example `winleap.conf`:

```ini
# mark -> WM_CLASS
1=zen
2=discord
3=obsidian

# first key picks first instance, second picks second; left-side keys help kb+mouse flow
instance_keys=12345qwert

# write logs only when true (or when using --debug); log file is created on first debug run
debug=false
```

Debug Log

- Default debug log path: `$XDG_STATE_HOME/winleap/debug.log`
- Fallback: `~/.local/state/winleap/debug.log`
- Log file is only written when debug is enabled.

### Build

#### Nix

```bash
nix-shell --run "gcc -O2 -Wall -Wextra -o winleap winleap.c -lX11"
```

#### Direct Build

```bash
gcc -O2 -Wall -Wextra -o winleap winleap.c -lX11
```

### TODO

- Wayland support
