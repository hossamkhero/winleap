{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "winleap-dev";

  buildInputs = with pkgs; [
    # X11 libs for winleap
    xorg.libX11
    xorg.libXi
    xorg.libXtst

    # Compiler
    gcc
    pkg-config

    # Useful X11 tools for debugging/testing
    xdotool
    xorg.xev
    xorg.xinput
    xorg.xprop
    xorg.xwininfo
  ];

  shellHook = ''
    echo "Winleap Development Shell"
    echo "========================="
    echo ""
    echo "To compile winleap:"
    echo "  gcc -O2 -Wall -Wextra -o winleap winleap.c -lX11"
    echo ""
    echo "To run it:"
    echo "  ./winleap 1"
    echo "  ./winleap --current-workspace 1"
    echo "  ./winleap --debug 1"
    echo ""

    # switch to zsh once done
    zsh
  '';
}
