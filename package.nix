{ lib, stdenv, pkg-config, glfw, libGL, cjson, python3 }:

stdenv.mkDerivation {
  pname = "paintboard";
  version = "0.2.0";

  src = lib.cleanSourceWith {
    src = ./.;
    filter = path: type:
      lib.cleanSourceFilter path type
      && !(type == "regular" && baseNameOf path == "paintboard");
  };

  nativeBuildInputs = [ pkg-config python3 ];
  buildInputs = [ glfw libGL cjson ];
  nativeCheckInputs = [ python3 ];

  makeFlags = [ "PREFIX=$(out)" "PYTHON=${python3}/bin/python3" ];
  postInstall = ''
    patchShebangs $out/bin/paintboard-bridge
  '';

  # Unit and MCP stdio tests run without a window.
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
