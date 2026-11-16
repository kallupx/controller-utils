#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <algorithm>
#include "button_glyphs.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <openvr.h>
#include "body_pose.h"
#include "interaction/pointer_math.h"
#include "mesh.h"
#include "model_renderer.h"
#include "interaction/pose_math.h"
#include "gaze.h"
#include "vr.h"
#include "ui/renderer.h"

namespace {
using ct::ui::ModelRenderer;

#include "dashboard.h"
#include "alignment.h"
#include "pointer.h"

struct Overlays {
    Vr &vr;
    vr::VROverlayHandle_t handle = 0;
    ct::Matrix head_from_eye[2];
    bool visible = false, geometry_checked = false, content_checked = false;
    float height_ratio = 1;
    explicit Overlays(Vr &runtime) : vr(runtime) {}
    ~Overlays() {
        if (handle) {
            vr.overlay->HideOverlay(handle);
            vr.overlay->ClearOverlayTexture(handle);
            vr.overlay->DestroyOverlay(handle);
        }
    }
    void initialize() {
        check(vr.overlay->CreateOverlay("controllerutils.body.0", "Steam gamepad body", &handle),
              "create");
        check(vr.overlay->SetOverlayFlag(handle, vr::VROverlayFlags_VisibleInDashboard, true),
              "dashboard visibility");
        check(vr.overlay->SetOverlaySortOrder(handle, 100), "display order");
        check(vr.overlay->SetOverlayAlpha(handle, 1), "model opacity");
        check(vr.overlay->SetOverlayWidthInMeters(handle, 1.6f), "quad width");
        check(vr.overlay->SetOverlayFlag(handle, vr::VROverlayFlags_SideBySide_Parallel, true),
              "stereo");
        for (int index = 0; index < 2; ++index) {
            head_from_eye[index] =
                matrix(vr.system->GetEyeToHeadTransform(static_cast<vr::EVREye>(index)));
            const auto identity = ct::identity();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(head_from_eye[index][c * 4 + r] - identity[c * 4 + r]) > .001f)
                        throw std::runtime_error(
                            "this stereo quad requires parallel head-space eye axes");
        }
    }
    void hide() {
        if (visible) {
            check(vr.overlay->HideOverlay(handle), "hide");
        }
        visible = false;
    }
    float quad_height() {
        vr::HmdMatrix34_t bottom{}, top{};
        check(vr.overlay->GetTransformForOverlayCoordinates(handle, vr::TrackingUniverseStanding,
                                                            {.5f, 0}, &bottom),
              "quad bottom");
        check(vr.overlay->GetTransformForOverlayCoordinates(handle, vr::TrackingUniverseStanding,
                                                            {.5f, 1}, &top),
              "quad top");
        float squared = 0;
        for (int i = 0; i < 3; ++i)
            squared += std::pow(top.m[i][3] - bottom.m[i][3], 2);
        const float result = std::sqrt(squared);
        if (!std::isfinite(result) || result < .02f || result > 3.3f)
            throw std::runtime_error("unsupported stereo quad height");
        return result;
    }
    void render(ModelRenderer &gpu, const ct::Matrix &head, const ct::BodyPose &pose) {
        auto quad = head;
        const auto center =
            ct::transform(ct::multiply(ct::inverse_rigid(head), pose.model), gpu.mesh_center);
        const std::array<ct::Point, 2> eyes{
            {{head_from_eye[0][12], head_from_eye[0][13], head_from_eye[0][14]},
             {head_from_eye[1][12], head_from_eye[1][13], head_from_eye[1][14]}}};
        const auto layout = ct::fit_model_quad(center, gpu.mesh_radius, eyes);
        if (!layout) {
            hide();
            return;
        }
        const float quad_width = layout->width;
        const auto position = ct::transform(head, layout->center);
        for (int i = 0; i < 3; ++i)
            quad[12 + i] = position[i];
        check(vr.overlay->SetOverlayWidthInMeters(handle, quad_width), "model crop width");
        for (int index = 0; index < 2; ++index) {
            const auto &eye = head_from_eye[index];
            const auto projection = ct::quad_projection(
                quad_width, quad_width * height_ratio, .6f,
                {eye[12] - layout->center[0], eye[13] - layout->center[1], eye[14]});
            gpu.render(projection, ct::inverse_rigid(ct::multiply(head, eye)), pose.model, index);
        }
        const auto transform = matrix34(quad);
        check(vr.overlay->SetOverlayTransformAbsolute(handle, vr::TrackingUniverseStanding,
                                                      &transform),
              "quad transform");
        constexpr int width = 2 * ModelRenderer::Size;
        if (!content_checked) {
            std::vector<unsigned char> pixels(width * ModelRenderer::Size * 4);
            glReadPixels(0, 0, width, ModelRenderer::Size, GL_RGBA, GL_UNSIGNED_BYTE,
                         pixels.data());
            size_t covered = 0;
            for (size_t i = 3; i < pixels.size(); i += 4)
                if (pixels[i])
                    ++covered;
            if (covered < 1000)
                throw std::runtime_error("submitted GPU texture contains no visible model");
            content_checked = true;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const vr::Texture_t texture = {
            reinterpret_cast<void *>(static_cast<uintptr_t>(gpu.textures[0])),
            vr::TextureType_OpenGL, vr::ColorSpace_Auto};
        check(vr.overlay->SetOverlayTexture(handle, &texture), "GPU texture");
        if (!geometry_checked) {
            const float initial = quad_height();
            check(vr.overlay->SetOverlayTexelAspect(handle, initial / quad_width),
                  "square stereo quad");
            const float height = quad_height();
            height_ratio = height / quad_width;
            if (std::abs(height_ratio - 1) > .01f)
                throw std::runtime_error("stereo quad dimensions did not match the render frustum");
            geometry_checked = true;
        }
        if (!visible) {
            check(vr.overlay->ShowOverlay(handle), "show");
        }
        vr::VREvent_t event{};
        for (int count = 0;
             count < 64 && vr.overlay->PollNextOverlayEvent(handle, &event, sizeof(event));
             ++count) {
        }
        visible = true;
    }
};

struct Scene {
    Vr &vr;
    Overlays overlays;
    Dashboard dashboard;
    ControllerPointer pointer;
    AlignmentOverlay alignment;
    std::optional<ct::Matrix> origin;
    ct::BodyPose pose;
    bool active = false, running = true;
    double last_origin_attempt = 0;
    Scene(Vr &runtime, ct::ui::EventSink events, const std::filesystem::path &root)
        : vr(runtime), overlays(vr), dashboard(vr.overlay, std::move(events), root),
          pointer(vr, dashboard), alignment(dashboard) {
        overlays.initialize();
        dashboard.initialize();
        try {
            origin = matrix(vr.system->GetRawZeroPoseToStandingAbsoluteTrackingPose());
        } catch (const std::exception &) {
        }
    }
    void accept(const ct::ui::Command &command) {
        if (dashboard.accept(command) || pointer.accept(command))
            return;
        if (alignment.accept(command)) {
            if (alignment.active && (alignment.state == "connecting" || alignment.state == "palm" ||
                                     alignment.state == "stabilizing"))
                active = false;
            return;
        }
        if (const auto next = std::get_if<ct::BodyPose>(&command)) {
            if (next->optical_time < pose.optical_time ||
                next->position_time <= pose.position_time ||
                next->orientation_time <= pose.orientation_time)
                throw std::runtime_error("unordered renderer timestamps");
            pose = *next;
            active = true;
            return;
        }
        if (const auto op = std::get_if<ct::ui::Operation>(&command)) {
            if (*op == ct::ui::Operation::Hide) {
                alignment.active = active = false;
                return;
            }
            if (*op == ct::ui::Operation::Quit) {
                running = active = false;
                return;
            }
        }
        throw std::runtime_error("unsupported renderer command");
    }
    void frame(ModelRenderer &gpu) {
        vr::VREvent_t event{};
        for (int count = 0; count < 64 && vr.system->PollNextEvent(&event, sizeof(event));
             ++count) {
            if (event.eventType == vr::VREvent_Quit) {
                running = false;
                dashboard.emit(ct::ui::Quit{});
            }
            if (event.eventType == vr::VREvent_StandingZeroPoseReset ||
                event.eventType == vr::VREvent_SeatedZeroPoseReset ||
                event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                try {
                    const auto next =
                        matrix(vr.system->GetRawZeroPoseToStandingAbsoluteTrackingPose());
                    if (origin) {
                        const auto delta = ct::multiply(next, ct::inverse_rigid(*origin));
                        float difference = 0;
                        for (int i = 0; i < 16; ++i)
                            difference = std::max(difference, std::abs(next[i] - (*origin)[i]));
                        if (difference > 1e-5f) {
                            active = false;
                            pointer.set_enabled(false);
                            ct::ui::OriginChanged changed;
                            for (int i = 0; i < 12; ++i)
                                changed.transform[i] = delta[(i % 4) * 4 + i / 4];
                            dashboard.emit(changed);
                        }
                    }
                    origin = next;
                } catch (const std::exception &) {
                    active = false;
                    origin.reset();
                }
            }
        }
        const auto head = vr.head();
        if (!origin && native_time() - last_origin_attempt >= 1) {
            last_origin_attempt = native_time();
            try {
                origin = matrix(vr.system->GetRawZeroPoseToStandingAbsoluteTrackingPose());
            } catch (const std::exception &) {
            }
        }
        dashboard.events();
        dashboard.render(head.has_value());
        const auto display_pose = pose.in_standing(origin.value_or(ct::identity()));
        bool valid = active && (!pose.raw || origin.has_value()) && pose.fresh(native_time(), 1.) &&
                     head.has_value();
        if (valid) {
            float distance = 0;
            for (int i = 0; i < 3; ++i)
                distance += std::pow(display_pose.model[12 + i] - (*head)[12 + i], 2);
            valid = distance >= .05f * .05f && distance <= 1.5f * 1.5f;
        }
        gpu.buttons.update(pointer.buttons, pointer.triggers, pointer.left_stick, pointer.stick,
                           pointer.input_time, native_time(), running && valid);
        gpu.touchpads.update(pointer.buttons, pointer.left, pointer.right, pointer.input_time,
                             native_time(), running && valid);
        if (running && valid)
            overlays.render(gpu, *head, display_pose);
        else
            overlays.hide();
        alignment.render(head.has_value(), native_time());
        pointer.update(head);
    }
};

void initialize_gpu(ModelRenderer &gpu, const std::filesystem::path &path, bool stereo) {
    std::ifstream source(path, std::ios::binary);
    if (!source)
        throw std::runtime_error("cannot open the controller mesh");
    auto vertices = ct::read_mesh(source);
    auto buttons_path = path;
    buttons_path.replace_extension(".buttons");
    if (std::filesystem::exists(buttons_path)) {
        std::ifstream buttons_file(buttons_path, std::ios::binary);
        gpu.buttons.load(buttons_file, vertices);
    }
    std::optional<ct::Surface> appearance;
    auto appearance_path = path;
    appearance_path.replace_extension(".surface");
    if (std::filesystem::exists(appearance_path)) {
        std::ifstream appearance_file(appearance_path, std::ios::binary);
        appearance = ct::read_surface(appearance_file, vertices);
    }
    gpu.initialize(vertices, stereo, appearance ? &*appearance : nullptr);
}

} // namespace

