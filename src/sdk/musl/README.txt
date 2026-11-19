Bundled musl 1.2.5
==================

The include/ and lib/ directories contain the musl C library for the
rv32imafc / ilp32f target. COPYRIGHT is the complete upstream notice and
must accompany redistribution of the library. The SDK's top-level Apache
license does not replace this third-party notice.

Upstream source: https://musl.libc.org/releases/musl-1.2.5.tar.gz
Upstream tarball SHA256: a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4

The build recipe is openfpgaCore/src/firmware/musl/build_musl.sh.
Recipe SHA256: e0172d1efbe3c53c458226edd8dd65bd58ca8fd2df23c3156a9c8fba48b674f8
It applies the RISC-V single-float setjmp/longjmp adaptation before building
static libraries. Use the SDK's pinned toolchain when rebuilding; these
hashes identify the bundled files, not a claim of reproduction by any
host compiler. No library binaries were changed during this notice audit.

Bundled file SHA256:
de6bb084326aefa2f1a2ca8d82780f972162f018764b05f366c44849960adaf0  lib/libc.a
f0a17a43c74d2fe5474fa2fd29c8f14799e777d7d75a2cc4d11c20a6e7b161c5  lib/libm.a
74f49f2cec2e69b05e2e2bbbd729ed853b5639c3dbd1b80f0f63be294083effa  lib/crt1.o
9b11766315db4e143727fe6c9f033ee8dcac0fcad3fe8bf12713a4ae9066f4e0  lib/crti.o
17c88dd9136333e1c98b51f128d054bb7ac777110b9415cf6b3dd660e610a0cc  lib/crtn.o
