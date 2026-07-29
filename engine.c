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

#include "lib/cJSON/cJSON.h"

#define TDLIB_IMPLEMENTATION
#include "tdlib.h"

#define MAP_INITSZ 64
#define MAP_LOADF  0.5

#define MIN_SCORE 1e-6

typedef struct {
    String_View term;
    s64 freq; /* occupied when freq > 0 */
} TF_Entry;

typedef struct {
    TF_Entry *data;
    size_t size, alloc;
} TF_Map;

typedef struct {
    String path;

    String content;
    String text; /* extracted content from supported doc */

    TF_Map tf;
    size_t token_count;
} Document;

typedef struct {
    Document *data;
    size_t size, alloc;
} Document_Vector;

typedef struct {
    size_t doc_index;
    f64 score;
} Search_Result;

typedef struct {
    Search_Result *data;
    size_t size, alloc;
} Search_Result_Vector;

size_t html_text_size(lxb_dom_node_t *node)
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
        len += html_text_size(c);
    return len;
}

void html_write_text(lxb_dom_node_t *node, char **ptr)
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
        html_write_text(c, ptr);
}

char *html_extract(const char *html)
{
    lxb_html_document_t *doc = lxb_html_document_create();
    lxb_html_document_parse(doc, (lxb_char_t*)html, strlen(html));

    size_t n_title;
    const char *title = (const char*)lxb_html_document_title(doc, &n_title);

    lxb_dom_node_t *body = lxb_dom_interface_node(doc->body);
    size_t n_body = html_text_size(body);

    char *buf = malloc(n_title + 1 + n_body + 1);
    char *p = buf;

    memcpy(p, title, n_title);
    p += n_title;
    *p++ = '\n';

    html_write_text(body, &p);
    *p = 0;

    lxb_html_document_destroy(doc);
    return buf;
}

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

TF_Entry *tf_lookup(const TF_Map *map, String_View term)
{
    if (map->alloc == 0)
        return NULL;

    size_t start = td_sv_hash(term) % map->alloc;
    size_t index = start;

    do {
        if (map->data[index].freq <= 0)
            return NULL;

        if (td_sv_equal(map->data[index].term, term))
            return &map->data[index];

        index = (index + 1) % map->alloc;
    } while (index != start);   /* keep probing until we wrap back to the start */

    return NULL;
}

void tf_rehash(TF_Map *map, size_t needed)
{
    size_t alloc = map->alloc;

    if (alloc == 0)
        alloc = MAP_INITSZ;

    while ((f64)needed/alloc > MAP_LOADF)
        alloc *= 2;

    if (alloc == map->alloc)
        return;

    TF_Entry *old_tf = map->data;
    size_t old_alloc = map->alloc;

    TF_Entry *new_tf = calloc(alloc, sizeof(*new_tf));
    if (!new_tf)
        TD_FATAL("tf_rehash: out of memory\n");

    map->data = new_tf;
    map->alloc = alloc;

    for (size_t i = 0; i < old_alloc; ++i) {
        if (old_tf[i].freq <= 0)
            continue;

        /* @Todo hash cachable */
        size_t index = td_sv_hash(old_tf[i].term) % map->alloc;
        while (map->data[index].freq != 0)
            index = (index + 1) % map->alloc;
        map->data[index] = old_tf[i];
    }

    free(old_tf);
}

void tf_insert(TF_Map *map, TF_Entry tf)
{
    tf_rehash(map, map->size + 1);

    size_t index = td_sv_hash(tf.term) % map->alloc;

    while (map->data[index].freq != 0) {
        if (td_sv_equal(map->data[index].term, tf.term)) {
            map->data[index].freq += tf.freq;
            return;
        }

        index = (index + 1) % map->alloc;
    }

    map->data[index] = tf;
    map->size++;
}

f64 tf_weight(String_View term, const Document *d)
{
    TF_Entry *entry = tf_lookup(&d->tf, term);
    if (!entry)
        return 0.0;

    return log(1 + entry->freq);
}

