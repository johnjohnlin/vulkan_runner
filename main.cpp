/*
* Vulkan Example - Minimal headless rendering example
*
* Copyright
*   (C) 2017-2022 by Sascha Willems - www.saschawillems.de
*   (C) 2023 Yu-Sheng Lin
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/
#include "VulkanTools.h"
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
using namespace std;

#define NDEBUG 1
#define DEBUG (!NDEBUG)
#define BUFFER_ELEMENTS 32
#define LOG(...) printf(__VA_ARGS__)

#if DEBUG
static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugMessageCallback(
	vk::DebugReportFlagsEXT flags,
	vk::DebugReportObjectTypeEXT objectType,
	uint64_t object,
	size_t location,
	int32_t messageCode,
	const char* pLayerPrefix,
	const char* pMessage,
	void* pUserData)
{
	LOG("[VALIDATION]: %s - %s\n", pLayerPrefix, pMessage);
	return VK_FALSE;
}
#endif

// helper class since they are usually used together
struct BufferMemory {
	vk::DeviceMemory memory;
	vk::Buffer buffer;

	static uint32_t GetMemoryTypeIndex(
		vk::PhysicalDevice phy_dev,
		uint32_t type_bits,
		vk::MemoryPropertyFlags properties
	) {
		vk::PhysicalDeviceMemoryProperties device_memory_properties = phy_dev.getMemoryProperties();
		for (uint32_t i = 0; i < device_memory_properties.memoryTypeCount; i++) {
			if ((type_bits & 1) == 1) {
				if ((device_memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
					return i;
				}
			}
			type_bits >>= 1;
		}
		return 0;
	}

	static BufferMemory CreateOnDevice(
		vk::PhysicalDevice phy_dev,
		vk::Device dev,
		vk::BufferUsageFlags usage_flags,
		vk::MemoryPropertyFlags memory_property_flags,
		vk::DeviceSize size,
		void *data = nullptr
	) {
		BufferMemory ret;

		// Create the buffer handle
		vk::BufferCreateInfo buffer_create_info_{
			{}, size, usage_flags, vk::SharingMode::eExclusive
		};
		ret.buffer = dev.createBuffer(buffer_create_info_);

		// Create the memory backing up the buffer handle
		vk::MemoryRequirements mem_reqs = dev.getBufferMemoryRequirements(ret.buffer);
		vk::MemoryAllocateInfo mem_alloc{
			mem_reqs.size,
			GetMemoryTypeIndex(phy_dev, mem_reqs.memoryTypeBits, memory_property_flags)
		};
		ret.memory = dev.allocateMemory(mem_alloc);

		if (data != nullptr) {
			void *mapped;
			dev.mapMemory(ret.memory, 0, size, {}, &mapped);
			dev.unmapMemory(ret.memory);
		}

		dev.bindBufferMemory(ret.buffer, ret.memory, 0);
		return ret;
	}

	void DestroyByDevice(vk::Device dev) {
		dev.destroy(buffer);
		dev.freeMemory(memory);
	}

	void CopyTo(BufferMemory& dst, vk::DeviceSize size) {
		vk::CommandBufferAllocateInfo cmdbuf_allocate_info{command_pool_, vk::CommandBufferLevel::ePrimary, 1};
		vk::CommandBuffer copy_cmd = device_.allocateCommandBuffers(cmdbuf_allocate_info).at(0);
		vk::CommandBufferBeginInfo cmdbuf_info;
		VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));
		copyRegion.size = indexBufferSize;
		vkCmdCopyBuffer(copyCmd, stagingBuffer, index_buffer_, 1, &copyRegion);
		VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));
		submitWork(copyCmd, queue_);
	}
};

// helper class since they are usually used together
struct FrameBufferAttachment {
	vk::DeviceMemory memory;
	vk::Image image;
	vk::ImageView view;
	void DestroyByDevice(vk::Device dev) {
		dev.destroy(view);
		dev.destroy(image);
		dev.freeMemory(memory);
	}
};

class VulkanExample {
	vk::Instance instance_;
	vk::PhysicalDevice physical_device_;
	vk::Device device_;
	uint32_t queue_family_index_;
	vk::PipelineCache pipeline_cache_;
	vk::Queue queue_;
	vk::CommandPool command_pool_;
	vk::CommandBuffer command_buffer_;
	vk::DescriptorSetLayout descriptor_set_layout_;
	vk::PipelineLayout pipeline_layout_;
	vk::Pipeline pipeline_;
	vector<vk::ShaderModule> shader_modules;
	BufferMemory vertex_bufmem_, index_bufmem_;
	int32_t width_, height_;
	vk::Framebuffer frame_buffer_;
	FrameBufferAttachment color_attachment_, depth_attachment_;
	vk::RenderPass render_pass_;
#if DEBUG
	vk::DebugReportCallbackEXT debugReportCallback{};
#endif
public:

#if 0
	/*
		Submit command buffer to a queue_ and wait for fence until queue_ operations have been finished
	*/
	void submitWork(vk::CommandBuffer cmdBuffer, vk::Queue queue_) {
		vk::SubmitInfo submitInfo = vks::initializers::submitInfo();
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmdBuffer;
		vk::FenceCreateInfo fenceInfo = vks::initializers::fenceCreateInfo();
		vk::Fence fence;
		VK_CHECK_RESULT(vkCreateFence(device_, &fenceInfo, nullptr, &fence));
		VK_CHECK_RESULT(vkQueueSubmit(queue_, 1, &submitInfo, fence));
		VK_CHECK_RESULT(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX));
		vkDestroyFence(device_, fence, nullptr);
	}
