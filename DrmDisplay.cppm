module;
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

export module DrmDisplay;

import std;  

export class DrmScreen {
private:
    int drm_fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0; 
    uint32_t fb_id = 0;
    uint32_t handle = 0;
    uint32_t crtc_id = 0;
    uint32_t* pixel_buffer = nullptr;
    size_t buffer_size = 0;
    drmModeCrtc* saved_crtc = nullptr;
    uint32_t connector_id = 0;
    drmModeModeInfo mode{};

    void cleanup() noexcept {
        if (pixel_buffer && pixel_buffer != MAP_FAILED) {
            ::munmap(pixel_buffer, buffer_size);
            pixel_buffer = nullptr;
        }
        if (drm_fd >= 0) {
            if (saved_crtc) {
                ::drmModeSetCrtc(drm_fd, crtc_id, saved_crtc->buffer_id, saved_crtc->x, saved_crtc->y, &connector_id, 1, &saved_crtc->mode);
                ::drmModeFreeCrtc(saved_crtc);
                saved_crtc = nullptr;
            }
            if (handle) {
                struct drm_mode_destroy_dumb destroy_req { .handle = handle };
                ::drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
                handle = 0;
            }
            if (fb_id) {
                ::drmModeRmFB(drm_fd, fb_id);
                fb_id = 0;
            }
            ::close(drm_fd);
            drm_fd = -1;
        }
    }

public:
    DrmScreen() = default;
    
    ~DrmScreen() {
        cleanup();
    }

    DrmScreen(const DrmScreen&) = delete;
    DrmScreen& operator=(const DrmScreen&) = delete;

    DrmScreen(DrmScreen&& other) noexcept { *this = std::move(other); }
    DrmScreen& operator=(DrmScreen&& other) noexcept {
        if (this != &other) {
            cleanup(); 

            drm_fd       = std::exchange(other.drm_fd, -1);
            width        = std::exchange(other.width, 0);       
            height       = std::exchange(other.height, 0);     
            pitch        = std::exchange(other.pitch, 0);       
            fb_id        = std::exchange(other.fb_id, 0);
            handle       = std::exchange(other.handle, 0);
            crtc_id      = std::exchange(other.crtc_id, 0);
            pixel_buffer = std::exchange(other.pixel_buffer, nullptr);
            buffer_size  = std::exchange(other.buffer_size, 0);
            saved_crtc   = std::exchange(other.saved_crtc, nullptr);
            connector_id = std::exchange(other.connector_id, 0);
            mode = other.mode;
            other.mode = {}; 
        }
        return *this;
    }

    bool init() {
        drm_fd = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (drm_fd < 0) return false;

        auto* resources = ::drmModeGetResources(drm_fd);
        if (!resources) { ::close(drm_fd); drm_fd = -1; return false; }

        drmModeConnector* connector = nullptr;
        for (int i = 0; i < resources->count_connectors; ++i) {
            connector = ::drmModeGetConnector(drm_fd, resources->connectors[i]);
            if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
                break; 
            }
            ::drmModeFreeConnector(connector);
            connector = nullptr;
        }

        if (!connector) {
            ::drmModeFreeResources(resources);
            ::close(drm_fd);
            drm_fd = -1;
            return false;
        }

        mode = connector->modes[0]; 
        width = mode.hdisplay;
        height = mode.vdisplay;
        connector_id = connector->connector_id;

        struct drm_mode_create_dumb create_req {};
        create_req.width = width;
        create_req.height = height;
        create_req.bpp = 32;

        if (::drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
            ::drmModeFreeConnector(connector);
            ::drmModeFreeResources(resources);
            cleanup();
            return false;
        }
        handle = create_req.handle;
        buffer_size = create_req.size;
        pitch = create_req.pitch; 

        if (::drmModeAddFB(drm_fd, width, height, 24, 32, pitch, handle, &fb_id) < 0) {
            ::drmModeFreeConnector(connector);
            ::drmModeFreeResources(resources);
            cleanup();
            return false;
        }

        struct drm_mode_map_dumb map_req {};
        map_req.handle = handle;
        if (::drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
            ::drmModeFreeConnector(connector);
            ::drmModeFreeResources(resources);
            cleanup();
            return false;
        }

        pixel_buffer = static_cast<uint32_t*>(::mmap(0, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map_req.offset));
        if (pixel_buffer == MAP_FAILED) {
            ::drmModeFreeConnector(connector);
            ::drmModeFreeResources(resources);
            cleanup();
            return false;
        }

        crtc_id = resources->crtcs[0];
        saved_crtc = ::drmModeGetCrtc(drm_fd, crtc_id);

        if (::drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode) < 0) {
            ::drmModeFreeConnector(connector);
            ::drmModeFreeResources(resources);
            cleanup();
            return false;
        }

        ::drmModeFreeConnector(connector);
        ::drmModeFreeResources(resources);
        return true;
    }

    void clear(uint32_t hex_color) {
        if (!pixel_buffer) return;
        std::fill_n(pixel_buffer, buffer_size / 4, hex_color);
    }

    void set_pixel(uint32_t x, uint32_t y, uint32_t hex_color) {
        if (!pixel_buffer || x >= width || y >= height) return;
        uint32_t pixel_index = (y * (pitch / 4)) + x;
        pixel_buffer[pixel_index] = hex_color;
    }

    std::pair<uint32_t, uint32_t> get_resolution() const {
        return {width, height};
    }
};
