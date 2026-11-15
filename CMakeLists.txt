cmake_minimum_required(VERSION 3.21)
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/switch.toolchain.cmake")
endif()
project(reNsanity LANGUAGES C CXX)
enable_language(ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)

set(TE_GAME "${CMAKE_SOURCE_DIR}/game")
set(TE_ENGINE "${CMAKE_SOURCE_DIR}/engine")
set(TE_TOOLS "${CMAKE_SOURCE_DIR}/tools")
set(TE_WASI_SDK "${TE_TOOLS}/toolchain/wasi-sdk")
set(TE_WABT "${TE_TOOLS}/toolchain/wabt")
foreach(tool "${TE_WASI_SDK}/bin/clang++" "${TE_WASI_SDK}/bin/wasm-ld" "${TE_WABT}/bin/wasm2c" "${TE_WABT}/bin/wasm-objdump")
    if(NOT EXISTS "${tool}")
        message(FATAL_ERROR "reNsanity: ${tool} missing -- run ./build.sh")
    endif()
endforeach()

set(TE_GEN "${CMAKE_BINARY_DIR}/gen")
set(TE_WASM "${CMAKE_BINARY_DIR}/wasm")
set(TE_W2C "${CMAKE_BINARY_DIR}/w2c")
file(MAKE_DIRECTORY "${TE_GEN}/seam" "${TE_WASM}/obj" "${TE_WASM}/rep" "${TE_W2C}")

find_package(SDL2 CONFIG REQUIRED)
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
find_library(FFMPEG_AVCODEC_LIBRARY NAMES avcodec)
find_library(FFMPEG_AVUTIL_LIBRARY NAMES avutil)
find_library(FFMPEG_SWSCALE_LIBRARY NAMES swscale)
foreach(component avcodec avutil swscale)
    string(TOUPPER ${component} upper)
    add_library(FFmpeg::${component} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${component} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_${upper}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
endforeach()
find_library(TE_DEKO3D_LIBRARY deko3d HINTS "$ENV{DEVKITPRO}/libnx/lib" REQUIRED)

add_subdirectory(engine)

add_compile_definitions(_GNU_SOURCE)
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>" "$<$<COMPILE_LANGUAGE:C,CXX>:-frounding-math>")
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fPIE>" "$<$<COMPILE_LANGUAGE:C,CXX>:-fomit-frame-pointer>"
                    "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-math-errno>")

file(GLOB TE_SEAM_HEADERS CONFIGURE_DEPENDS "${TE_GAME}/include/platform/*.h")
set(TE_SEAM_FORWARDERS "")
foreach(h ${TE_SEAM_HEADERS})
    get_filename_component(stem "${h}" NAME_WE)
    if(NOT stem STREQUAL "memory")
        list(APPEND TE_SEAM_FORWARDERS "${TE_GEN}/seam/seam_forwarders_${stem}.cpp")
    endif()
endforeach()
list(APPEND TE_SEAM_FORWARDERS "${TE_GEN}/seam/seam_forwarders_second.cpp")
set(TE_SEAM_OUTPUTS "${TE_GEN}/seam/ps2e_seam.h" "${TE_GEN}/seam/seam_imports.txt" "${TE_GEN}/seam/seam_host_thunks.c"
    "${TE_GEN}/seam/seam_stubs.cpp" ${TE_SEAM_FORWARDERS})
