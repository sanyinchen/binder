# Demo service + client, built from AIDL generated at build time by the
# project-local aidl compiler (tools/bin/aidl).

set(AIDL_BIN   ${CMAKE_SOURCE_DIR}/tools/bin/aidl)
set(AIDL_SRC   ${CMAKE_CURRENT_SOURCE_DIR}/aidl)
set(AIDL_OUT   ${CMAKE_CURRENT_BINARY_DIR}/aidl-gen)

set(DEMO_AIDL_FILES
  ${AIDL_SRC}/com/example/demo/IDemoService.aidl
  ${AIDL_SRC}/com/example/demo/IDemoCallback.aidl
)

set(DEMO_AIDL_GENERATED
  ${AIDL_OUT}/com/example/demo/IDemoService.cpp
  ${AIDL_OUT}/com/example/demo/IDemoCallback.cpp
)

add_custom_command(
  OUTPUT ${DEMO_AIDL_GENERATED}
         ${AIDL_OUT}/com/example/demo/BnDemoService.h
         ${AIDL_OUT}/com/example/demo/BnDemoCallback.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${AIDL_OUT}
  COMMAND ${AIDL_BIN} --lang=cpp -I ${AIDL_SRC}
          --header_out=${AIDL_OUT} --out=${AIDL_OUT}
          ${DEMO_AIDL_FILES}
  DEPENDS ${DEMO_AIDL_FILES} ${AIDL_BIN}
  COMMENT "aidl --lang=cpp: com.example.demo"
  VERBATIM
)

add_library(demo_aidl STATIC ${DEMO_AIDL_GENERATED})
target_include_directories(demo_aidl PUBLIC ${AIDL_OUT})
target_link_libraries(demo_aidl PUBLIC binder utils base)

# demo_support: demo_service starts servicemanager itself if none is running.
add_executable(demo_service demo_service.cpp)
target_link_libraries(demo_service PRIVATE demo_aidl binder utils cutils log base demo_support)
add_dependencies(demo_service servicemanager)

add_executable(demo_client demo_client.cpp)
target_link_libraries(demo_client PRIVATE demo_aidl binder utils cutils log base)
