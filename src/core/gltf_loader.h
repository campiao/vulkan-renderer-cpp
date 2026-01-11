#pragma once

#include "core/vk_descriptors.h"
#include <core/types.h>
#include <memory>
#include <optional>
#include <unordered_map>

class VulkanEngine;

struct GLTFMaterial {
    MaterialInstance data;
};

// NOTE(champ): Oriented bouding boxes for frustum culling
// sphere_radius exists so that I can use other algorithms if I want
struct Bounds
{
    glm::vec3 origin;
    float sphere_radius;
    glm::vec3 extents;
};

struct GeoSurface {
    u32 startIndex;
    u32 count;
    std::shared_ptr<GLTFMaterial> material;
    Bounds bounds;
};

struct MeshAsset {
    std::string name;
    
    std::vector<GeoSurface> surfaces;
    GPUMeshBuffers meshBuffers;
};

namespace gltf {
    
    std::optional<std::vector<std::shared_ptr<MeshAsset>>>
        loadMeshes(VulkanEngine *engine, const char *filePath);
    
    struct LoadedScene : public IRenderable {
        std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
        std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
        std::unordered_map<std::string, AllocatedImage> images;
        std::unordered_map<std::string, std::shared_ptr<GLTFMaterial>> materials;
        std::vector<std::shared_ptr<Node>> top_nodes;
        std::vector<VkSampler> samplers;
        
        DescriptorAllocatorGrowable descriptor_pool;
        AllocatedBuffer material_data_buffer;
        VulkanEngine* creator;
        
        ~LoadedScene() { clear_all(); } 
        virtual void draw(const glm::mat4& top_matrix, DrawContext& ctx);
        void clear_all();
    };
    
    std::optional<std::shared_ptr<LoadedScene>> load_scene_from_file(VulkanEngine* engine, std::string_view path);
    
}
