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

#define TABLE_INITSZ 64
#define TABLE_LOADF  0.5

#define FNV_PRIME_32 0x01000193U
#define FNV_OFFSET_BASIS_32 0x811C9DC5U

typedef struct {
    char *data;
    size_t count;
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
    char *path;
    char *contents;
    char *text;

    // @Todo Rename
    size_t length; /* for contents */
    size_t ntokens;

    TF_Table tf;
} Document;

typedef struct {
    Document *doc;
    size_t size, alloc;
} Document_Vector;

/* == UTILS == */

#define FATAL(...)                              \
    do {                                        \
        fprintf(stderr, __VA_ARGS__);           \
        exit(EXIT_FAILURE);                     \
    } while (0)

void to_uppercase(char *str, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        str[i] = (char)toupper(str[i]);
}

bool sv_equal(String_View a, String_View b)
{
    if (a.count != b.count)
        return false;
    for (size_t i = 0; i < a.count; i++) {
        if (a.data[i] != b.data[i])
            return false;
    }

    return true;
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

    doc->path = strdup(path);
    doc->contents = content;
    doc->length = (size_t)size;

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
    while ((unsigned char)corpus->count > 0 && isspace((unsigned char)*corpus->data)) {
        corpus->data++;
        corpus->count--;
    }

    if (corpus->count == 0)
        return (String_View){ 0 };

    if (isalpha((unsigned char)*corpus->data)) {
        size_t n = 0;
        while (n < corpus->count && isalnum((unsigned char)corpus->data[n]))
            n++;

        String_View token = {
            .data = corpus->data,
            .count = n
        };

        corpus->data += n;
        corpus->count -= n;

        return token;
    }

    if (isdigit((unsigned char)*corpus->data)) {
        size_t n = 0;
        while (n < corpus->count && isdigit((unsigned char)corpus->data[n]))
            n++;
        String_View token = {
            .data = corpus->data,
            .count = n
        };

        corpus->data += n;
        corpus->count -= n;

        return token;
    }

    // for punctuation
    String_View token = {
        .data = corpus->data,
        .count = 1
    };

    corpus->data++;
    corpus->count--;

    return token;
}

/* == TF TABLE == */

uint32_t tf_table_hash(String_View sv)
{
    uint32_t hash = FNV_OFFSET_BASIS_32;
    for (size_t i = 0; i < sv.count; ++i) {
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
        if (tf_table_search(&dv.doc[i].tf, t))
            df++;
    }

    return log(1 + (double)dv.size / (1 + df));
}

/* == DOCUMENT VECTOR == */

static void docs_alloc(Document_Vector *dv, size_t capacity)
{
    if (dv->alloc < capacity) {
        if (dv->alloc == 0)
            dv->alloc = DOCUMENT_VECINITSZ;
        while (dv->alloc < capacity)
            dv->alloc *= 2;
        dv->doc = realloc(dv->doc, dv->alloc * sizeof(*(dv)->doc));
        if (!dv->doc)
            FATAL("docs_alloc: out of memory!\n");
    }
}

void doc_free(Document *d)
{
    free(d->path);
    free(d->contents);
    free(d->text);
    free(d->tf.tf);
}

void docs_free(Document_Vector *dv)
{
    for (size_t i = 0; i < dv->size; ++i)
        doc_free(&dv->doc[i]);

    free(dv->doc);

    dv->doc = NULL;
    dv->size = 0;
    dv->alloc = 0;
}

void docs_push(Document_Vector *d, Document doc)
{
    docs_alloc(d, d->size + 1);
    d->doc[d->size++] = doc;
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

        docs_push(docs, doc);
    }

    closedir(dir);
    return 1;
}

int main(void)
{
    Document_Vector docs = { 0 };
    load_directory("gdb_docs", &docs);

    printf("Loaded %zu documents\n", docs.size);

    for (size_t i = 0; i < docs.size; ++i) {
        docs.doc[i].text = html_extract(docs.doc[i].contents);
        to_uppercase(docs.doc[i].text, strlen(docs.doc[i].text));

        String_View input = {
            .data = docs.doc[i].text,
            .count = strlen(docs.doc[i].text),
        };

        for (;;) {
            String_View token = next_token(&input);
            if (!token.data)
                break;
            docs.doc[i].ntokens++;
            tf_table_insert(&docs.doc[i].tf,
                            (Term_Frequency){ .term = token, .freq = 1 });
        }
    }

    // Compute tf-idf
    for (size_t i = 0; i < docs.size; ++i) {
        printf("\n%s (%zu bytes)\n",
               docs.doc[i].path,
               docs.doc[i].length);
        
        for (size_t j = 0; j < docs.doc[i].tf.alloc; ++j) {
            Term_Frequency *tf = &docs.doc[i].tf.tf[j];

            if (tf->freq <= 0)
                continue;

            double term_freq = compute_term_frequency(tf->term, docs.doc[i]);
            double inv_term_freq = compute_inverse_document_frequency(tf->term, docs);
            double score = term_freq * inv_term_freq;
            printf("  %16.*s -> %4ld, %.4f, %.4f, %.4f\n",
                   (int)tf->term.count,
                   tf->term.data,
                   tf->freq,
                   term_freq,
                   inv_term_freq,
                   score);
        }
    }

    docs_free(&docs);

    return 0;
}