namespace ct::ui {
class Renderer::Impl {
  public:
    ModelRenderer gpu;
    Vr vr;
    Presentation &presentation;
    std::unique_ptr<Scene> scene;
    Impl(Runtime &runtime, Presentation &channel, const std::filesystem::path &root)
        : vr(runtime), presentation(channel) {
        initialize_gpu(gpu, root / "assets/controller.stl", true);
        scene = std::make_unique<Scene>(
            vr, [&channel](Event event) { channel.emit(std::move(event)); }, root);
    }
    void run() {
        while (presentation.running() && scene->running) {
            const auto next_frame =
                std::chrono::steady_clock::now() + std::chrono::microseconds(11111);
            for (const auto &command : presentation.commands()) {
                scene->accept(command);
                if (!scene->running)
                    break;
            }
            if (!scene->running || !presentation.running())
                break;
            scene->frame(gpu);
            std::this_thread::sleep_until(next_frame);
        }
        scene->overlays.hide();
    }
};
Renderer::Renderer(Runtime &runtime, Presentation &presentation, const std::filesystem::path &root)
    : implementation_(std::make_unique<Impl>(runtime, presentation, root)) {}
Renderer::~Renderer() = default;
void Renderer::run() {
    implementation_->run();
}
} // namespace ct::ui
