cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(SA_ScreenStream)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -std=gnu99 -DTAIPOOL_AS_STDLIB")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-rtti -fno-exceptions")

include_directories(
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

if (NOT ${RELEASE})
  add_definitions(-DENABLE_LOGGING)
endif()

add_executable(SA_ScreenStream
  main.c
  renderer.c
  encoder.c
)

target_link_libraries(SA_ScreenStream
  taipool
  taihen_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmem_stub
  jpeg
  k
  gcc
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceKernelThreadMgr_stub
  SceSysmodule_stub
  ScePower_stub
  SceJpegEnc_stub
  SceAppMgr_stub
  SceCtrl_stub
)

set_target_properties(SA_ScreenStream
  PROPERTIES LINK_FLAGS "-nostdlib"
)

vita_create_self(SA_ScreenStream.suprx SA_ScreenStream
  CONFIG ${CMAKE_SOURCE_DIR}/SA_ScreenStream.yml
)
