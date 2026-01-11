target("hello")
    set_group("Executables")
    set_kind("binary")
    set_languages("c++17")
    add_rules("utils.glsl2spv", {outputdir="$(builddir)/$(plat)/$(arch)/$(mode)/shaders"})

    add_files("**.cpp")
    add_files( "../assets/shaders/*.comp","../assets/shaders/*.vert","../assets/shaders/*.frag")
    add_files("../external/imgui/*.cpp", "../external/stb/*.cpp")
    add_headerfiles("**.h")
    add_includedirs(".")
    add_includedirs("../external/vma/", "../external/stb", "../external/cgltf/")
    add_includedirs("../external/glm","../external/imgui")

    -- Set this definition to make GLM work with Vulkan
    -- Otherwise we can't see the meshes :(
    add_defines("GLM_FORCE_DEPTH_ZERO_TO_ONE")
    add_defines("GLM_ENABLE_EXPERIMENTAL")

    add_deps("SDL3", "Vulkan", "spdlog", "vkbootstrap")
    add_syslinks("user32")

    before_link(function (target)
        os.cp("binaries/*", "$(builddir)/$(plat)/$(arch)/$(mode)/")
        os.cp("assets/models/*", "$(builddir)/$(plat)/$(arch)/$(mode)/models/")
    end)