add_custom_command(OUTPUT ${TE_SEAM_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_seam.py" --game "${TE_GAME}" --out "${TE_GEN}/seam" --module game
    DEPENDS "${TE_TOOLS}/gen_seam.py" "${TE_TOOLS}/second_seam.tsv" "${TE_TOOLS}/wasm_side_functions.txt"
            "${TE_TOOLS}/platform_imports.tsv" ${TE_SEAM_HEADERS}
    COMMENT "gen_seam: the platform seam (imports, forwarders, host thunks, stubs)")
add_custom_target(te_seam DEPENDS ${TE_SEAM_OUTPUTS})

file(GLOB TE_GAME_SOURCES CONFIGURE_DEPENDS "${TE_GAME}/src/game/*.cpp" "${TE_GAME}/src/*.cpp")
list(APPEND TE_GAME_SOURCES "${TE_GAME}/src/platform/ps2/libc.cpp" "${TE_GAME}/src/platform/ps2/libm.cpp"
    "${TE_GAME}/src/platform/ps2/malloc.cpp")
file(GLOB TE_WASM_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/*.cpp")
file(GLOB TE_GAME_HEADERS CONFIGURE_DEPENDS "${TE_GAME}/include/*.h" "${TE_GAME}/include/*/*.h"
    "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim/*.h" "${CMAKE_SOURCE_DIR}/platform/nativeee/*.h")
set(TE_CXX ${Python3_EXECUTABLE} "${TE_TOOLS}/te_cxx.py" --wasi-sdk "${TE_WASI_SDK}" --game "${TE_GAME}"
    --shim "${CMAKE_SOURCE_DIR}/platform/nativeee/sdk_shim" -I "${TE_GEN}/seam" -I "${CMAKE_SOURCE_DIR}/platform/nativeee")
file(MAKE_DIRECTORY "${TE_GEN}/vtable")
add_custom_command(OUTPUT "${TE_GEN}/vtable/vtable_thunks.cpp"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_vtable_thunks.py" --tsv "${TE_TOOLS}/vtable_slots.tsv" --pool 48
            --wasm-out "${TE_GEN}/vtable/vtable_thunks.cpp"
    DEPENDS "${TE_TOOLS}/gen_vtable_thunks.py" "${TE_TOOLS}/vtable_slots.tsv"
    COMMENT "gen_vtable_thunks: type-adapted vtable trampolines (wasm side)")
set(TE_WASM_OBJECTS "")
foreach(src ${TE_GAME_SOURCES} ${TE_WASM_PLATFORM_SOURCES} ${TE_SEAM_FORWARDERS} "${TE_GEN}/vtable/vtable_thunks.cpp")
    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${src}")
    string(REPLACE "/" "_" obj "${rel}")
    set(out "${TE_WASM}/obj/${obj}.o")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${TE_CXX} -o "${out}" "${src}"
        DEPENDS "${src}" "${TE_TOOLS}/te_cxx.py" ${TE_GAME_HEADERS} "${TE_GEN}/seam/ps2e_seam.h"
        COMMENT "wasm32 ${rel}")
    list(APPEND TE_WASM_OBJECTS "${out}")
endforeach()
add_custom_command(OUTPUT "${TE_WASM}/obj/wasi_stubs.o"
    COMMAND "${TE_WASI_SDK}/bin/clang" --target=wasm32-wasi -O2 -c "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/wasi_stubs.c" -o "${TE_WASM}/obj/wasi_stubs.o"
    DEPENDS "${CMAKE_SOURCE_DIR}/platform/nativeee/wasm/wasi_stubs.c" COMMENT "wasm32 wasi_stubs.c")
list(APPEND TE_WASM_OBJECTS "${TE_WASM}/obj/wasi_stubs.o")
set(TE_WASM_OBJECTS_RSP "${TE_WASM}/objects.rsp")
list(JOIN TE_WASM_OBJECTS "\n" _te_objects_text)
set(_te_objects_text "${_te_objects_text}\n")
if(EXISTS "${TE_WASM_OBJECTS_RSP}")
    file(READ "${TE_WASM_OBJECTS_RSP}" _te_objects_old)
else()
    set(_te_objects_old "")
endif()
if(NOT _te_objects_text STREQUAL _te_objects_old)
    file(WRITE "${TE_WASM_OBJECTS_RSP}" "${_te_objects_text}")
endif()

add_custom_command(OUTPUT "${TE_WASM}/retail_image.s"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_retail_image.py" --game "${TE_GAME}" --nm "${TE_WASI_SDK}/bin/llvm-nm"
            --out "${TE_WASM}/retail_image.s" "@${TE_WASM_OBJECTS_RSP}"
    DEPENDS "${TE_TOOLS}/gen_retail_image.py" "${TE_TOOLS}/rspfile.py" "${TE_GAME}/symbol_addrs.txt" ${TE_WASM_OBJECTS} "${TE_WASM_OBJECTS_RSP}"
    COMMENT "gen_retail_image: retail data symbols at retail addresses")
add_custom_command(OUTPUT "${TE_WASM}/retail_image.o"
    COMMAND "${TE_WASI_SDK}/bin/clang" --target=wasm32-wasi -c "${TE_WASM}/retail_image.s" -o "${TE_WASM}/retail_image.o"
    DEPENDS "${TE_WASM}/retail_image.s")
add_custom_command(OUTPUT "${TE_WASM}/retail_table.c"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_retail_table.py" --game "${TE_GAME}" --nm "${TE_WASI_SDK}/bin/llvm-nm"
            --out "${TE_WASM}/retail_table.c" "@${TE_WASM_OBJECTS_RSP}"
    DEPENDS "${TE_TOOLS}/gen_retail_table.py" "${TE_TOOLS}/rspfile.py" "${TE_GAME}/symbol_addrs.txt" ${TE_WASM_OBJECTS} "${TE_WASM_OBJECTS_RSP}"
    COMMENT "gen_retail_table: retail code address -> function")
add_custom_command(OUTPUT "${TE_WASM}/retail_table.o"
    COMMAND "${TE_WASI_SDK}/bin/clang" --target=wasm32-wasi -O2 -c "${TE_WASM}/retail_table.c" -o "${TE_WASM}/retail_table.o"
    DEPENDS "${TE_WASM}/retail_table.c")

