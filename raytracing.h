#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <wrl.h>

#include <d3d12.h>

#include "dxr/DXRHelper.h"
#include "dxr/BottomLevelASGenerator.h"
#include "dxr/TopLevelASGenerator.h"
#include "dxr/RootSignatureGenerator.h"
#include "dxr/RaytracingPipelineGenerator.h"
#include "dxr/ShaderBindingTableGenerator.h"

#include <vector>

#include "vertex.h"

using Microsoft::WRL::ComPtr;

struct AccelerationStructureBuffers {
	ComPtr<ID3D12Resource> pScratch;
	ComPtr<ID3D12Resource> pResult;
	ComPtr<ID3D12Resource> pInstanceDesc;
};

AccelerationStructureBuffers
createBottomLevelAS(ComPtr<ID3D12Device5> &device, ComPtr<ID3D12GraphicsCommandList4> &commandList,
	std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> &vertexBuffers) {

	nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;

	// add all the vertex buffers
	for (const auto& buffer : vertexBuffers) {
		bottomLevelAS.AddVertexBuffer(buffer.first.Get(), 0, buffer.second, sizeof(Vertex), 0, 0);
	}

	// AS build requires some scratch temp memory for the build 
	UINT64 scratchSizeInBytes = 0;
	UINT64 resultSizeInBytes = 0;

	bottomLevelAS.ComputeASBufferSizes(device.Get(), false, &scratchSizeInBytes, &resultSizeInBytes);

	// Create buffers
	AccelerationStructureBuffers buffers;

	buffers.pScratch = nv_helpers_dx12::CreateBuffer(device.Get(), scratchSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, nv_helpers_dx12::kDefaultHeapProps);

	buffers.pResult = nv_helpers_dx12::CreateBuffer(device.Get(), resultSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		nv_helpers_dx12::kDefaultHeapProps);

	// Build the BLAS
	bottomLevelAS.Generate(commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(),
		false, nullptr);

	return buffers;
}

AccelerationStructureBuffers
createTopLevelAS(ComPtr<ID3D12Device5> &device, ComPtr<ID3D12GraphicsCommandList4> &commandList,
	nv_helpers_dx12::TopLevelASGenerator &topLevelASGenerator,
	const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>> &instances) {
	// Gather all the instances
	for (int i = 0; i < instances.size(); i++) {
		topLevelASGenerator.AddInstance(instances[i].first.Get(), instances[i].second, static_cast<UINT>(i), static_cast<UINT>(0));
	}

	UINT64 scratchSize, resultSize, instanceDescsSize;

	topLevelASGenerator.ComputeASBufferSizes(device.Get(), true, &scratchSize, &resultSize, &instanceDescsSize);

	// Create scratch and result buffers on GPU (default heap)
	AccelerationStructureBuffers buffers;

	buffers.pScratch = nv_helpers_dx12::CreateBuffer(device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nv_helpers_dx12::kDefaultHeapProps);

	buffers.pResult = nv_helpers_dx12::CreateBuffer(device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);

	// The buffer for instances will be copied into via mapping, so it has to be alocated on the upload heap 
	buffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(device.Get(), instanceDescsSize, D3D12_RESOURCE_FLAG_NONE, 
		D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	// Now build the TLAS 
	topLevelASGenerator.Generate(commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), buffers.pInstanceDesc.Get());

	return buffers;
}

// Create BLAS and TLAS
void
createAccelerationStructures(ComPtr<ID3D12Device5>& device, ComPtr<ID3D12GraphicsCommandList4>& commandList,
	ComPtr<ID3D12Resource>& vertexBuffer, nv_helpers_dx12::TopLevelASGenerator& topLevelASGenerator,
	AccelerationStructureBuffers& bottomLevelBuffers, AccelerationStructureBuffers& topLevelBuffers) {

	std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vertexBuffers = { {vertexBuffer.Get(), 3} };

	bottomLevelBuffers = createBottomLevelAS(device, commandList, vertexBuffers);

	std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>> instances = { { bottomLevelBuffers.pResult, DirectX::XMMatrixIdentity() } };

	topLevelBuffers = createTopLevelAS(device, commandList, topLevelASGenerator, instances);
}

ComPtr<ID3D12RootSignature> createRayGenSignature(ComPtr<ID3D12Device5>& device) {
	nv_helpers_dx12::RootSignatureGenerator rootSignatureGenerator;

	rootSignatureGenerator.AddHeapRangesParameter(
		{ {0 /*u0*/, 
		   1 /*1 descriptor */, 
		   0 /*use the implicit register space 0*/, 
		   D3D12_DESCRIPTOR_RANGE_TYPE_UAV /* UAV representing the output buffer*/, 
		   0 /*heap slot where the UAV is defined*/}, 
		  {0 /*t0*/, 
		   1, 
		   0, 
		   D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Top-level acceleration structure*/, 
		   1} }
	);

	return rootSignatureGenerator.Generate(device.Get(), true);
}

ComPtr<ID3D12RootSignature> createHitSignature(ComPtr<ID3D12Device5>& device) {
	nv_helpers_dx12::RootSignatureGenerator rootSignatureGenerator;

	rootSignatureGenerator.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV);

	return rootSignatureGenerator.Generate(device.Get(), true);
}

ComPtr<ID3D12RootSignature> createMissSignature(ComPtr<ID3D12Device5>& device) {
	nv_helpers_dx12::RootSignatureGenerator rootSignatureGenerator;

	return rootSignatureGenerator.Generate(device.Get(), true);
}

