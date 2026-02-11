{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
  pname = "winleap";
  version = "0.1.0";

  src = ./.;

  buildInputs = [
    pkgs.xorg.libX11
  ];

  buildPhase = ''
    $CC -O2 -Wall -Wextra -o winleap winleap.c -lX11
  '';

  installPhase = ''
    mkdir -p $out/bin
    mkdir -p $out/share/doc/winleap
    cp winleap $out/bin/
    cp winleap.conf.example $out/share/doc/winleap/
  '';
}
