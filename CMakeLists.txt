cmake_minimum_required(VERSION 3.16)
project(nailong-pet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# If SFML is not found automatically, point to your installation, e.g.:
#   cmake -DSFML_DIR="C:/SFML-2.6.2/lib/cmake/SFML" ..
#   cmake -DSFML_DIR="C:/vcpkg/installed/x64-windows/share/sfml" ..
find_package(SFML 3 COMPONENTS Graphics REQUIRED)

add_executable(nailong-pet src/main.cpp)

target_link_libraries(nailong-pet PRIVATE
    SFML::Graphics
)

# Copy SFML DLLs next to the executable (requires CMake >= 3.21)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET nailong-pet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:nailong-pet>
            $<TARGET_FILE_DIR:nailong-pet>
        COMMAND_EXPAND_LISTS
    )
endif()
