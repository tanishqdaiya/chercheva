#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>

#include "lib/lexbor/html/html.h"
#include "lib/lexbor/dom/dom.h"

#define DOCUMENT_VECINITSZ 32
#define VECINITSZ 32

#define TABLE_INITSZ 64
#define TABLE_LOADF  0.5

#define EPSILON 1e-9

#define FNV_PRIME_32 0x01000193U
#define FNV_OFFSET_BASIS_32 0x811C9DC5U

/*
 * String:
 *  data is either NULL or points in range [0, alloc)
 *  size excludes terminating '\0'
 *  data[size] is always '\0'
 */
typedef struct {
    char *data;
    size_t size, alloc;
} String;

typedef struct {
    const char *data;
    size_t size;
} String_View;

typedef struct {
    String_View term;
    long freq; /* occupied when freq > 0 */
} Term_Frequency;

typedef struct {
    Term_Frequency *tf;
    size_t size, alloc;
} TF_Table;

typedef struct {
    String path;
    String content;
    String text;

    // @Todo Rename
    size_t ntokens;

    TF_Table tf;
} Document;

typedef struct {
    Document *data;
    size_t size, alloc;
} Document_Vector;

typedef struct {
    size_t doc_index;
    double score;
} Result_Pair;

typedef struct {
    Result_Pair *data;
    size_t size, alloc;
} Result_Vector;

/* == UTILS == */

#define FATAL(...)                              \
    do {                                        \
        fprintf(stderr, __VA_ARGS__);           \
        exit(EXIT_FAILURE);                     \
    } while (0)

/* It is a resizing function which required the vector in its right structural
   format and the required capacity. If the capacity exceeds the pre-allocated
   size of the vector, we resize. This function is unsafe and provides no
   guaranteed successful reallocation */
#define vec_alloc(vector, capacity)                                     \
    do {                                                                \
        if ((capacity) > (vector)->alloc) {                             \
            if ((vector)->alloc == 0) (vector)->alloc = VECINITSZ;      \
            while ((capacity) > (vector)->alloc) (vector)->alloc *= 2;  \
            (vector)->data = realloc((vector)->data,                    \
                                     (vector)->alloc * sizeof(*(vector)->data)); \
            if ((vector)->data == NULL) {                               \
                FATAL("TD_REALLOC: out of memory");                     \
            }                                                           \
        }                                                               \
    } while (0)

#define vec_append(vector, item)                        \
    do {                                                \
        vec_alloc((vector), (vector)->size + 1);        \
        (vector)->data[(vector)->size++] = (item);      \
    } while (0)

#define vec_append_bulk(vector, items, count)                   \
    do {                                                        \
        vec_alloc((vector), (vector)->size + count);            \
        memcpy((vector)->data + (vector)->size,                 \
               (items),                                         \
               (count)*sizeof(*(vector)->data));                \
        (vector)->size += (count);                              \
    } while (0)

void to_uppercase(String str)
{
    for (size_t i = 0; i < str.size; ++i)
        str.data[i] = (char)toupper((unsigned char)str.data[i]);
}

bool sv_equal(String_View a, String_View b)
{
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; i++) {
        if (a.data[i] != b.data[i])
            return false;
    }

    return true;
}

void string_append_cstr(String *string, const char *cstr)
{
    size_t n = strlen(cstr);

    vec_alloc(string, string->size + n + 1);
    memcpy(string->data + string->size, cstr, n);

    string->size += n;
    string->data[string->size] = '\0';
}

void string_clear(String *string)
{
    free(string->data);
    *string = (String) { 0 };
}

int read_file(const char *path, Document *doc)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    char *content = malloc((size_t)size + 1);
    if (!content)
        return 0;

    if (fread(content, 1, (size_t)size, fp) != (size_t)size) {
        free(content);
        fclose(fp);
        return 0;
    }

    content[size] = '\0';
    fclose(fp);

    doc->path.data = strdup(path);
    doc->path.size = strlen(path);
    doc->content = (String) {
        .data = content,
        .size = size,
        .alloc = size + 1,
    };

    return 1;
}

/* == HTML EXTRACTION == */

