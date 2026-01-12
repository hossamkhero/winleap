{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
  pname = "atsw-grabber";
  version = "0.1.0";

  src = ./.;

  buildInputs = [ 
    pkgs.xorg.libX11 
  ];

  # We only need to compile our one file
  buildPhase = ''
    $CC -o grab_keys grab_keys.c -lX11
  '';

  # Install executionable to $out/bin
  installPhase = ''
    mkdir -p $out/bin
    cp grab_keys $out/bin/
  '';
}
