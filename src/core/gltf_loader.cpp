#include "core/gltf_loader.h"

#include "engine.h"
#include "stb_image.h"
#include "types.h"
#include "vk_initializers.h"

//#define STB_IMAGE_IMPLEMENTATION
//#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <glm/gtx/quaternion.hpp>

static VkFilter extract_filter(cgltf_filter_type filter);
static VkSamplerMipmapMode extract_mipmap_mode(cgltf_filter_type filter);
static std::optional<AllocatedImage> gltf_load_image(VulkanEngine* engine, cgltf_image* image);

std::optional<std::shared_ptr<gltf::LoadedScene>>
gltf::load_scene_from_file(VulkanEngine* engine, 
                           std::string_view path)
{
    spdlog::info("Loading GLTF file at: {}", path);

    std::shared_ptr<LoadedScene> scene = std::make_shared<LoadedScene>();
    scene->creator = engine;
    LoadedScene& file = *scene.get();

    cgltf_options opts = {};
    cgltf_data* data = NULL;
    cgltf_result result = cgltf_parse_file(&opts, path.data(), &data);
    if (result != cgltf_result_success) {
        spdlog::error("Failed to load GTLF file at: {}!", path);
        return {};
    }
    result = cgltf_load_buffers(&opts, data, path.data());
    if (result != cgltf_result_success) {
        spdlog::error("Failed to load buffers!");
        cgltf_free(data);
        return {};
    }

    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}
    };
    file.descriptor_pool.init(engine->_device, data->materials_count, sizes);

    for (int i = 0; i < data->samplers_count; i++) {
        auto sampler = data->samplers[i];

        VkSamplerCreateInfo sampler_CI = {};
        sampler_CI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_CI.pNext = nullptr;
        sampler_CI.maxLod = VK_LOD_CLAMP_NONE;
        sampler_CI.minLod = 0;
        sampler_CI.magFilter = extract_filter(sampler.mag_filter);
        sampler_CI.minFilter = extract_filter(sampler.min_filter);
        sampler_CI.mipmapMode = extract_mipmap_mode(sampler.min_filter);

        VkSampler new_sampler = {0};
        vkCreateSampler(engine->_device, &sampler_CI, nullptr, &new_sampler);

        file.samplers.push_back(new_sampler);
    }

    // temporal arrays for all the objects to use while creating the GLTF data
    std::vector<std::shared_ptr<MeshAsset>> meshes;
    std::vector<std::shared_ptr<Node>> nodes;
    std::vector<AllocatedImage> images;
    std::vector<std::shared_ptr<GLTFMaterial>> materials;

    // @SECTION: load all textures
    u32 unnamed_image_count = 0;
    for (int i = 0; i < data->textures_count; i++)
    {
        const cgltf_texture* texture = &data->textures[i];
        cgltf_image* image = texture->image;
        std::optional<AllocatedImage> new_image = gltf_load_image(engine, image);

        if (new_image.has_value())
        {
            images.push_back(*new_image);
            if (image->name)
            {
                file.images[image->name] = *new_image;
            }
            else
            {
                char str[20];
                snprintf(str, sizeof(str), "unnamed_%d", unnamed_image_count);
                file.images[str] = *new_image;
                unnamed_image_count += 1;
            }
        }
        else
        {
            images.push_back(engine->_errorCheckboardImage);
        }

    }

    // @SECTION: load all materials
    file.material_data_buffer = engine->create_buffer(sizeof(GLTFMetallic_Roughness::MaterialConstants) * data->materials_count,
                                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                      VMA_MEMORY_USAGE_CPU_TO_GPU);
    int data_index = 0;
    GLTFMetallic_Roughness::MaterialConstants* scene_material_constants = (GLTFMetallic_Roughness::MaterialConstants*)file.material_data_buffer.info.pMappedData;

    for (int i = 0; i < data->materials_count; i++)
    {
        cgltf_material material = data->materials[i];

        std::shared_ptr<GLTFMaterial> new_material = std::make_shared<GLTFMaterial>();
        materials.push_back(new_material);
        file.materials[material.name] = new_material;

        GLTFMetallic_Roughness::MaterialConstants constants;
        constants.colorFactors.x = material.pbr_metallic_roughness.base_color_factor[0];
        constants.colorFactors.y = material.pbr_metallic_roughness.base_color_factor[1];
        constants.colorFactors.z = material.pbr_metallic_roughness.base_color_factor[2];
        constants.colorFactors.w = material.pbr_metallic_roughness.base_color_factor[3];
        constants.metal_rough_factors.x = material.pbr_metallic_roughness.metallic_factor;
        constants.metal_rough_factors.y = material.pbr_metallic_roughness.roughness_factor;

        // write constants to buffer
        scene_material_constants[data_index] = constants;

        MaterialPass pass_type = MaterialPass::GLTF_PBR_OPAQUE;
        if (material.alpha_mode == cgltf_alpha_mode_blend)
        {
            if (material.name)
                pass_type = MaterialPass::GLTF_PBR_TRANSPARENT;
        }
        GLTFMetallic_Roughness::MaterialResources resources = {0};
        resources.colorImage = engine->_whiteImage;
        resources.colorSampler = engine->_defaultSamplerlinear;
        resources.metalRoughImage = engine->_whiteImage;
        resources.metalRoughSampler = engine->_defaultSamplerlinear;

        resources.dataBuffer = file.material_data_buffer.buffer;
        resources.dataBufferOffset = data_index * sizeof(GLTFMetallic_Roughness::MaterialConstants);

        // grab textures from file
        auto texture = material.pbr_metallic_roughness.base_color_texture.texture;
        if (texture) {
            cgltf_size texture_index = (cgltf_size)(texture - data->textures);
            cgltf_size sampler_index = (cgltf_size)(texture->sampler - data->samplers);
            resources.colorImage = images[texture_index];
            resources.colorSampler = file.samplers[sampler_index];
        }

        new_material->data = engine->metalRoughMat.write_material(engine->_device, pass_type, resources, file.descriptor_pool);

        data_index += 1;
    }

    // @SECTION: load all meshes
    std::vector<u32> indices;
    std::vector<Vertex> vertices;

    for (cgltf_size i = 0; i < data->meshes_count; i++) {
        const cgltf_mesh *mesh = &data->meshes[i];
        std::shared_ptr<MeshAsset> new_mesh = std::make_shared<MeshAsset>();
        meshes.push_back(new_mesh);
        file.meshes[mesh->name] = new_mesh;
        new_mesh->name = mesh->name;

        // clear the mesh arrays for each mesh
        indices.clear();
        vertices.clear();

        for (cgltf_size prim_idx = 0; prim_idx < mesh->primitives_count;
             prim_idx++) {
            const cgltf_primitive *prim = &mesh->primitives[prim_idx];
            GeoSurface newSurface = {0};
            newSurface.startIndex = (u32)indices.size();
            newSurface.count = (u32)prim->indices->count;

            size_t initial_vtx = vertices.size();

            // load indicies
            indices.reserve(newSurface.startIndex + newSurface.count);
            for (cgltf_size index_idx = 0; index_idx < newSurface.count;
                 index_idx++) {
                const u32 idx =
                (u32)cgltf_accessor_read_index(prim->indices, index_idx);
                indices.push_back(idx + initial_vtx);
            }

            cgltf_accessor* position_accessor = {};
            cgltf_accessor* normal_accessor = {};
            cgltf_accessor* uv_accessor = {};
            cgltf_accessor* color_accessor = {};

            for (cgltf_size i = 0; i < prim->attributes_count; i++) {
                const cgltf_attribute att = prim->attributes[i];
                switch (att.type) {
                    case cgltf_attribute_type_position: {
                        position_accessor = att.data;
                    } break;
                    case cgltf_attribute_type_normal: {
                        normal_accessor = att.data;
                    } break;

                    case cgltf_attribute_type_texcoord: {
                        uv_accessor = att.data;
                    } break;
                    case cgltf_attribute_type_color: {
                        color_accessor = att.data;
                    } break;
                    default: {
                    }
                }
            }
            vertices.resize(vertices.size() + position_accessor->count);

            for (cgltf_size i = 0; i < position_accessor->count; i++) {
                Vertex newVertex = {};
                newVertex.normal = {1, 0, 0};
                newVertex.color = glm::vec4{1.0f};
                newVertex.uv_x = 0;
                newVertex.uv_y = 0;

                // load vertex positions
                cgltf_float pos[3] = {};
                cgltf_accessor_read_float(position_accessor, i, pos, 3);
                newVertex.position.x = pos[0];
                newVertex.position.y = pos[1];
                newVertex.position.z = pos[2];

                // load vertex normals
                if (normal_accessor != nullptr) {
                    cgltf_float normal[3] = {};
                    cgltf_accessor_read_float(normal_accessor, i, normal, 3);
                    newVertex.normal.x = normal[0];
                    newVertex.normal.y = normal[1];
                    newVertex.normal.z = normal[2];
                }

                // load vertex uvs
                if (uv_accessor != nullptr) {
                    cgltf_float uvs[2] = {};
                    cgltf_accessor_read_float(uv_accessor, i, uvs, 2);
                    newVertex.uv_x = uvs[0];
                    newVertex.uv_y = uvs[1];
                }

                // load vertex colors;
                if (color_accessor != nullptr) {
                    cgltf_float colors[4] = {};
                    cgltf_accessor_read_float(color_accessor, i, colors, 4);
                    newVertex.color.x = colors[0];
                    newVertex.color.y = colors[1];
                    newVertex.color.z = colors[2];
                    newVertex.color.w = colors[3];
                }

                // vertices.push_back(newVertex);

                vertices[initial_vtx + i] = newVertex;
            }

            if (prim->material)
            {
                cgltf_size material_index = (cgltf_size)(prim->material - data->materials);
                newSurface.material = materials[material_index];
            }
            else
            {
                newSurface.material = materials[0];
            }
            // NOTE(champ): min and max vertex for bounds
            glm::vec3 min_pos = vertices[initial_vtx].position;
            glm::vec3 max_pos = vertices[initial_vtx].position;
            // NOTE(champ): this seems ineficient to loop through the vertices array again
            for (int i = 0; i < vertices.size(); i ++ )
            {
                min_pos = glm::min(min_pos, vertices[i].position);
                max_pos = glm::max(max_pos, vertices[i].position);
            }

            // calculate origin and extents from the min/max
            newSurface.bounds.origin =  (max_pos + min_pos) / 2.0f;
            newSurface.bounds.extents = (max_pos - min_pos) / 2.0f;
            newSurface.bounds.sphere_radius = glm::length(newSurface.bounds.extents);

            new_mesh->surfaces.push_back(newSurface);
        }
        new_mesh->meshBuffers = engine->upload_mesh(indices, vertices);
    }




    // @SECTION: load all nodes
    for (int i = 0; i < data->nodes_count; i++)
    {
        const cgltf_node* node = &data->nodes[i];
        std::shared_ptr<Node> new_node;
        if (node->mesh)
        {
            new_node = std::make_shared<MeshNode>();
            cgltf_size mesh_index = (cgltf_size)(node->mesh - data->meshes);
            assert((mesh_index < data->meshes_count) && (mesh_index < meshes.size()));
            static_cast<MeshNode*>(new_node.get())->mesh = meshes[mesh_index];
        }
        else
        {
            new_node = std::make_shared<Node>();
        }

        nodes.push_back(new_node);
        file.nodes[node->name];

        // NOTE(champ): check if node transform is defined as a single matrix emcompassing
        // transformation + rotation + scaling
        // or if these components are separated
        if (node->has_matrix)
        {
            memcpy(&new_node->localTransform, node->matrix, sizeof(node->matrix));
        }
        else
        {
            glm::vec3 tl(node->translation[0], node->translation[1], node->translation[2]);
            glm::quat rot(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]);
            glm::vec3 sc(node->scale[0], node->scale[1], node->scale[2]);

            glm::mat4 tm = glm::translate(glm::mat4(1.0f), tl);
            glm::mat4 rm = glm::toMat4(rot);
            glm::mat4 sm = glm::scale(glm::mat4(1.0f), sc);

            new_node->localTransform = tm * rm * sm;
        }
    }

    for (int i = 0; i < data->nodes_count; i++)
    {
        cgltf_node& node = data->nodes[i];
        std::shared_ptr<Node>& scene_node = nodes[i];

        for (int j = 0; j < node.children_count; j++)
        {
            auto c = node.children[j];
            cgltf_size children_index = (cgltf_size)(c - data->nodes);
            assert((children_index < data->nodes_count) && (children_index < nodes.size()));
            scene_node->children.push_back(nodes[children_index]);
            nodes[children_index]->parent = scene_node;
        }
    }

    for (auto& node: nodes)
    {
        if (node->parent.lock() == nullptr)
        {
            file.top_nodes.push_back(node);
            node->refresh_transform(glm::mat4(1.0f));
        }
    }

    cgltf_free(data);
    return scene;
}