ComPtr<ID3D12StateObject>
createRaytracingPipelineState(ComPtr<ID3D12Device5>& device, 
	ComPtr<IDxcBlob> &rayGenLibrary,
	ComPtr<IDxcBlob> &hitLibrary,
	ComPtr<IDxcBlob> &missLibrary,
	ComPtr<ID3D12RootSignature> &rayGenSignature,
	ComPtr<ID3D12RootSignature> &hitSignature,
	ComPtr<ID3D12RootSignature> &missSignature,
	ComPtr<ID3D12StateObjectProperties> &raytracingStateObjectProperties
	) {
	nv_helpers_dx12::RayTracingPipelineGenerator pipeline(device.Get());

	// Compile shaders
	rayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders/RayGen.hlsl");
	hitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders/Hit.hlsl");
	missLibrary = nv_helpers_dx12::CompileShaderLibrary(L"shaders/Miss.hlsl");

	pipeline.AddLibrary(rayGenLibrary.Get(), { L"RayGen" });
	pipeline.AddLibrary(missLibrary.Get(), { L"Miss" });
	pipeline.AddLibrary(hitLibrary.Get(), { L"ClosestHit" });

	// Create root signatures 
	rayGenSignature = createRayGenSignature(device);
	hitSignature = createHitSignature(device);
	missSignature = createMissSignature(device);
	
	// Associate the shader code with the root signatures 
	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");

	pipeline.AddRootSignatureAssociation(rayGenSignature.Get(), { L"RayGen" });
	pipeline.AddRootSignatureAssociation(missSignature.Get(), { L"Miss" });
	pipeline.AddRootSignatureAssociation(hitSignature.Get(), { L"HitGroup" });

	pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance

	pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates

	pipeline.SetMaxRecursionDepth(1);

	ComPtr<ID3D12StateObject> raytracingPipelineState = pipeline.Generate();

	throwIfFailed(raytracingPipelineState->QueryInterface(IID_PPV_ARGS(&raytracingStateObjectProperties)));

	return raytracingPipelineState;
}

ComPtr<ID3D12Resource>
createRaytracingOutputBuffer(ComPtr<ID3D12Device5>& device, uint32_t width, uint32_t height) {
	D3D12_RESOURCE_DESC desc = {};

	desc.DepthOrArraySize = 1;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; // The backbuffer is actually DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, but sRGB // formats cannot be used with UAVs.
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	desc.Width = width;
	desc.Height = height;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;

	ComPtr<ID3D12Resource> outputBuffer;

	throwIfFailed(device->CreateCommittedResource(&nv_helpers_dx12::kDefaultHeapProps,
		D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, 
		nullptr, IID_PPV_ARGS(&outputBuffer)));

	return outputBuffer;
}

ComPtr<ID3D12DescriptorHeap>
createShaderResourceHeap(ComPtr<ID3D12Device5>& device, ComPtr<ID3D12Resource> outputBuffer, 
	AccelerationStructureBuffers &topLevelASBuffers) {
	// Create the SRV/UAV/CBV descriptor heap 
	// We need 2 entries, one for the output buffer UAV, one SRV for the TLAS

	ComPtr<ID3D12DescriptorHeap> descriptorHeap = nv_helpers_dx12::CreateDescriptorHeap(device.Get(),
		2, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

	// Get a handle to the heap memory on CPU side so we can write to it 
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();

	// Create the UAV. Based on root signature created earlier it is the first entry in the heap 
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

	device->CreateUnorderedAccessView(outputBuffer.Get(), nullptr, &uavDesc, srvHandle);

	// Now add the TLAS SRV after the output buffer next in the descriptor heap 
	srvHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = topLevelASBuffers.pResult->GetGPUVirtualAddress();

	device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

	return descriptorHeap;
}

ComPtr<ID3D12Resource>
createShaderBindingTable(ComPtr<ID3D12Device5>& device, 
	nv_helpers_dx12::ShaderBindingTableGenerator &sbtGenerator, ComPtr<ID3D12DescriptorHeap> &srvUavHeap,
	ComPtr<ID3D12Resource> &vertexBuffer,
	ComPtr<ID3D12StateObjectProperties> &raytracingStateObjectProperties) {
	
	sbtGenerator.Reset();

	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = srvUavHeap->GetGPUDescriptorHandleForHeapStart();
	UINT64* heapPointer = reinterpret_cast<UINT64*>(srvUavHeapHandle.ptr);

	sbtGenerator.AddRayGenerationProgram(L"RayGen", { heapPointer });
	sbtGenerator.AddMissProgram(L"Miss", {});
	sbtGenerator.AddMissProgram(L"Miss", {}); // hack because miss section size is only 32 but it needs to be padded to 64 
	sbtGenerator.AddHitGroup(L"HitGroup", { (void*) (vertexBuffer->GetGPUVirtualAddress())});

	// Create the SBT on the upload heap
	uint32_t sbtSize = sbtGenerator.ComputeSBTSize();

	ComPtr<ID3D12Resource> sbtStorage = nv_helpers_dx12::CreateBuffer(device.Get(), sbtSize,
		D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
		nv_helpers_dx12::kUploadHeapProps);

	sbtGenerator.Generate(sbtStorage.Get(), raytracingStateObjectProperties.Get());

	return sbtStorage;
}