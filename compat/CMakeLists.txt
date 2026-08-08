# Host compatibility shims replacing libselinux and libvintf.
# See the headers under include/ for what each one does and why.

add_library(selinux_stub STATIC selinux_stub.cpp)
target_include_directories(selinux_stub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(vintf_stub STATIC vintf_stub.cpp)
target_include_directories(vintf_stub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
