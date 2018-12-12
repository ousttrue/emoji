cmake_minimum_required(VERSION 3.0)
project(clfontpng)

LINK_DIRECTORIES(C:/vcpkg/installed/x64-windows/lib)
file(GLOB SRC clfontpng.cc)
add_executable(${PROJECT_NAME} ${SRC})
TARGET_INCLUDE_DIRECTORIES(${PROJECT_NAME} PUBLIC
    C:/vcpkg/installed/x64-windows/include
)
TARGET_COMPILE_DEFINITIONS(${PROJECT_NAME} PUBLIC
    NOMINMAX
)
TARGET_LINK_LIBRARIES(${PROJECT_NAME}
    freetype
)