static VkFilter
extract_filter(cgltf_filter_type filter)
{
    switch (filter) {
        case cgltf_filter_type_linear:
        case cgltf_filter_type_linear_mipmap_linear:
        case cgltf_filter_type_linear_mipmap_nearest:
        {
            return VK_FILTER_LINEAR;
        }
        case cgltf_filter_type_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
        case cgltf_filter_type_nearest_mipmap_linear:
        default:
        {
            return VK_FILTER_NEAREST;
        }
    }
}

static VkSamplerMipmapMode
extract_mipmap_mode(cgltf_filter_type filter)
{
    switch (filter) {
        case cgltf_filter_type_nearest_mipmap_linear:
        case cgltf_filter_type_linear_mipmap_linear:
        {
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
        case cgltf_filter_type_linear_mipmap_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
        default:
        {
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
    }
}

void
gltf::LoadedScene::draw(const glm::mat4& top_matrix,
                        DrawContext& ctx)
{
    for (std::shared_ptr<Node>& node: top_nodes)
    {
        node->draw(top_matrix, ctx);
    }
}

void
gltf::LoadedScene::clear_all()
{
    VkDevice device = creator->_device;

    descriptor_pool.destroy_pools(device);
    creator->destroy_buffer(material_data_buffer);

    for (auto& [k,v]: meshes)
    {
        creator->destroy_buffer(v->meshBuffers.indexBuffer);
        creator->destroy_buffer(v->meshBuffers.vertexBuffer);
    }

    for (auto& [k, v] : images) {

        if (v.image == creator->_errorCheckboardImage.image) {
            //dont destroy the default images
            continue;
        }
        creator->destroy_image(v);
    }

	for (auto& sampler : samplers) {
		vkDestroySampler(device, sampler, nullptr);
    }
}

static std::optional<AllocatedImage>
gltf_load_image(VulkanEngine* engine,
                cgltf_image* image)
{
    AllocatedImage new_image = {0};
    int w, h, nr_channels;

    if (image->uri)
    {
        unsigned char* data = stbi_load(image->uri, &w, &h, &nr_channels, 4);
        if (data)
        {
            VkExtent3D image_size = {0};
            image_size.width = w;
            image_size.height = h;
            image_size.depth = 1;

            new_image = engine->create_image(data, image_size,
                                             VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
                                             true);
            stbi_image_free(data);
        }
    }
    else if (image->buffer_view)
    {
        cgltf_buffer_view* buffer_view = image->buffer_view;
        cgltf_buffer* buffer = buffer_view->buffer;

        if (buffer)
        {
            unsigned char* data = stbi_load_from_memory((unsigned char*)buffer->data + buffer_view->offset,
                                                        (int)buffer_view->size,
                                                        &w, &h, &nr_channels,4);
            if (data)
            {
                VkExtent3D image_size = {0};
                image_size.width = w;
                image_size.height = h;
                image_size.depth = 1;

                new_image = engine->create_image(data, image_size,
                                                 VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
                                                 true);
                stbi_image_free(data);
            }
        }
    }

    if (new_image.image == VK_NULL_HANDLE)
    {
        return {};
    }
    return new_image;
}

std::optional<std::vector<std::shared_ptr<MeshAsset>>>
gltf::loadMeshes(VulkanEngine *engine,
                 const char *filePath)
{
    spdlog::info("Loading GLTF file: {}", filePath);

    cgltf_options opts = {};
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&opts, filePath, &data);
    if (result != cgltf_result_success) {
        spdlog::error("Failed to load GTLF file at: {}!", filePath);
        return {};
    }
    result = cgltf_load_buffers(&opts, data, "model.gltf");
    if (result != cgltf_result_success) {
        spdlog::error("Failed to load buffers!");
        cgltf_free(data);
        return {};
    }
    std::vector<std::shared_ptr<MeshAsset>> meshes;
    // use the same vectors for all meshes
    std::vector<u32> indices;
    std::vector<Vertex> vertices;

    for (cgltf_size i = 0; i < data->meshes_count; i++) {
        const cgltf_mesh *mesh = &data->meshes[i];

        MeshAsset newMesh = {};
        newMesh.name = mesh->name;

        // clear the mesh arrays for each mesh
        indices.clear();
        vertices.clear();

        for (cgltf_size prim_idx = 0; prim_idx < mesh->primitives_count;
             prim_idx++) {
            const cgltf_primitive *prim = &mesh->primitives[prim_idx];
            GeoSurface newSurface = {};
            newSurface.startIndex = (u32)indices.size();
            newSurface.count = (u32)prim->indices->count;

            size_t initial_vtx = vertices.size();

            // load indicies
            indices.reserve(newSurface.startIndex + newSurface.count);
            for (cgltf_size index_idx = 0; index_idx < newSurface.count;
                 index_idx++) {
                const u32 idx =
                (u32)cgltf_accessor_read_index(prim->indices, index_idx);
                indices.push_back(idx + initial_vtx);
            }

            cgltf_accessor *position_accessor = {};
            cgltf_accessor *normal_accessor = {};
            cgltf_accessor *uv_accessor = {};
            cgltf_accessor *color_accessor = {};

            for (cgltf_size i = 0; i < prim->attributes_count; i++) {
                const cgltf_attribute att = prim->attributes[i];
                switch (att.type) {
                    case cgltf_attribute_type_position: {
                        position_accessor = att.data;
                    } break;
                    case cgltf_attribute_type_normal: {
                        normal_accessor = att.data;
                    } break;

                    case cgltf_attribute_type_texcoord: {
                        uv_accessor = att.data;
                    } break;
                    case cgltf_attribute_type_color: {
                        color_accessor = att.data;
                    } break;
                    default: {
                    }
                }
            }
            vertices.resize(vertices.size() + position_accessor->count);

            for (cgltf_size i = 0; i < position_accessor->count; i++) {
                Vertex newVertex = {};
                newVertex.normal = {1, 0, 0};
                newVertex.color = glm::vec4{1.0f};
                newVertex.uv_x = 0;
                newVertex.uv_y = 0;

                // load vertex positions
                cgltf_float pos[3] = {};
                cgltf_accessor_read_float(position_accessor, i, pos, 3);
                newVertex.position.x = pos[0];
                newVertex.position.y = pos[1];
                newVertex.position.z = pos[2];

                // load vertex normals
                if (normal_accessor != nullptr) {
                    cgltf_float normal[3] = {};
                    cgltf_accessor_read_float(normal_accessor, i, normal, 3);
                    newVertex.normal.x = normal[0];
                    newVertex.normal.y = normal[1];
                    newVertex.normal.z = normal[2];
                }

                // load vertex uvs
                if (uv_accessor != nullptr) {
                    cgltf_float uvs[2] = {};
                    cgltf_accessor_read_float(uv_accessor, i, uvs, 2);
                    newVertex.uv_x = uvs[0];
                    newVertex.uv_y = uvs[1];
                }

                // load vertex colors;
                if (color_accessor != nullptr) {
                    cgltf_float colors[4] = {};
                    cgltf_accessor_read_float(color_accessor, i, colors, 4);
                    newVertex.color.x = colors[0];
                    newVertex.color.y = colors[1];
                    newVertex.color.z = colors[2];
                    newVertex.color.w = colors[3];
                }

                // vertices.push_back(newVertex);
                vertices[initial_vtx + i] = newVertex;
            }

            newMesh.surfaces.push_back(newSurface);
        }

        constexpr bool overrideColors = false;
        if (overrideColors) {
            for (Vertex &vtx : vertices) {
                vtx.color = glm::vec4(vtx.normal, 1.0f);
            }
        }
        newMesh.meshBuffers = engine->upload_mesh(indices, vertices);
        meshes.emplace_back(std::make_shared<MeshAsset>(std::move(newMesh)));
    }

    cgltf_free(data);
    return meshes;
}
