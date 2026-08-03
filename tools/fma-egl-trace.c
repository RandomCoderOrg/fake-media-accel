#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

struct image_record {
    EGLImageKHR image;
    int width;
    int height;
    uint32_t fourcc;
};

static struct image_record records[128];
static unsigned next_record;
static __eglMustCastToProperFunctionPointerType (*real_get_proc)(const char *);
static PFNEGLCREATEIMAGEKHRPROC real_create_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC real_image_target;
static EGLBoolean (*real_make_current)(EGLDisplay, EGLSurface, EGLSurface,
                                       EGLContext);
static unsigned make_current_calls;

__attribute__((constructor)) static void trace_loaded(void) {
    fprintf(stderr, "fma-egl-trace: loaded\n");
}

EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw,
                          EGLSurface read, EGLContext context) {
    if (!real_make_current)
        real_make_current = dlsym(RTLD_NEXT, "eglMakeCurrent");
    if (!real_make_current)
        return EGL_FALSE;
    EGLBoolean result = real_make_current(display, draw, read, context);
    ++make_current_calls;
    if (result != EGL_TRUE || make_current_calls <= 12 ||
        make_current_calls % 120 == 0)
        fprintf(stderr,
                "fma-egl-trace: make-current call=%u context=%p result=%u "
                "error=0x%x\n",
                make_current_calls, (void *)context, result,
                result == EGL_TRUE ? EGL_SUCCESS : eglGetError());
    return result;
}

static EGLint attribute(const EGLint *attributes, EGLint key,
                        EGLint fallback) {
    if (!attributes)
        return fallback;
    for (const EGLint *item = attributes; item[0] != EGL_NONE; item += 2)
        if (item[0] == key)
            return item[1];
    return fallback;
}

static struct image_record *find_record(EGLImageKHR image) {
    for (unsigned i = 0; i < sizeof(records) / sizeof(records[0]); ++i)
        if (records[i].image == image)
            return &records[i];
    return NULL;
}

static EGLImageKHR trace_create_image(EGLDisplay display, EGLContext context,
                                      EGLenum target, EGLClientBuffer buffer,
                                      const EGLint *attributes) {
    EGLImageKHR image = real_create_image(display, context, target, buffer,
                                          attributes);
    int width = attribute(attributes, EGL_WIDTH, 0);
    int height = attribute(attributes, EGL_HEIGHT, 0);
    uint32_t fourcc = (uint32_t)attribute(
        attributes, EGL_LINUX_DRM_FOURCC_EXT, 0);
    int fd = attribute(attributes, EGL_DMA_BUF_PLANE0_FD_EXT, -1);
    int offset = attribute(attributes, EGL_DMA_BUF_PLANE0_OFFSET_EXT, -1);
    int pitch = attribute(attributes, EGL_DMA_BUF_PLANE0_PITCH_EXT, -1);
    struct image_record *record = &records[
        next_record++ % (sizeof(records) / sizeof(records[0]))];
    *record = (struct image_record){image, width, height, fourcc};
    fprintf(stderr,
            "fma-egl-trace: create image=%p size=%dx%d fourcc=%c%c%c%c "
            "fd=%d offset=%d pitch=%d\n",
            (void *)image, width, height, fourcc & 0xff,
            (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff,
            (fourcc >> 24) & 0xff, fd, offset, pitch);
    return image;
}

static void trace_image_target(GLenum target, GLeglImageOES image) {
    real_image_target(target, image);
    struct image_record *record = find_record((EGLImageKHR)image);
    if (!record || target != GL_TEXTURE_2D || record->width <= 0 ||
        record->height <= 0)
        return;

    GLint texture = 0;
    GLint width = 0;
    GLint height = 0;
    GLint internal = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / height / 4)
        return;

    size_t bytes = (size_t)width * height * 4;
    uint8_t *pixels = malloc(bytes);
    if (!pixels)
        return;
    glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    size_t center = ((size_t)(height / 2) * width + width / 2) * 4;
    fprintf(stderr,
            "fma-egl-trace: bind image=%p texture=%d gl=%dx%d internal=0x%x "
            "first=%u,%u,%u,%u center=%u,%u,%u,%u\n",
            (void *)image, texture, width, height, internal, pixels[0],
            pixels[1], pixels[2], pixels[3], pixels[center],
            pixels[center + 1], pixels[center + 2], pixels[center + 3]);
    free(pixels);
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(
    const char *name) {
    if (!real_get_proc)
        real_get_proc = dlsym(RTLD_NEXT, "eglGetProcAddress");
    if (!real_get_proc)
        return NULL;
    if (strcmp(name, "eglCreateImageKHR") == 0) {
        if (!real_create_image)
            real_create_image = (PFNEGLCREATEIMAGEKHRPROC)real_get_proc(name);
        return (__eglMustCastToProperFunctionPointerType)trace_create_image;
    }
    if (strcmp(name, "glEGLImageTargetTexture2DOES") == 0) {
        if (!real_image_target)
            real_image_target =
                (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)real_get_proc(name);
        return (__eglMustCastToProperFunctionPointerType)trace_image_target;
    }
    return real_get_proc(name);
}
