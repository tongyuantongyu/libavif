git clone -b v0.16.3 --depth 1 https://chromium.googlesource.com/codecs/libgav1

cd libgav1
patch -p1 < ../libgav1_fix_mingw_build.patch
git clone -b lts_2021_03_24 --depth 1 https://github.com/abseil/abseil-cpp.git third_party/abseil-cpp
mkdir build
cd build

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIBGAV1_THREADPOOL_USE_STD_MUTEX=1 ..
ninja
cd ../..
