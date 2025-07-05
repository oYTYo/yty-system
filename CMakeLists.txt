# ===================================================================
# yty-system/CMakeLists.txt (最终修正版)
# ===================================================================

set(MODULE yty-system)

# 2. 列出模块需要编译的源文件 (.cc) 和头文件 (.h)
set(SOURCE_FILES
  helper/yty-camera-helper.cc
  helper/yty-server-helper.cc
  model/yty-camera.cc
  model/yty-server.cc
  model/yty-bitrate-sampler.cc # <-- 我之前漏掉了这一行！现在加回来。
)

set(HEADER_FILES
  helper/yty-camera-helper.h
  helper/yty-server-helper.h
  model/yty-camera.h
  model/yty-server.h
  model/yty-bitrate-sampler.h # <-- 对应的头文件也加上，保持规范。
)

# 3. 查找项目依赖的外部库
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)
find_package(Threads REQUIRED)

# 4. 使用 ns-3 的 "build_lib" 函数来定义模块库
build_lib(
  LIBNAME           ${MODULE}
  SOURCE_FILES      ${SOURCE_FILES}
  HEADER_FILES      ${HEADER_FILES}
  LIBRARIES_TO_LINK
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libapplications}
    Threads::Threads
    PkgConfig::ZMQ
    ${NLOHMANN_JSON_LIBRARIES}
)

# 5. 告诉编译器去哪里寻找头文件 (.h)
target_include_directories(${MODULE} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/model>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/helper>
    ${NLOHMANN_JSON_INCLUDE_DIRS}
)