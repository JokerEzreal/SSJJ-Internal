#pragma once

#include "mono_types.h"

namespace mono {

    // Load mono-2.0-bdwgc.dll (or mono.dll fallback) and resolve all exports.
    // Returns true if all critical functions were resolved.
    bool initialize();

    // Release the module handle (optional cleanup).
    void shutdown();

    // Domain
    extern mono_get_root_domain_fn       get_root_domain;
    extern mono_domain_get_fn            domain_get;
    extern mono_domain_assembly_open_fn  domain_assembly_open;

    // Thread
    extern mono_thread_attach_fn  thread_attach;
    extern mono_thread_detach_fn  thread_detach;

    // Assembly
    extern mono_assembly_foreach_fn        assembly_foreach;
    extern mono_assembly_get_image_fn      assembly_get_image;
    extern mono_assembly_loaded_fn         assembly_loaded;
    extern mono_assembly_load_from_full_fn assembly_load_from_full;

    // Image
    extern mono_image_get_name_fn             image_get_name;
    extern mono_image_open_from_data_full_fn  image_open_from_data_full;
    extern mono_image_close_fn                image_close;

    // Class
    extern mono_class_from_name_fn             class_from_name;
    extern mono_class_get_method_from_name_fn  class_get_method_from_name;
    extern mono_class_get_methods_fn           class_get_methods;
    extern mono_class_get_fields_fn            class_get_fields;
    extern mono_class_get_parent_fn            class_get_parent;
    extern mono_class_get_name_fn              class_get_name;
    extern mono_class_get_namespace_fn         class_get_namespace;

    // Method
    extern mono_method_get_name_fn   method_get_name;
    extern mono_method_full_name_fn  method_full_name;

    // Runtime
    extern mono_runtime_invoke_fn       runtime_invoke;
    extern mono_runtime_object_init_fn  runtime_object_init;

    // Object
    extern mono_object_new_fn         object_new;
    extern mono_object_unbox_fn       object_unbox;
    extern mono_object_get_class_fn   object_get_class;

    // String
    extern mono_string_new_fn        string_new;
    extern mono_string_new_utf16_fn  string_new_utf16;
    extern mono_string_to_utf8_fn    string_to_utf8;

    // Memory
    extern mono_free_fn              free_mem;

    // Internal call
    extern mono_add_internal_call_fn  add_internal_call;

    // Field
    extern mono_field_get_value_fn              field_get_value;
    extern mono_field_set_value_fn              field_set_value;
    extern mono_field_get_offset_fn             field_get_offset;
    extern mono_field_get_name_fn               field_get_name;
    extern mono_class_get_field_from_name_fn    class_get_field_from_name;

    // Type
    extern mono_type_get_type_fn  type_get_type;
    extern mono_type_is_byref_fn  type_is_byref;

    // JIT
    extern mono_compile_method_fn  compile_method;

    // GC handles
    extern mono_gchandle_new_fn         gchandle_new;
    extern mono_gchandle_free_fn        gchandle_free;
    extern mono_gchandle_get_target_fn  gchandle_get_target;

} // namespace mono
