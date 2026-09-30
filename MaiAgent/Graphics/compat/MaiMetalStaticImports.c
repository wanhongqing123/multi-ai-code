#include "../graphics-internal.h"

// iOS links the OBS core and Metal backend into one static archive.
// Metal gs_* entry points have a private prefix to avoid colliding with
// the public wrappers in graphics.c.
#define MAI_IMPORT(field, symbol)                  \
    do {                                           \
        extern __typeof__(*exports->field) symbol; \
        exports->field = symbol;                   \
    } while (false)

bool load_graphics_imports(struct gs_exports* exports, void* module, const char* module_name) {
    bool success = true;
    (void)module;
    (void)module_name;

    MAI_IMPORT(device_get_name, device_get_name);
    exports->gpu_get_driver_version = NULL;
    exports->gpu_get_renderer = NULL;
    exports->gpu_get_dmem = NULL;
    exports->gpu_get_smem = NULL;
    MAI_IMPORT(device_get_type, device_get_type);
    exports->device_enum_adapters = NULL;
    MAI_IMPORT(device_preprocessor_name, device_preprocessor_name);
    MAI_IMPORT(device_create, device_create);
    MAI_IMPORT(device_destroy, device_destroy);
    MAI_IMPORT(device_enter_context, device_enter_context);
    MAI_IMPORT(device_leave_context, device_leave_context);
    MAI_IMPORT(device_get_device_obj, device_get_device_obj);
    MAI_IMPORT(device_swapchain_create, device_swapchain_create);
    MAI_IMPORT(device_resize, device_resize);
    MAI_IMPORT(device_get_color_space, device_get_color_space);
    MAI_IMPORT(device_update_color_space, device_update_color_space);
    MAI_IMPORT(device_get_size, device_get_size);
    MAI_IMPORT(device_get_width, device_get_width);
    MAI_IMPORT(device_get_height, device_get_height);
    MAI_IMPORT(device_texture_create, device_texture_create);
    MAI_IMPORT(device_cubetexture_create, device_cubetexture_create);
    MAI_IMPORT(device_voltexture_create, device_voltexture_create);
    MAI_IMPORT(device_zstencil_create, device_zstencil_create);
    MAI_IMPORT(device_stagesurface_create, device_stagesurface_create);
    MAI_IMPORT(device_samplerstate_create, device_samplerstate_create);
    MAI_IMPORT(device_vertexshader_create, device_vertexshader_create);
    MAI_IMPORT(device_pixelshader_create, device_pixelshader_create);
    MAI_IMPORT(device_vertexbuffer_create, device_vertexbuffer_create);
    MAI_IMPORT(device_indexbuffer_create, device_indexbuffer_create);
    MAI_IMPORT(device_timer_create, device_timer_create);
    MAI_IMPORT(device_timer_range_create, device_timer_range_create);
    MAI_IMPORT(device_get_texture_type, device_get_texture_type);
    MAI_IMPORT(device_load_vertexbuffer, device_load_vertexbuffer);
    MAI_IMPORT(device_load_indexbuffer, device_load_indexbuffer);
    MAI_IMPORT(device_load_texture, device_load_texture);
    MAI_IMPORT(device_load_samplerstate, device_load_samplerstate);
    MAI_IMPORT(device_load_vertexshader, device_load_vertexshader);
    MAI_IMPORT(device_load_pixelshader, device_load_pixelshader);
    MAI_IMPORT(device_load_default_samplerstate, device_load_default_samplerstate);
    MAI_IMPORT(device_get_vertex_shader, device_get_vertex_shader);
    MAI_IMPORT(device_get_pixel_shader, device_get_pixel_shader);
    MAI_IMPORT(device_get_render_target, device_get_render_target);
    MAI_IMPORT(device_get_zstencil_target, device_get_zstencil_target);
    MAI_IMPORT(device_set_render_target, device_set_render_target);
    MAI_IMPORT(device_set_render_target_with_color_space,
               device_set_render_target_with_color_space);
    MAI_IMPORT(device_set_cube_render_target, device_set_cube_render_target);
    MAI_IMPORT(device_enable_framebuffer_srgb, device_enable_framebuffer_srgb);
    MAI_IMPORT(device_framebuffer_srgb_enabled, device_framebuffer_srgb_enabled);
    MAI_IMPORT(device_copy_texture_region, device_copy_texture_region);
    MAI_IMPORT(device_copy_texture, device_copy_texture);
    MAI_IMPORT(device_stage_texture, device_stage_texture);
    MAI_IMPORT(device_begin_frame, device_begin_frame);
    MAI_IMPORT(device_begin_scene, device_begin_scene);
    MAI_IMPORT(device_draw, device_draw);
    MAI_IMPORT(device_load_swapchain, device_load_swapchain);
    MAI_IMPORT(device_end_scene, device_end_scene);
    MAI_IMPORT(device_clear, device_clear);
    MAI_IMPORT(device_is_present_ready, device_is_present_ready);
    MAI_IMPORT(device_present, device_present);
    MAI_IMPORT(device_flush, device_flush);
    MAI_IMPORT(device_set_cull_mode, device_set_cull_mode);
    MAI_IMPORT(device_get_cull_mode, device_get_cull_mode);
    MAI_IMPORT(device_enable_blending, device_enable_blending);
    MAI_IMPORT(device_enable_depth_test, device_enable_depth_test);
    MAI_IMPORT(device_enable_stencil_test, device_enable_stencil_test);
    MAI_IMPORT(device_enable_stencil_write, device_enable_stencil_write);
    MAI_IMPORT(device_enable_color, device_enable_color);
    MAI_IMPORT(device_blend_function, device_blend_function);
    MAI_IMPORT(device_blend_function_separate, device_blend_function_separate);
    MAI_IMPORT(device_blend_op, device_blend_op);
    MAI_IMPORT(device_depth_function, device_depth_function);
    MAI_IMPORT(device_stencil_function, device_stencil_function);
    MAI_IMPORT(device_stencil_op, device_stencil_op);
    MAI_IMPORT(device_set_viewport, device_set_viewport);
    MAI_IMPORT(device_get_viewport, device_get_viewport);
    MAI_IMPORT(device_set_scissor_rect, device_set_scissor_rect);
    MAI_IMPORT(device_ortho, device_ortho);
    MAI_IMPORT(device_frustum, device_frustum);
    MAI_IMPORT(device_projection_push, device_projection_push);
    MAI_IMPORT(device_projection_pop, device_projection_pop);

    MAI_IMPORT(gs_swapchain_destroy, mai_metal_gs_swapchain_destroy);

    MAI_IMPORT(gs_texture_destroy, mai_metal_gs_texture_destroy);
    MAI_IMPORT(gs_texture_get_width, mai_metal_gs_texture_get_width);
    MAI_IMPORT(gs_texture_get_height, mai_metal_gs_texture_get_height);
    MAI_IMPORT(gs_texture_get_color_format, mai_metal_gs_texture_get_color_format);
    MAI_IMPORT(gs_texture_map, mai_metal_gs_texture_map);
    MAI_IMPORT(gs_texture_unmap, mai_metal_gs_texture_unmap);
    exports->gs_texture_is_rect = NULL;
    MAI_IMPORT(gs_texture_get_obj, mai_metal_gs_texture_get_obj);

    MAI_IMPORT(gs_cubetexture_destroy, mai_metal_gs_cubetexture_destroy);
    MAI_IMPORT(gs_cubetexture_get_size, mai_metal_gs_cubetexture_get_size);
    MAI_IMPORT(gs_cubetexture_get_color_format, mai_metal_gs_cubetexture_get_color_format);

    MAI_IMPORT(gs_voltexture_destroy, mai_metal_gs_voltexture_destroy);
    MAI_IMPORT(gs_voltexture_get_width, mai_metal_gs_voltexture_get_width);
    MAI_IMPORT(gs_voltexture_get_height, mai_metal_gs_voltexture_get_height);
    MAI_IMPORT(gs_voltexture_get_depth, mai_metal_gs_voltexture_get_depth);
    MAI_IMPORT(gs_voltexture_get_color_format, mai_metal_gs_voltexture_get_color_format);

    MAI_IMPORT(gs_stagesurface_destroy, mai_metal_gs_stagesurface_destroy);
    MAI_IMPORT(gs_stagesurface_get_width, mai_metal_gs_stagesurface_get_width);
    MAI_IMPORT(gs_stagesurface_get_height, mai_metal_gs_stagesurface_get_height);
    MAI_IMPORT(gs_stagesurface_get_color_format, mai_metal_gs_stagesurface_get_color_format);
    MAI_IMPORT(gs_stagesurface_map, mai_metal_gs_stagesurface_map);
    MAI_IMPORT(gs_stagesurface_unmap, mai_metal_gs_stagesurface_unmap);

    MAI_IMPORT(gs_zstencil_destroy, mai_metal_gs_zstencil_destroy);

    MAI_IMPORT(gs_samplerstate_destroy, mai_metal_gs_samplerstate_destroy);

    MAI_IMPORT(gs_vertexbuffer_destroy, mai_metal_gs_vertexbuffer_destroy);
    MAI_IMPORT(gs_vertexbuffer_flush, mai_metal_gs_vertexbuffer_flush);
    MAI_IMPORT(gs_vertexbuffer_flush_direct, mai_metal_gs_vertexbuffer_flush_direct);
    MAI_IMPORT(gs_vertexbuffer_get_data, mai_metal_gs_vertexbuffer_get_data);

    MAI_IMPORT(gs_indexbuffer_destroy, mai_metal_gs_indexbuffer_destroy);
    MAI_IMPORT(gs_indexbuffer_flush, mai_metal_gs_indexbuffer_flush);
    MAI_IMPORT(gs_indexbuffer_flush_direct, mai_metal_gs_indexbuffer_flush_direct);
    MAI_IMPORT(gs_indexbuffer_get_data, mai_metal_gs_indexbuffer_get_data);
    MAI_IMPORT(gs_indexbuffer_get_num_indices, mai_metal_gs_indexbuffer_get_num_indices);
    MAI_IMPORT(gs_indexbuffer_get_type, mai_metal_gs_indexbuffer_get_type);

    MAI_IMPORT(gs_timer_destroy, mai_metal_gs_timer_destroy);
    MAI_IMPORT(gs_timer_begin, mai_metal_gs_timer_begin);
    MAI_IMPORT(gs_timer_end, mai_metal_gs_timer_end);
    MAI_IMPORT(gs_timer_get_data, mai_metal_gs_timer_get_data);
    MAI_IMPORT(gs_timer_range_destroy, mai_metal_gs_timer_range_destroy);
    MAI_IMPORT(gs_timer_range_begin, mai_metal_gs_timer_range_begin);
    MAI_IMPORT(gs_timer_range_end, mai_metal_gs_timer_range_end);
    MAI_IMPORT(gs_timer_range_get_data, mai_metal_gs_timer_range_get_data);

    MAI_IMPORT(gs_shader_destroy, mai_metal_gs_shader_destroy);
    MAI_IMPORT(gs_shader_get_num_params, mai_metal_gs_shader_get_num_params);
    MAI_IMPORT(gs_shader_get_param_by_idx, mai_metal_gs_shader_get_param_by_idx);
    MAI_IMPORT(gs_shader_get_param_by_name, mai_metal_gs_shader_get_param_by_name);
    MAI_IMPORT(gs_shader_get_viewproj_matrix, mai_metal_gs_shader_get_viewproj_matrix);
    MAI_IMPORT(gs_shader_get_world_matrix, mai_metal_gs_shader_get_world_matrix);
    MAI_IMPORT(gs_shader_get_param_info, mai_metal_gs_shader_get_param_info);
    MAI_IMPORT(gs_shader_set_bool, mai_metal_gs_shader_set_bool);
    MAI_IMPORT(gs_shader_set_float, mai_metal_gs_shader_set_float);
    MAI_IMPORT(gs_shader_set_int, mai_metal_gs_shader_set_int);
    MAI_IMPORT(gs_shader_set_matrix3, mai_metal_gs_shader_set_matrix3);
    MAI_IMPORT(gs_shader_set_matrix4, mai_metal_gs_shader_set_matrix4);
    MAI_IMPORT(gs_shader_set_vec2, mai_metal_gs_shader_set_vec2);
    MAI_IMPORT(gs_shader_set_vec3, mai_metal_gs_shader_set_vec3);
    MAI_IMPORT(gs_shader_set_vec4, mai_metal_gs_shader_set_vec4);
    MAI_IMPORT(gs_shader_set_texture, mai_metal_gs_shader_set_texture);
    MAI_IMPORT(gs_shader_set_val, mai_metal_gs_shader_set_val);
    MAI_IMPORT(gs_shader_set_default, mai_metal_gs_shader_set_default);
    MAI_IMPORT(gs_shader_set_next_sampler, mai_metal_gs_shader_set_next_sampler);

    exports->device_nv12_available = NULL;
    exports->device_p010_available = NULL;
    exports->device_texture_create_nv12 = NULL;
    exports->device_texture_create_p010 = NULL;

    MAI_IMPORT(device_is_monitor_hdr, device_is_monitor_hdr);

    MAI_IMPORT(device_debug_marker_begin, device_debug_marker_begin);
    MAI_IMPORT(device_debug_marker_end, device_debug_marker_end);

    exports->gs_get_adapter_count = NULL;

    /* OSX/Cocoa specific functions */
#ifdef __APPLE__
    MAI_IMPORT(device_shared_texture_available, device_shared_texture_available);
    MAI_IMPORT(device_texture_open_shared, device_texture_open_shared);
    MAI_IMPORT(device_texture_create_from_iosurface, device_texture_create_from_iosurface);
    MAI_IMPORT(gs_texture_rebind_iosurface, mai_metal_gs_texture_rebind_iosurface);

    /* win32 specific functions */
#elif _WIN32
    MAI_IMPORT(device_gdi_texture_available, device_gdi_texture_available);
    MAI_IMPORT(device_shared_texture_available, device_shared_texture_available);
    exports->device_get_duplicator_monitor_info = NULL;
    exports->device_duplicator_get_monitor_index = NULL;
    exports->device_duplicator_create = NULL;
    exports->gs_duplicator_destroy = NULL;
    exports->gs_duplicator_update_frame = NULL;
    exports->gs_duplicator_get_texture = NULL;
    exports->gs_duplicator_get_color_space = NULL;
    exports->gs_duplicator_get_sdr_white_level = NULL;
    exports->device_can_adapter_fast_clear = NULL;
    exports->device_texture_create_gdi = NULL;
    exports->gs_texture_get_dc = NULL;
    exports->gs_texture_release_dc = NULL;
    exports->device_texture_open_shared = NULL;
    exports->device_texture_open_nt_shared = NULL;
    exports->device_texture_get_shared_handle = NULL;
    exports->device_texture_wrap_obj = NULL;
    exports->device_texture_acquire_sync = NULL;
    exports->device_texture_release_sync = NULL;
    exports->device_stagesurface_create_nv12 = NULL;
    exports->device_stagesurface_create_p010 = NULL;
    exports->device_register_loss_callbacks = NULL;
    exports->device_unregister_loss_callbacks = NULL;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)
    MAI_IMPORT(device_texture_create_from_dmabuf, device_texture_create_from_dmabuf);
    MAI_IMPORT(device_query_dmabuf_capabilities, device_query_dmabuf_capabilities);
    MAI_IMPORT(device_query_dmabuf_modifiers_for_format, device_query_dmabuf_modifiers_for_format);
    MAI_IMPORT(device_texture_create_from_pixmap, device_texture_create_from_pixmap);
    MAI_IMPORT(device_query_sync_capabilities, device_query_sync_capabilities);
    MAI_IMPORT(device_sync_create, device_sync_create);
    MAI_IMPORT(device_sync_create_from_syncobj_timeline_point,
               device_sync_create_from_syncobj_timeline_point);
    MAI_IMPORT(device_sync_destroy, device_sync_destroy);
    MAI_IMPORT(device_sync_export_syncobj_timeline_point,
               device_sync_export_syncobj_timeline_point);
    MAI_IMPORT(device_sync_signal_syncobj_timeline_point,
               device_sync_signal_syncobj_timeline_point);
    MAI_IMPORT(device_sync_wait, device_sync_wait);
#endif

    return success;
}
