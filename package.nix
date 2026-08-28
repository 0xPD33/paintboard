{ lib, stdenv, pkg-config, glfw, libGL }:

stdenv.mkDerivation {
  pname = "paintboard";
  version = "0.1.0";

  src = lib.cleanSource ./.;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ glfw libGL ];

  makeFlags = [ "PREFIX=$(out)" ];

  # The self test never opens a window, so it runs fine in the sandbox.
  doCheck = true;
  checkTarget = "test";

  meta = {
    description = "Local offline drawing board for sketches and diagrams";
    homepage = "https://github.com/0xPD33/paintboard";
    license = lib.licenses.mit;
    mainProgram = "paintboard";
    platforms = lib.platforms.linux;
  };
}
