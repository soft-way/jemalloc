
How to install jemalloc in vcpkg for Windows
=================================

1. Backup existing old version jemalloc in vcpkg HOME /ports/jemalloc

2. Copy vcpkg/ports into vcpkg HOME
   You need to change SHA512 in file ports/jemalloc/portfile.cmake

3. Install jemalloc
   vcpkg install jemalloc:x64-windows
