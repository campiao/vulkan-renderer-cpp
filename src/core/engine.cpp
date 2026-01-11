#include "engine.h"
#include <filesystem>

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"

#include "VkBootstrap.h"

#define VMA_IMPLEMENTATION
// #define VMA_USE_STL_CONTAINERS 1
#include "vk_mem_alloc.h"

#include "glm/gtx/transform.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include "core/vk_images.h"
#include "core/vk_initializers.h"
#include "vk_pipelines.h"

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }

void
VulkanEngine::init(int w, int h,
                   const char *title,
                   bool useValidationLayers)
{
    // only one engine initialization per application
    assert(loadedEngine == nullptr);
    loadedEngine = this;
    
    _useValidationLayers = useValidationLayers;
    
    const bool ok = SDL_Init(SDL_INIT_VIDEO);
    if (!ok) {
        spdlog::error("Failed to initialized SDL!");
        abort();
    }
    SDL_WindowFlags flags =
    (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    _window = SDL_CreateWindow(title, w, h, flags);
    if (_window == nullptr) {
        spdlog::error("Failed to create SDL window!");
        abort();
    }
    spdlog::info("Window created successfully.");
    bool res = SDL_HideCursor();
    assert(res);
    
    _windowExtent.width = w;
    _windowExtent.height = h;
    
    init_vulkan();
    init_swapchain();
    init_commands();
    init_sync_structures();
    init_descriptors();
    init_pipelines();
    init_imgui();
    init_default_data();
    
    // NOTE(champ): Load all paths for .glb/.gltf files available
    // and be able to select them in the UI
    // Once selected, load them from disk if not already loaded!
    
    // TODO(champ):
    load_gltf_filepaths_in_folder("models");
    
    std::string_view file_to_load = "models/porsche_911.glb";
    auto structure_file = gltf::load_scene_from_file(this, file_to_load);
    if (structure_file.has_value())
    {
        loadedScenes["structure"] = *structure_file;
    }
    else
    {
        spdlog::warn("Failed to load GLTF file at: {}!", file_to_load);
    }
    
    _isInitialized = true;
}

void VulkanEngine::run() {
    SDL_Event e;
    bool quit = false;
    
    while (!quit) {
        SDL_Time start_ticks;
        SDL_GetCurrentTime(&start_ticks);
        // handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            }
            
            if (e.type == SDL_EVENT_WINDOW_MINIMIZED) {
                _stopRendering = true;
            }
            
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) {
                    quit = true;
                }
            }
            
            camera::processSDLEvent(&this->mainCamera, e);
            // TODO(champ): Make it possible to hide and show cursor on right mouse button
            // The code below causes the mouse to be locked when crashing the application
            // inside a debugger, being unable to abort/retry/etc. Alt+F4 also does not
            // solve the issue. Only way to exit was through my laptop's touchscreen,
            // I never would have thought that this feature could be actually useful someday
            
            //if (this->mainCamera.isMouseLocked)
            //{
            //SDL_SetWindowRelativeMouseMode(_window, false);
            //}
            //else
            //{
            //SDL_SetWindowRelativeMouseMode(_window, true);
            //}
            
            ImGui_ImplSDL3_ProcessEvent(&e);
        }
        if (_resizeRequested) {
            resize_swapchain();
        }
        
        if (_stopRendering) {
            continue;
        }
        
        // imgui new frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        
        if (ImGui::Begin("Debug Window")) {
            ImGui::SliderFloat("Render scale", &_renderScale, 0.3f, 1.0f);
            
            ComputeEffect &selected = backgroundEffects[currentBackgroundEffect];
            
            ImGui::Text("Selected effect: %s", selected.name);
            
            ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0,
                             backgroundEffects.size() - 1);
            
            ImGui::InputFloat4("data1", (float *)&selected.data.data1);
            ImGui::InputFloat4("data2", (float *)&selected.data.data2);
            ImGui::InputFloat4("data3", (float *)&selected.data.data3);
            ImGui::InputFloat4("data4", (float *)&selected.data.data4);
            
            // NOTE(champ): camera stuff
            if (ImGui::CollapsingHeader("Camera data"))
            {
                ImGui::InputFloat3( "Position",(float*) &mainCamera.position);
                ImGui::InputFloat3( "Velocity",(float*) &mainCamera.velocity);
                ImGui::InputFloat( "Pitch rotation", &mainCamera.pitch);
                ImGui::InputFloat( "Yaw rotation", &mainCamera.yaw);
            }
            
            // NOTE(champ): GLTF Models available
            if (!this->gltfFilesPath.size()==0)
            {
                if (ImGui::CollapsingHeader("Models"))
                {
                    static int item_selected_idx = 0;
                    auto combo_preview_value = gltfFilesPath[item_selected_idx];
                    if (ImGui::BeginCombo("Available models", combo_preview_value.data()))
                    {
                        for (int n = 0; n < gltfFilesPath.size(); n++)
                        {
                            const bool is_selected = (item_selected_idx == n);
                            if (ImGui::Selectable(gltfFilesPath[n].data(), is_selected))
                                item_selected_idx = n;
                            
                            // Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }
        }
        ImGui::End();
        
        ImGui::Begin("Stats");
        ImGui::Text("frametime:   %f ms", stats.frame_time);
        ImGui::Text("draw time:   %f ms", stats.mesh_draw_time);
        ImGui::Text("update time: %f ms", stats.scene_update_time);
        ImGui::Text("triangles:   %i", stats.triangle_count);
        ImGui::Text("draws:       %i", stats.drawcall_count);
        ImGui::End();
        
        // some imgui UI to test
        ImGui::ShowDemoWindow();
        
        // make imgui calculate internal draw structures
        ImGui::Render();
        
        draw();
        
        SDL_Time end_ticks;
        SDL_GetCurrentTime(&end_ticks);
        
        // Convert to miliseconds
        SDL_Time diff = (float)(end_ticks - start_ticks);
        stats.frame_time = diff / 1000000.0f;
    }
}

void VulkanEngine::init_vulkan() {
    vkb::InstanceBuilder builder;
    
    auto instance_result = builder.set_app_name("Vulkan Engine App")
        .request_validation_layers()
        .request_validation_layers(_useValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();
    if (!instance_result) {
        spdlog::error("Failed to create VkInstance!");
        abort();
    }
    vkb::Instance vkb_instance = instance_result.value();
    _instance = vkb_instance.instance;
    _debugMessenger = vkb_instance.debug_messenger;
    
    SDL_Vulkan_CreateSurface(_window, _instance, nullptr, &_surface);
    
    // Vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = true;
    features13.synchronization2 = true;
    
    // Vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;
    
    // Use vkbootstrap to select a GPU
    vkb::PhysicalDeviceSelector selector(vkb_instance);
    vkb::PhysicalDevice physicalDevice = selector.set_minimum_version(1, 3)
        .set_required_features_13(features13)
        .set_required_features_12(features12)
        .set_surface(_surface)
        .select()
        .value();
    
    vkb::DeviceBuilder deviceBuilder(physicalDevice);
    vkb::Device vkbDevice = deviceBuilder.build().value();
    _device = vkbDevice.device;
    _physicalDevice = vkbDevice.physical_device;
    
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily =
        vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
    
    // initialize the memory allocator
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = _physicalDevice;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);
    
    _mainDeletionQueue.push_function([&]() { vmaDestroyAllocator(_allocator); });
}
void VulkanEngine::init_swapchain() {
    create_swapchain(_windowExtent.width, _windowExtent.height);
    
    // draw image size matches monitor size
    VkExtent3D drawImageExtent = {};
    
    SDL_Rect rect = {};
    SDL_GetDisplayBounds(SDL_GetDisplayForWindow(_window), &rect);
    
    drawImageExtent.width = rect.w;
    drawImageExtent.height = rect.h;
    drawImageExtent.depth = 1;
    
    // hard coded 16 bit float image format
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;
    
    VkImageUsageFlags drawImageUsages{};
    // Copy from and into the image
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // "Compute shaders can write to it" layout
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    // "Graphics pipelines can draw geometry to it" layout
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    
    VkImageCreateInfo imageCI = vkinit::image_create_info(
                                                          _drawImage.imageFormat, drawImageUsages, drawImageExtent);
    
    // allocate the draw image from GPU local memory
    VmaAllocationCreateInfo img_allocCI = {};
    img_allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    img_allocCI.requiredFlags =
        VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    vmaCreateImage(_allocator, &imageCI, &img_allocCI, &_drawImage.image,
                   &_drawImage.allocation, nullptr);
    
    VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(
                                                                 _drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkCreateImageView(_device, &viewCI, nullptr, &_drawImage.imageView));
    
    // DEPTH IMAGE
    _depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
    _depthImage.imageExtent = drawImageExtent;
    
    VkImageUsageFlags depthImageUsages = {};
    depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    
    VkImageCreateInfo depthCI = vkinit::image_create_info(
                                                          _depthImage.imageFormat, depthImageUsages, drawImageExtent);
    
    vmaCreateImage(_allocator, &depthCI, &img_allocCI, &_depthImage.image,
                   &_depthImage.allocation, nullptr);
    
    VkImageViewCreateInfo depthViewCI = vkinit::imageview_create_info(
                                                                      _depthImage.imageFormat, _depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VK_CHECK(vkCreateImageView(_device, &depthViewCI, nullptr,
                               &_depthImage.imageView));
    
    _mainDeletionQueue.push_function([=]() {
                                         vkDestroyImageView(_device, _drawImage.imageView, nullptr);
                                         vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);
                                         vkDestroyImageView(_device, _depthImage.imageView, nullptr);
                                         vmaDestroyImage(_allocator, _depthImage.image, _depthImage.allocation);
                                     });
}
void VulkanEngine::init_commands() {
    // create a command pool for commands submitted to the graphics queue.
    // we also want the pool to allow for resetting of individual command buffers
    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(
                                                                               _graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    
    for (int i = 0; i < FRAME_OVERLAP; i++) {
        
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr,
                                     &_frames[i]._commandPool));
        
        // allocate the default command buffer that we will use for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo =
            vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);
        
        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo,
                                          &_frames[i]._mainCommandBuffer));
    }
    
    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr,
                                 &_immCommandPool));
    
    // allocate the command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo =
        vkinit::command_buffer_allocate_info(_immCommandPool, 1);
    
    VK_CHECK(
             vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immCommandBuffer));
    
    _mainDeletionQueue.push_function(
                                     [=]() { vkDestroyCommandPool(_device, _immCommandPool, nullptr); });
}
void VulkanEngine::init_sync_structures() {
    // one fence to control when the GPU has finished rendering the frame
    // two semaphores to synchronize rendering with swapchain
    
    VkFenceCreateInfo fenceCI =
        vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCI = vkinit::semaphore_create_info();
    
    for (auto &frame : _frames) {
        VK_CHECK(vkCreateFence(_device, &fenceCI, nullptr, &frame._renderFence));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCI, nullptr,
                                   &frame._swapchainSemaphore));
    }
    
    _submitSemaphores.reserve(_swapchainImages.size());
    for (int i = 0; i < _swapchainImages.size(); i++) {
        VkSemaphore semaphore;
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCI, nullptr, &semaphore));
        _submitSemaphores.push_back(semaphore);
    }
    
    VK_CHECK(vkCreateFence(_device, &fenceCI, nullptr, &_immFence));
    _mainDeletionQueue.push_function(
                                     [=]() { vkDestroyFence(_device, _immFence, nullptr); });
}

