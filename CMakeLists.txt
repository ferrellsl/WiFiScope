cmake_minimum_required(VERSION 3.15)
project(WiFiScope CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # no runtime DLLs to ship

add_executable(WiFiScope WIN32 src/main.cpp src/Wlan.cpp src/Wlan.h src/Oui.cpp src/Oui.h res/WiFiScope.rc)
target_link_libraries(WiFiScope PRIVATE d2d1 dwrite dwmapi wlanapi shell32 ole32)
target_compile_definitions(WiFiScope PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(WiFiScope PRIVATE /W3 /utf-8)
# (the manifest is in the .rc: no second one from the linker)
target_link_options(WiFiScope PRIVATE /MANIFEST:NO)

# a console check of the beacon decoder (tests/decode_test.cpp)
add_executable(decode_test tests/decode_test.cpp src/Wlan.cpp)
target_link_libraries(decode_test PRIVATE wlanapi)
target_compile_definitions(decode_test PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(decode_test PRIVATE /W3 /utf-8)
