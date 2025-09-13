cmake_minimum_required(VERSION 3.10)
project(trunk-decoder)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include directories
include_directories(src)
include_directories(include)
include_directories(lib/op25)
include_directories(lib/op25/imbe_vocoder)

# Add source files  
set(SOURCES
    src/main.cc
    src/p25_decoder.cc
    src/p25_frame_parser.cc
    src/p25_des_decrypt.cc
    src/p25_aes_decrypt.cc
    src/p25_adp_decrypt.cc
    src/http_service.cc
    src/api_service.cc
    src/job_manager.cc
    # OP25 IMBE vocoder files (working trunk-recorder implementation)
    lib/op25/imbe_vocoder/imbe_vocoder.cc
    lib/op25/imbe_vocoder/decode.cc
    lib/op25/imbe_vocoder/dsp_sub.cc
    lib/op25/imbe_vocoder/math_sub.cc
    lib/op25/imbe_vocoder/uv_synt.cc
    lib/op25/imbe_vocoder/v_synt.cc
    lib/op25/imbe_vocoder/v_uv_det.cc
    lib/op25/imbe_vocoder/pitch_est.cc
    lib/op25/imbe_vocoder/sa_decode.cc
    lib/op25/imbe_vocoder/sa_encode.cc
    lib/op25/imbe_vocoder/encode.cc
    lib/op25/imbe_vocoder/aux_sub.cc
    lib/op25/imbe_vocoder/basicop2.cc
    lib/op25/imbe_vocoder/ch_decode.cc
    lib/op25/imbe_vocoder/ch_encode.cc
    lib/op25/imbe_vocoder/dc_rmv.cc
    lib/op25/imbe_vocoder/pe_lpf.cc
    lib/op25/imbe_vocoder/pitch_ref.cc
    lib/op25/imbe_vocoder/qnt_sub.cc
    lib/op25/imbe_vocoder/rand_gen.cc
    lib/op25/imbe_vocoder/sa_enh.cc
    lib/op25/imbe_vocoder/tbls.cc
)

# Create executable
add_executable(trunk-decoder ${SOURCES})

# Try to find and link boost if available
find_package(Boost COMPONENTS program_options)
if(Boost_FOUND)
    target_include_directories(trunk-decoder PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(trunk-decoder ${Boost_LIBRARIES})
    target_compile_definitions(trunk-decoder PRIVATE HAVE_BOOST=1)
else()
    message(STATUS "Building without Boost - using simple argument parsing")
    target_compile_definitions(trunk-decoder PRIVATE HAVE_BOOST=0)
endif()