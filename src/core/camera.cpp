#include "camera.h"
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>

glm::mat4 camera::getViewMatrix(Camera* camera)
{
    glm::mat4 camera_translation = glm::translate(glm::mat4{1.0f}, camera->position);
    glm::mat4 camera_rotation = getRotationMatrix(camera);
    return glm::inverse(camera_translation * camera_rotation);
}

glm::mat4 camera::getRotationMatrix(Camera* camera)
{
    glm::quat pitchRotation = glm::angleAxis(camera->pitch, glm::vec3 { 1.f, 0.f, 0.f });
    glm::quat yawRotation = glm::angleAxis(camera->yaw, glm::vec3 { 0.f, -1.f, 0.f });
    return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);  
}

void camera::processSDLEvent(Camera* camera, SDL_Event& e)
{
    //TODO: Improve movement
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_W) { camera->velocity.z = -1; }
        if (e.key.key == SDLK_S) { camera->velocity.z = 1; }
        if (e.key.key == SDLK_A) { camera->velocity.x = -1; }
        if (e.key.key == SDLK_D) { camera->velocity.x = 1; }
        if (e.key.key == SDLK_E) { camera->velocity.y = 1;}
        if (e.key.key == SDLK_Q) { camera->velocity.y = -1;}
    }
    
    if (e.type == SDL_EVENT_KEY_UP) {
        if (e.key.key == SDLK_W) { camera->velocity.z = 0; }
        if (e.key.key == SDLK_S) { camera->velocity.z = 0; }
        if (e.key.key == SDLK_A) { camera->velocity.x = 0; }
        if (e.key.key == SDLK_D) { camera->velocity.x = 0; }
        if (e.key.key == SDLK_E) { camera->velocity.y = 0; }
        if (e.key.key == SDLK_Q) { camera->velocity.y = 0;}
    }
    
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (e.button.button == SDL_BUTTON_RIGHT) 
        {
            camera->isMouseLocked = !camera->isMouseLocked;
            bool cursor_is_visible = SDL_CursorVisible();
            if (camera->isMouseLocked  && !cursor_is_visible)
            {
                SDL_ShowCursor();
            }
            else if (!camera->isMouseLocked && cursor_is_visible)
            {
                SDL_HideCursor();
            }
        }
    }
    
    if (!camera->isMouseLocked){
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            // NOTE(champ): Clamp yaw and pitch values if rotation is restriced
            camera->yaw += (float)e.motion.xrel / 200.f;
            camera->pitch -= (float)e.motion.yrel / 200.f;
        }
    }
}

void camera::update(Camera* camera)
{
    glm::mat4 rotation = camera::getRotationMatrix(camera);
    camera->position += glm::vec3(rotation * glm::vec4(camera->velocity * 0.5f, 0.0f));
}
