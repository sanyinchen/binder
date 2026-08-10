# download_client -- downloads a file through download.service.
#
# It links the generated interfaces and nothing from the service side: there is
# no HTTP code in the client process, and no knowledge that http.service exists.

add_executable(download_client download.cpp)
target_link_libraries(download_client PRIVATE service_aidl binder utils cutils log base)
