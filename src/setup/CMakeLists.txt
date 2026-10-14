# Luling setup bootstrapper (replaces Inno Setup; see installer-plan).
# A directory of its own so that it can be optimized for size (/O1) without
# overriding the project's /O2 per target (D9025).

foreach(lang C CXX)
    string(REPLACE "/O2" "/O1" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "/Ob1" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
endforeach()

# The project adds /W4 for every directory; set it per target here so the
# vendored LZMA SDK can use /W0 without D9025 override warnings.
get_directory_property(setup_dir_options COMPILE_OPTIONS)
list(REMOVE_ITEM setup_dir_options /W4)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${setup_dir_options}")

set(LULING_LZMA_DIR "${PROJECT_SOURCE_DIR}/third_party/lzma/C")
add_library(luling_lzma_dec STATIC
    ${LULING_LZMA_DIR}/7zCrc.c ${LULING_LZMA_DIR}/7zCrcOpt.c ${LULING_LZMA_DIR}/Alloc.c
    ${LULING_LZMA_DIR}/Bra.c ${LULING_LZMA_DIR}/Bra86.c ${LULING_LZMA_DIR}/BraIA64.c
    ${LULING_LZMA_DIR}/CpuArch.c ${LULING_LZMA_DIR}/Delta.c ${LULING_LZMA_DIR}/Lzma2Dec.c
    ${LULING_LZMA_DIR}/LzmaDec.c ${LULING_LZMA_DIR}/Sha256.c ${LULING_LZMA_DIR}/Sha256Opt.c
    ${LULING_LZMA_DIR}/Xz.c ${LULING_LZMA_DIR}/XzCrc64.c ${LULING_LZMA_DIR}/XzCrc64Opt.c
    ${LULING_LZMA_DIR}/XzDec.c)
target_include_directories(luling_lzma_dec PUBLIC ${LULING_LZMA_DIR})
target_compile_definitions(luling_lzma_dec PUBLIC Z7_ST)
target_compile_options(luling_lzma_dec PRIVATE /W0 /TC)

add_library(luling_setup_payload STATIC setup_payload.cpp)
target_link_libraries(luling_setup_payload PUBLIC luling_lzma_dec)

add_executable(luling_setup_p0 setup_p0_main.cpp)
target_link_libraries(luling_setup_p0 PRIVATE luling_setup_payload)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/setup.ico
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/pack_installer/make_setup_icon.py
            ${PROJECT_SOURCE_DIR}/src/app/luling.ico ${CMAKE_CURRENT_BINARY_DIR}/setup.ico
    DEPENDS ${PROJECT_SOURCE_DIR}/src/app/luling.ico ${PROJECT_SOURCE_DIR}/tools/pack_installer/make_setup_icon.py
    VERBATIM)
set_source_files_properties(setup.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/setup.ico)

# Wizard UI + install engine; installed copies run as the uninstaller.
add_executable(luling_setup WIN32
    setup_main.cpp setup_engine.cpp setup_common.cpp setup_log.cpp setup_options.cpp setup_process.cpp
    setup_registry.cpp setup_service.cpp setup_shortcut.cpp setup_transaction.cpp setup_strings.cpp
    setup_ui.cpp setup_uninstall.cpp setup.rc setup.manifest ${CMAKE_CURRENT_BINARY_DIR}/setup.ico)
target_include_directories(luling_setup PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(luling_setup PRIVATE luling_setup_payload shell32 ole32 uuid advapi32 user32 gdi32 comctl32
    d2d1 dwrite windowscodecs dwmapi)
target_compile_definitions(luling_setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# The default install is per-user (%LOCALAPPDATA%\Programs), so no elevation is
# needed and asking for it would prompt for nothing. A user who still picks a
# machine-wide directory gets the plain access-denied error instead.
target_link_options(luling_setup PRIVATE /MANIFESTUAC:level='asInvoker')

foreach(setup_target luling_lzma_dec luling_setup_payload luling_setup_p0 luling_setup)
    target_compile_options(${setup_target} PRIVATE $<$<CONFIG:Release>:/Gy /Gw>)
endforeach()
foreach(setup_target luling_setup_payload luling_setup_p0 luling_setup)
    target_compile_options(${setup_target} PRIVATE /W4)
endforeach()
foreach(setup_target luling_setup_p0 luling_setup)
    set_target_properties(${setup_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_options(${setup_target} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endforeach()
