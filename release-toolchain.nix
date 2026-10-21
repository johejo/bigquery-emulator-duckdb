{ pkgs, libcPkgs }:
let
  # nixpkgs skipped glibc 2.28 (2.27 -> 2.30). Use its last 19.03 revision's
  # 2.27 sysroot; the release binary may require up to 2.28, including DuckDB.
  # threadModel is metadata expected by the current split GCC runtime packages.
  libc = libcPkgs.glibc // { threadModel = "posix"; };
  llvm = pkgs.llvmPackages_23;
  gccPackages = pkgs.gccNGPackages_16;
  gccCompiler = pkgs.gcc.cc;
  bintools = pkgs.wrapBintoolsWith {
    bintools = llvm.bintools-unwrapped;
    inherit libc;
  };
  # GCC needs GNU as while building its runtimes; Clang uses its integrated
  # assembler and the LLD wrapper above for the emulator itself.
  gccBintools = pkgs.wrapBintoolsWith {
    bintools = pkgs.binutils-unwrapped;
    inherit libc;
  };
  wrapGcc = runtime: pkgs.wrapCCWith {
    cc = gccCompiler;
    inherit libc;
    bintools = gccBintools;
    extraPackages = [ runtime ];
    # Suppress the compiler's baked-in glibc include path, retaining its own
    # intrinsic headers and the old libc headers supplied by the wrapper.
    nixSupport.cc-cflags = [
      "-B${runtime}/lib"
      "-nostdinc"
      "-isystem" "${gccCompiler}/lib/gcc/${pkgs.stdenv.hostPlatform.config}/${gccCompiler.version}/include"
    ];
  };
  libgcc = (gccPackages.libgcc.override {
    stdenv = pkgs.overrideCC pkgs.stdenv (wrapGcc gccPackages.libgcc-no-libc);
  }).overrideAttrs (previous: {
    # The split package also builds a pre-libc libgcc and clears SHLIB_LC.
    # Restore it here: old loaders need the dependency for libc's IFUNCs.
    makeFlags = previous.makeFlags ++ [ "SHLIB_LC=-lc" ];
  });
  runtimeStdenv = pkgs.overrideCC pkgs.stdenv (wrapGcc libgcc);
  libstdcxx = (gccPackages.libstdcxx.override {
    stdenv = runtimeStdenv;
    inherit libgcc;
    libbacktrace = pkgs.libbacktrace.override {
      stdenv = runtimeStdenv;
      enableStatic = true;
    };
  }).overrideAttrs (previous: {
    dontDisableStatic = true;
    configureFlags = previous.configureFlags ++ [ "--enable-static" ];
  });
  clang = pkgs.wrapCCWith {
    cc = llvm.clang-unwrapped;
    inherit libc bintools;
    libcxx = libstdcxx;
    # Do not pull the default GCC's runtimes, built against the current glibc,
    # into the target's search paths. The compiler itself still runs on Nix.
    gccForLibs = null;
    extraPackages = [ libgcc ];
    # The wrapper exposes C++ headers even for `clang file.c`. Keep GCC's
    # C++ compatibility headers (notably stdatomic.h) out of C compilation.
    extraBuildCommands = ''
      substituteInPlace "$out/nix-support/libcxx-cxxflags" \
        --replace-fail '-isystem ' '-cxx-isystem '
    '';
    nixSupport = {
      cc-cflags = [ "-B${libgcc}/lib" ];
      cc-ldflags = [ "-L${libgcc}/lib" "-L${libstdcxx}/lib" ];
    };
  };
in
{
  inherit libc bintools clang;
}