set(TE_WASM_EXPORTS entry te_retail_image te_retail_functions te_retail_function_count __heap_base __data_end
    te_vt_thunks te_vt_thunk_count te_vt_targets te_vt_signature_count)
set(TE_EXPORT_FLAGS "")
foreach(e ${TE_WASM_EXPORTS})
    list(APPEND TE_EXPORT_FLAGS "--export=${e}")
endforeach()
add_custom_command(OUTPUT "${TE_WASM}/game.wasm"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/link_wasm.py" --wasm-ld "${TE_WASI_SDK}/bin/wasm-ld"
            --wasm-objdump "${TE_WABT}/bin/wasm-objdump" --image-start 2989456 --image-end 4043264
            --stack-size 262144 --out "${TE_WASM}/game.wasm" --
            "${TE_WASM}/retail_table.o" "@${TE_WASM_OBJECTS_RSP}"
            -L "${TE_WASI_SDK}/share/wasi-sysroot/lib/wasm32-wasi" -lc "${TE_WASM}/retail_image.o"
            --no-entry --import-memory --initial-memory=33554432 --max-memory=33554432
            --allow-undefined-file="${TE_GEN}/seam/seam_imports.txt" ${TE_EXPORT_FLAGS} --export-table --gc-sections
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/check_module.py" --wasm-objdump "${TE_WABT}/bin/wasm-objdump"
            --wasm "${TE_WASM}/game.wasm" --imports "${TE_GEN}/seam/seam_imports.txt" --image-start 2989456
            --image-end 4043264
    DEPENDS "${TE_WASM}/retail_image.o" "${TE_WASM}/retail_table.o" ${TE_WASM_OBJECTS} "${TE_WASM_OBJECTS_RSP}" "${TE_GEN}/seam/seam_imports.txt"
            "${TE_TOOLS}/check_module.py" "${TE_TOOLS}/link_wasm.py" "${TE_TOOLS}/rspfile.py"
    COMMENT "wasm-ld game.wasm in two passes (+ import/export/layout check)")

set(TE_W2C_PARTS 8)
set(TE_W2C_SOURCES "")
math(EXPR last "${TE_W2C_PARTS} - 1")
foreach(i RANGE 0 ${last})
    list(APPEND TE_W2C_SOURCES "${TE_W2C}/game_${i}.c")
endforeach()
add_custom_command(OUTPUT ${TE_W2C_SOURCES} "${TE_W2C}/game.h" "${TE_W2C}/game-impl.h"
    COMMAND "${TE_WABT}/bin/wasm2c" "${TE_WASM}/game.wasm" -o "${TE_W2C}/game.c" --module-name=game --num-outputs=${TE_W2C_PARTS}
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_call_indirect.py" "${TE_W2C}/game-impl.h" --adapt-header "${TE_GEN}/eabi_adapt.h"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_sigfix.py" "${TE_W2C}/game-impl.h" ${TE_W2C_SOURCES}
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_unchecked.py" "${TE_W2C}/game-impl.h"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_f32const.py" ${TE_W2C_SOURCES}
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/w2c_prefetch.py" "${TE_W2C}/game-impl.h" ${TE_W2C_SOURCES}
    DEPENDS "${TE_WASM}/game.wasm" "${TE_TOOLS}/w2c_call_indirect.py" "${TE_TOOLS}/w2c_sigfix.py" "${TE_TOOLS}/w2c_prefetch.py"
            "${TE_TOOLS}/w2c_unchecked.py" "${TE_TOOLS}/w2c_f32const.py"
    COMMENT "wasm2c game.wasm (+ call_indirect type-adapter fallback + direct-call signature adapters)")
add_custom_command(OUTPUT "${TE_GEN}/vtable/vtable_slots_host.c"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_vtable_thunks.py" --tsv "${TE_TOOLS}/vtable_slots.tsv"
            --host-out "${TE_GEN}/vtable/vtable_slots_host.c" --wasm "${TE_WASM}/game.wasm" --wasm-objdump "${TE_WABT}/bin/wasm-objdump"
            --symbols "${TE_GAME}/symbol_addrs.txt" --module game
    DEPENDS "${TE_WASM}/game.wasm" "${TE_TOOLS}/gen_vtable_thunks.py" "${TE_TOOLS}/vtable_slots.tsv" "${TE_GAME}/symbol_addrs.txt"
    COMMENT "gen_vtable_thunks: the vtable slot table for the patcher (host side)")
