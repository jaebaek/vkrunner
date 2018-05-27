cmake_minimum_required(VERSION 3.0)

project(vkrunner)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "bin")

if (NOT VULKAN_DIR)
  set(VULKAN_DIR $ENV{VULKAN_SDK})
endif()

message("Using Vulkan found at ${VULKAN_DIR}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} \
                   -Wall -Wuninitialized -Wempty-body -Wformat \
                   -Wformat-security -Winit-self -Wundef \
                   -Wvla -Wpointer-arith -Wmissing-declarations")

add_subdirectory(src)