f64 idf_weight(String_View term, Document_Vector docs)
{
    size_t df = 0;
    for (size_t i = 0; i < docs.size; ++i) {
        if (tf_lookup(&docs.data[i].tf, term))
            df++;
    }

    return log(1 + (f64)docs.size / (1 + df));
}

void doc_free(Document *d)
{
    td_string_clear(&d->path);
    td_string_clear(&d->content);
    free(d->text.data);
    free(d->tf.data);
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

int load_documents_from_dir(const char *dirname, Document_Vector *docs)
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

        td_vec_append(docs, doc);
    }

    closedir(dir);
    return 1;
}

Search_Result_Vector search(Document_Vector docs, String query)
{
    Search_Result_Vector results = { 0 };
    for (size_t i = 0; i < docs.size; ++i) {
        String_View input = {
            .data = query.data,
            .size = query.size,
        };

        f64 score = 0.0;

        for (;;) {
            String_View token = next_token(&input);
            if (!token.data)
                break;

            f64 tf = tf_weight(token, &docs.data[i]);
            f64 idf = idf_weight(token, docs);

            score += tf * idf;
        }

        /* skip irrelevant */
        if (score < MIN_SCORE)
            continue;

        td_vec_append(&results, ((Search_Result) {
                    .doc_index = i,
                    .score = score,
                }));
    }
    return results;
}

s32 search_result_cmp(const void *a, const void *b)
{
    const Search_Result *ra = a;
    const Search_Result *rb = b;

    /* descending */
    if (ra->score < rb->score)
        return 1;
    if (ra->score > rb->score)
        return -1;

    return 0;
}

void index_documents(Document_Vector *docs)
{
    /* build term frequency table for each document */
    for (size_t i = 0; i < docs->size; ++i) {
        Document *d = &docs->data[i];
        d->text.data = html_extract(d->content.data);
        d->text.size = strlen(d->text.data);
        td_string_toupper(d->text);

        String_View input = {
            .data = d->text.data,
            .size = d->text.size,
        };

        for (;;) {
            String_View token = next_token(&input);
            if (!token.data)
                break;
            d->token_count++;
            tf_insert(&d->tf,
                      (TF_Entry){ .term = token, .freq = 1 });
        }
    }
}

// @Cleanup: Manage alloc/free better
int save_index_as_json(Document_Vector docs, const char *path)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        goto end;

    for (size_t i = 0; i < docs.size; ++i) {
        cJSON  *doc = cJSON_CreateObject();
        if (!doc)
            goto end;
        cJSON_AddItemToObject(root, path, doc);

        for (size_t j = 0; j < docs.data[i].tf.alloc; ++j) {
            TF_Entry *entry = &docs.data[i].tf.data[j];
            if (entry->freq <= 0)
                continue;

            // @Cleanup Probably better to use start and end index since we're storing
            // data anyways.  But all this needs to be reworked anyways.  This,
            // at its current state is horrible.
            char *term = td_sv_to_cstr(entry->term);
            if (!term)
                goto end;
            
            cJSON_AddNumberToObject(doc, term, entry->freq);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        fprintf(stderr, "Failed to generate json");
        goto end;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json);
        goto end;
    }

    fputs(json, fp);
    fclose(fp);
    free(json);
    
    return 1;

end:
    cJSON_Delete(root);
    return 0;
}

int main()
{
    Document_Vector docs = { 0 };

    if (load_documents_from_dir("gdb_docs", &docs))
        printf("Loaded %zu documents\n", docs.size);

    index_documents(&docs);

    save_index_as_json(docs, "index.json");

    /* process queries (very basic, it doesn't even account for cosine
     * similarity) */
    String query = { 0 };
    td_string_append_cstr(&query, "configure");
    td_string_toupper(query);

    Search_Result_Vector results = search(docs, query);
    qsort(results.data, results.size, sizeof(*results.data), search_result_cmp);
    for (size_t i = 0; i < results.size; ++i) {
        Document *doc = &docs.data[results.data[i].doc_index];
        printf("%s -> %.6f\n",
               doc->path.data,
               results.data[i].score);
    }

    free(results.data);
    docs_free(&docs);

    return 0;
}