#endif

	VulkanExample() {
		LOG("Running headless rendering example\n");

		vk::ApplicationInfo app_info{
			"Vulkan headless example",
			VK_MAKE_VERSION(1,0,0),
			"VulkanExample",
			VK_MAKE_VERSION(1,0,0),
			VK_API_VERSION_1_0,
		};
		vk::InstanceCreateInfo instance_create_info{
			{},
			&app_info
		};

		uint32_t layerCount = 1;
		const char* validationLayers[] = { "VK_LAYER_KHRONOS_validation" };

		std::vector<const char*> instance_extensions = {};
#if DEBUG
		// Check if layers are available
		uint32_t instanceLayerCount;
		vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr);
		std::vector<vk::LayerProperties> instanceLayers(instanceLayerCount);
		vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.data());

		bool layersAvailable = true;
		for (auto layerName : validationLayers) {
			bool layerAvailable = false;
			for (auto instanceLayer : instanceLayers) {
				if (strcmp(instanceLayer.layerName, layerName) == 0) {
					layerAvailable = true;
					break;
				}
			}
			if (!layerAvailable) {
				layersAvailable = false;
				break;
			}
		}

		if (layersAvailable) {
			instanceExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
			instanceCreateInfo.ppEnabledLayerNames = validationLayers;
			instanceCreateInfo.enabledLayerCount = layerCount;
		}
#endif
		instance_create_info.enabledExtensionCount = instance_extensions.size();
		instance_create_info.ppEnabledExtensionNames = instance_extensions.data();
		instance_ = vk::createInstance(instance_create_info);

#if DEBUG
		if (layersAvailable) {
			vk::DebugReportCallbackCreateInfoEXT debugReportCreateInfo = {};
			debugReportCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
			debugReportCreateInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
			debugReportCreateInfo.pfnCallback = (PFN_vkDebugReportCallbackEXT)debugMessageCallback;

			// We have to explicitly load this function.
			PFN_vkCreateDebugReportCallbackEXT vkCreateDebugReportCallbackEXT = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));
			assert(vkCreateDebugReportCallbackEXT);
			VK_CHECK_RESULT(vkCreateDebugReportCallbackEXT(instance_, &debugReportCreateInfo, nullptr, &debugReportCallback));
		}