size_t html_measure(lxb_dom_node_t *node)
{
    size_t len = 0;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t n;
        lxb_dom_node_text_content(node, &n);
        len += n;
    }

    // Space for block elements to keep the tokens separated
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) len += 1;

    for (lxb_dom_node_t *c = lxb_dom_node_first_child(node); c; c = lxb_dom_node_next(c))
        len += html_measure(c);
    return len;
}

void html_pack(lxb_dom_node_t *node, char **ptr)
{
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t n;
        const char *text = (const char *)lxb_dom_node_text_content(node, &n);
        memcpy(*ptr, text, n);
        *ptr += n;
    }

    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        **ptr = ' ';
        (*ptr)++;
    }

    for (lxb_dom_node_t *c = lxb_dom_node_first_child(node); c; c = lxb_dom_node_next(c))
        html_pack(c, ptr);
}

char *html_extract(const char *html)
{
    lxb_html_document_t *doc = lxb_html_document_create();
    lxb_html_document_parse(doc, (lxb_char_t*)html, strlen(html));

    size_t n_title;
    const char *title = (const char*)lxb_html_document_title(doc, &n_title);

    lxb_dom_node_t *body = lxb_dom_interface_node(doc->body);
    size_t n_body = html_measure(body);

    char *buf = malloc(n_title + 1 + n_body + 1);
    char *p = buf;

    memcpy(p, title, n_title);
    p += n_title;
    *p++ = '\n';

    html_pack(body, &p);
    *p = 0;

    lxb_html_document_destroy(doc);
    return buf;
}

/* == LEXER == */

String_View next_token(String_View *corpus)
{
    // trim left
    while ((unsigned char)corpus->size > 0 && isspace((unsigned char)*corpus->data)) {
        corpus->data++;
        corpus->size--;
    }

    if (corpus->size == 0)
        return (String_View){ 0 };

    if (isalpha((unsigned char)*corpus->data)) {
        size_t n = 0;
        while (n < corpus->size && isalnum((unsigned char)corpus->data[n]))
            n++;

        String_View token = {
            .data = corpus->data,
            .size = n
        };

        corpus->data += n;
        corpus->size -= n;

        return token;
    }

    if (isdigit((unsigned char)*corpus->data)) {
        size_t n = 0;
        while (n < corpus->size && isdigit((unsigned char)corpus->data[n]))
            n++;
        String_View token = {
            .data = corpus->data,
            .size = n
        };

        corpus->data += n;
        corpus->size -= n;

        return token;
    }

    // for punctuation
    String_View token = {
        .data = corpus->data,
        .size = 1
    };

    corpus->data++;
    corpus->size--;

    return token;
}

/* == TF TABLE == */

uint32_t tf_table_hash(String_View sv)
{
    uint32_t hash = FNV_OFFSET_BASIS_32;
    for (size_t i = 0; i < sv.size; ++i) {
        hash ^= (uint8_t)sv.data[i];
        hash *= FNV_PRIME_32;
    }
    return hash;
}

Term_Frequency *tf_table_search(TF_Table *table, String_View term)
{
    if (table->alloc == 0)
        return NULL;

    size_t start = tf_table_hash(term) % table->alloc;
    size_t index = start;

    do {
        if (table->tf[index].freq <= 0)
            return NULL;

        if (sv_equal(table->tf[index].term, term))
            return &table->tf[index];

        index = (index + 1) % table->alloc;
    } while (index != start);   /* keep probing until we wrap back to the start */

    return NULL;
}

void tf_table_rehash(TF_Table *table, size_t needed)
{
    size_t alloc = table->alloc;

    if (alloc == 0)
        alloc = TABLE_INITSZ;

    while ((double)needed/alloc > TABLE_LOADF)
        alloc *= 2;

    if (alloc == table->alloc)
        return;

    Term_Frequency *old_tf = table->tf;
    size_t old_alloc = table->alloc;

    Term_Frequency *new_tf = calloc(alloc, sizeof(*new_tf));
    if (!new_tf)
        FATAL("tf_table_rehash: out of memory\n");

    table->tf = new_tf;
    table->alloc = alloc;

    for (size_t i = 0; i < old_alloc; ++i) {
        if (old_tf[i].freq <= 0)
            continue;

        /* @Todo hash cachable */
        size_t index = tf_table_hash(old_tf[i].term) % table->alloc;
        while (table->tf[index].freq != 0)
            index = (index + 1) % table->alloc;
        table->tf[index] = old_tf[i];
    }

    free(old_tf);
}

