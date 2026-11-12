vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO krisoft20/vkdeflate
    REF "v${VERSION}"
    SHA512 5aa27aed4e2985cd60decc0783fee8e68e0f1bac2b02bc6dae3d32fcdc9f511f204c2a174ada2f2aa2ebd7361a4549271c03ded8c930f74b805c281a5d39e35b
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