#endif

		// Vulkan device creation (only select the first one)
		physical_device_ = instance_.enumeratePhysicalDevices().at(0);
		LOG("GPU: %s\n", physical_device_.getProperties().deviceName.begin());

		// Request a graphics queue idx and get logical device, queue, and command pool
		std::vector<vk::QueueFamilyProperties> queue_family_properties = physical_device_.getQueueFamilyProperties();
		for (uint32_t i = 0; i < queue_family_properties.size(); i++) {
			if (queue_family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
				queue_family_index_ = i;
				const float queue_priority = 0.0f;
				vk::DeviceQueueCreateInfo queue_create_info{
					{}, i, 1, &queue_priority
				};
				vk::DeviceCreateInfo device_create_info{
					{}, 1, &queue_create_info,
					0, nullptr, 0, nullptr
				};
				device_ = physical_device_.createDevice(device_create_info);
				queue_ = device_.getQueue(queue_family_index_, 0);
				vk::CommandPoolCreateInfo cmd_pool_info{
					{}, queue_family_index_
				};
				command_pool_ = device_.createCommandPool(cmd_pool_info);
				break;
			}
		}

		/*
			Prepare vertex and index buffers
		*/
		struct Vertex {
			float position[3];
			float color[3];
		};
		{
			std::vector<Vertex> vertices = {
				{ {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
				{ { -1.0f,  1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
				{ {  0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } }
			};
			std::vector<uint32_t> indices = { 0, 1, 2 };
			const vk::DeviceSize vertex_buf_size = vertices.size() * sizeof(Vertex);
			const vk::DeviceSize index_buf_size = indices.size() * sizeof(uint32_t);

			// Copy input data to VRAM using a staging buffer
			{
				BufferMemory staging_bufmem;
				staging_bufmem = BufferMemory::CreateOnDevice(
					physical_device_,
					device_,
					vk::BufferUsageFlagBits::eTransferSrc,
					vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
					vertex_buf_size, vertices.data()
				);
				vertex_bufmem_ = BufferMemory::CreateOnDevice(
					physical_device_,
					device_,
					vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
					vk::MemoryPropertyFlagBits::eDeviceLocal,
					vertex_buf_size
				);
				staging_bufmem.CopyTo(vertex_bufmem_, vertex_buf_size);
				staging_bufmem.DestroyByDevice(device_);

				// Indices
				staging_bufmem = BufferMemory::CreateOnDevice(
					physical_device_,
					device_,
					vk::BufferUsageFlagBits::eTransferSrc,
					vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
					index_buf_size, indices.data()
				);
				index_bufmem_ = BufferMemory::CreateOnDevice(
					physical_device_,
					device_,
					vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer,
					vk::MemoryPropertyFlagBits::eDeviceLocal,
					index_buf_size
				);
				staging_bufmem.CopyTo(index_bufmem_, index_buf_size);
				staging_bufmem.DestroyByDevice(device_);
			}
		}

#if 0
		/*
			Create frame_buffer attachments
		*/
		width_ = 1024;
		height_ = 1024;
		vk::Format colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
		vk::Format depthFormat;
		vks::tools::getSupportedDepthFormat(physical_device_, &depthFormat);
		{
			// Color attachment
			vk::ImageCreateInfo image = vks::initializers::imageCreateInfo();
			image.imageType = VK_IMAGE_TYPE_2D;
			image.format = colorFormat;
			image.extent.width_ = width_;
			image.extent.height_ = height_;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

			vk::MemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
			vk::MemoryRequirements memReqs;

			VK_CHECK_RESULT(vkCreateImage(device_, &image, nullptr, &color_attachment_.image));
			vkGetImageMemoryRequirements(device_, color_attachment_.image, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device_, &memAlloc, nullptr, &color_attachment_.memory));
			VK_CHECK_RESULT(vkBindImageMemory(device_, color_attachment_.image, color_attachment_.memory, 0));

			vk::ImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
			colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorImageView.format = colorFormat;
			colorImageView.subresourceRange = {};
			colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorImageView.subresourceRange.baseMipLevel = 0;
			colorImageView.subresourceRange.levelCount = 1;
			colorImageView.subresourceRange.baseArrayLayer = 0;
			colorImageView.subresourceRange.layerCount = 1;
			colorImageView.image = color_attachment_.image;
			VK_CHECK_RESULT(vkCreateImageView(device_, &colorImageView, nullptr, &color_attachment_.view));

			// Depth stencil attachment
			image.format = depthFormat;
			image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

			VK_CHECK_RESULT(vkCreateImage(device_, &image, nullptr, &depth_attachment_.image));
			vkGetImageMemoryRequirements(device_, depth_attachment_.image, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device_, &memAlloc, nullptr, &depth_attachment_.memory));
			VK_CHECK_RESULT(vkBindImageMemory(device_, depth_attachment_.image, depth_attachment_.memory, 0));

			vk::ImageViewCreateInfo depthStencilView = vks::initializers::imageViewCreateInfo();
			depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			depthStencilView.format = depthFormat;
			depthStencilView.flags = 0;
			depthStencilView.subresourceRange = {};
			depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			if (depthFormat >= VK_FORMAT_D16_UNORM_S8_UINT)
				depthStencilView.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			depthStencilView.subresourceRange.baseMipLevel = 0;
			depthStencilView.subresourceRange.levelCount = 1;
			depthStencilView.subresourceRange.baseArrayLayer = 0;
			depthStencilView.subresourceRange.layerCount = 1;
			depthStencilView.image = depth_attachment_.image;
			VK_CHECK_RESULT(vkCreateImageView(device_, &depthStencilView, nullptr, &depth_attachment_.view));
		}

		/*
			Create renderpass
		*/
		{
			std::array<vk::AttachmentDescription, 2> attchmentDescriptions = {};
			// Color attachment
			attchmentDescriptions[0].format = colorFormat;
			attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
			attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			// Depth attachment
			attchmentDescriptions[1].format = depthFormat;
			attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
			attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			vk::AttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
			vk::AttachmentReference depthReference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

			vk::SubpassDescription subpassDescription = {};
			subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpassDescription.colorAttachmentCount = 1;
			subpassDescription.pColorAttachments = &colorReference;
			subpassDescription.pDepthStencilAttachment = &depthReference;

			// Use subpass dependencies for layout transitions
			std::array<vk::SubpassDependency, 2> dependencies;

			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			// Create the actual renderpass
			vk::RenderPassCreateInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attchmentDescriptions.size());
			renderPassInfo.pAttachments = attchmentDescriptions.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpassDescription;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();
			VK_CHECK_RESULT(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &render_pass_));

			vk::ImageView attachments[2];
			attachments[0] = color_attachment_.view;
			attachments[1] = depth_attachment_.view;

			vk::FramebufferCreateInfo framebufferCreateInfo = vks::initializers::framebufferCreateInfo();
			framebufferCreateInfo.render_pass_ = render_pass_;
			framebufferCreateInfo.attachmentCount = 2;
			framebufferCreateInfo.pAttachments = attachments;
			framebufferCreateInfo.width_ = width_;
			framebufferCreateInfo.height_ = height_;
			framebufferCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device_, &framebufferCreateInfo, nullptr, &frame_buffer_));
		}

		/*
			Prepare graphics pipeline_
		*/
		{
			std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings = {};
			vk::DescriptorSetLayoutCreateInfo descriptorLayout =
				vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
			VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device_, &descriptorLayout, nullptr, &descriptor_set_layout_));

			vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo =
				vks::initializers::pipelineLayoutCreateInfo(nullptr, 0);

			// MVP via push constant block
			vk::PushConstantRange pushConstantRange = vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), 0);
			pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
			pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

			VK_CHECK_RESULT(vkCreatePipelineLayout(device_, &pipelineLayoutCreateInfo, nullptr, &pipeline_layout_));

			vk::PipelineCacheCreateInfo pipelineCacheCreateInfo = {};
			pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			VK_CHECK_RESULT(vkCreatePipelineCache(device_, &pipelineCacheCreateInfo, nullptr, &pipeline_cache_));

			// Create pipeline_
			vk::PipelineInputAssemblyStateCreateInfo inputAssemblyState =
				vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);

			vk::PipelineRasterizationStateCreateInfo rasterizationState =
				vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);

			vk::PipelineColorBlendAttachmentState blendAttachmentState =
				vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);

			vk::PipelineColorBlendStateCreateInfo colorBlendState =
				vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);

			vk::PipelineDepthStencilStateCreateInfo depthStencilState =
				vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);

			vk::PipelineViewportStateCreateInfo viewportState =
				vks::initializers::pipelineViewportStateCreateInfo(1, 1);

			vk::PipelineMultisampleStateCreateInfo multisampleState =
				vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT);

			std::vector<vk::DynamicState> dynamicStateEnables = {
				VK_DYNAMIC_STATE_VIEWPORT,
				VK_DYNAMIC_STATE_SCISSOR
			};
			vk::PipelineDynamicStateCreateInfo dynamicState =
				vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);

			vk::GraphicsPipelineCreateInfo pipelineCreateInfo =
				vks::initializers::pipelineCreateInfo(pipeline_layout_, render_pass_);

			std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages{};

			pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
			pipelineCreateInfo.pRasterizationState = &rasterizationState;
			pipelineCreateInfo.pColorBlendState = &colorBlendState;
			pipelineCreateInfo.pMultisampleState = &multisampleState;
			pipelineCreateInfo.pViewportState = &viewportState;
			pipelineCreateInfo.pDepthStencilState = &depthStencilState;
			pipelineCreateInfo.pDynamicState = &dynamicState;
			pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
			pipelineCreateInfo.pStages = shaderStages.data();

			// Vertex bindings an attributes
			// Binding description
			std::vector<vk::VertexInputBindingDescription> vertexInputBindings = {
				vks::initializers::vertexInputBindingDescription(0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
			};

			// Attribute descriptions
			std::vector<vk::VertexInputAttributeDescription> vertexInputAttributes = {
				vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0),					// Position
				vks::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3),	// Color
			};

			vk::PipelineVertexInputStateCreateInfo vertexInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
			vertexInputState.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
			vertexInputState.pVertexBindingDescriptions = vertexInputBindings.data();
			vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
			vertexInputState.pVertexAttributeDescriptions = vertexInputAttributes.data();

			pipelineCreateInfo.pVertexInputState = &vertexInputState;

			shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			shaderStages[0].pName = "main";
			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].pName = "main";
			shaderStages[0].module = vks::tools::loadShader("triangle.vert.spv", device_);
			shaderStages[1].module = vks::tools::loadShader("triangle.frag.spv", device_);
			shader_modules = { shaderStages[0].module, shaderStages[1].module };
			VK_CHECK_RESULT(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pipelineCreateInfo, nullptr, &pipeline_));
		}

		/*
			Command buffer creation
		*/
		{
			vk::CommandBuffer command_buffer_;
			vk::CommandBufferAllocateInfo cmdBufAllocateInfo =
				vks::initializers::commandBufferAllocateInfo(command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device_, &cmdBufAllocateInfo, &command_buffer_));

			vk::CommandBufferBeginInfo cmdBufInfo =
				vks::initializers::commandBufferBeginInfo();

			VK_CHECK_RESULT(vkBeginCommandBuffer(command_buffer_, &cmdBufInfo));

			vk::ClearValue clearValues[2];
			clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 1.0f } };
			clearValues[1].depthStencil = { 1.0f, 0 };

			vk::RenderPassBeginInfo renderPassBeginInfo = {};
			renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassBeginInfo.renderArea.extent.width_ = width_;
			renderPassBeginInfo.renderArea.extent.height_ = height_;
			renderPassBeginInfo.clearValueCount = 2;
			renderPassBeginInfo.pClearValues = clearValues;
			renderPassBeginInfo.render_pass_ = render_pass_;
			renderPassBeginInfo.frame_buffer_ = frame_buffer_;

			vkCmdBeginRenderPass(command_buffer_, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			vk::Viewport viewport = {};
			viewport.height_ = (float)height_;
			viewport.width_ = (float)width_;
			viewport.minDepth = (float)0.0f;
			viewport.maxDepth = (float)1.0f;
			vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

			// Update dynamic scissor state
			vk::Rect2D scissor = {};
			scissor.extent.width_ = width_;
			scissor.extent.height_ = height_;
			vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

			vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

			// Render scene
			vk::DeviceSize offsets[1] = { 0 };
			vkCmdBindVertexBuffers(command_buffer_, 0, 1, &vertex_buffer_, offsets);
			vkCmdBindIndexBuffer(command_buffer_, index_buffer_, 0, VK_INDEX_TYPE_UINT32);

			std::vector<glm::vec3> pos = {
				glm::vec3(-1.5f, 0.0f, -4.0f),
				glm::vec3( 0.0f, 0.0f, -2.5f),
				glm::vec3( 1.5f, 0.0f, -4.0f),
			};

			for (auto v : pos) {
				glm::mat4 mvpMatrix = glm::perspective(glm::radians(60.0f), (float)width_ / (float)height_, 0.1f, 256.0f) * glm::translate(glm::mat4(1.0f), v);
				vkCmdPushConstants(command_buffer_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvpMatrix), &mvpMatrix);
				vkCmdDrawIndexed(command_buffer_, 3, 1, 0, 0, 0);
			}

			vkCmdEndRenderPass(command_buffer_);

			VK_CHECK_RESULT(vkEndCommandBuffer(command_buffer_));

			submitWork(command_buffer_, queue_);
			device_.waitIdle();
		}

		/*
			Copy frame_buffer_ image to host visible image
		*/
		const char* imagedata;
		{
			// Create the linear tiled destination image to copy to and to read the memory from
			vk::ImageCreateInfo imgCreateInfo(vks::initializers::imageCreateInfo());
			imgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
			imgCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			imgCreateInfo.extent.width_ = width_;
			imgCreateInfo.extent.height_ = height_;
			imgCreateInfo.extent.depth = 1;
			imgCreateInfo.arrayLayers = 1;
			imgCreateInfo.mipLevels = 1;
			imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imgCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
			imgCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			// Create the image
			vk::Image dstImage;
			VK_CHECK_RESULT(vkCreateImage(device_, &imgCreateInfo, nullptr, &dstImage));
			// Create memory to back up the image
			vk::MemoryRequirements memRequirements;
			vk::MemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
			vk::DeviceMemory dstImageMemory;
			vkGetImageMemoryRequirements(device_, dstImage, &memRequirements);
			memAllocInfo.allocationSize = memRequirements.size;
			// Memory must be host visible to copy from
			memAllocInfo.memoryTypeIndex = getMemoryTypeIndex(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device_, &memAllocInfo, nullptr, &dstImageMemory));
			VK_CHECK_RESULT(vkBindImageMemory(device_, dstImage, dstImageMemory, 0));

			// Do the actual blit from the offscreen image to our host visible destination image
			vk::CommandBufferAllocateInfo cmdBufAllocateInfo = vks::initializers::commandBufferAllocateInfo(command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
			vk::CommandBuffer copyCmd;
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device_, &cmdBufAllocateInfo, &copyCmd));
			vk::CommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

			// Transition destination image to transfer destination layout
			vks::tools::insertImageMemoryBarrier(
				copyCmd,
				dstImage,
				0,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				vk::ImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

			// color_attachment_.image is already in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, and does not need to be transitioned

			vk::ImageCopy imageCopyRegion{};
			imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageCopyRegion.srcSubresource.layerCount = 1;
			imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageCopyRegion.dstSubresource.layerCount = 1;
			imageCopyRegion.extent.width_ = width_;
			imageCopyRegion.extent.height_ = height_;
			imageCopyRegion.extent.depth = 1;

			vkCmdCopyImage(
				copyCmd,
				color_attachment_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&imageCopyRegion);

			// Transition destination image to general layout, which is the required layout for mapping the image memory later on
			vks::tools::insertImageMemoryBarrier(
				copyCmd,
				dstImage,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_MEMORY_READ_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				vk::ImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

			VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

			submitWork(copyCmd, queue_);

			// Get layout of the image (including row pitch)
			vk::ImageSubresource subResource{};
			subResource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			vk::SubresourceLayout subResourceLayout;

			vkGetImageSubresourceLayout(device_, dstImage, &subResource, &subResourceLayout);

			// Map image memory so we can start copying from it
			vkMapMemory(device_, dstImageMemory, 0, VK_WHOLE_SIZE, 0, (void**)&imagedata);
			imagedata += subResourceLayout.offset;

		/*
			Save host visible frame_buffer_ image to disk (ppm format)
		*/

			const char* filename = "headless.ppm";
			std::ofstream file(filename, std::ios::out | std::ios::binary);

			// ppm header
			file << "P6\n" << width_ << "\n" << height_ << "\n" << 255 << "\n";

			// If source is BGR (destination is always RGB) and we can't use blit (which does automatic conversion), we'll have to manually swizzle color components
			// Check if source is BGR and needs swizzle
			std::vector<vk::Format> formatsBGR = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM };
			const bool colorSwizzle = (std::find(formatsBGR.begin(), formatsBGR.end(), VK_FORMAT_R8G8B8A8_UNORM) != formatsBGR.end());

			// ppm binary pixel data
			for (int32_t y = 0; y < height_; y++) {
				unsigned int *row = (unsigned int*)imagedata;
				for (int32_t x = 0; x < width_; x++) {
					if (colorSwizzle) {
						file.write((char*)row + 2, 1);
						file.write((char*)row + 1, 1);
						file.write((char*)row, 1);
					}
					else {
						file.write((char*)row, 3);
					}
					row++;
				}
				imagedata += subResourceLayout.rowPitch;
			}
			file.close();

			LOG("Framebuffer image saved to %s\n", filename);

			// Clean up resources
			vkUnmapMemory(device_, dstImageMemory);
			vkFreeMemory(device_, dstImageMemory, nullptr);
			vkDestroyImage(device_, dstImage, nullptr);
		}
