# ---------------------------------------------------------------------------
# Service interfaces
#
# Each service keeps its own .aidl next to its implementation
# (http/aidl/, download/aidl/); the AIDL compiler requires the file path below
# those roots to mirror the package, hence the com/service/<name>/ nesting.
#
# One library comes out of them, because download imports the http interfaces
# and the client needs the download ones -- splitting it would only add
# ordering to get wrong. The client links this same target, so there is one
# definition of each interface and the two sides cannot disagree about it.
# ---------------------------------------------------------------------------
set(AIDL_BIN  ${CMAKE_SOURCE_DIR}/tools/bin/aidl)
set(HTTP_AIDL ${CMAKE_CURRENT_SOURCE_DIR}/http/aidl)
set(DL_AIDL   ${CMAKE_CURRENT_SOURCE_DIR}/download/aidl)
set(AIDL_OUT  ${CMAKE_CURRENT_BINARY_DIR}/aidl-gen)

set(AIDL_FILES
  ${HTTP_AIDL}/com/service/http/IHttpService.aidl
  ${HTTP_AIDL}/com/service/http/IHttpProgressCallback.aidl
  ${DL_AIDL}/com/service/download/IDownloadService.aidl
  ${DL_AIDL}/com/service/download/IDownloadCallback.aidl
)

set(AIDL_GENERATED
  ${AIDL_OUT}/com/service/http/IHttpService.cpp
  ${AIDL_OUT}/com/service/http/IHttpProgressCallback.cpp
  ${AIDL_OUT}/com/service/download/IDownloadService.cpp
  ${AIDL_OUT}/com/service/download/IDownloadCallback.cpp
)

add_custom_command(
  OUTPUT ${AIDL_GENERATED}
         ${AIDL_OUT}/com/service/http/BnHttpService.h
         ${AIDL_OUT}/com/service/http/BnHttpProgressCallback.h
         ${AIDL_OUT}/com/service/download/BnDownloadService.h
         ${AIDL_OUT}/com/service/download/BnDownloadCallback.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AIDL_OUT}
  COMMAND ${AIDL_BIN} --lang=cpp -I ${HTTP_AIDL} -I ${DL_AIDL}
          --header_out=${AIDL_OUT} --out=${AIDL_OUT}
          ${AIDL_FILES}
  DEPENDS ${AIDL_FILES} ${AIDL_BIN}
  COMMENT "aidl --lang=cpp: com.service.http + com.service.download"
  VERBATIM
)

add_library(service_aidl STATIC ${AIDL_GENERATED})
target_include_directories(service_aidl PUBLIC ${AIDL_OUT})
target_link_libraries(service_aidl PUBLIC binder utils base)

# ---------------------------------------------------------------------------
# service_manager -- one binary, three roles: launcher, http service, download
# service.
#
# Keeping them in one target is deliberate: the launcher starts a role by
# exec'ing /proc/self/exe with an argument, so it never has to locate a
# companion binary (servicemanager aside).
# ---------------------------------------------------------------------------
add_executable(service_manager
  main.cpp
  http/HttpClient.cpp
  http/HttpService.cpp
  download/DownloadService.cpp
)

target_include_directories(service_manager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(service_manager PRIVATE service_aidl binder utils cutils log base)

# HTTPS is optional: build it in when OpenSSL is around, otherwise the service
# handles http:// only and says so clearly on an https:// request (see
# tlsSupported() in HttpClient.cpp).
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  message(STATUS "http service: HTTPS enabled (OpenSSL ${OPENSSL_VERSION})")
  target_compile_definitions(service_manager PRIVATE HTTP_SERVICE_HAVE_TLS)
  target_link_libraries(service_manager PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else()
  message(STATUS "http service: no OpenSSL found, http:// only")
endif()

# The launcher fork/execs servicemanager, so it has to exist by then.
add_dependencies(service_manager servicemanager)
