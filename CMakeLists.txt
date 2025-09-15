cmake_minimum_required(VERSION 3.10)
project(trunk-decoder)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set CMake policy to suppress Boost FindBoost module warning
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# Add include directories
include_directories(src)
include_directories(include)
include_directories(lib/op25)
include_directories(lib/op25/imbe_vocoder)

# Find required packages
find_package(Boost COMPONENTS program_options filesystem system)
find_package(nlohmann_json QUIET)

# If nlohmann_json not found, try to use system version or download
if(NOT nlohmann_json_FOUND)
    find_path(NLOHMANN_JSON_INCLUDE_DIR 
        NAMES nlohmann/json.hpp
        PATHS /usr/include /usr/local/include /opt/homebrew/include
    )
    
    if(NLOHMANN_JSON_INCLUDE_DIR)
        add_library(nlohmann_json INTERFACE)
        target_include_directories(nlohmann_json INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
        message(STATUS "Found nlohmann/json at: ${NLOHMANN_JSON_INCLUDE_DIR}")
    else()
        message(WARNING "nlohmann/json not found - plugins may not compile correctly")
    endif()
endif()

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

# Link libraries to main executable
if(Boost_FOUND)
    message(STATUS "Boost found - linking libraries")
    target_include_directories(trunk-decoder PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(trunk-decoder ${Boost_LIBRARIES})
    target_compile_definitions(trunk-decoder PRIVATE HAVE_BOOST=1)
    
    # Additional explicit linking for macOS
    target_link_libraries(trunk-decoder dl pthread)
    
    message(STATUS "Boost libraries: ${Boost_LIBRARIES}")
    message(STATUS "Boost include dirs: ${Boost_INCLUDE_DIRS}")
elseif(APPLE AND EXISTS "/opt/homebrew/lib/libboost_filesystem.dylib")
    message(STATUS "Using Homebrew Boost filesystem library")
    target_include_directories(trunk-decoder PRIVATE "/opt/homebrew/include")
    target_link_libraries(trunk-decoder "/opt/homebrew/lib/libboost_filesystem.dylib")
    target_compile_definitions(trunk-decoder PRIVATE HAVE_BOOST=1)
    target_link_libraries(trunk-decoder dl pthread)
else()
    message(WARNING "Boost not found - some features may not work")
    target_compile_definitions(trunk-decoder PRIVATE HAVE_BOOST=0)
    target_link_libraries(trunk-decoder dl pthread)
endif()

# Link nlohmann_json if available
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(trunk-decoder nlohmann_json::nlohmann_json)
endif()

# Plugin directory
set(PLUGIN_DIR ${CMAKE_BINARY_DIR}/plugins)
file(MAKE_DIRECTORY ${PLUGIN_DIR})

# Common plugin settings
function(create_plugin PLUGIN_NAME SOURCE_FILE)
    if(EXISTS ${CMAKE_SOURCE_DIR}/${SOURCE_FILE})
        add_library(${PLUGIN_NAME} SHARED ${SOURCE_FILE})
        target_include_directories(${PLUGIN_NAME} PRIVATE 
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
        
        # Link required libraries
        if(TARGET nlohmann_json::nlohmann_json)
            target_link_libraries(${PLUGIN_NAME} nlohmann_json::nlohmann_json)
        endif()
        
        # Link Boost for plugins if available
        if(Boost_FOUND)
            target_link_libraries(${PLUGIN_NAME} ${Boost_LIBRARIES})
        elseif(APPLE AND EXISTS "/opt/homebrew/lib/libboost_filesystem.dylib")
            # Only link filesystem - system is header-only in newer Boost versions
            target_link_libraries(${PLUGIN_NAME}
                "/opt/homebrew/lib/libboost_filesystem.dylib"
            )
        endif()
        
        # Set output properties
        set_target_properties(${PLUGIN_NAME} PROPERTIES 
            OUTPUT_NAME "lib${PLUGIN_NAME}"
            LIBRARY_OUTPUT_DIRECTORY ${PLUGIN_DIR}
            PREFIX ""
            CXX_VISIBILITY_PRESET default
            POSITION_INDEPENDENT_CODE ON
        )
        
        # Add standard system libraries
        target_link_libraries(${PLUGIN_NAME} pthread)
        
        # Create .so symlink for macOS .dylib files (plugins expect .so extension)
        if(APPLE)
            add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E create_symlink 
                    lib${PLUGIN_NAME}.dylib 
                    ${PLUGIN_DIR}/lib${PLUGIN_NAME}.so
                COMMENT "Creating .so symlink for ${PLUGIN_NAME}"
            )
        endif()
        
        message(STATUS "Configured plugin: ${PLUGIN_NAME}")
    else()
        message(STATUS "Plugin source not found: ${SOURCE_FILE} - creating stub")
        
        # Create a minimal stub plugin file if it doesn't exist
        file(WRITE ${CMAKE_SOURCE_DIR}/${SOURCE_FILE}
"#include \"plugin_api.h\"
#include <memory>

class ${PLUGIN_NAME}_impl : public Plugin_Api {
public:
    std::string get_plugin_name() override { return \"${PLUGIN_NAME}\"; }
    std::string get_plugin_version() override { return \"1.0.0\"; }
    
    int init(const nlohmann::json& config) override { 
        // TODO: Implement plugin initialization
        return 0; 
    }
    
    int start() override { 
        // TODO: Implement plugin start
        return 0; 
    }
    
    int stop() override { 
        // TODO: Implement plugin stop
        return 0; 
    }
    
    nlohmann::json get_stats() override {
        return nlohmann::json::object();
    }
};

extern \"C\" {
    std::shared_ptr<Plugin_Api> create_plugin() {
        return std::make_shared<${PLUGIN_NAME}_impl>();
    }
}
")
        
        # Now create the plugin with the stub file
        add_library(${PLUGIN_NAME} SHARED ${SOURCE_FILE})
        target_include_directories(${PLUGIN_NAME} PRIVATE 
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
        
        if(TARGET nlohmann_json::nlohmann_json)
            target_link_libraries(${PLUGIN_NAME} nlohmann_json::nlohmann_json)
        endif()
        
        if(Boost_FOUND)
            target_link_libraries(${PLUGIN_NAME} ${Boost_LIBRARIES})
        endif()
        
        set_target_properties(${PLUGIN_NAME} PROPERTIES 
            OUTPUT_NAME "lib${PLUGIN_NAME}"
            LIBRARY_OUTPUT_DIRECTORY ${PLUGIN_DIR}
            PREFIX ""
            CXX_VISIBILITY_PRESET default
            POSITION_INDEPENDENT_CODE ON
        )
        
        target_link_libraries(${PLUGIN_NAME} pthread)
        message(STATUS "Created stub plugin: ${PLUGIN_NAME}")
    endif()
endfunction()

# Input plugins
create_plugin(api_input "plugins/api_input.cc")
create_plugin(p25_tsbk_udp_input "plugins/p25_tsbk_udp_input.cc")

# Output plugins  
create_plugin(file_output "plugins/file_output.cc")

# Create a simple input plugin stub for P25 TSBK UDP if it doesn't exist
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/plugins/p25_tsbk_udp_input.cc)
    file(WRITE ${CMAKE_SOURCE_DIR}/plugins/p25_tsbk_udp_input.cc
"#include \"plugin_api.h\"
#include <memory>
#include <thread>
#include <atomic>

class P25TsbkUdpInput : public Input_Plugin_Api {
private:
    std::atomic<bool> running{false};
    std::thread worker_thread;
    
public:
    std::string get_plugin_name() override { return \"P25 TSBK UDP Input\"; }
    std::string get_plugin_version() override { return \"1.0.0\"; }
    
    int init(const nlohmann::json& config) override { 
        // TODO: Initialize UDP socket and configuration
        return 0; 
    }
    
    int start() override { 
        running = true;
        worker_thread = std::thread([this]() {
            while(running) {
                // TODO: Listen for UDP packets and process them
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        return 0; 
    }
    
    int stop() override { 
        running = false;
        if(worker_thread.joinable()) {
            worker_thread.join();
        }
        return 0; 
    }
    
    nlohmann::json get_stats() override {
        nlohmann::json stats;
        stats[\"packets_received\"] = 0;
        stats[\"running\"] = running.load();
        return stats;
    }
};

extern \"C\" {
    std::shared_ptr<Input_Plugin_Api> create_input_plugin() {
        return std::make_shared<P25TsbkUdpInput>();
    }
}
")
endif()

# Create plugins directory in source if it doesn't exist
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/plugins)

# Print build information
message(STATUS "Build configuration:")
message(STATUS "  Boost found: ${Boost_FOUND}")
message(STATUS "  nlohmann_json found: ${nlohmann_json_FOUND}")
message(STATUS "  Plugin directory: ${PLUGIN_DIR}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")

# Custom target to show plugin status
add_custom_target(show_plugins
    COMMAND echo "Built plugins:"
    COMMAND ls -la ${PLUGIN_DIR}/
    DEPENDS api_input p25_tsbk_udp_input file_output
    COMMENT "Showing built plugins"
)