void tf_table_insert(TF_Table *table, Term_Frequency tf)
{
    tf_table_rehash(table, table->size + 1);

    size_t index = tf_table_hash(tf.term) % table->alloc;

    while (table->tf[index].freq != 0) {
        if (sv_equal(table->tf[index].term, tf.term)) {
            table->tf[index].freq += tf.freq;
            return;
        }

        index = (index + 1) % table->alloc;
    }

    table->tf[index] = tf;
    table->size++;
}

/* == TF-IDF == */

double compute_term_frequency(String_View t, Document d)
{
    Term_Frequency *kv = tf_table_search(&d.tf, t);
    if (!kv)
        return 0.0;

    return log(1 + kv->freq);
}

double compute_inverse_document_frequency(String_View t, Document_Vector dv)
{
    long df = 0;
    for (size_t i = 0; i < dv.size; ++i) {
        if (tf_table_search(&dv.data[i].tf, t))
            df++;
    }

    return log(1 + (double)dv.size / (1 + df));
}

/* == DOCUMENT VECTOR == */
void doc_free(Document *d)
{
    string_clear(&d->path);
    string_clear(&d->content);
    free(d->text.data);
    free(d->tf.tf);
}

void docs_free(Document_Vector *dv)
{
    for (size_t i = 0; i < dv->size; ++i)
        doc_free(&dv->data[i]);

    free(dv->data);

    dv->data = NULL;
    dv->size = 0;
    dv->alloc = 0;
}

int load_directory(const char *dirname, Document_Vector *docs)
{
    DIR *dir = opendir(dirname);
    if (!dir)
        return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (!S_ISREG(st.st_mode))
            continue;

        Document doc = { 0 };
        if (!read_file(path, &doc))
            continue;

        vec_append(docs, doc);
    }

    closedir(dir);
    return 1;
}

/* == SEARCHING & INDEXING == */


Result_Vector search(Document_Vector docs, String query)
{
    Result_Vector rv = { 0 };
    for (size_t i = 0; i < docs.size; ++i) {
        String_View input = {
            .data = query.data,
            .size = query.size,
        };

        double score = 0.0;

        for (;;) {
            String_View token = next_token(&input);
            if (!token.data)
                break;

            double tf = compute_term_frequency(token, docs.data[i]);
            double idf = compute_inverse_document_frequency(token, docs);

            score += tf * idf;
        }

        /* skip irrelevant */
        if (score < EPSILON)
            continue;

        Result_Pair rp = {
            .doc_index = i,
            .score = score,
        };

        vec_append(&rv, rp);
    }
    return rv;
}

int result_pair_cmp(const void *a, const void *b)
{
    const Result_Pair *ra = a;
    const Result_Pair *rb = b;

    /* descending */
    if (ra->score < rb->score)
        return 1;
    if (ra->score > rb->score)
        return -1;

    return 0;
}

int main()
{
    Document_Vector docs = { 0 };

    load_directory("gdb_docs", &docs);
    printf("Loaded %zu documents\n", docs.size);

    /* build term frequency table for each document */
    for (size_t i = 0; i < docs.size; ++i) {
        Document *d = &docs.data[i];
        d->text.data = html_extract(d->content.data);
        d->text.size = strlen(d->text.data);
        to_uppercase(d->text);

        String_View input = {
            .data = d->text.data,
            .size = d->text.size,
        };

        for (;;) {
            String_View token = next_token(&input);
            if (!token.data)
                break;
            d->ntokens++;
            tf_table_insert(&d->tf,
                            (Term_Frequency){ .term = token, .freq = 1 });
        }
    }

    /* process queries (very basic, it doesn't even account for cosine
     * similarity */
    String query = { 0 };
    string_append_cstr(&query, "having gdb infer the source language");
    to_uppercase(query);

    Result_Vector rv = search(docs, query);

    qsort(rv.data, rv.size, sizeof(*rv.data), result_pair_cmp);
    for (size_t i = 0; i < rv.size; ++i) {
        Document *doc = &docs.data[rv.data[i].doc_index];
        printf("%s -> %.6f\n",
               doc->path.data,
               rv.data[i].score);
    }

    free(rv.data);
    docs_free(&docs);

    return 0;
}
