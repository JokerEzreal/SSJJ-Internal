#include "mono_api.h"
#include "mono_types.h"

namespace mono {

    // ========================================================================
    // Global function pointer definitions
    // ========================================================================

    // Domain
    mono_get_root_domain_fn       get_root_domain       = nullptr;
    mono_domain_get_fn            domain_get             = nullptr;
    mono_domain_assembly_open_fn  domain_assembly_open   = nullptr;

    // Thread
    mono_thread_attach_fn  thread_attach  = nullptr;
    mono_thread_detach_fn  thread_detach  = nullptr;

    // Assembly
    mono_assembly_foreach_fn        assembly_foreach        = nullptr;
    mono_assembly_get_image_fn      assembly_get_image      = nullptr;
    mono_assembly_loaded_fn         assembly_loaded         = nullptr;
    mono_assembly_load_from_full_fn assembly_load_from_full = nullptr;

    // Image
    mono_image_get_name_fn             image_get_name             = nullptr;
    mono_image_open_from_data_full_fn  image_open_from_data_full  = nullptr;
    mono_image_close_fn                image_close                = nullptr;

    // Class
    mono_class_from_name_fn             class_from_name             = nullptr;
    mono_class_get_method_from_name_fn  class_get_method_from_name  = nullptr;
    mono_class_get_methods_fn           class_get_methods           = nullptr;
    mono_class_get_fields_fn            class_get_fields            = nullptr;
    mono_class_get_parent_fn            class_get_parent            = nullptr;
    mono_class_get_name_fn              class_get_name              = nullptr;
    mono_class_get_namespace_fn         class_get_namespace         = nullptr;

    // Method
    mono_method_get_name_fn   method_get_name  = nullptr;
    mono_method_full_name_fn  method_full_name = nullptr;

    // Runtime
    mono_runtime_invoke_fn       runtime_invoke      = nullptr;
    mono_runtime_object_init_fn  runtime_object_init = nullptr;

    // Object
    mono_object_new_fn         object_new       = nullptr;
    mono_object_unbox_fn       object_unbox     = nullptr;
    mono_object_get_class_fn   object_get_class = nullptr;

    // String
    mono_string_new_fn        string_new       = nullptr;
    mono_string_new_utf16_fn  string_new_utf16 = nullptr;
    mono_string_to_utf8_fn    string_to_utf8   = nullptr;

    // Internal call
    mono_add_internal_call_fn  add_internal_call = nullptr;

    // Field
    mono_field_get_value_fn              field_get_value        = nullptr;
    mono_field_set_value_fn              field_set_value        = nullptr;
    mono_field_get_offset_fn             field_get_offset       = nullptr;
    mono_field_get_name_fn               field_get_name         = nullptr;
    mono_class_get_field_from_name_fn    class_get_field_from_name = nullptr;

    // Type
    mono_type_get_type_fn  type_get_type = nullptr;
    mono_type_is_byref_fn  type_is_byref = nullptr;

    // JIT
    mono_compile_method_fn  compile_method = nullptr;

    // GC handles
    mono_gchandle_new_fn         gchandle_new        = nullptr;
    mono_gchandle_free_fn        gchandle_free       = nullptr;
    mono_gchandle_get_target_fn  gchandle_get_target = nullptr;

    // ========================================================================
    // Internal state
    // ========================================================================

    static HMODULE s_mono_module = nullptr;

    // ========================================================================
    // Helper: resolve a single export from the mono module
    // ========================================================================

    template <typename T>
    static bool resolve(HMODULE mod, const char* name, T& out) {
        out = reinterpret_cast<T>(GetProcAddress(mod, name));
        return out != nullptr;
    }

    // ========================================================================
    // Public API
    // ========================================================================