add_custom_command(OUTPUT "${TE_GEN}/eabi_dispatch.c" "${TE_GEN}/eabi_adapt.h"
    COMMAND ${Python3_EXECUTABLE} "${TE_TOOLS}/gen_eabi_dispatch.py" --wasm-objdump "${TE_WABT}/bin/wasm-objdump"
            --wasm "${TE_WASM}/game.wasm" --module game --out "${TE_GEN}/eabi_dispatch.c" --adapt-header "${TE_GEN}/eabi_adapt.h"
    DEPENDS "${TE_WASM}/game.wasm" "${TE_TOOLS}/gen_eabi_dispatch.py" COMMENT "gen_eabi_dispatch (+ call-site type adapters)")
set_source_files_properties(${TE_W2C_SOURCES} PROPERTIES OBJECT_DEPENDS "${TE_GEN}/eabi_adapt.h")

set(TE_WASM_RT "${TE_WABT}/share/wabt/wasm2c")
add_library(tegame STATIC
    ${TE_W2C_SOURCES}
    "${TE_WASM_RT}/wasm-rt-impl.c"
    "${TE_WASM_RT}/wasm-rt-mem-impl.c"
    "${TE_WASM_RT}/wasm-rt-exceptions-impl.c"
    "${TE_GEN}/seam/seam_host_thunks.c"
    "${TE_GEN}/eabi_dispatch.c"
    "${TE_GEN}/vtable/vtable_slots_host.c")
target_include_directories(tegame PUBLIC "${TE_W2C}" "${TE_WABT}/include" "${TE_WASM_RT}" "${TE_GEN}/seam" "${TE_GEN}")
target_compile_definitions(tegame PUBLIC WASM_RT_USE_MMAP=0 WASM_RT_MEMCHECK_BOUNDS_CHECK=1)
target_compile_definitions(tegame PUBLIC TE_W2C_UNCHECKED_MEMORY=1)
target_compile_definitions(tegame PUBLIC WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1)
target_compile_definitions(tegame PUBLIC TE_W2C_STATIC_RAM=1)
set_target_properties(tegame PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_options(tegame PRIVATE -O2 -w)
set_source_files_properties(${TE_W2C_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")
add_dependencies(tegame te_seam)
target_include_directories(tegame BEFORE PUBLIC "${CMAKE_SOURCE_DIR}/platform/nativeee/shim")
target_compile_options(tegame PUBLIC "$<$<COMPILE_LANGUAGE:C,CXX>:-include${CMAKE_SOURCE_DIR}/platform/nativeee/shim/te_setjmp.h>")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(TE_ENGINE_LIBS
    ps2r_title_hle
    ps2r_title_vu1_library
    ps2r_title_audio
    ps2r_ps2e_draw
    ps2r_ps2e_movie
    ps2r_ipu
    ps2r_gs_hle_texsrc
    ps2r_gs_hle_targets
    ps2r_gs_xfer
    ps2r_gs_vram
    ps2r_render_deko
    ps2r_platform_sdl
    ps2r_gs_ir
    ps2r_gs_format
    ps2r_cdvd
    ps2r_vu
    ps2r_ee
    ps2r_sound
    ps2r_core
    ps2r_host)
add_library(nativeee_libs INTERFACE)
target_link_libraries(nativeee_libs INTERFACE ${TE_ENGINE_LIBS})
target_include_directories(nativeee_libs INTERFACE "${TE_ENGINE}")
target_link_libraries(nativeee_libs INTERFACE SDL2::SDL2 FFmpeg::avcodec FFmpeg::swscale swresample FFmpeg::avutil dav1d z m "${TE_DEKO3D_LIBRARY}")

add_subdirectory(platform/nativeee)

add_executable(reNsanity runner/main.cpp runner/te_env.cpp runner/te_run.c
    runner/te_fiber.c runner/te_fiber_switch.S runner/te_game.cpp
    runner/te_switch.cpp runner/te_switch_malloc.cpp runner/te_switch_exit.cpp)
set_target_properties(reNsanity PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_include_directories(reNsanity PRIVATE "${CMAKE_SOURCE_DIR}/runner" "${CMAKE_SOURCE_DIR}/platform/nativeee/host")
target_compile_options(reNsanity PRIVATE -Wall -Wextra)
target_link_options(reNsanity PRIVATE -Wl,--wrap=atexit,--wrap=exit)
target_link_options(reNsanity PRIVATE -Wl,--wrap=__syscall_thread_create,--wrap=threadClose)
target_link_options(reNsanity PRIVATE -Wl,--wrap=_malloc_r,--wrap=_free_r,--wrap=_memalign_r,--wrap=_calloc_r)
target_link_libraries(reNsanity PRIVATE tegame -Wl,--whole-archive te_platform_host -Wl,--no-whole-archive nativeee_libs te_seam_stubs)

nx_generate_nacp(reNsanity.nacp NAME "reNsanity" AUTHOR "xwrtr" VERSION "1.0.0")
nx_create_nro(reNsanity NACP reNsanity.nacp ICON "${CMAKE_SOURCE_DIR}/runner/icon.jpg")
