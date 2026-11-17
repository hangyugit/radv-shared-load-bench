// SPDX-License-Identifier: MIT
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#define VK_CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
   std::fprintf(stderr, "%s failed: %d\n", #call, r); std::exit(1); } } while (0)

int main(int argc, char **argv)
{
   if (argc < 2) {
      std::fprintf(stderr, "usage: %s shader.spv [iterations=128] [groups=256] [wave=32] [samples=21] [mode=0] [width=4] [seed=1]\n", argv[0]);
      return 1;
   }
   uint32_t iterations = argc > 2 ? std::stoul(argv[2]) : 128;
   uint32_t groups = argc > 3 ? std::stoul(argv[3]) : 256;
   uint32_t wave = argc > 4 ? std::stoul(argv[4]) : 32;
   unsigned samples = argc > 5 ? std::stoul(argv[5]) : 21;
   unsigned mode = argc > 6 ? std::stoul(argv[6]) : 0;
   unsigned width = argc > 7 ? std::stoul(argv[7]) : 4;
   unsigned seed = argc > 8 ? std::stoul(argv[8]) : 1;
   if (!groups || !samples || !iterations || iterations > 65536 || mode > 1 || width < 1 || width > 4 || (wave != 32 && wave != 64)) return 1;
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.pApplicationName = "radv-shared128-repro";
   app.apiVersion = VK_API_VERSION_1_3;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   ici.pApplicationInfo = &app;
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
   uint32_t count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
   std::vector<VkPhysicalDevice> devices(count);
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
   bool compile_only = std::getenv("LDS_COMPILE_ONLY") != nullptr;
   uint32_t device_id = std::getenv("LDS_DEVICE_ID") ? std::stoul(std::getenv("LDS_DEVICE_ID"), nullptr, 0) : 0x7550;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties props{};
   for (auto dev : devices) {
      vkGetPhysicalDeviceProperties(dev, &props);
      if (props.vendorID == 0x1002 && (props.deviceID == device_id || compile_only)) { physical = dev; break; }
   }
   if (!physical) { std::fprintf(stderr, "AMD GPU (1002:%04x) not found\n", device_id); return 1; }
   std::printf("device=%s driver=%u.%u.%u wave=%u\n", props.deviceName,
               VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
               VK_VERSION_PATCH(props.driverVersion), wave);

   VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   features.pNext = &f13;
   vkGetPhysicalDeviceFeatures2(physical, &features);
   if (!f13.subgroupSizeControl || !f13.maintenance4) return 1;
   f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   f13.subgroupSizeControl = true;
   f13.maintenance4 = true;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
   std::vector<VkQueueFamilyProperties> queues(count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
   uint32_t family = count;
   for (uint32_t i = 0; i < count; ++i)
      if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && queues[i].timestampValidBits) { family = i; break; }
   if (family == count) return 1;
   float priority = 1;
   VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   dci.pNext = &f13; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, family, 0, &queue);
   VkPhysicalDeviceMemoryProperties memprops;
   vkGetPhysicalDeviceMemoryProperties(physical, &memprops);
   struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void *map; } buffers[2];
   size_t elements = size_t(groups) * 128 * 4;
   size_t sizes[] = {256 * 4 * sizeof(uint32_t), elements * sizeof(uint32_t)};
   for (unsigned i = 0; i < 2; ++i) {
      VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bci.size = sizes[i]; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffers[i].buffer));
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device, buffers[i].buffer, &req);
      uint32_t type = memprops.memoryTypeCount;
      for (uint32_t t = 0; t < memprops.memoryTypeCount; ++t) {
         VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         if ((req.memoryTypeBits & (1u << t)) && (memprops.memoryTypes[t].propertyFlags & flags) == flags) { type = t; break; }
      }
      if (type == memprops.memoryTypeCount) return 1;
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = req.size; mai.memoryTypeIndex = type;
      VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &buffers[i].memory));
      VK_CHECK(vkBindBufferMemory(device, buffers[i].buffer, buffers[i].memory, 0));
      VK_CHECK(vkMapMemory(device, buffers[i].memory, 0, VK_WHOLE_SIZE, 0, &buffers[i].map));
      std::memset(buffers[i].map, 0, sizes[i]);
   }
   auto *input = static_cast<uint32_t *>(buffers[0].map);
   for (unsigned record = 0; record < 256; ++record) {
      input[record * 4] = (record + 17) & 255;
      for (unsigned k = 1; k < width; ++k) {
         input[record * 4 + k] = 0x9e3779b9u * (seed + record * 7 + k);
         input[record * 4] ^= input[record * 4 + k];
      }
   }
   VkDescriptorSetLayoutBinding bindings[2]{};
   for (unsigned i = 0; i < 2; ++i) { bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
   VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
   lci.bindingCount = 2; lci.pBindings = bindings;
   VkDescriptorSetLayout setlayout;
   VK_CHECK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &setlayout));
   VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
   VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   plci.setLayoutCount = 1; plci.pSetLayouts = &setlayout; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &push;
   VkPipelineLayout layout;
   VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
   std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
   if (!file) return 1;
   size_t length = size_t(file.tellg());
   if (!length || length % 4) return 1;
   std::vector<uint32_t> code(length / 4);
   file.seekg(0); file.read(reinterpret_cast<char *>(code.data()), length);
   VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   smci.codeSize = length; smci.pCode = code.data();
   VkShaderModule module;
   VK_CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
   subgroup.requiredSubgroupSize = wave;
   VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
   pci.layout = layout; pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
   pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = module; pci.stage.pName = "main"; pci.stage.pNext = &subgroup;
   VkPipeline pipeline;
   VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
   if (compile_only) { std::printf("compiled=1 errors=0\n"); return 0; }
   VkDescriptorPoolSize poolsize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
   VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
   dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &poolsize;
   VkDescriptorPool pool;
   VK_CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
   dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &setlayout;
   VkDescriptorSet set;
   VK_CHECK(vkAllocateDescriptorSets(device, &dai, &set));
   VkDescriptorBufferInfo infos[2];
   VkWriteDescriptorSet writes[2];
   for (unsigned i = 0; i < 2; ++i) {
      infos[i] = {buffers[i].buffer, 0, sizes[i]};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = set;
      writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
   }
   vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
   VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   cpci.queueFamilyIndex = family;
   VkCommandPool cmdpool;
   VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &cmdpool));
   VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   cai.commandPool = cmdpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
   VkCommandBuffer cmd;
   VK_CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
   VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
   qpci.queryType = VK_QUERY_TYPE_TIMESTAMP; qpci.queryCount = 2;
   VkQueryPool query;
   VK_CHECK(vkCreateQueryPool(device, &qpci, nullptr, &query));
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
   vkCmdResetQueryPool(cmd, query, 0, 2);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
   vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &iterations);
   vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query, 0);
   vkCmdDispatch(cmd, groups, 1, 1);
   VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
   barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
   vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query, 1);
   VK_CHECK(vkEndCommandBuffer(cmd));
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
   submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
   std::vector<double> timings;
   unsigned warmup_ms = std::getenv("LDS_WARMUP_MS") ? std::stoul(std::getenv("LDS_WARMUP_MS")) : 500;
   auto warmup_start = std::chrono::steady_clock::now();
   while (timings.size() < samples) {
      VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      VK_CHECK(vkQueueWaitIdle(queue));
      uint64_t timestamps[2];
      VK_CHECK(vkGetQueryPoolResults(device, query, 0, 2, sizeof(timestamps), timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      uint64_t mask = queues[family].timestampValidBits == 64 ? UINT64_MAX : (uint64_t(1) << queues[family].timestampValidBits) - 1;
      if (std::chrono::steady_clock::now() - warmup_start >= std::chrono::milliseconds(warmup_ms))
         timings.push_back(((timestamps[1] - timestamps[0]) & mask) * double(props.limits.timestampPeriod) / 1000.0);
   }
   unsigned errors = 0;
   auto *result = static_cast<uint32_t *>(buffers[1].map);
   uint32_t reference[128][4]{};
   for (unsigned lane = 0; lane < 128; ++lane) {
      if (!mode) reference[lane][0] = (lane + 1 + 17 * iterations) & 255;
      else for (unsigned i = 0; i < iterations; ++i)
         for (unsigned j = 0; j < 8; ++j) {
            unsigned record = (lane + 1 + 17 * i + 19 * j) & 255;
            for (unsigned k = 0; k < width; ++k) reference[lane][k] += input[record * 4 + k];
         }
   }
   uint64_t hash = 14695981039346656037ull;
   for (size_t i = 0; i < elements; ++i) {
      uint32_t expected = reference[(i / 4) % 128][i % 4];
      hash = (hash ^ result[i]) * 1099511628211ull;
      if (result[i] != expected) {
         if (errors < 4) std::fprintf(stderr, "mismatch[%zu]: got %u expected %u\n", i, result[i], expected);
         ++errors;
      }
   }
   std::sort(timings.begin(), timings.end());
   std::printf("shader=%s iterations=%u groups=%u mode=%u width=%u seed=%u samples=%u elements=%zu errors=%u hash=%016llx min_us=%.3f median_us=%.3f max_us=%.3f\n",
      argv[1], iterations, groups, mode, width, seed, samples, elements, errors, (unsigned long long)hash, timings.front(), timings[timings.size()/2], timings.back());
   vkDestroyQueryPool(device, query, nullptr); vkDestroyCommandPool(device, cmdpool, nullptr);
   vkDestroyDescriptorPool(device, pool, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
   vkDestroyShaderModule(device, module, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   vkDestroyDescriptorSetLayout(device, setlayout, nullptr);
   for (auto &buffer : buffers) { vkUnmapMemory(device, buffer.memory); vkDestroyBuffer(device, buffer.buffer, nullptr); vkFreeMemory(device, buffer.memory, nullptr); }
   vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
   return errors ? 2 : 0;
}