    bool initialize() {
        // Try the Boehm GC variant first (used by Unity), then fall back to mono.dll
        s_mono_module = GetModuleHandleA("mono-2.0-bdwgc.dll");
        if (!s_mono_module) {
            s_mono_module = GetModuleHandleA("mono.dll");
        }
        if (!s_mono_module) {
            return false;
        }

        // Resolve all exports. Non-critical functions may be null on older runtimes.
        resolve(s_mono_module, "mono_get_root_domain",             get_root_domain);
        resolve(s_mono_module, "mono_domain_get",                  domain_get);
        resolve(s_mono_module, "mono_domain_assembly_open",        domain_assembly_open);

        resolve(s_mono_module, "mono_thread_attach",               thread_attach);
        resolve(s_mono_module, "mono_thread_detach",               thread_detach);

        resolve(s_mono_module, "mono_assembly_foreach",            assembly_foreach);
        resolve(s_mono_module, "mono_assembly_get_image",          assembly_get_image);
        resolve(s_mono_module, "mono_assembly_loaded",             assembly_loaded);
        resolve(s_mono_module, "mono_assembly_load_from_full",     assembly_load_from_full);

        resolve(s_mono_module, "mono_image_get_name",              image_get_name);
        resolve(s_mono_module, "mono_image_open_from_data_full",   image_open_from_data_full);
        resolve(s_mono_module, "mono_image_close",                 image_close);

        resolve(s_mono_module, "mono_class_from_name",             class_from_name);
        resolve(s_mono_module, "mono_class_get_method_from_name",  class_get_method_from_name);
        resolve(s_mono_module, "mono_class_get_methods",           class_get_methods);
        resolve(s_mono_module, "mono_class_get_fields",            class_get_fields);
        resolve(s_mono_module, "mono_class_get_parent",            class_get_parent);
        resolve(s_mono_module, "mono_class_get_name",              class_get_name);
        resolve(s_mono_module, "mono_class_get_namespace",         class_get_namespace);

        resolve(s_mono_module, "mono_method_get_name",             method_get_name);
        resolve(s_mono_module, "mono_method_full_name",            method_full_name);

        resolve(s_mono_module, "mono_runtime_invoke",              runtime_invoke);
        resolve(s_mono_module, "mono_runtime_object_init",         runtime_object_init);

        resolve(s_mono_module, "mono_object_new",                  object_new);
        resolve(s_mono_module, "mono_object_unbox",                object_unbox);
        resolve(s_mono_module, "mono_object_get_class",            object_get_class);

        resolve(s_mono_module, "mono_string_new",                  string_new);
        resolve(s_mono_module, "mono_string_new_utf16",            string_new_utf16);
        resolve(s_mono_module, "mono_string_to_utf8",              string_to_utf8);

        resolve(s_mono_module, "mono_add_internal_call",           add_internal_call);

        resolve(s_mono_module, "mono_field_get_value",             field_get_value);
        resolve(s_mono_module, "mono_field_set_value",             field_set_value);
        resolve(s_mono_module, "mono_field_get_offset",            field_get_offset);
        resolve(s_mono_module, "mono_field_get_name",              field_get_name);
        resolve(s_mono_module, "mono_class_get_field_from_name",   class_get_field_from_name);

        resolve(s_mono_module, "mono_type_get_type",               type_get_type);
        resolve(s_mono_module, "mono_type_is_byref",               type_is_byref);

        resolve(s_mono_module, "mono_compile_method",              compile_method);

        resolve(s_mono_module, "mono_gchandle_new",                gchandle_new);
        resolve(s_mono_module, "mono_gchandle_free",               gchandle_free);
        resolve(s_mono_module, "mono_gchandle_get_target",         gchandle_get_target);

        // Validate critical functions required for basic operation
        if (!get_root_domain || !thread_attach || !class_from_name || !runtime_invoke) {
            shutdown();
            return false;
        }

        return true;
    }

    void shutdown() {
        // Zero all function pointers
        get_root_domain       = nullptr;
        domain_get            = nullptr;
        domain_assembly_open  = nullptr;

        thread_attach  = nullptr;
        thread_detach  = nullptr;

        assembly_foreach        = nullptr;
        assembly_get_image      = nullptr;
        assembly_loaded         = nullptr;
        assembly_load_from_full = nullptr;

        image_get_name             = nullptr;
        image_open_from_data_full  = nullptr;
        image_close                = nullptr;

        class_from_name             = nullptr;
        class_get_method_from_name  = nullptr;
        class_get_methods           = nullptr;
        class_get_fields            = nullptr;
        class_get_parent            = nullptr;
        class_get_name              = nullptr;
        class_get_namespace         = nullptr;

        method_get_name  = nullptr;
        method_full_name = nullptr;

        runtime_invoke      = nullptr;
        runtime_object_init = nullptr;

        object_new       = nullptr;
        object_unbox     = nullptr;
        object_get_class = nullptr;

        string_new       = nullptr;
        string_new_utf16 = nullptr;
        string_to_utf8   = nullptr;

        add_internal_call = nullptr;

        field_get_value        = nullptr;
        field_set_value        = nullptr;
        field_get_offset       = nullptr;
        field_get_name         = nullptr;
        class_get_field_from_name = nullptr;

        type_get_type = nullptr;
        type_is_byref = nullptr;

        compile_method = nullptr;

        gchandle_new        = nullptr;
        gchandle_free       = nullptr;
        gchandle_get_target = nullptr;

        // We obtained the handle via GetModuleHandle (not LoadLibrary),
        // so we do not call FreeLibrary.
        s_mono_module = nullptr;
    }

} // namespace mono
