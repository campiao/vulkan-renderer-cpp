#include "core/engine.h"


int main() {
    spdlog::info("Initializing Application!");
    VulkanEngine app;
    
#if defined _DEBUG
    constexpr bool useValidationLayers = true;
#else
    constexpr bool useValidationLayers = false;
#endif
    app.init(1024, 720, "Vulkan Engine", useValidationLayers);
    app.run();
    
    app.cleanup();
}
