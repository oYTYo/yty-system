# 定义一个变量 MODULE，其值为 yty-system。
set(MODULE yty-system)

# 把模块里所有 .cc 源文件列出来，赋值给 SOURCE_FILES 变量。
# 我已经为您添加了helper文件，您接下来会需要它们。
set(SOURCE_FILES
  helper/yty-camera-helper.cc
  helper/yty-server-helper.cc
  model/yty-camera.cc
  model/yty-server.cc
)

# 把所有 .h 头文件列出来，赋值给 HEADER_FILES 变量。
set(HEADER_FILES
  helper/yty-camera-helper.h
  helper/yty-server-helper.h
  model/yty-camera.h
  model/yty-server.h
)

# build_lib 是 ns-3 的自定义 CMake 宏，用于声明并构建一个模块静态/动态库。
build_lib(
  LIBNAME           ${MODULE} # 库名字
  SOURCE_FILES      ${SOURCE_FILES} # 源文件清单
  HEADER_FILES      ${HEADER_FILES} # 头文件清单
  LIBRARIES_TO_LINK # 依赖的其它 ns-3 库
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libapplications}
)

# 把model和helper也加入include的路径里，因为有些代码include可能找不到二级目录里的文件！
target_include_directories(${MODULE} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/model>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/helper>
)