cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your VitaSDK installation!")
  endif()
endif()

project(FateReVita LANGUAGES C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(VITA_APP_NAME "Fate/stay night REMASTERED")
set(VITA_TITLEID  "FSNRV0001")
set(VITA_VERSION  "01.00")

add_executable(${PROJECT_NAME}
  src/main.cpp
  src/audio.cpp
  src/renderer.cpp
  src/text.cpp
  src/script_vm.cpp
  src/title_screen.cpp
  src/extras.cpp
  src/video_player.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${VITASDK}/arm-vita-eabi/include
  ${VITASDK}/arm-vita-eabi/include/freetype2
)

target_compile_options(${PROJECT_NAME} PRIVATE
  -O2
  -Wall
  -Wextra
  -Wno-unused-parameter
  -fno-exceptions
)

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  SceShaccCgExt
  SceShaccCg_stub
  mathneon
  freetype
  bz2
  png
  z
  vorbisfile
  vorbis
  ogg
  m
  taihen_stub
  SceGxm_stub
  SceDisplay_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceVideodec_stub
  ScePower_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceKernelDmacMgr_stub
  SceIofilemgr_stub
  SceLibKernel_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

set(VPK_ARGS
  NAME "${VITA_APP_NAME}"
  FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png" "sce_sys/icon0.png"
  FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/pic0.png" "sce_sys/pic0.png"
  FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png" "sce_sys/livearea/contents/bg.png"
  FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png" "sce_sys/livearea/contents/startup.png"
  FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml" "sce_sys/livearea/contents/template.xml"
)

# Optionally bundle vita_data inside the VPK if -DBUNDLE_DATA=ON
option(BUNDLE_DATA "Bundle ../vita_data into app0:data inside the VPK" OFF)
if(BUNDLE_DATA AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vita_data")
  list(APPEND VPK_ARGS FILE "${CMAKE_CURRENT_SOURCE_DIR}/../vita_data" "data")
endif()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  ${VPK_ARGS}
)
