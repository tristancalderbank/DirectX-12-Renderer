# CMakeList.txt : CMake project for DirectX12, include source and define
# project specific logic here.
#
cmake_minimum_required(VERSION 3.9.1)

project("DirectX12")

# Add source to this project's executable.
add_executable(DirectX12 WIN32
	"d3dx12.h" 
	"helpers.h"
	"main.cpp"
	  )

target_link_libraries(DirectX12
	d3d12.lib
	D3DCompiler.lib
	dxgi.lib
	dxguid.lib
)

set_property(TARGET DirectX12 PROPERTY CXX_STANDARD 20)
set_property(TARGET DirectX12 PROPERTY CXX_STANDARD_REQUIRED ON)

# copy shaders to .exe directory 
configure_file("${PROJECT_SOURCE_DIR}/shaders.hlsl" "${PROJECT_BINARY_DIR}/shaders.hlsl" COPYONLY)