void VulkanEngine::init_descriptors() {
    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
    };
    
    _globalDescriptorAllocator.init(_device, 10, sizes);
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorLayout =
            builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }
    {
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		_singleImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		_gpuSceneDataDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	}
    // allocate a descriptor set for our draw image
    _drawImageDescriptors =
        _globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);
    
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView = _drawImage.imageView;
    
    VkWriteDescriptorSet drawImageWrite = {};
    drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext = nullptr;
    
    drawImageWrite.dstBinding = 0;
    drawImageWrite.dstSet = _drawImageDescriptors;
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo = &imgInfo;
    
    vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);
    
    for (int i = 0; i < FRAME_OVERLAP; i++) {
        // create a descriptor pool
        std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frame_sizes = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
        };
        
        _frames[i]._frameDescriptors = DescriptorAllocatorGrowable{};
        _frames[i]._frameDescriptors.init(_device, 1000, frame_sizes);
        
        _mainDeletionQueue.push_function(
                                         [&, i]() { _frames[i]._frameDescriptors.destroy_pools(_device); });
    }
    
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        _gpuSceneDataDescriptorLayout = builder.build(
                                                      _device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    {
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        _singleImageDescriptorLayout = builder.build(
                                                     _device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    
    
    // make sure both the descriptor allocator and the new layout get cleaned up
    // properly
    _mainDeletionQueue.push_function([&]() {
                                         _globalDescriptorAllocator.destroy_pools(_device);
                                         vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
                                         vkDestroyDescriptorSetLayout(_device, _gpuSceneDataDescriptorLayout,
                                                                      nullptr);
                                     });
}

void VulkanEngine::init_pipelines() {
    // Compute pipeline
    init_background_pipeline();
    
    // Graphics pipelines
    init_mesh_pipeline();
    metalRoughMat.build_pipelines(this);
}

void VulkanEngine::init_background_pipeline() {
    VkPipelineLayoutCreateInfo computeLayout = {};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;
    
    VkPushConstantRange pushConstant = {};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstancts);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    
    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;
    
    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr,
                                    &_gradientPipelineLayout));
    
    // layout code
    VkShaderModule gradientShader;
    if (!vkutil::load_shader_module("shaders/gradient_color.comp.spv", _device,
                                    &gradientShader)) {
        spdlog::error("Error when building the compute shader \n");
    }
    VkShaderModule skyShader;
    if (!vkutil::load_shader_module("shaders/sky.comp.spv", _device,
                                    &skyShader)) {
        spdlog::error("Error when building the compute shader \n");
    }
    
    VkPipelineShaderStageCreateInfo stageinfo{};
    stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageinfo.pNext = nullptr;
    stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageinfo.module = gradientShader;
    stageinfo.pName = "main";
    
    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType =
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.layout = _gradientPipelineLayout;
    computePipelineCreateInfo.stage = stageinfo;
    
    ComputeEffect gradient = {};
    gradient.layout = _gradientPipelineLayout;
    gradient.name = "gradient";
    gradient.data = {};
    
    // default colors
    gradient.data.data1 = glm::vec4(1, 0, 0, 1);
    gradient.data.data2 = glm::vec4(0, 0, 1, 1);
    
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                                      &computePipelineCreateInfo, nullptr,
                                      &gradient.pipeline));
    
    // change the shader module only to create the sky shader
    computePipelineCreateInfo.stage.module = skyShader;
    
    ComputeEffect sky;
    sky.layout = _gradientPipelineLayout;
    sky.name = "sky";
    sky.data = {};
    // default sky parameters
    sky.data.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);
    
    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                                      &computePipelineCreateInfo, nullptr,
                                      &sky.pipeline));
    
    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);
    
    _mainDeletionQueue.push_function([=]() {
                                         vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
                                         vkDestroyPipeline(_device, sky.pipeline, nullptr);
                                         vkDestroyPipeline(_device, gradient.pipeline, nullptr);
                                     });
    // add the 2 background effects into the array
    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);
}

