cmake_minimum_required(VERSION 3.14)
project(amdc LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)

add_library(amdcf STATIC src/fast.cpp src/amdcf_c.cpp)                       # fast-decode codec: link this into your own program
target_include_directories(amdcf PUBLIC src)
target_link_libraries(amdcf PUBLIC Threads::Threads)

add_library(amdcs STATIC src/solid.cpp)                       # optional solid multi-file mode on top of amdcf
target_include_directories(amdcs PUBLIC src)
target_link_libraries(amdcs PUBLIC amdcf)

add_library(amdc1lib STATIC src/cm.cpp src/transforms.cpp src/container.cpp)   # max-ratio context-mixing codec
target_include_directories(amdc1lib PUBLIC src)
target_link_libraries(amdc1lib PUBLIC Threads::Threads)

add_executable(amdcf_cli src/amdcf_main.cpp)
target_link_libraries(amdcf_cli amdcf)
set_target_properties(amdcf_cli PROPERTIES OUTPUT_NAME amdcf)

add_executable(amdc1_cli src/main.cpp)
target_link_libraries(amdc1_cli amdc1lib)
set_target_properties(amdc1_cli PROPERTIES OUTPUT_NAME amdc1)

add_executable(amdcs_cli src/solid_main.cpp)
target_link_libraries(amdcs_cli amdcs)
set_target_properties(amdcs_cli PROPERTIES OUTPUT_NAME amdcs)

add_library(amdcb_lib STATIC src/bwt.cpp src/bwtcodec.cpp)     # BWT codec
target_include_directories(amdcb_lib PUBLIC src)
target_link_libraries(amdcb_lib PUBLIC Threads::Threads)

add_executable(amdcread src/amdcread.cpp)                      # reader for AMDC / AMDF / AMDB / AMDS files
target_link_libraries(amdcread amdcs amdc1lib amdcb_lib)

enable_testing()
add_executable(test_capi tests/test_capi.c)                    # the C header compiled as C
target_link_libraries(test_capi amdcf)
add_test(NAME capi COMMAND test_capi)
add_executable(test_solid tests/test_solid.cpp)
target_link_libraries(test_solid amdcs)
add_test(NAME solid COMMAND test_solid)
add_executable(test_fast tests/test_fast.cpp)
target_link_libraries(test_fast amdcf)
add_executable(test_amdc1 tests/test_amdc1.cpp)
target_link_libraries(test_amdc1 amdc1lib)
add_test(NAME fast COMMAND test_fast)
add_test(NAME cm COMMAND test_amdc1)
