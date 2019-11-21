include(vcpkg_common_functions)

vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO soft-way/jemalloc
    REF 5.2.1-1
    SHA512 f8be1d60ee5f63f9b476e3b67de4633de90175ea8341940484bc22c69baa9421d441cb46f834d7232499fbf3c398369e28cf63c8c0eac576baa696b1c904d81f
    HEAD_REF master
)

if (VCPKG_CRT_LINKAGE STREQUAL "dynamic")
    set(BUILD_STATIC_LIBRARY OFF)
else()
    set(BUILD_STATIC_LIBRARY ON)
endif()
vcpkg_configure_cmake(
    DISABLE_PARALLEL_CONFIGURE
    SOURCE_PATH ${SOURCE_PATH}
    PREFER_NINJA
    OPTIONS -DGIT_FOUND=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON -Dwithout-export=${BUILD_STATIC_LIBRARY}
)

vcpkg_install_cmake()

vcpkg_copy_pdbs()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)

# Handle copyright
file(COPY ${SOURCE_PATH}/COPYING DESTINATION ${CURRENT_PACKAGES_DIR}/share/jemalloc)
file(RENAME ${CURRENT_PACKAGES_DIR}/share/jemalloc/COPYING ${CURRENT_PACKAGES_DIR}/share/jemalloc/copyright)
