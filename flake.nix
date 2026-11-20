{
  description = "RamkolFX - 3D spatial audio, EQ and bass enhancer for PipeWire";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      packages = forAll (pkgs:
        let
          # Pinned FetchContent dependencies (see daemon/ and gui/ CMakeLists).
          mysofa = pkgs.fetchurl {
            url = "https://github.com/hoene/libmysofa/archive/refs/tags/v1.3.5.tar.gz";
            sha256 = "f29508c335c83d8703f943ffc9ca783ac39aca84e851357f13a55af0f8143137";
          };
          kissfft = pkgs.fetchurl {
            url = "https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz";
            sha256 = "205a8f6a448ef12b091f8ac6a514b5091bb5f6b0b543431ed75f673116cf5cbf";
          };
          imgui = pkgs.fetchurl {
            url = "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.6.tar.gz";
            sha256 = "c5fbc5dcab1d46064001c3b84d7a88812985cde7e0e9ced03f5677bec1ba502a";
          };
          ortLib = "${pkgs.lib.getLib pkgs.onnxruntime}/lib/libonnxruntime.so";
          ramkolfx = pkgs.stdenv.mkDerivation {
            pname = "ramkolfx";
            version = "0.2.0";
            src = pkgs.lib.cleanSourceWith {
              src = ./.;
              filter = path: type:
                let b = baseNameOf path; in
                !(builtins.elem b [ "build" ".git" ".cache" "dist" ]);
            };

            nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
            buildInputs = with pkgs; [ pipewire zlib SDL2 fontconfig dbus libGL libx11 onnxruntime ];

            # No network in the sandbox: unpack the pinned deps up front and
            # point FetchContent at them.
            preConfigure = ''
              mkdir -p $TMPDIR/deps
              tar -xzf ${mysofa} -C $TMPDIR/deps
              tar -xzf ${kissfft} -C $TMPDIR/deps
              tar -xzf ${imgui} -C $TMPDIR/deps
              cmakeFlagsArray+=($(sh packaging/ci/vendor-flags.sh $TMPDIR/deps))
              # Auto-detect dlopens ONNX Runtime; Nix's linker can't find it by name.
              test -e ${ortLib} || { echo "missing ${ortLib}" >&2; exit 1; }
            '';
            cmakeFlags = [
              "-DRAMKOLFX_BUILD_GUI=ON"
              "-DRAMKOLFX_BUILD_TESTS=OFF"
              "-DRAMKOLFX_HRTF_DATA_DIR=${placeholder "out"}/share/ramkolfx/hrtf"
              "-DRAMKOLFX_MODEL_DATA_DIR=${placeholder "out"}/share/ramkolfx/models"
              "-DRAMKOLFX_ORT_LIBRARY=${ortLib}"
              "-DRAMKOLFX_SYSTEMD_USER_UNIT_DIR=${placeholder "out"}/lib/systemd/user"
            ];
            # Skip the statically-linked dependencies' own install rules.
            installPhase = ''
              runHook preInstall
              cmake --install . --component ramkolfx
              runHook postInstall
            '';

            meta = {
              description = "3D spatial audio, EQ and bass enhancer for PipeWire";
              homepage = "https://github.com/Coldsteel48/RamkolFX";
              license = pkgs.lib.licenses.gpl3Only;
              platforms = pkgs.lib.platforms.linux;
              mainProgram = "ramkolfx-gui";
            };
          };
        in { default = ramkolfx; ramkolfx = ramkolfx; });

      apps = forAll (pkgs: {
        default = { type = "app"; program = "${self.packages.${pkgs.system}.default}/bin/ramkolfx-gui"; };
        daemon  = { type = "app"; program = "${self.packages.${pkgs.system}.default}/bin/ramkolfxd"; };
      });
    };
}
