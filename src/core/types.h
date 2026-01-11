#pragma once

#include "spdlog/spdlog.h"
#include <cstdint>
#include <deque>
#include <string>

#include "vk_mem_alloc.h"
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.h>

#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include "glm/vec4.hpp"


using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

static_assert(sizeof(i8) == 1, "int8 size incorrect.");
static_assert(sizeof(i16) == 2, "int16 size incorrect.");
static_assert(sizeof(i32) == 4, "int32 size incorrect.");
static_assert(sizeof(i64) == 8, "int64 size incorrect.");
static_assert(sizeof(u8) == 1, "uint8 size incorrect.");
static_assert(sizeof(u16) == 2, "uint16 size incorrect.");
static_assert(sizeof(u32) == 4, "uint32 size incorrect.");
static_assert(sizeof(u64) == 8, "uint64 size incorrect.");

#define VK_CHECK(x)                                                            \
do {                                                                         \
VkResult err = x;                                                          \
if (err) {                                                                 \
spdlog::error("Detected Vulkan error: {}", string_VkResult(err));        \
abort();                                                                 \
}                                                                          \
} while (0)

// NOTE: This approach does not scale!
// But for now it works since we have very few objects
struct DeletionQueue {
    std::deque<std::function<void()>> deletors;
    
    void push_function(std::function<void()> &&function) {
        deletors.push_back(function);
    }
    
    void flush() {
        // reverse iterate the deletion queue to execute all the functions
        for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
            (*it)(); // call functors
        }
        
        deletors.clear();
    }
};

struct AllocatedImage {
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
};

struct AllocatedBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
};

struct Vertex {
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

// holds the resources needed for a mesh
struct GPUMeshBuffers {
    
    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
};

// push constants for our mesh object draws
struct GPUDrawPushConstants {
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
};

struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 viewproj;
    glm::vec4 ambientColor;
    glm::vec4 sunlightDirection;
    glm::vec4 sunlightColor;
};

enum class MaterialPass: u8 {
    GLTF_PBR_OPAQUE = 0,
    GLTF_PBR_TRANSPARENT,
    OTHER,
};

struct MaterialPipeline {
    VkPipeline       pipeline;
    VkPipelineLayout pipelineLayout;
};

struct MaterialInstance{
    MaterialPipeline* pipeline;
    VkDescriptorSet   materialSet;
    MaterialPass      passType;
};


struct DrawContext;

// NOTE: These structs and corresponding draw functions handles drawing by adding
// the object to a DrawContext which is then drawn when "full"
struct IRenderable {
    virtual void draw(const glm::mat4& topMatrix, DrawContext& context) = 0; 
};

struct Node : public IRenderable {
    
    std::weak_ptr<Node> parent;
    std::vector<std::shared_ptr<Node>> children;
    
    glm::mat4 localTransform;
    glm::mat4 worldTransform;
    
    void refresh_transform(const glm::mat4& parentMatrix)
    {
        worldTransform = parentMatrix * localTransform;
        for (auto c: children) {
            c->refresh_transform(worldTransform);
        }
    }
    
    virtual void draw(const glm::mat4& topMatrix, DrawContext& context)
    {
        for (auto& c: children){
            c->draw(topMatrix, context);
        }
    }
};