void VulkanEngine::init_mesh_pipeline() {
    VkShaderModule triangleVertShader;
    if (!vkutil::load_shader_module("shaders/colored_triangle_mesh.vert.spv",
                                    _device, &triangleVertShader)) {
        spdlog::error("Error when building the compute shader \n");
    }
    VkShaderModule triangleFragShader;
    if (!vkutil::load_shader_module("shaders/tex_image.frag.spv", _device,
                                    &triangleFragShader)) {
        spdlog::error("Error when building the compute shader \n");
    }
    
    VkPushConstantRange bufferRange = {};
    bufferRange.offset = 0;
    bufferRange.size = sizeof(GPUDrawPushConstants);
    bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    VkPipelineLayoutCreateInfo pipeline_layout_info =
        vkinit::pipeline_layout_create_info();
    pipeline_layout_info.pPushConstantRanges = &bufferRange;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pSetLayouts = &_singleImageDescriptorLayout;
    pipeline_layout_info.setLayoutCount = 1;
    VK_CHECK(vkCreatePipelineLayout(_device, &pipeline_layout_info, nullptr,
                                    &_meshPipelineLayout));
    
    PipelineBuilder pipelineBuilder;
    
    // use the triangle layout we created
    pipelineBuilder._pipelineLayout = _meshPipelineLayout;
    // connecting the vertex and pixel shaders to the pipeline
    pipelineBuilder.set_shaders(triangleVertShader, triangleFragShader);
    // it will draw triangles
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    // filled triangles
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    // no backface culling
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    // no multisampling
    pipelineBuilder.set_multisampling_none();
    // no blending
    pipelineBuilder.disable_blending();
    // pipelineBuilder.enable_blending_alphablend();
    // pipelineBuilder.enable_blending_additive();
    
    // pipelineBuilder.disable_depthtest();
    pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    
    // connect the image format we will draw into, from draw image
    pipelineBuilder.set_color_attachment_format(_drawImage.imageFormat);
    pipelineBuilder.set_depth_format(_depthImage.imageFormat);
    
    // finally build the pipeline
    _meshPipeline = pipelineBuilder.build_pipeline(_device);
    
    // clean structures
    vkDestroyShaderModule(_device, triangleFragShader, nullptr);
    vkDestroyShaderModule(_device, triangleVertShader, nullptr);
    
    _mainDeletionQueue.push_function([&]() {
                                         vkDestroyPipelineLayout(_device, _meshPipelineLayout, nullptr);
                                         vkDestroyPipeline(_device, _meshPipeline, nullptr);
                                     });
}

void VulkanEngine::init_imgui() {
    
    // 1: create descriptor pool for IMGUI
    //  the size of the pool is very oversize, but it's copied from imgui demo
    //  itself.
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};
    
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    
    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));
    
    // 2: initialize imgui library
    
    // this initializes the core structures of imgui
    ImGui::CreateContext();
    
    // this initializes imgui for SDL
    ImGui_ImplSDL3_InitForVulkan(_window);
    
    // this initializes imgui for Vulkan
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = _instance;
    init_info.PhysicalDevice = _physicalDevice;
    init_info.Device = _device;
    init_info.Queue = _graphicsQueue;
    init_info.DescriptorPool = imguiPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    init_info.UseDynamicRendering = true;
    
    // dynamic rendering parameters for imgui to use
    init_info.PipelineRenderingCreateInfo = {};
    init_info.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats =
        &_swapchainImageFormat;
    
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    
    ImGui_ImplVulkan_Init(&init_info);
    
    // ImGui_ImplVulkan_CreateFontsTexture();
    
    // add the destroy the imgui created structures
    _mainDeletionQueue.push_function([=]() {
                                         ImGui_ImplVulkan_Shutdown();
                                         vkDestroyDescriptorPool(_device, imguiPool, nullptr);
                                     });
}

