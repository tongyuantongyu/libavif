git clone -b 0.5 --depth 1 https://github.com/xiph/rav1e.git

cd rav1e

# Debug symbols bloats the binary
patch -p1 < ../rav1e_no_debug_symbol.patch
echo stable-x86_64-pc-windows-gnu > rust-toolchain
mkdir build.libavif
cargo cinstall --release --library-type=staticlib --prefix=usr --destdir build.libavif
cd ..
