#pragma  once

#include "types.h"
#include "SDL3/SDL_events.h"

struct Camera {
    glm::vec3 velocity;
    glm::vec3 position;
    glm::vec3 world_up = {0.0f, 1.0f, 0.0f}; 
    float pitch = 0;
    float yaw   = 0;
    bool isMouseLocked = false;
    bool is_pitch_locked = true;
    bool is_yaw_restricted = true;
    std::string follow_target;
};

namespace camera{
    glm::mat4 getViewMatrix(Camera* camera);
    glm::mat4 getRotationMatrix(Camera* camera);
    void processSDLEvent(Camera* camera, SDL_Event& e);
    void update(Camera* camera);
}