#endif
		queue_.waitIdle();
	}

	~VulkanExample()
	{
		vertex_bufmem_.DestroyByDevice(device_);
		index_bufmem_.DestroyByDevice(device_);
		color_attachment_.DestroyByDevice(device_);
		depth_attachment_.DestroyByDevice(device_);
		device_.destroyRenderPass(render_pass_);
		device_.destroyFramebuffer(frame_buffer_);
		device_.destroyPipelineLayout(pipeline_layout_);
		device_.destroyDescriptorSetLayout(descriptor_set_layout_);
		device_.destroyPipeline(pipeline_);
		device_.destroyPipelineCache(pipeline_cache_);
		device_.destroyCommandPool(command_pool_);
		for (auto shadermodule : shader_modules) {
			device_.destroyShaderModule(shadermodule);
		}
		device_.destroy();
#if DEBUG
		if (debugReportCallback) {
			PFN_vkDestroyDebugReportCallbackEXT vkDestroyDebugReportCallback = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"));
			assert(vkDestroyDebugReportCallback);
			vkDestroyDebugReportCallback(instance_, debugReportCallback, nullptr);
		}
#endif
		instance_.destroy();
	}
};

int main(int argc, char* argv[]) {
	unique_ptr<VulkanExample> vulkan_example(new VulkanExample);
	return 0;
}