void VulkanEngine::init_default_data() {
    // load mesh data from GLB file
    _testMeshes = gltf::loadMeshes(this, "models/basicmesh.glb").value();
    
    // create default images and samplers
    u32 white = glm::packUnorm4x8(glm::vec4(1,1,1,1));
    _whiteImage = create_image((void*)&white,VkExtent3D{1,1,1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    
    u32 black= glm::packUnorm4x8(glm::vec4(0,0,0,1));
    _blackImage = create_image((void*)&black,VkExtent3D{1,1,1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    
    u32 grey= glm::packUnorm4x8(glm::vec4(0.66f,0.66f,0.66f,1));
    _greyImage = create_image((void*)&grey,VkExtent3D{1,1,1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    
    u32 magenta = glm::packUnorm4x8(glm::vec4(1,0,1,1));
    // std::array<u32, 16*16> pixels;
    u32 pixels[16*16];
    for (int x = 0; x < 16; x++) {
		for (int y = 0; y < 16; y++) {
			pixels[y*16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
		}
	}
    
    _errorCheckboardImage= create_image(pixels,VkExtent3D{16,16,1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
    
    VkSamplerCreateInfo samplerCI = {};
    samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCI.magFilter = VK_FILTER_NEAREST;
    samplerCI.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(_device, &samplerCI, nullptr, &_defaulSamplerNearest);
    
    samplerCI.magFilter = VK_FILTER_LINEAR;
    samplerCI.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(_device, &samplerCI, nullptr, &_defaultSamplerlinear);
    
    // initialize GLTFMaterial data
    GLTFMetallic_Roughness::MaterialResources matResources;
    matResources.colorImage = _whiteImage;
    matResources.colorSampler = _defaultSamplerlinear;
    matResources.metalRoughImage = _whiteImage;
    matResources.metalRoughSampler = _defaultSamplerlinear;
    
    AllocatedBuffer materialConstants = create_buffer(sizeof(GLTFMetallic_Roughness::MaterialConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    GLTFMetallic_Roughness::MaterialConstants* sceneUniformData = (GLTFMetallic_Roughness::MaterialConstants*)materialConstants.allocation->GetMappedData();
    sceneUniformData->colorFactors = glm::vec4{1,1,1,1};
    sceneUniformData->metal_rough_factors = glm::vec4{1,0.5f,0,0};
    
    matResources.dataBuffer = materialConstants.buffer;
    matResources.dataBufferOffset = 0;
    _defaulMatData = metalRoughMat.write_material(_device, MaterialPass::GLTF_PBR_OPAQUE, matResources, _globalDescriptorAllocator);
    
    for (auto& m: this->_testMeshes) {
        std::shared_ptr<MeshNode> newNode = std::make_shared<MeshNode>();
        newNode->mesh = m;
        newNode->localTransform = glm::mat4{1.0f};
        newNode->worldTransform = glm::mat4{1.0f};
        
        for (auto& s: newNode->mesh->surfaces) {
            s.material = std::make_shared<GLTFMaterial>(GLTFMaterial{this->_defaulMatData});
        }
        
        this->loadedNodes[std::string{m->name}] = newNode;
    }
    
    this->mainCamera.velocity = glm::vec3{0.f};
    this->mainCamera.position = glm::vec3{0.0f,1.8f,5.0f};
    this->mainCamera.pitch = 0.0f;
    this->mainCamera.yaw = glm::radians(0.0f);;
    
    _mainDeletionQueue.push_function([&]()
                                     {
                                         vkDestroySampler(_device, _defaultSamplerlinear, nullptr);
                                         vkDestroySampler(_device, _defaulSamplerNearest, nullptr);
                                         
                                         destroy_image(_whiteImage);
                                         destroy_image(_blackImage);
                                         destroy_image(_greyImage);
                                         destroy_image(_errorCheckboardImage);
                                     });
    
    _mainDeletionQueue.push_function([=] ()
                                     {
                                         destroy_buffer(materialConstants);
                                     });
}

void VulkanEngine::create_swapchain(u32 w, u32 h) {
    vkb::SwapchainBuilder swapchainBuilder{_physicalDevice, _device, _surface};
    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    
    VkSurfaceFormatKHR surfaceFormat{};
    surfaceFormat.format = _swapchainImageFormat;
    surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    vkb::Swapchain vkbSwapchain =
        swapchainBuilder.set_desired_format(surfaceFormat)
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(w, h)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();
    
    _swapchainExtent = vkbSwapchain.extent;
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::resize_swapchain() {
    vkDeviceWaitIdle(_device);
    
    destroy_swapchain();
    
    int width, height;
    SDL_GetWindowSize(_window, &width, &height);
    _windowExtent.width = width;
    _windowExtent.height = height;
    
    create_swapchain(_windowExtent.width, _windowExtent.height);
    
    _resizeRequested = false;
}

void VulkanEngine::destroy_swapchain() {
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    
    for (auto &imageView : _swapchainImageViews) {
        vkDestroyImageView(_device, imageView, nullptr);
    }
}

void VulkanEngine::draw() {
    FrameData &currentFrame = get_current_frame();
    this->update_scene();
    
    // Wait for the GPU to finish all its work
    VK_CHECK(vkWaitForFences(_device, 1, &currentFrame._renderFence, true,
                             1000000000));
    
    currentFrame._deletionQueue.flush();
    currentFrame._frameDescriptors.clear_pools(_device);
    
    u32 swapchainImageIndex;
    VkResult res = vkAcquireNextImageKHR(_device, _swapchain, 1000000000,
                                         currentFrame._swapchainSemaphore,
                                         nullptr, &swapchainImageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        _resizeRequested = true;
        return;
    }
    VK_CHECK(vkResetFences(_device, 1, &currentFrame._renderFence));
    
    VkCommandBuffer cmd = currentFrame._mainCommandBuffer;
    // Reset the command buffer to start writing again
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(
                                                                              VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    
    _drawExtent.width =
        std::min(_swapchainExtent.width, _drawImage.imageExtent.width) *
        _renderScale;
    _drawExtent.height =
        std::min(_swapchainExtent.height, _drawImage.imageExtent.height) *
        _renderScale;
    
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
    
    // make the swapchain image into a writeable format
    vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
    
    draw_background(cmd);
    
    // trasition the draw image to optimal for mat for graphics pipeline
    vkutil::transition_image(cmd, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::transition_image(cmd, _depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    
    draw_geometry(cmd);
    
    // transition the draw image and the swapchain image into their correct
    // transfer layouts
    vkutil::transition_image(cmd, _drawImage.image,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex],
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmd, _drawImage.image,
                                _swapchainImages[swapchainImageIndex],
                                _drawExtent, _swapchainExtent);
    
    // set swapchain image layout to Attachment Optimal so we can draw it
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    
    // draw imgui into the swapchain image
    draw_imgui(cmd, _swapchainImageViews[swapchainImageIndex]);
    
    // make the swapchain image into presentable mode
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex],
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    
    // we can no longer add commands
    VK_CHECK(vkEndCommandBuffer(cmd));
    
    // prepare the submission to the queue.
    // we want to wait on the _presentSemaphore, as that semaphore is signaled
    // when the swapchain is ready we will signal the _renderSemaphore, to signal
    // that rendering has finished
    
    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
    
    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(
                                                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR,
                                                                   currentFrame._swapchainSemaphore);
    VkSemaphoreSubmitInfo signalInfo =
        vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                      _submitSemaphores.at(swapchainImageIndex));
    
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);
    
    // submit command buffer to the queue and execute it.
    //  _renderFence will now block until the graphic commands finish execution
    VK_CHECK(
             vkQueueSubmit2(_graphicsQueue, 1, &submit, currentFrame._renderFence));
    
    // prepare present
    //  this will put the image we just rendered to into the visible window.
    //  we want to wait on the _renderSemaphore for that,
    //  as its necessary that drawing commands have finished before the image is
    //  displayed to the user
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;
    
    presentInfo.pWaitSemaphores = &_submitSemaphores.at(swapchainImageIndex);
    presentInfo.waitSemaphoreCount = 1;
    
    presentInfo.pImageIndices = &swapchainImageIndex;
    
    VkResult presentRes = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
    if (presentRes == VK_ERROR_OUT_OF_DATE_KHR ||
        presentRes == VK_SUBOPTIMAL_KHR) {
        _resizeRequested = true;
    }
    
    // increase the number of frames drawn
    _frameNumber += 1;
}

void VulkanEngine::draw_background(VkCommandBuffer cmd) {
    // // make a clear-color from frame number. This will flash with a 120 frame
    // // period.
    // VkClearColorValue clearValue;
    // float flash = std::abs(std::sin(_frameNumber / 120.f));
    // clearValue = {{0.0f, 0.0f, flash, 1.0f}};
    
    // VkImageSubresourceRange clearRange =
    //     vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
    
    ComputeEffect &effect = backgroundEffects[currentBackgroundEffect];
    
    // bind the gradient drawing compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);
    
    // bind the descriptor set containing the draw image for the compute pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            _gradientPipelineLayout, 0, 1, &_drawImageDescriptors,
                            0, nullptr);
    
    vkCmdPushConstants(cmd, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(ComputePushConstancts), &effect.data);
    // execute the compute pipeline dispatch. We are using 16x16 workgroup size so
    // we need to divide by it
    vkCmdDispatch(cmd, std::ceil(_drawExtent.width / 16.0),
                  std::ceil(_drawExtent.height / 16.0), 1);
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmd) {
    stats.drawcall_count = 0;
    stats.triangle_count = 0;
    SDL_Time start_ticks = 0;
    SDL_GetCurrentTime(&start_ticks);
    
    std::vector<u32> opaque_draws;
    opaque_draws.reserve(_mainDrawContext.opaqueSurfaces.size());
    
    for (u32 i = 0; i < _mainDrawContext.opaqueSurfaces.size(); i++)
    {
        // NOTE(champ): this method of frustum culling ended up not really being useful
        // on my machine. At most it saves a couple of ms, but also adds some especially
        // when almost all meshes are inside the frustum
        
        //if (is_renderobj_visible(_mainDrawContext.opaqueSurfaces[i], _sceneData.viewproj))
        {
            opaque_draws.push_back(i);
        }
    }
    
    // sort the opaque surfaces by material and mesh
    std::sort(opaque_draws.begin(), opaque_draws.end(),
              [&](const auto& iA, const auto& iB)
              {
                  const RenderObject& A = _mainDrawContext.opaqueSurfaces[iA];
                  const RenderObject& B = _mainDrawContext.opaqueSurfaces[iB];
                  if (A.material == B.material)
                  {
                      return A.indexBuffer < B.indexBuffer;
                  }
                  return A.material < B.material;
              });
    
    
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
                                                                        _drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(
                                                                              _depthImage.imageView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    
    VkRenderingInfo renderInfo =
        vkinit::rendering_info(_drawExtent, &colorAttachment, &depthAttachment);
    vkCmdBeginRendering(cmd, &renderInfo);
    
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshPipeline);
    // set dynamic viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = _drawExtent.width;
    viewport.height = _drawExtent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = _drawExtent.width;
    scissor.extent.height = _drawExtent.height;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    VkDescriptorSet image_set = get_current_frame()._frameDescriptors.allocate(_device, _singleImageDescriptorLayout);
    {
        DescriptorWriter writer;
        writer.write_image(0, _errorCheckboardImage.imageView, _defaulSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device, image_set);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshPipelineLayout, 0, 1, &image_set, 0, nullptr);
    
    AllocatedBuffer gpuSceneDataBuffer =
        create_buffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VMA_MEMORY_USAGE_CPU_TO_GPU);
    
    // add it to the deletion queue of this frame
    get_current_frame()._deletionQueue.push_function(
                                                     [=]() { destroy_buffer(gpuSceneDataBuffer); });
    
    // write the buffer
    GPUSceneData *sceneUniformData =
    (GPUSceneData *)gpuSceneDataBuffer.allocation->GetMappedData();
    *sceneUniformData = _sceneData;
    
    // create a descriptor set that binds that buffer and update it
    VkDescriptorSet globalDescriptor =
        get_current_frame()._frameDescriptors.allocate(
                                                       _device, _gpuSceneDataDescriptorLayout);
    
    
    DescriptorWriter writer;
    writer.write_buffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0,
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.update_set(_device, globalDescriptor);
    
    // @SECTION: Faster drawing by skipping binding the pipeline if it is already binded
    MaterialPipeline* last_pipeline = nullptr;
    MaterialInstance* last_material = nullptr;
    VkBuffer last_index_buffer = VK_NULL_HANDLE;
    auto draw_render_object = [&](const RenderObject& obj)
    {
        if (obj.material != last_material)
        {
            last_material = obj.material;
            // rebind pipeline if material has changed
            if (obj.material->pipeline != last_pipeline)
            {
                last_pipeline = obj.material->pipeline;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj.material->pipeline->pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj.material->pipeline->pipelineLayout, 0, 1, &globalDescriptor, 0, nullptr);
                VkViewport viewport = {0};
                viewport.x = 0;
                viewport.y = 0;
                viewport.width = (float)_windowExtent.width;
                viewport.height = (float)_windowExtent.height;
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                
                VkRect2D scissor = {0};
                scissor.offset.x = 0;
                scissor.offset.y = 0;
                scissor.extent.width = _windowExtent.width;
                scissor.extent.height = _windowExtent.height;
                vkCmdSetScissor(cmd, 0, 1, &scissor);
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, obj.material->pipeline->pipelineLayout, 1, 1, &obj.material->materialSet, 0, nullptr);
        }
        
        // rebind index buffer
        if (obj.indexBuffer != last_index_buffer)
        {
            last_index_buffer = obj.indexBuffer;
            vkCmdBindIndexBuffer(cmd, obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }
        
        GPUDrawPushConstants pushConstants;
        pushConstants.vertexBuffer = obj.vertexBufferAddr;
        pushConstants.worldMatrix = obj.transform;
        vkCmdPushConstants(cmd, obj.material->pipeline->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);
        
        vkCmdDrawIndexed(cmd, obj.indexCount, 1, obj.firstIndex, 0,0);
        
        stats.drawcall_count += 1;
        stats.triangle_count += obj.indexCount / 3;
    };
    
    for (auto& obj_index: opaque_draws) {
        draw_render_object(_mainDrawContext.opaqueSurfaces[obj_index]);
    }
    for (const RenderObject& obj: _mainDrawContext.transparentSurfaces) {
        draw_render_object(obj);
    }
    
    SDL_Time end_ticks = 0;
    SDL_GetCurrentTime(&end_ticks);
    
    SDL_Time diff = (float)(end_ticks - start_ticks);
    stats.mesh_draw_time = diff / 1000000.0f;
    
    // auto currentMesh = _testMeshes[_currentTestMesh];
    // GPUDrawPushConstants push_constants;
    // // Monkey model is facing the positive Z axis
    // // so we have to rotate it so it faces us
    // // NOTE: Maybe just rotate the camera?
    
    // glm::mat4 model = glm::rotate(
    //     glm::mat4(1.0f), glm::radians(_currentRotationAngle), glm::vec3(0, 1, 0));
    // glm::mat4 view = glm::translate(glm::vec3{0, 0, -5});
    // // camera projection
    // glm::mat4 projection = glm::perspective(
    //     glm::radians(70.0f), (float)_drawExtent.width / (float)_drawExtent.height,
    //     100.0f, 0.1f);
    
    // // invert the Y direction on projection matrix so that we are more similar
    // // to opengl and gltf axis
    // projection[1][1] *= -1;
    
    // push_constants.worldMatrix = projection * view * model;
    // // (x,y,z) (r,g,b) z -> positive b
    // // push_constants.worldMatrix = glm::mat4(1.0f);
    // // push_constants.worldMatrix[1][1] *= -1;
    // push_constants.vertexBuffer = currentMesh->meshBuffers.vertexBufferAddress;
    
    // vkCmdPushConstants(cmd, _meshPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
    //                    sizeof(GPUDrawPushConstants), &push_constants);
    // vkCmdBindIndexBuffer(cmd, currentMesh->meshBuffers.indexBuffer.buffer, 0,
    //                      VK_INDEX_TYPE_UINT32);
    
    // vkCmdDrawIndexed(cmd, currentMesh->surfaces[0].count, 1,
    //                  currentMesh->surfaces[0].startIndex, 0, 0);
    
    // spdlog::info("Mesh count: {}!Mesh startIndex: {}!",
    //            _testMeshes[0]->surfaces[0].count,
    //          _testMeshes[0]->surfaces[0].startIndex);
    
    vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd,
                              VkImageView targetImageView) {
    
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
                                                                        targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo =
        vkinit::rendering_info(_swapchainExtent, &colorAttachment, nullptr);
    
    vkCmdBeginRendering(cmd, &renderInfo);
    
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    
    vkCmdEndRendering(cmd);
}

void VulkanEngine::update_scene()
{
    SDL_Time start_ticks = 0;
    SDL_GetCurrentTime(&start_ticks);
    
    this->_mainDrawContext.opaqueSurfaces.clear();
    this->_mainDrawContext.transparentSurfaces.clear();
    
    camera::update(&this->mainCamera);
    glm::mat4 view = camera::getViewMatrix(&this->mainCamera);
    
    //this->loadedNodes[this->_testMeshes[this->_currentTestMesh]->name]->draw(glm::mat4{1.0f}, this->_mainDrawContext);
    
    this->_sceneData.view = view;
    this->_sceneData.projection = glm::perspective(glm::radians(70.0f),
                                                   (float)this->_windowExtent.width / (float)this->_windowExtent.height,
                                                   1000.0f,
                                                   0.1f);
    // invert the projection matrix for Vulkan
    this->_sceneData.projection[1][1] *= -1;
    this->_sceneData.viewproj = this->_sceneData.projection * this->_sceneData.view;
    
    // default lighting parameters
    this->_sceneData.ambientColor = glm::vec4(0.1f);
    this->_sceneData.sunlightColor = glm::vec4(1.0f);
    this->_sceneData.sunlightDirection = glm::vec4(0.0f,1.0f,0.5,1.0f);
    
    /*for (int x = -3; x < 3; x++)
    {
        glm::mat4 scale = glm::scale(glm::vec3{0.2});
        glm::mat4 translation = glm::translate(glm::vec3{x,1,0});

        for (auto node: this->loadedNodes)
        {
            this->loadedNodes["Cube"]->draw(translation * scale, this->_mainDrawContext);
        }
    }*/
    
    // NOTE(champ): add all loaded scenes to be drawn
    // maybe we dont want to draw all scenes we have loaded?
    for (const auto& pair: loadedScenes)
    {
        
        if (pair.second)
        {
            // NOTE(champ): make sure this only happens to the player's car!
            //glm::vec3 camera_offset = this->mainCamera.position + glm::vec3(0.0f,-5.0f, 5.0f);
            glm::vec3 camera_offset = {0, 0, 0};
            pair.second->draw(glm::translate(glm::mat4(1.0f),camera_offset),
                              this->_mainDrawContext);
        }
    }
    //loadedScenes["structure"]->draw(glm::mat4(1.0f), _mainDrawContext);
    
    SDL_Time end_ticks = 0;
    SDL_GetCurrentTime(&end_ticks);
    
    float diff = (float)(end_ticks - start_ticks);
    stats.scene_update_time = diff / 1000000.0f;
}

void VulkanEngine::immediate_submit(
                                    std::function<void(VkCommandBuffer cmd)> &&function) {
    
    VK_CHECK(vkResetFences(_device, 1, &_immFence));
    VK_CHECK(vkResetCommandBuffer(_immCommandBuffer, 0));
    
    VkCommandBuffer cmd = _immCommandBuffer;
    
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(
                                                                              VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
    
    function(cmd);
    
    VK_CHECK(vkEndCommandBuffer(cmd));
    
    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);
    
    // submit command buffer to the queue and execute it.
    //  _renderFence will now block until the graphic commands finish execution
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, _immFence));
    
    VK_CHECK(vkWaitForFences(_device, 1, &_immFence, true, 9999999999));
}

AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize,
                                            VkBufferUsageFlags usage,
                                            VmaMemoryUsage memoryUsage) {
    VkBufferCreateInfo bufferCI = {};
    bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCI.pNext = nullptr;
    bufferCI.size = allocSize;
    bufferCI.usage = usage;
    
    VmaAllocationCreateInfo vmaAllocCI = {};
    vmaAllocCI.usage = memoryUsage;
    vmaAllocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    
    AllocatedBuffer newBuffer;
    VK_CHECK(vmaCreateBuffer(_allocator, &bufferCI, &vmaAllocCI,
                             &newBuffer.buffer, &newBuffer.allocation,
                             &newBuffer.info));
    return newBuffer;
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer &buffer) {
    vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

GPUMeshBuffers VulkanEngine::upload_mesh(const std::vector<u32> &indices,
                                         std::vector<Vertex> &vertices) {
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(u32);
    
    GPUMeshBuffers newSurface;
    newSurface.vertexBuffer = create_buffer(
                                            vertexBufferSize,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                            VMA_MEMORY_USAGE_GPU_ONLY);
    
    // find the adress of the vertex buffer
    VkBufferDeviceAddressInfo deviceAdressInfo = {};
    deviceAdressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    deviceAdressInfo.buffer = newSurface.vertexBuffer.buffer;
    newSurface.vertexBufferAddress =
        vkGetBufferDeviceAddress(_device, &deviceAdressInfo);
    
    newSurface.indexBuffer = create_buffer(indexBufferSize,
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_GPU_ONLY);
    
    // staging buffer to copy contents to GPU only memory
    AllocatedBuffer staging = create_buffer(vertexBufferSize + indexBufferSize,
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VMA_MEMORY_USAGE_CPU_ONLY);
    
    void *data = staging.allocation->GetMappedData();
    
    // copy vertex buffer
    memcpy(data, vertices.data(), vertexBufferSize);
    // copy index buffer
    memcpy((char *)data + vertexBufferSize, indices.data(), indexBufferSize);
    
    immediate_submit([&](VkCommandBuffer cmd) {
                         VkBufferCopy vertexCopy{0};
                         vertexCopy.dstOffset = 0;
                         vertexCopy.srcOffset = 0;
                         vertexCopy.size = vertexBufferSize;
                         
                         vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1,
                                         &vertexCopy);
                         
                         VkBufferCopy indexCopy{0};
                         indexCopy.dstOffset = 0;
                         indexCopy.srcOffset = vertexBufferSize;
                         indexCopy.size = indexBufferSize;
                         
                         vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1,
                                         &indexCopy);
                     });
    
    destroy_buffer(staging);
    
    return newSurface;
}

AllocatedImage VulkanEngine::create_image(VkExtent3D size,
                                          VkFormat format,
                                          VkImageUsageFlags usage,
                                          bool mipmapped)
{
    AllocatedImage newImage = {};
    newImage.imageFormat = format;
    newImage.imageExtent = size;
    
    VkImageCreateInfo imageCI = vkinit::image_create_info(format, usage, size);
    if (mipmapped) {
        imageCI.mipLevels = static_cast<u32>(std::floor(std::log2(std::max(size.height, size.width)))) + 1;
    }
    
    VmaAllocationCreateInfo allocationCI = {};
    allocationCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocationCI.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vmaCreateImage(_allocator, &imageCI, &allocationCI, &newImage.image, &newImage.allocation, nullptr));
    
    // if depth format, set correct usage flag
    VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
    if (format == VK_FORMAT_D32_SFLOAT){
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    
    VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, newImage.image, aspectFlag);
    viewCI.subresourceRange.levelCount = imageCI.mipLevels;
    
    VK_CHECK(vkCreateImageView(_device, &viewCI, nullptr, &newImage.imageView));
    
    return newImage;
}

AllocatedImage VulkanEngine::create_image(void* data,
                                          VkExtent3D size,
                                          VkFormat format,
                                          VkImageUsageFlags usage,
                                          bool mipmapped)
{
    size_t dataSize = size.depth * size.width * size.height * 4;
    AllocatedBuffer stagingBuffer = create_buffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    
    memcpy(stagingBuffer.info.pMappedData, data, dataSize);
    
    AllocatedImage newImage = create_image(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);
    
    immediate_submit([&](VkCommandBuffer cmd)
                     {
                         vkutil::transition_image(cmd, newImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                         
                         VkBufferImageCopy copyRegion = {};
                         copyRegion.bufferOffset = 0;
                         copyRegion.bufferRowLength = 0;
                         copyRegion.bufferImageHeight = 0;
                         copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                         copyRegion.imageSubresource.mipLevel = 0;
                         copyRegion.imageSubresource.baseArrayLayer = 0;
                         copyRegion.imageSubresource.layerCount = 1;
                         copyRegion.imageExtent = size;
                         
                         vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
                         
                         if (mipmapped)
                         {
                             vkutil::generate_mipmaps(cmd, newImage.image,
                                                      VkExtent2D{newImage.imageExtent.width, newImage.imageExtent.height});
                         }
                         else
                         {
                             vkutil::transition_image(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                         }
                     });
    
    destroy_buffer(stagingBuffer);
    return newImage;
}

void VulkanEngine::destroy_image(const AllocatedImage& image)
{
    vkDestroyImageView(_device, image.imageView, nullptr);
    vmaDestroyImage(_allocator, image.image, image.allocation);
}

void VulkanEngine::cleanup()
{
    if (_isInitialized) {
        // make sure the GPU has stopped doing its work
        vkDeviceWaitIdle(_device);
        
        loadedScenes.clear();
        
        spdlog::info("Destroying the application!");
        
        for (auto &frame : _frames) {
            vkDestroyCommandPool(_device, frame._commandPool, nullptr);
            vkDestroyFence(_device, frame._renderFence, nullptr);
            vkDestroySemaphore(_device, frame._swapchainSemaphore, nullptr);
            
            frame._deletionQueue.flush();
        }
        
        for (auto &mesh : _testMeshes) {
            destroy_buffer(mesh->meshBuffers.indexBuffer);
            destroy_buffer(mesh->meshBuffers.vertexBuffer);
        }
        
        _mainDeletionQueue.flush();
        
        for (auto &semaphore : _submitSemaphores) {
            vkDestroySemaphore(_device, semaphore, nullptr);
        }
        _submitSemaphores.clear();
        
        destroy_swapchain();
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);
        
        vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
        
        vkDestroyInstance(_instance, nullptr);
        SDL_DestroyWindow(_window);
    }
    
    // clear engine pointer
    loadedEngine = nullptr;
}

FrameData &VulkanEngine::get_current_frame() {
    return _frames[_frameNumber % FRAME_OVERLAP];
}

void GLTFMetallic_Roughness::build_pipelines(VulkanEngine* engine)
{
    VkShaderModule meshVertShader;
    if (!vkutil::load_shader_module("shaders/mesh.vert.spv", engine->_device, &meshVertShader)){
        spdlog::error("Failed to build the mesh vertex shader module!");
    }
    VkShaderModule meshFragShader;
    if (!vkutil::load_shader_module("shaders/mesh.frag.spv", engine->_device, &meshFragShader)){
        spdlog::error("Failed to build the mesh fragment shader module!");
    }
    
    VkPushConstantRange matrixRange = {};
    matrixRange.offset = 0;
    matrixRange.size = sizeof(GPUDrawPushConstants);
    matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    DescriptorLayoutBuilder builder;
    builder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    
    _materialLayout = builder.build(engine->_device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    
    VkDescriptorSetLayout layouts[] = {
        engine->_gpuSceneDataDescriptorLayout,
        _materialLayout,
    };
    VkPipelineLayoutCreateInfo meshLayoutCI = vkinit::pipeline_layout_create_info();
    meshLayoutCI.setLayoutCount = 2;
    meshLayoutCI.pSetLayouts = layouts;
    meshLayoutCI.pushConstantRangeCount = 1;
    meshLayoutCI.pPushConstantRanges = &matrixRange;
    
    VkPipelineLayout newLayout;
    VK_CHECK(vkCreatePipelineLayout(engine->_device, &meshLayoutCI, nullptr, &newLayout));
    
    _opaquePipeline.pipelineLayout = newLayout;
    _transparentPipeline.pipelineLayout = newLayout;
    
    PipelineBuilder pipelineBuilder;
    pipelineBuilder.set_shaders(meshVertShader, meshFragShader);
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    pipelineBuilder.set_multisampling_none();
    pipelineBuilder.disable_blending();
    pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    pipelineBuilder.set_color_attachment_format(engine->_drawImage.imageFormat);
    pipelineBuilder.set_depth_format(engine->_depthImage.imageFormat);
    pipelineBuilder._pipelineLayout = newLayout;
    
    _opaquePipeline.pipeline = pipelineBuilder.build_pipeline(engine->_device);
    
    // transparent variant with blending
    pipelineBuilder.enable_blending_additive();
    pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    
    _transparentPipeline.pipeline = pipelineBuilder.build_pipeline(engine->_device);
    
    // cleanup
    vkDestroyShaderModule(engine->_device, meshVertShader, nullptr);
    vkDestroyShaderModule(engine->_device, meshFragShader, nullptr);
}

MaterialInstance GLTFMetallic_Roughness::write_material(VkDevice device,
                                                        MaterialPass pass,
                                                        const MaterialResources& resources,
                                                        DescriptorAllocatorGrowable& desciptorAllocator)
{
    MaterialInstance matData;
    matData.passType = pass;
    if (pass == MaterialPass::GLTF_PBR_TRANSPARENT) {
        matData.pipeline = &_transparentPipeline;
    } else {
        matData.pipeline = &_opaquePipeline;
    }
    matData.materialSet = desciptorAllocator.allocate(device, _materialLayout);
    
    _writer.clear();
    _writer.write_buffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    _writer.write_image(1, resources.colorImage.imageView, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    _writer.write_image(2, resources.metalRoughImage.imageView, resources.metalRoughSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    _writer.update_set(device, matData.materialSet);
    
    return matData;
}

void MeshNode::draw(const glm::mat4& topMatrix,
                    DrawContext& context)
{
    glm::mat4 nodeMatrix = topMatrix * this->worldTransform;
    
    for (auto s: this->mesh->surfaces) {
        RenderObject obj;
        obj.firstIndex = s.startIndex;
        obj.indexCount = s.count;
        obj.indexBuffer = this->mesh->meshBuffers.indexBuffer.buffer;
        obj.material = &s.material->data;
        obj.transform = nodeMatrix;
        obj.vertexBufferAddr = this->mesh->meshBuffers.vertexBufferAddress;
        obj.bounds = s.bounds;
        
        if (s.material->data.passType == MaterialPass::GLTF_PBR_TRANSPARENT)
        {
            context.transparentSurfaces.push_back(obj);
        }
        else
        {
            context.opaqueSurfaces.push_back(obj);
        }
    }
    
    Node::draw(topMatrix,context);
}

static bool
is_renderobj_visible(const RenderObject& obj,glm::mat4& viewproj)
{
    glm::vec3 corners[] = {
        glm::vec3 { 1, 1, 1 },
        glm::vec3 { 1, 1, -1 },
        glm::vec3 { 1, -1, 1 },
        glm::vec3 { 1, -1, -1 },
        glm::vec3 { -1, 1, 1 },
        glm::vec3 { -1, 1, -1 },
        glm::vec3 { -1, -1, 1 },
        glm::vec3 { -1, -1, -1 },
    };
    
    glm::mat4 matrix = viewproj * obj.transform;
    glm::vec3 min = {1.5, 1.5, 1.5};
    glm::vec3 max = {-1.5, -1.5, -1.5};
    
    // NOTE(champ): project each corner of the view frustum into clip space
    for (int i = 0; i < 8; i++)
    {
        glm::vec4 v = matrix * glm::vec4(obj.bounds.origin + (corners[i] * obj.bounds.extents), 1.0f);
        
        // perspective correction using the w component
        v.x /= v.w;
        v.y /= v.w;
        v.z /= v.w;
        
        min = glm::min(min, glm::vec3{v});
        max = glm::max(max, glm::vec3{v});
    }
    
    // NOTE(champ): check if the clip space box is within the view
    if (min.z > 1.f || max.z < 0.f || min.x > 1.f || max.x < -1.f || min.y > 1.f || max.y < -1.f)
    {
        return false;
    }
    else
    {
        return true;
    }
}

void
VulkanEngine::load_gltf_filepaths_in_folder(const std::string& directory)
{
    // NOTE(champ): This operation could be stored in a map so that we dont add
    // the paths again if they are already present
    for (const auto& entry: std::filesystem::directory_iterator(directory))
    {
        if (entry.is_regular_file())
        {
            std::string extension = entry.path().extension().string();
            if (extension == ".glb" || extension == ".gltf")
            {
                std::string relative_filepath = entry.path().string();;
                this->gltfFilesPath.push_back(relative_filepath);
            }
        }
    }
}
