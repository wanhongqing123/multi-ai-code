#pragma once

// OBS's OpenGL sources include glad/glad.h. Android links GLES directly;
// this compatibility header supplies the GLES declarations and constants.
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#define GLAD_GL_VERSION_3_3 0
#define GLAD_GL_VERSION_4_3 0
#define GLAD_GL_ARB_debug_output 0
#define GLAD_GL_EXT_texture_sRGB_decode 0
#define GLAD_GL_ARB_copy_image 0
#define GLAD_GL_NV_copy_image 0
#define GLAD_GL_EXT_texture_filter_anisotropic 0

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RG16
#define GL_RG16 0x822C
#endif
#ifndef GL_MIRROR_CLAMP_EXT
#define GL_MIRROR_CLAMP_EXT 0x8742
#endif
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#endif
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#endif
#ifndef GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

#define glClearDepth glClearDepthf
