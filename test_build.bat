@echo off

@rem This is an attemp to compile the project without relying on an external build system. As of now, it is slower than using xmake, probably would be faster if I used a unity build?

set warnings=/W0
set flags=/std:c++17 /utf-8 /nologo /EHsc /Zi

@rem setup includes so that it includes every library necessary
set includes=/Iexternal/SDL3/include /Iexternal/spdlog/include /Iexternal/vkbootstrap /Iexternal/vma/ /I%VULKAN_SDK%/Include/ /Iexternal/cgltf/ /Iexternal/glm/ /Iexternal/imgui/ /Iexternal/stb/ /Isrc/
@rem setup links for external libraries
set links=/link /LIBPATH:external/ /LIBPATH:%VULKAN_SDK%/Lib SDL3/lib/SDL3.lib spdlog/lib/spdlogd.lib vulkan-1.lib user32.lib
set sources=src/main.cpp src/core/camera.cpp src/core/engine.cpp src/core/gltf_loader.cpp src/core/vk_descriptors.cpp src/core/vk_images.cpp src/core/vk_initializers.cpp src/core/vk_pipelines.cpp external/vkbootstrap/VkBootstrap.cpp external/stb/stb_image.cpp external/imgui/imgui.cpp external/imgui/imgui_demo.cpp external/imgui/imgui_draw.cpp external/imgui/imgui_impl_sdl3.cpp external/imgui/imgui_impl_vulkan.cpp external/imgui/imgui_tables.cpp external/imgui/imgui_widgets.cpp
set defines=/DGLM_ENABLE_EXPERIMENTAL /DGLM_FORCE_DEPTH_ZERO_TO_ONE

echo Compiling on Windows using MSVC
cl %flags% %warnings% %defines% /Feout/main.exe %includes% %sources% %links%
