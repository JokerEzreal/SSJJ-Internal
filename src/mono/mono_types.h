#pragma once

#include <Windows.h>
#include <cstdint>

// Mono primitive types
typedef int32_t  mono_bool;
typedef uint16_t mono_unichar2;

// Mono image open status (used by image/assembly loading functions)
typedef enum {
    MONO_IMAGE_OK,
    MONO_IMAGE_ERROR_ERRNO,
    MONO_IMAGE_MISSING_ASSEMBLYREF,
    MONO_IMAGE_IMAGE_INVALID
} MonoImageOpenStatus;

// Opaque Mono runtime types
typedef struct _MonoDomain       MonoDomain;
typedef struct _MonoAssembly     MonoAssembly;
typedef struct _MonoImage        MonoImage;
typedef struct _MonoClass        MonoClass;
typedef struct _MonoMethod       MonoMethod;
typedef struct _MonoObject       MonoObject;
typedef struct _MonoString       MonoString;
typedef struct _MonoProperty     MonoProperty;
typedef struct _MonoType         MonoType;
typedef struct _MonoThread       MonoThread;
typedef struct _MonoMethodDesc   MonoMethodDesc;
typedef struct _MonoArray        MonoArray;
typedef struct _MonoClassField   MonoClassField;
typedef struct _MonoAssemblyName MonoAssemblyName;

// Callback type used by mono_assembly_foreach
typedef void (*MonoAssemblyIterFunc)(MonoAssembly* assembly, void* user_data);

// ============================================================================
// Function pointer typedefs for all Mono C API exports
// ============================================================================

// Domain
typedef MonoDomain*   (*mono_get_root_domain_fn)();
typedef MonoDomain*   (*mono_domain_get_fn)();
typedef MonoAssembly* (*mono_domain_assembly_open_fn)(MonoDomain* domain, const char* name);

// Thread
typedef MonoThread* (*mono_thread_attach_fn)(MonoDomain* domain);
typedef void        (*mono_thread_detach_fn)(MonoThread* thread);

// Assembly
typedef void          (*mono_assembly_foreach_fn)(MonoAssemblyIterFunc func, void* user_data);
typedef MonoImage*    (*mono_assembly_get_image_fn)(MonoAssembly* assembly);
typedef MonoAssembly* (*mono_assembly_loaded_fn)(MonoAssemblyName* aname);
typedef MonoAssembly* (*mono_assembly_load_from_full_fn)(MonoImage* image, const char* fname, MonoImageOpenStatus* status, mono_bool refonly);

// Image
typedef const char* (*mono_image_get_name_fn)(MonoImage* image);
typedef MonoImage*  (*mono_image_open_from_data_full_fn)(char* data, uint32_t data_len, mono_bool need_copy, MonoImageOpenStatus* status, mono_bool refonly);
typedef void        (*mono_image_close_fn)(MonoImage* image);

// Class
typedef MonoClass*      (*mono_class_from_name_fn)(MonoImage* image, const char* name_space, const char* name);
typedef MonoMethod*     (*mono_class_get_method_from_name_fn)(MonoClass* klass, const char* name, int param_count);
typedef MonoMethod*     (*mono_class_get_methods_fn)(MonoClass* klass, void** iter);
typedef MonoClassField* (*mono_class_get_fields_fn)(MonoClass* klass, void** iter);
typedef MonoClass*      (*mono_class_get_parent_fn)(MonoClass* klass);
typedef const char*     (*mono_class_get_name_fn)(MonoClass* klass);
typedef const char*     (*mono_class_get_namespace_fn)(MonoClass* klass);

// Method
typedef const char* (*mono_method_get_name_fn)(MonoMethod* method);
typedef char*       (*mono_method_full_name_fn)(MonoMethod* method, mono_bool signature);

// Runtime
typedef MonoObject* (*mono_runtime_invoke_fn)(MonoMethod* method, void* obj, void** params, MonoObject** exc);
typedef void        (*mono_runtime_object_init_fn)(MonoObject* this_obj);

// Object
typedef MonoObject* (*mono_object_new_fn)(MonoDomain* domain, MonoClass* klass);
typedef void*       (*mono_object_unbox_fn)(MonoObject* obj);
typedef MonoClass*  (*mono_object_get_class_fn)(MonoObject* obj);

// String
typedef MonoString* (*mono_string_new_fn)(MonoDomain* domain, const char* text);
typedef MonoString* (*mono_string_new_utf16_fn)(MonoDomain* domain, const mono_unichar2* text, int32_t len);
typedef char*       (*mono_string_to_utf8_fn)(MonoString* string_obj);

// Internal call registration
typedef void (*mono_add_internal_call_fn)(const char* name, const void* method);

// Field
typedef void            (*mono_field_get_value_fn)(MonoObject* obj, MonoClassField* field, void* value);
typedef void            (*mono_field_set_value_fn)(MonoObject* obj, MonoClassField* field, void* value);
typedef uint32_t        (*mono_field_get_offset_fn)(MonoClassField* field);
typedef const char*     (*mono_field_get_name_fn)(MonoClassField* field);
typedef MonoClassField* (*mono_class_get_field_from_name_fn)(MonoClass* klass, const char* name);

// Type
typedef int       (*mono_type_get_type_fn)(MonoType* type);
typedef mono_bool (*mono_type_is_byref_fn)(MonoType* type);

// JIT
typedef void* (*mono_compile_method_fn)(MonoMethod* method);

// GC handles
typedef uint32_t    (*mono_gchandle_new_fn)(MonoObject* obj, mono_bool pinned);
typedef void        (*mono_gchandle_free_fn)(uint32_t gchandle);
typedef MonoObject* (*mono_gchandle_get_target_fn)(uint32_t gchandle);
