target("Vulkan")
    set_kind("headeronly")
    set_group("Dependencies")

    -- NOTE: Is the line below needed? Is it even needed on other vendor packages?
    -- add_headerfiles(os.getenv("VULKAN_SDK") .."/Include/**.hpp")
    add_includedirs(os.getenv("VULKAN_SDK") .. "/Include", {public = true})
    add_linkdirs(os.getenv("VULKAN_SDK") .. "/Lib", {public = true})
    add_syslinks("vulkan-1.lib", {public = true})
