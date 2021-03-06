#include "simulation.h"
#include "renderer.h"
#include "compute.h"

comp_simulation::comp_simulation(const VkQueue* pQ,
	const VkQueue* gQ,
	const VkDevice* dev) : simulation(pQ, gQ, dev)
{
	compute = new ComputeConfig();
	renderer = Renderer::get();
}

void comp_simulation::createBufferObjects()
{
	for (auto &b : buffers)
	{
		b->createSpecificBuffer();
	}
}

int comp_simulation::findComputeQueueFamily(VkPhysicalDevice pd)
{
	// find and return the index of compute queue family for device

	int index = -1;

	// as before, find them, set them
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &queueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &queueFamilyCount, queueFamilies.data());

	// find suitable family that supports compute.
	unsigned int i = 0;
	for (const auto& queueFamily : queueFamilies)
	{

		// check for compute support 
		if (queueFamily.queueCount > 0 && queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			index = i;
			break;
		}
		i++;
	}

	return index;
}

void comp_simulation::createCommandPools(QueueFamilyIndices& queueFamilyIndices, VkPhysicalDevice& phys)
{
	// record commands for drawing on the graphics queue
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	// create the pool
	if (vkCreateCommandPool(device, &poolInfo, nullptr, &renderer->gfxCommandPool) != VK_SUCCESS)
		throw std::runtime_error("failed to create gfx command pool!");


	int queueIndex = findComputeQueueFamily(phys);
	// store value
	queueFamilyIndices.computeFamily = queueIndex;
	vkGetDeviceQueue(device, queueIndex, 0, &compute->queue);

	VkCommandPoolCreateInfo cmdPoolInfo = {};
	cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdPoolInfo.queueFamilyIndex = queueFamilyIndices.computeFamily;
	cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &compute->commandPool) != VK_SUCCESS)
		throw std::runtime_error("Failed creating compute cmd pool");
}

void comp_simulation::frame()
{
	renderer->drawFrame();			   // render
	dispatchCompute();				   // submit compute
	renderer->updateUniformBuffer();   // update
	renderer->updateCompute();		   // up

}

// compute command buffers
void comp_simulation::allocateComputeCommandBuffers()
{
	VkCommandBufferAllocateInfo cmdBufAllocateInfo{};
	cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmdBufAllocateInfo.commandPool = compute->commandPool;
	cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmdBufAllocateInfo.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &compute->commandBuffer) != VK_SUCCESS)
		throw std::runtime_error("Failed allocating buffer for compute commands");

	// Fence for compute CB sync
	VkFenceCreateInfo fenceCreateInfo{};
	fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateFence(device, &fenceCreateInfo, nullptr, &compute->fence) != VK_SUCCESS)
		throw std::runtime_error("Failed creating compute fence");
}

void comp_simulation::recordComputeCommands()
{
	// create command buffer
	VkCommandBufferBeginInfo cmdBufInfo{};
	cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

	if (vkBeginCommandBuffer(compute->commandBuffer, &cmdBufInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to create compute command buffer");

	// Compute particle movement

	// Add memory barrier to ensure that the (graphics) vertex shader has fetched attributes before compute starts to write to the buffer
	VkBufferMemoryBarrier bufferBarrier{};
	bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bufferBarrier.buffer = buffers[INSTANCE]->buffer[buffIndex];
	//bufferBarrier.size = VK_WHOLE_SIZE;
	bufferBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;						// Vertex shader invocations have finished reading from the buffer
	bufferBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;								// Compute shader wants to write to the buffer
																							// Compute and graphics queue may have different queue families (see VulkanDevice::createLogicalDevice)
																							// For the barrier to work across different queues, we need to set their family indices
	bufferBarrier.srcQueueFamilyIndex = renderer->queueFamilyIndices.graphics;			// Required as compute and graphics queue may have different families
	bufferBarrier.dstQueueFamilyIndex = renderer->queueFamilyIndices.compute;			// Required as compute and graphics queue may have different families

	vkCmdPipelineBarrier(
		compute->commandBuffer,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, // no flags
		0, nullptr,
		1, &bufferBarrier,
		0, nullptr);

	vkCmdBindPipeline(compute->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute->pipeline);
	vkCmdBindDescriptorSets(compute->commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute->pipelineLayout, 0, 1, &compute->descriptorSet, 0, 0);

	// time compute
	vkCmdResetQueryPool(compute->commandBuffer, renderer->computeQueryPool, 0, 2);
	vkCmdWriteTimestamp(compute->commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, renderer->computeQueryPool, 0);

	// Dispatch the compute     
	vkCmdDispatch(compute->commandBuffer, renderer->PARTICLE_COUNT, 1, 1);

	vkCmdWriteTimestamp(compute->commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, renderer->computeQueryPool, 1);

	// Add memory barrier to ensure that compute shader has finished writing to the buffer
	// Without this the (rendering) vertex shader may display incomplete results (partial data from last frame) 
	bufferBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;								// Compute shader has finished writes to the buffer
	bufferBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;						// Vertex shader invocations want to read from the buffer
	bufferBarrier.buffer = buffers[INSTANCE]->buffer[buffIndex];
	//bufferBarrier.size = VK_WHOLE_SIZE;
	// Compute and graphics queue may have different queue families (see VulkanDevice::createLogicalDevice)
	// For the barrier to work across different queues, we need to set their family indices
	bufferBarrier.srcQueueFamilyIndex = renderer->queueFamilyIndices.compute;			// Required as compute and graphics queue may have different families
	bufferBarrier.dstQueueFamilyIndex = renderer->queueFamilyIndices.graphics;			// Required as compute and graphics queue may have different families

	vkCmdPipelineBarrier(
		compute->commandBuffer,
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, // no flags
		0, nullptr,
		1, &bufferBarrier,
		0, nullptr);

	vkEndCommandBuffer(compute->commandBuffer);
}

void comp_simulation::dispatchCompute()
{
	// wait until presentation is finished before drawing the next frame
	vkQueueWaitIdle(presentQueue);

	auto fenceResult = vkWaitForFences(device, 1, &compute->fence, VK_TRUE, UINT64_MAX);
	// Submit compute commands
	while (fenceResult != VK_SUCCESS)
	{
		if (fenceResult == VK_ERROR_DEVICE_LOST)
			throw std::runtime_error("device crashed");

		fenceResult = vkWaitForFences(device, 1, &compute->fence, VK_TRUE, UINT64_MAX);
	};

	vkResetFences(device, 1, &compute->fence);
	VkSubmitInfo computeSubmitInfo{};
	computeSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	computeSubmitInfo.commandBufferCount = 1;
	computeSubmitInfo.pCommandBuffers = &compute->commandBuffer;

	auto re = vkQueueSubmit(compute->queue, 1, &computeSubmitInfo, compute->fence);
	if (re != VK_SUCCESS)
	{
		std::cout << "result: " << re << std::endl;
		throw std::runtime_error("failed to submit compute queue");
	}
	
}

void comp_simulation::cleanup()
{
	compute->cleanup(device);
}