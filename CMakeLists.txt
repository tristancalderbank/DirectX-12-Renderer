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
	"raytracing.h" 
	"vertex.h" 
	"dxr/DXSampleHelper.h"
	"dxr/TopLevelASGenerator.cpp"
	"dxr/BottomLevelASGenerator.cpp"
	"dxr/RootSignatureGenerator.cpp"
	"dxr/RaytracingPipelineGenerator.cpp"
	"dxr/ShaderBindingTableGenerator.cpp"
)

target_link_libraries(DirectX12
	d3d12.lib
	D3DCompiler.lib
	dxgi.lib
	dxguid.lib
	dxcompiler.lib
)

set_property(TARGET DirectX12 PROPERTY CXX_STANDARD 20)
set_property(TARGET DirectX12 PROPERTY CXX_STANDARD_REQUIRED ON)

# dll's
configure_file("${PROJECT_SOURCE_DIR}/dxcompiler.dll" "${PROJECT_BINARY_DIR}/dxcompiler.dll" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/dxil.dll" "${PROJECT_BINARY_DIR}/dxil.dll" COPYONLY)

# copy shaders to .exe directory 
configure_file("${PROJECT_SOURCE_DIR}/shaders/Vertex.hlsl" "${PROJECT_BINARY_DIR}/shaders/Vertex.hlsl" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/shaders/Pixel.hlsl" "${PROJECT_BINARY_DIR}/shaders/Pixel.hlsl" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/shaders/Common.hlsl" "${PROJECT_BINARY_DIR}/shaders/Common.hlsl" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/shaders/RayGen.hlsl" "${PROJECT_BINARY_DIR}/shaders/RayGen.hlsl" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/shaders/Hit.hlsl" "${PROJECT_BINARY_DIR}/shaders/Hit.hlsl" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/shaders/Miss.hlsl" "${PROJECT_BINARY_DIR}/shaders/Miss.hlsl" COPYONLY)
