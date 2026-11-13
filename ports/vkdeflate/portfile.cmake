vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO krisoft20/vkdeflate
    REF "v${VERSION}"
    SHA512 7ffd18913cdcd0e72c99797107df32d43a2f6ec8d49d0875cee54e10c2a8050bb313cda6de1f5ded170ae56f3b9dbbbaefd6b36f7598b6893a6dff4613d5aace
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DVKDEFLATE_TOOLS=OFF
        "-DVULKAN_INCLUDE=${CURRENT_INSTALLED_DIR}/include"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
