#pragma once

#include "core/gltf_loader.h"
#include "core/types.h"
#include "vk_descriptors.h"
#include "core/camera.h"

constexpr u32 FRAME_OVERLAP = 2;

struct EngineStats
{
    float frame_time;
    int   triangle_count;
    int   drawcall_count;
    float scene_update_time;
    float mesh_draw_time;
};

struct FrameData {
    VkCommandPool _commandPool;
    VkCommandBuffer _mainCommandBuffer;
    
    VkSemaphore _swapchainSemaphore;
    VkFence _renderFence;
    
    DeletionQueue _deletionQueue;
    DescriptorAllocatorGrowable _frameDescriptors;
};

struct ComputePushConstancts {
    glm::vec4 data1;
    glm::vec4 data2;
    glm::vec4 data3;
    glm::vec4 data4;
};

struct ComputeEffect {
    const char *name;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    ComputePushConstancts data;
};

struct GLTFMetallic_Roughness {
    MaterialPipeline _opaquePipeline;
    MaterialPipeline _transparentPipeline;
    VkDescriptorSetLayout _materialLayout;
    
    struct MaterialConstants {
        glm::vec4 colorFactors;
        glm::vec4 metal_rough_factors;
        // pading to 256 bytes
        glm::vec4 extra[14];
    };
    struct MaterialResources {
        AllocatedImage colorImage;
        VkSampler colorSampler;
        AllocatedImage metalRoughImage;
        VkSampler metalRoughSampler;
        VkBuffer dataBuffer;
        u32 dataBufferOffset;
    };
    
    DescriptorWriter _writer;
    
    void build_pipelines(VulkanEngine* engine);
    void clear_resources(VkDevice device);
    MaterialInstance write_material(VkDevice device,
                                    MaterialPass pass,
                                    const MaterialResources& resources,
                                    DescriptorAllocatorGrowable& descriptorAllocator); 
};

struct RenderObject {
    u32 indexCount;
    u32 firstIndex;
    VkBuffer indexBuffer;
    MaterialInstance* material;
    glm::mat4 transform;
    VkDeviceAddress vertexBufferAddr;
    Bounds bounds;
};

struct DrawContext {
    std::vector<RenderObject> opaqueSurfaces;
    std::vector<RenderObject> transparentSurfaces;
};

struct MeshNode : public Node {
    std::shared_ptr<MeshAsset> mesh;
    
    virtual void draw(const glm::mat4& topMatrix, DrawContext& context) override;
};

static bool is_renderobj_visible(const RenderObject& obj,glm::mat4& viewproj);

struct VulkanEngine {
    static VulkanEngine &Get();
    void init(int w, int h, const char *title, bool useValidationLayers);
    void update_scene();
    void run();
    void draw();
    void draw_background(VkCommandBuffer cmd);
    void draw_geometry(VkCommandBuffer cmd);
    void draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView);
    void cleanup();
    
    // Vulkan resources initialization
    void init_vulkan();
    void init_swapchain();
    void init_commands();
    void init_sync_structures();
    void init_descriptors();
    void init_pipelines();
    void init_background_pipeline();
    void init_mesh_pipeline();
    void init_imgui();
    void init_default_data();
    void create_swapchain(u32 w, u32 h);
    void resize_swapchain();
    
    void destroy_swapchain();
    FrameData &get_current_frame();
    void immediate_submit(std::function<void(VkCommandBuffer cmd)> &&function);
    AllocatedBuffer create_buffer(size_t allocSize, 
                                  VkBufferUsageFlags usage,
                                  VmaMemoryUsage memoryUsage);
    void destroy_buffer(const AllocatedBuffer &buffer);
    GPUMeshBuffers upload_mesh(const std::vector<u32> &indices,
                               std::vector<Vertex> &vertices);
    
    AllocatedImage create_image(VkExtent3D size,
                                VkFormat format,
                                VkImageUsageFlags usage,
                                bool mipmapped = false);
    AllocatedImage create_image(void* data,
                                VkExtent3D size,
                                VkFormat format,
                                VkImageUsageFlags usage,
                                bool mipmapped = false);
    void destroy_image(const AllocatedImage& image);
    void load_gltf_filepaths_in_folder(const std::string& directory);
    
    
    bool _useValidationLayers = false;
    bool _isInitialized = false;
    bool _resizeRequested = false;
    int _frameNumber = 0;
    bool _stopRendering = false;
    VkExtent2D _windowExtent;
    struct SDL_Window *_window = nullptr;
    VmaAllocator _allocator;
    
    VkInstance _instance;
    VkDebugUtilsMessengerEXT _debugMessenger;
    VkPhysicalDevice _physicalDevice;
    VkDevice _device;
    VkSurfaceKHR _surface;
    VkSwapchainKHR _swapchain;
    VkFormat _swapchainImageFormat;
    std::vector<VkImage> _swapchainImages;
    std::vector<VkImageView> _swapchainImageViews;
    VkExtent2D _swapchainExtent;
    VkQueue _graphicsQueue;
    u32 _graphicsQueueFamily;
    
    FrameData _frames[FRAME_OVERLAP];
    // NOTE: This is clearly overkill and lowkey "wrong"
    // Could probably just use a normal array?
    std::vector<VkSemaphore> _submitSemaphores;
    AllocatedImage _drawImage;
    AllocatedImage _depthImage;
    VkExtent2D _drawExtent;
    float _renderScale = 1.0f;
    
    DescriptorAllocatorGrowable _globalDescriptorAllocator;
    VkDescriptorSet _drawImageDescriptors;
    VkDescriptorSetLayout _drawImageDescriptorLayout;
    VkDescriptorSetLayout _singleImageDescriptorLayout;
    
    
    VkPipelineLayout _gradientPipelineLayout;
    VkPipeline _meshPipeline;
    VkPipelineLayout _meshPipelineLayout;
    
    VkFence _immFence;
    VkCommandBuffer _immCommandBuffer;
    VkCommandPool _immCommandPool;
    
    std::vector<ComputeEffect> backgroundEffects;
    int currentBackgroundEffect = 0;
    
    std::vector<std::shared_ptr<MeshAsset>> _testMeshes;
    int _currentTestMesh = 2;
    float _currentRotationAngle = 0.0f;
    
    GPUSceneData _sceneData;
    VkDescriptorSetLayout _gpuSceneDataDescriptorLayout;
    AllocatedImage _whiteImage;
    AllocatedImage _blackImage;
    AllocatedImage _greyImage;
    AllocatedImage _errorCheckboardImage;
    VkSampler _defaultSamplerlinear;
    VkSampler _defaulSamplerNearest;
    
    MaterialInstance _defaulMatData;
    GLTFMetallic_Roughness metalRoughMat;
    
    DrawContext _mainDrawContext;
    std::unordered_map<std::string, std::shared_ptr<Node>> loadedNodes;
    std::unordered_map<std::string, std::shared_ptr<gltf::LoadedScene>> loadedScenes;
    std::vector<std::string> gltfFilesPath;
    Camera mainCamera;
    
    EngineStats stats;
    
    DeletionQueue _mainDeletionQueue;
};
