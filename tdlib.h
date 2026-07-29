/* tdlib.h -- the original tdlib tailored for this project
   tdlib source: https://github.com/tanishqdaiya/tdlib/ */
#ifndef TDLIB_H
#define TDLIB_H

#ifndef TD_LIBDEF
#    ifdef TDLIB_STATIC
#        define TD_LIBDEF static
#    else
#        define TD_LIBDEF extern
#    endif
#endif

#include <stdint.h>

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float  f32;
typedef double f64;

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MALLOC
#    define TD_MALLOC(sz) malloc(sz)
#endif

#ifndef TD_REALLOC
#    define TD_REALLOC(ptr, sz) realloc((ptr), (sz))
#endif

#ifndef TD_FREE
#    define TD_FREE(p) free(p)
#endif

#ifndef TD_VECINITSZ
#    define TD_VECINITSZ 32
#endif

#ifndef FNV_PRIME_32
#    define FNV_PRIME_32 0x01000193U
#endif

#ifndef FNV_OFFSET_BASIS_32
#    define FNV_OFFSET_BASIS_32 0x811C9DC5U
#endif

typedef struct {
    char *data;
    size_t size, alloc;
} String;

typedef struct {
    const char *data;
    size_t size;
} String_View;

#ifndef TD_FATAL
#define TD_FATAL(...)                           \
    do {                                        \
        fprintf(stderr, __VA_ARGS__);           \
        exit(EXIT_FAILURE);                     \
    } while (0)
#endif

/* It is a resizing function which required the vector in its right structural
   format and the required capacity. If the capacity exceeds the pre-allocated
   size of the vector, we resize. This function is unsafe and provides no
   guaranteed successful reallocation */
#define td__vec_alloc(vector, capacity)                                 \
    do {                                                                \
        if ((capacity) > (vector)->alloc) {                             \
            if ((vector)->alloc == 0) (vector)->alloc = TD_VECINITSZ;   \
            while ((capacity) > (vector)->alloc) (vector)->alloc *= 2;  \
            (vector)->data = TD_REALLOC((vector)->data,                 \
                                     (vector)->alloc * sizeof(*(vector)->data)); \
            if ((vector)->data == NULL) {                               \
                TD_FATAL("TD_REALLOC: out of memory");                  \
            }                                                           \
        }                                                               \
    } while (0)

#define td_vec_append(vector, item)                     \
    do {                                                \
        td__vec_alloc((vector), (vector)->size + 1);    \
        (vector)->data[(vector)->size++] = (item);      \
    } while (0)

#define td_vec_append_bulk(vector, items, count)        \
    do {                                                \
        vec_alloc((vector), (vector)->size + count);    \
        memcpy((vector)->data + (vector)->size,         \
               (items),                                 \
               (count)*sizeof(*(vector)->data));        \
        (vector)->size += (count);                      \
    } while (0)

TD_LIBDEF void td_string_toupper(String str);
TD_LIBDEF bool td_sv_equal(String_View a, String_View b);
TD_LIBDEF void td_string_append_cstr(String *string, const char *cstr);
TD_LIBDEF void td_string_clear(String *string);
TD_LIBDEF char *td_sv_to_cstr(String_View sv);
TD_LIBDEF int td_read_entire_file(String *str, const char *path);

#endif /* TDLIB_H */

#ifdef TDLIB_IMPLEMENTATION

#include <ctype.h>

TD_LIBDEF void td_string_toupper(String str)
{
    for (size_t i = 0; i < str.size; ++i)
        str.data[i] = (char)toupper((unsigned char)str.data[i]);
}

TD_LIBDEF bool td_sv_equal(String_View a, String_View b)
{
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; i++) {
        if (a.data[i] != b.data[i])
            return false;
    }

    return true;
}

TD_LIBDEF void td_string_append_cstr(String *string, const char *cstr)
{
    size_t n = strlen(cstr);

    td__vec_alloc(string, string->size + n + 1);
    memcpy(string->data + string->size, cstr, n);

    string->size += n;
    string->data[string->size] = '\0';
}

TD_LIBDEF void td_string_clear(String *string)
{
    free(string->data);
    *string = (String) { 0 };
}

/* fnv1a */
TD_LIBDEF u32 td_sv_hash(String_View sv)
{
    u32 hash = FNV_OFFSET_BASIS_32;
    for (size_t i = 0; i < sv.size; ++i) {
        hash ^= (u8)sv.data[i];
        hash *= FNV_PRIME_32;
    }
    return hash;
}

TD_LIBDEF char *td_sv_to_cstr(String_View sv)
{
    char *s = malloc(sv.size + 1);
    if (!s)
        return NULL;

    memcpy(s, sv.data, sv.size);
    s[sv.size] = '\0';
    return s;
}

TD_LIBDEF int td_read_entire_file(String *str, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    char *content = TD_MALLOC((size_t)size + 1);
    if (!content)
        return 0;

    if (fread(content, 1, (size_t)size, fp) != (size_t)size) {
        free(content);
        fclose(fp);
        return 0;
    }

    content[size] = '\0'; // @Think whether this is needed
    fclose(fp);

    str->data = content;
    str->size = size;
    str->alloc = size + 1;

    return 1;
}

#endif /* TDLIB_IMPLEMENTATION */
