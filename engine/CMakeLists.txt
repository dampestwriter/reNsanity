set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-ffp-contract=off -frounding-math)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-semantic-interposition -fomit-frame-pointer -fno-math-errno")

find_program(PS2R_UAM uam HINTS "$ENV{DEVKITPRO}/tools/bin" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engine/render/backends/deko" "${CMAKE_CURRENT_BINARY_DIR}/engine/ipu" "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(PS2R_PORTLIBS "$ENV{DEVKITPRO}/portlibs/switch")
set(PS2R_DEKO_GEN "${CMAKE_CURRENT_BINARY_DIR}/engine/render/backends/deko/dkgen")
set(PS2R_IPU_GEN "${CMAKE_CURRENT_BINARY_DIR}/engine/ipu")
set(PS2R_INPUT_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(ps2r_title_hle_SOURCES
    games/slus_20909_crash_twinsanity/hle/math/math_library.cpp
    games/slus_20909_crash_twinsanity/hle/math/collision_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/culling_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/culling_planes.cpp
    games/slus_20909_crash_twinsanity/hle/math/clipping_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/joint_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/ray_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/particle_view_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/decal_math.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/rounding_guard.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.cpp
    games/slus_20909_crash_twinsanity/hle/input/input.cpp
    games/slus_20909_crash_twinsanity/hle/saves/save_storage.cpp
    games/slus_20909_crash_twinsanity/hle/stream/files.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.cpp
)
set(ps2r_title_vu1_library_SOURCES
    games/slus_20909_crash_twinsanity/vu1_library/library_common.cpp
    games/slus_20909_crash_twinsanity/vu1_library/skinned_lit_strip.cpp
    games/slus_20909_crash_twinsanity/vu1_library/zero_kick.cpp
)
set(ps2r_title_audio_SOURCES
    games/slus_20909_crash_twinsanity/hle/audio/audio.cpp
    games/slus_20909_crash_twinsanity/hle/audio/sample_voice.cpp
    games/slus_20909_crash_twinsanity/hle/audio/volume_ramp.cpp
    games/slus_20909_crash_twinsanity/hle/audio/music_asset.cpp
)
set(ps2r_ps2e_draw_SOURCES
    engine/platform/ps2e/ps2e_draw.cpp
    engine/platform/ps2e/ps2e_draw_backend_switch.cpp
    engine/platform/ps2e/present_pacer.cpp
    engine/platform/ps2e/scene_interpolator.cpp
)
set(ps2r_ps2e_movie_SOURCES
    engine/platform/ps2e/ps2e_movie.cpp
)
set(ps2r_ipu_SOURCES
    engine/ipu/hle/pss_demux.cpp
    engine/ipu/hle/fmv_audio_capture.cpp
    engine/ipu/hle/ffmpeg_video_decoder.cpp
    engine/ipu/hle/yuv_rgba.cpp
)
set(ps2r_gs_hle_texsrc_SOURCES
    engine/gs/hle/texsrc/clut_heal_cache.cpp
    engine/gs/hle/texsrc/texture_decoder.cpp
)
set(ps2r_gs_hle_targets_SOURCES
    engine/gs/hle/targets/vram_layout.cpp
)
set(ps2r_gs_xfer_SOURCES
    engine/gs/xfer/transfer_engine.cpp
)
set(ps2r_gs_vram_SOURCES
    engine/gs/vram/page_record.cpp
    engine/gs/vram/vram_image.cpp
)
set(ps2r_render_deko_SOURCES
    engine/render/backends/deko/dk_device.cpp
    engine/render/backends/deko/dk_draw.cpp
    engine/render/blend_ff_map.cpp
    engine/render/cd_copy_policy.cpp
    engine/render/draw_policy.cpp
    engine/render/gs_specialize.cpp
    engine/render/texture_mips.cpp
    engine/render/vertex_convert.cpp
    "${PS2R_DEKO_GEN}/dk_shaders_gen.cpp"
)
set(ps2r_platform_sdl_SOURCES
    engine/platform/sdl/window.cpp
    engine/platform/sdl/audio_pull_ring.cpp
    engine/platform/sdl/sdl_audio_sink.cpp
    engine/platform/sdl/input/input_mapper.cpp
    engine/platform/sdl/input/gamepad_convert.cpp
    engine/platform/sdl/input/pad_button_map.cpp
    engine/platform/sdl/input/keyboard_mouse_backend.cpp
    engine/platform/sdl/input/gamepad_backend.cpp
    engine/platform/sdl/input/input_system.cpp
)
set(ps2r_gs_ir_SOURCES
    engine/gs/hle/ir/gsdl_format.cpp
)
set(ps2r_gs_format_SOURCES
    engine/gs/format/psm_layout.cpp
    engine/gs/format/clut_layout.cpp
    engine/gs/format/texa_expand.cpp
    engine/gs/format/pixel_convert.cpp
    engine/gs/format/block_decode.cpp
)
set(ps2r_cdvd_SOURCES
    engine/cdvd/iso_fs.cpp
    engine/services/read_timing.cpp
)
set(ps2r_vu_SOURCES
    engine/vu/vu_chop_round_guard.cpp
    engine/vu/vu_float.cpp
)
set(ps2r_ee_SOURCES
    engine/ee/cpu/guest_park.cpp
    engine/ee/recomp_abi/recomp_abi.cpp
    engine/ee/timing/tick_source.cpp
    engine/ee/timing/emu_domain.cpp
    engine/ee/timing/regime_detector.cpp
    engine/ee/timing/derived_clocks.cpp
    engine/ee/timing/tick_sync.cpp
    engine/ee/timing/boundary_protocol.cpp
    engine/ee/timing/field_pacer.cpp
    engine/ee/mem/main_memory.cpp
)
set(ps2r_sound_SOURCES
    engine/sound/vag_decoder.cpp
    engine/sound/spu2_dsp.cpp
    engine/sound/audio_mixer.cpp
)
set(ps2r_core_SOURCES
    engine/core/config/config.cpp
    engine/core/config/config_loader.cpp
    engine/core/config/settings_file.cpp
)
set(ps2r_host_SOURCES
    engine/platform/host/platform.cpp
    engine/platform/host/host_posix.cpp
)

foreach(lib IN ITEMS
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
        ps2r_host
    )
    add_library(${lib} STATIC ${${lib}_SOURCES})
    target_include_directories(${lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(${lib} PRIVATE -Wall -Wextra -Werror)
endforeach()

set_source_files_properties(
    PROPERTIES COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-fwrapv;-ffp-contract=off")
set_source_files_properties(
    games/slus_20909_crash_twinsanity/hle/math/culling_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/culling_planes.cpp
    games/slus_20909_crash_twinsanity/hle/math/clipping_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/joint_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/ray_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/particle_view_math.cpp
    games/slus_20909_crash_twinsanity/hle/math/decal_math.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/mesh_geometry.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/skin_geometry.cpp
    games/slus_20909_crash_twinsanity/hle/graphics/effects/shadow_geometry.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

foreach(lib IN ITEMS ps2r_vu ps2r_title_vu1_library)
    target_compile_options(${lib} PRIVATE -O2 -fno-strict-aliasing -fwrapv)
    set_target_properties(${lib} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endforeach()

foreach(lib IN ITEMS ps2r_ipu ps2r_render_deko)
    target_include_directories(${lib} SYSTEM PRIVATE "${PS2R_PORTLIBS}/include")
endforeach()
target_include_directories(ps2r_ps2e_draw SYSTEM PRIVATE "${PS2R_PORTLIBS}/include" "${PS2R_PORTLIBS}/include/SDL2")
target_include_directories(ps2r_platform_sdl SYSTEM PRIVATE "${PS2R_PORTLIBS}/include/SDL2" "${PS2R_PORTLIBS}/include")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/engine/platform/sdl/input/default_bindings.ini" _ps2r_input_ini)
file(WRITE "${PS2R_INPUT_GEN}/input_default_bindings.h"
"namespace ps2r::platform::sdl {
inline const char* const kDefaultBindingsIni = R\"PS2INI(${_ps2r_input_ini})PS2INI\";
}  // namespace ps2r::platform::sdl
")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/engine/platform/sdl/input/default_bindings.ini")
target_include_directories(ps2r_platform_sdl PRIVATE "${PS2R_INPUT_GEN}")

target_include_directories(ps2r_ipu PRIVATE "${PS2R_IPU_GEN}")

set(PS2R_DEKO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/render/backends/deko")
add_custom_command(
    OUTPUT "${PS2R_DEKO_GEN}/dk_shaders_gen.cpp" "${PS2R_DEKO_GEN}/dk_shaders_gen.h"
    COMMAND ${Python3_EXECUTABLE} "${PS2R_DEKO_DIR}/dk_shadergen.py"
            "${PS2R_DEKO_DIR}/dk_shader_sources.glsl" "${PS2R_UAM}" "${PS2R_DEKO_GEN}"
            "${PS2R_DEKO_DIR}/dk_ps2_variants.txt"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engine/render/backends/deko"
    DEPENDS "${PS2R_DEKO_DIR}/dk_shadergen.py"
            "${PS2R_DEKO_DIR}/dk_shader_sources.glsl" "${PS2R_DEKO_DIR}/dk_ps2_variants.txt"
    COMMENT "deko3d: shaders compiled by uam"
    VERBATIM)
target_include_directories(ps2r_render_deko PRIVATE "${PS2R_DEKO_GEN}")
