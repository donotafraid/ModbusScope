[settings]
os=Linux
arch=x86_64
compiler=clang
compiler.version=17
compiler.libcxx=libstdc++
compiler.cppstd=gnu17
build_type=Debug

[conf]
tools.build:cflags=["-fno-lto"]
tools.build:cxxflags=["-fno-lto", "--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11"]
tools.build:exelinkflags=["-fno-lto", "-L/usr/lib/gcc/x86_64-linux-gnu/11", "-Wl,-rpath,/usr/lib/gcc/x86_64-linux-gnu/11"]
tools.build:sharedlinkflags=["-fno-lto", "-L/usr/lib/gcc/x86_64-linux-gnu/11", "-Wl,-rpath,/usr/lib/gcc/x86_64-linux-gnu/11"]
tools.build:compiler_executables={"c": "/usr/bin/clang-17", "cpp": "/usr/bin/clang++-17"}

[buildenv]
CC=clang-17
CXX=clang++-17