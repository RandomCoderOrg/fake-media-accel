#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_x11.h>

#define PROBE_DRM_FOURCC(a, b, c, d)                                          \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |           \
     ((uint32_t)(d) << 24))
#define PROBE_DRM_FORMAT_R8 PROBE_DRM_FOURCC('R', '8', ' ', ' ')
#define PROBE_DRM_FORMAT_GR88 PROBE_DRM_FOURCC('G', 'R', '8', '8')

#define CHECK(expression, message)                                             \
    do {                                                                       \
        if (!(expression)) {                                                   \
            fprintf(stderr, "probe failed: %s (%s:%d)\n", message, __FILE__, \
                    __LINE__);                                                 \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static bool has_extension(const char *extensions, const char *wanted) {
    size_t length = strlen(wanted);
    const char *match = extensions;
    while (match && (match = strstr(match, wanted))) {
        if ((match == extensions || match[-1] == ' ') &&
            (match[length] == '\0' || match[length] == ' '))
            return true;
        match += length;
    }
    return false;
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        fprintf(stderr, "shader compile failed: %.*s\n", (int)length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(bool desktop_gl) {
    static const char gles_vertex_source[] =
        "attribute vec2 position;\n"
        "varying vec2 coordinate;\n"
        "void main() {\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "  coordinate = position * 0.5 + 0.5;\n"
        "}\n";
    static const char gles_fragment_source[] =
        "precision mediump float;\n"
        "uniform sampler2D y_plane;\n"
        "uniform sampler2D uv_plane;\n"
        "varying vec2 coordinate;\n"
        "void main() {\n"
        "  float y = texture2D(y_plane, coordinate).r;\n"
        "  vec2 uv = texture2D(uv_plane, coordinate).rg;\n"
        "  gl_FragColor = vec4(y, uv, 1.0);\n"
        "}\n";
    static const char desktop_vertex_source[] =
        "#version 120\n"
        "attribute vec2 position;\n"
        "varying vec2 coordinate;\n"
        "void main() {\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "  coordinate = position * 0.5 + 0.5;\n"
        "}\n";
    static const char desktop_fragment_source[] =
        "#version 120\n"
        "uniform sampler2D y_plane;\n"
        "uniform sampler2D uv_plane;\n"
        "varying vec2 coordinate;\n"
        "void main() {\n"
        "  float y = texture2D(y_plane, coordinate).r;\n"
        "  vec2 uv = texture2D(uv_plane, coordinate).rg;\n"
        "  gl_FragColor = vec4(y, uv, 1.0);\n"
        "}\n";
    const char *vertex_source = desktop_gl ? desktop_vertex_source
                                            : gles_vertex_source;
    const char *fragment_source = desktop_gl ? desktop_fragment_source
                                              : gles_fragment_source;
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment)
        return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        fprintf(stderr, "program link failed: %.*s\n", (int)length, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static bool near_byte(uint8_t actual, uint8_t expected) {
    int difference = (int)actual - (int)expected;
    return difference >= -3 && difference <= 3;
}

static bool pixel_matches(const uint8_t pixel[4], uint8_t y, uint8_t u,
                          uint8_t v) {
    return near_byte(pixel[0], y) && near_byte(pixel[1], u) &&
           near_byte(pixel[2], v);
}

static VAStatus fill_nv12_image(VADisplay display, const VAImage *image,
                                unsigned width, unsigned height, uint8_t y,
                                uint8_t u, uint8_t v) {
    uint8_t *pixels = NULL;
    VAStatus status = vaMapBuffer(display, image->buf, (void **)&pixels);
    if (status != VA_STATUS_SUCCESS)
        return status;
    for (unsigned row = 0; row < height; ++row)
        memset(pixels + image->offsets[0] + row * image->pitches[0], y,
               width);
    for (unsigned row = 0; row < height / 2; ++row) {
        uint8_t *destination =
            pixels + image->offsets[1] + row * image->pitches[1];
        for (unsigned column = 0; column < width; column += 2) {
            destination[column] = u;
            destination[column + 1] = v;
        }
    }
    return vaUnmapBuffer(display, image->buf);
}

static bool sample_pixel(uint8_t pixel[4]) {
    memset(pixel, 0, 4);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    return glGetError() == GL_NO_ERROR;
}

static unsigned probe_dimension(const char *name, unsigned fallback) {
    const char *value = getenv(name);
    if (!value || !*value)
        return fallback;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (*end || parsed < 2 || parsed > 8192 || (parsed & 1))
        return fallback;
    return (unsigned)parsed;
}

int main(void) {
    const unsigned width = probe_dimension("FMA_PROBE_WIDTH", 64);
    const unsigned height = probe_dimension("FMA_PROBE_HEIGHT", 64);
    const uint8_t expected_y = 64;
    const uint8_t expected_u = 96;
    const uint8_t expected_v = 160;
    const char *desktop_value = getenv("FMA_PROBE_DESKTOP_GL");
    const bool desktop_gl = desktop_value && *desktop_value &&
                            strcmp(desktop_value, "0") != 0;

    Display *x11 = XOpenDisplay(NULL);
    CHECK(x11 != NULL, "XOpenDisplay");

    VADisplay va_display = vaGetDisplay(x11);
    CHECK(va_display != NULL, "vaGetDisplay");
    int va_major = 0;
    int va_minor = 0;
    CHECK(vaInitialize(va_display, &va_major, &va_minor) == VA_STATUS_SUCCESS,
          "vaInitialize");
    VAConfigAttrib config_attribute = {.type = VAConfigAttribRTFormat};
    CHECK(vaGetConfigAttributes(va_display, VAProfileH264High, VAEntrypointVLD,
                                &config_attribute, 1) == VA_STATUS_SUCCESS,
          "vaGetConfigAttributes");
    CHECK(config_attribute.value & VA_RT_FORMAT_YUV420,
          "YUV420 config support");
    VAConfigID config = VA_INVALID_ID;
    CHECK(vaCreateConfig(va_display, VAProfileH264High, VAEntrypointVLD,
                         &config_attribute, 1, &config) == VA_STATUS_SUCCESS,
          "vaCreateConfig");
    VASurfaceID surface = VA_INVALID_ID;
    CHECK(vaCreateSurfaces(va_display, VA_RT_FORMAT_YUV420, width, height,
                           &surface, 1, NULL, 0) == VA_STATUS_SUCCESS,
          "vaCreateSurfaces");

    VAImageFormat formats[8];
    int format_count = 0;
    CHECK(vaQueryImageFormats(va_display, formats, &format_count) ==
              VA_STATUS_SUCCESS,
          "vaQueryImageFormats");
    VAImageFormat *nv12 = NULL;
    for (int i = 0; i < format_count; ++i)
        if (formats[i].fourcc == VA_FOURCC_NV12) {
            nv12 = &formats[i];
            break;
        }
    CHECK(nv12 != NULL, "NV12 image format");
    VAImage image;
    CHECK(vaCreateImage(va_display, nv12, width, height, &image) ==
              VA_STATUS_SUCCESS,
          "vaCreateImage");
    CHECK(fill_nv12_image(va_display, &image, width, height, expected_y,
                          expected_u, expected_v) == VA_STATUS_SUCCESS,
          "fill initial NV12 image");
    CHECK(vaPutImage(va_display, surface, image.image_id, 0, 0, width, height,
                     0, 0, width, height) == VA_STATUS_SUCCESS,
          "vaPutImage");
    CHECK(vaSyncSurface(va_display, surface) == VA_STATUS_SUCCESS,
          "vaSyncSurface");

    VADRMPRIMESurfaceDescriptor prime;
    memset(&prime, 0, sizeof(prime));
    CHECK(vaExportSurfaceHandle(
              va_display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
              &prime) == VA_STATUS_SUCCESS,
          "vaExportSurfaceHandle");
    CHECK(prime.num_objects == 1 && prime.num_layers == 1 &&
              prime.layers[0].num_planes == 2,
          "composed NV12 descriptor");

    EGLDisplay egl_display = eglGetDisplay((EGLNativeDisplayType)x11);
    CHECK(egl_display != EGL_NO_DISPLAY, "eglGetDisplay");
    EGLint egl_major = 0;
    EGLint egl_minor = 0;
    CHECK(eglInitialize(egl_display, &egl_major, &egl_minor), "eglInitialize");
    const char *extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
    CHECK(extensions && has_extension(extensions, "EGL_EXT_image_dma_buf_import"),
          "EGL_EXT_image_dma_buf_import");
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_to_texture =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    CHECK(create_image && destroy_image && image_to_texture,
          "EGL image entrypoints");

    EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, desktop_gl ? EGL_OPENGL_BIT : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig egl_config;
    EGLint config_count = 0;
    CHECK(eglChooseConfig(egl_display, config_attributes, &egl_config, 1,
                          &config_count) && config_count == 1,
          "eglChooseConfig");
    EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface pbuffer =
        eglCreatePbufferSurface(egl_display, egl_config, pbuffer_attributes);
    CHECK(pbuffer != EGL_NO_SURFACE, "eglCreatePbufferSurface");
    EGLint gles_context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                        EGL_NONE};
    EGLint desktop_context_attributes[] = {EGL_NONE};
    const EGLint *context_attributes = desktop_gl
        ? desktop_context_attributes : gles_context_attributes;
    CHECK(eglBindAPI(desktop_gl ? EGL_OPENGL_API : EGL_OPENGL_ES_API),
          "eglBindAPI");
    EGLContext egl_context = eglCreateContext(
        egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);
    CHECK(egl_context != EGL_NO_CONTEXT, "eglCreateContext");
    CHECK(eglMakeCurrent(egl_display, pbuffer, pbuffer, egl_context),
          "eglMakeCurrent");

    const uint32_t y_offset = prime.layers[0].offset[0];
    const uint32_t uv_offset = prime.layers[0].offset[1];
    const uint32_t y_pitch = prime.layers[0].pitch[0];
    const uint32_t uv_pitch = prime.layers[0].pitch[1];
    EGLint y_attributes[] = {
        EGL_WIDTH, (EGLint)prime.width,
        EGL_HEIGHT, (EGLint)prime.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)PROBE_DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime.objects[0].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)y_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)y_pitch,
        EGL_NONE,
    };
    EGLint uv_attributes[] = {
        EGL_WIDTH, (EGLint)((prime.width + 1) / 2),
        EGL_HEIGHT, (EGLint)((prime.height + 1) / 2),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)PROBE_DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime.objects[0].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)uv_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)uv_pitch,
        EGL_NONE,
    };
    EGLImageKHR egl_images[2] = {
        create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                     y_attributes),
        create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                     uv_attributes),
    };
    CHECK(egl_images[0] != EGL_NO_IMAGE_KHR &&
              egl_images[1] != EGL_NO_IMAGE_KHR,
          "eglCreateImageKHR DMA-BUF import");

    GLuint textures[2];
    glGenTextures(2, textures);
    for (unsigned i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        image_to_texture(GL_TEXTURE_2D, egl_images[i]);
    }
    CHECK(glGetError() == GL_NO_ERROR, "glEGLImageTargetTexture2DOES");

    GLuint program = create_program(desktop_gl);
    CHECK(program != 0, "shader program");
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "y_plane"), 0);
    glUniform1i(glGetUniformLocation(program, "uv_plane"), 1);
    static const GLfloat vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f,
                                       -1.0f, 1.0f, 1.0f, 1.0f};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glViewport(0, 0, 1, 1);
    uint8_t result[4] = {0};
    CHECK(sample_pixel(result), "initial EGL sample and readback");
    CHECK(pixel_matches(result, expected_y, expected_u, expected_v),
          "initial imported NV12 plane values");

    /*
     * VLC creates and imports its VA surfaces before MediaCodec has produced
     * the frame that will occupy them. Keep the EGLImages and textures alive,
     * update the already-imported DMA-BUF through VA-API, then sample it again.
     * A static import can pass while this streaming lifecycle stays stale.
     */
    const uint8_t updated_y = 176;
    const uint8_t updated_u = 48;
    const uint8_t updated_v = 208;
    glFinish();
    CHECK(fill_nv12_image(va_display, &image, width, height, updated_y,
                          updated_u, updated_v) == VA_STATUS_SUCCESS,
          "fill updated NV12 image");
    CHECK(vaPutImage(va_display, surface, image.image_id, 0, 0, width, height,
                     0, 0, width, height) == VA_STATUS_SUCCESS,
          "update already-imported surface");
    CHECK(vaSyncSurface(va_display, surface) == VA_STATUS_SUCCESS,
          "sync updated surface");

    uint8_t updated_result[4] = {0};
    CHECK(sample_pixel(updated_result), "updated EGL sample and readback");
    bool coherent = pixel_matches(updated_result, updated_y, updated_u,
                                  updated_v);

    uint8_t rebound_result[4] = {0};
    if (!coherent) {
        for (unsigned i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            image_to_texture(GL_TEXTURE_2D, egl_images[i]);
        }
        CHECK(glGetError() == GL_NO_ERROR, "rebind updated EGLImages");
        CHECK(sample_pixel(rebound_result), "rebound EGL sample and readback");
    } else {
        memcpy(rebound_result, updated_result, sizeof(rebound_result));
    }
    CHECK(pixel_matches(rebound_result, updated_y, updated_u, updated_v),
          "post-import DMA-BUF update visibility");

    /*
     * VLC 3.x's vaapi_x11 GL converter predates vaExportSurfaceHandle(). It
     * derives a VAImage, acquires a legacy DRM_PRIME handle for its buffer,
     * and constructs one EGLImage per NV12 plane from the VAImage layout.
     * Exercise that exact compatibility contract independently of VLC.
     */
    VAImage derived;
    memset(&derived, 0, sizeof(derived));
    CHECK(vaDeriveImage(va_display, surface, &derived) == VA_STATUS_SUCCESS,
          "legacy vaDeriveImage");
    CHECK(derived.format.fourcc == VA_FOURCC_NV12 && derived.num_planes == 2,
          "legacy derived NV12 layout");
    VABufferInfo legacy_info;
    memset(&legacy_info, 0, sizeof(legacy_info));
    legacy_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    CHECK(vaAcquireBufferHandle(va_display, derived.buf, &legacy_info) ==
              VA_STATUS_SUCCESS,
          "legacy vaAcquireBufferHandle");
    CHECK(legacy_info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
          "legacy DRM PRIME memory type");

    const int legacy_fd = (int)(uintptr_t)legacy_info.handle;
    EGLint legacy_y_attributes[] = {
        EGL_WIDTH, (EGLint)derived.width,
        EGL_HEIGHT, (EGLint)derived.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)PROBE_DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, legacy_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)derived.offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)derived.pitches[0],
        EGL_NONE,
    };
    EGLint legacy_uv_attributes[] = {
        EGL_WIDTH, (EGLint)((derived.width + 1) / 2),
        EGL_HEIGHT, (EGLint)((derived.height + 1) / 2),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)PROBE_DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, legacy_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)derived.offsets[1],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)derived.pitches[1],
        EGL_NONE,
    };
    EGLImageKHR legacy_images[2] = {
        create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                     legacy_y_attributes),
        create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                     legacy_uv_attributes),
    };
    CHECK(legacy_images[0] != EGL_NO_IMAGE_KHR &&
              legacy_images[1] != EGL_NO_IMAGE_KHR,
          "legacy EGLImage DMA-BUF import");

    GLuint legacy_textures[2];
    glGenTextures(2, legacy_textures);
    for (unsigned i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, legacy_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        image_to_texture(GL_TEXTURE_2D, legacy_images[i]);
    }
    CHECK(glGetError() == GL_NO_ERROR,
          "legacy glEGLImageTargetTexture2DOES");
    uint8_t legacy_result[4] = {0};
    CHECK(sample_pixel(legacy_result), "legacy EGL sample and readback");
    CHECK(pixel_matches(legacy_result, updated_y, updated_u, updated_v),
          "legacy imported NV12 plane values");

    printf("VA->DRM PRIME->EGL passed: api=%s renderer=%s size=%ux%u "
           "initial=%u,%u,%u,%u "
           "updated=%u,%u,%u,%u coherent=%s rebind=%s "
           "legacy=%u,%u,%u,%u va=%d.%d "
           "egl=%d.%d\n",
           desktop_gl ? "OpenGL" : "OpenGL-ES", glGetString(GL_RENDERER),
           width, height, result[0], result[1],
           result[2], result[3],
           updated_result[0], updated_result[1], updated_result[2],
           updated_result[3], coherent ? "yes" : "no",
           coherent ? "not-needed" : "required", legacy_result[0],
           legacy_result[1], legacy_result[2], legacy_result[3], va_major,
           va_minor, egl_major, egl_minor);

    glDeleteTextures(2, legacy_textures);
    destroy_image(egl_display, legacy_images[1]);
    destroy_image(egl_display, legacy_images[0]);
    CHECK(vaReleaseBufferHandle(va_display, derived.buf) == VA_STATUS_SUCCESS,
          "legacy vaReleaseBufferHandle");
    CHECK(vaDestroyImage(va_display, derived.image_id) == VA_STATUS_SUCCESS,
          "legacy vaDestroyImage");
    glDeleteProgram(program);
    glDeleteTextures(2, textures);
    destroy_image(egl_display, egl_images[1]);
    destroy_image(egl_display, egl_images[0]);
    close(prime.objects[0].fd);
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl_display, egl_context);
    eglDestroySurface(egl_display, pbuffer);
    eglTerminate(egl_display);
    vaDestroyImage(va_display, image.image_id);
    vaDestroySurfaces(va_display, &surface, 1);
    vaDestroyConfig(va_display, config);
    vaTerminate(va_display);
    XCloseDisplay(x11);
    return 0;
}
