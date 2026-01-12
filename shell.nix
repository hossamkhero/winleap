{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  name = "atsw-dev";

  buildInputs = with pkgs; [
    # X11 libs for our C program
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

  # Set up pkg-config path for X11
  shellHook = ''
    echo "ATSW Development Shell"
    echo "======================"
    echo ""
    echo "To compile grab_keys:"
    echo "  gcc -o grab_keys grab_keys.c -lX11"
    echo ""
    echo "To test the grabber:"
    echo "  ./grab_keys"
    echo ""
    echo "To test the bash wrapper:"
    echo "  ./atsw_mode.sh"
    echo ""


    # switch to zsh once done
    zsh
  '';
}
