#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "lib/lexbor/html/html.h"
#include "lib/lexbor/dom/dom.h"

#define TABLE_INITSZ 16
#define TABLE_LOADF  0.5

#define FNV_PRIME_32 0x01000193U
#define FNV_OFFSET_BASIS_32 0x811C9DC5U

typedef struct {
    char *data;
    size_t count;
} String_View;

typedef struct {
    String_View term;
    long freq;                  /* occupied when freq > 0 */
} Term_Frequency;

/* @Note that the table does not support deletion. If a term is deleted in this
 * open-addressed implementation by setting the frequency to zero, you will lose
 * all the collision cases with this due to the early return in the search.
 * Since our engine has not yet found a particular need for deletion, it is fine
 * for now. */
typedef struct {
    Term_Frequency *tf;
    size_t size, alloc;
} TF_Table;

size_t html_measure(lxb_dom_node_t *node) {
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

void html_pack(lxb_dom_node_t *node, char **ptr) {
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

char *html_extract(const char *html) {
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

String_View next_token(String_View *corpus)
{
    // trim left
    while (corpus->count > 0 && isspace(*corpus->data)) {
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
        if (table->tf[index].freq == 0)
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
    if (!new_tf) {
        fprintf(stderr, "tf_table_rehash: out of memory\n");
        exit(EXIT_FAILURE);
    }

    table->tf = new_tf;
    table->alloc = alloc;

    for (size_t i = 0; i < old_alloc; ++i) {
        if (old_tf[i].freq == 0)
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

static void tf_table_print(TF_Table *table)
{
    for (size_t i = 0; i < table->alloc; i++) {
        if (table->tf[i].freq == 0)
            continue;

        printf("%.*s: %ld\n",
               (int)table->tf[i].term.count,
               table->tf[i].term.data,
               table->tf[i].freq);
    }
}

int main()
{
    const char *fname = "gdb_docs/gdb_9.html";

    FILE *fp = fopen(fname, "rb");
    if (!fp) {
        perror(fname);
        exit(EXIT_FAILURE);
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);

    char *html_content = malloc(fsize + 1);
    if (!html_content) {
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    size_t bytes_read = fread(html_content, 1, fsize, fp);
    html_content[bytes_read] = '\0';
    fclose(fp);

    char *extracted = html_extract(html_content);
    if (!extracted) {
        fprintf(stderr, "html_extract failed\n");
        free(html_content);
        exit(EXIT_FAILURE);
    }

    size_t n = strlen(extracted);
    to_uppercase(extracted, n);

    TF_Table tf_table = { 0 };
    String_View corpus = {
        .data = extracted,
        .count = n
    };

    size_t total_tokens = 0;

    for (;;) {
        String_View token = next_token(&corpus);
        if (token.count == 0)
            break;

        total_tokens++;

        Term_Frequency *tf = tf_table_search(&tf_table, token);

        if (!tf) {
            Term_Frequency new_tf = {
                .term = token,
                .freq = 1
            };

            tf_table_insert(&tf_table, new_tf);
        } else {
            tf->freq += 1;
        }
    }

    printf("\n--- TF TABLE ---\n");

    for (size_t i = 0; i < tf_table.alloc; i++) {
        if (tf_table.tf[i].freq == 0)
            continue;

        printf("%.*s: %ld\n",
               (int)tf_table.tf[i].term.count,
               tf_table.tf[i].term.data,
               tf_table.tf[i].freq);
    }

    printf("\n--- STATISTICS ---\n");

    printf("tokens seen: %zu\n", total_tokens);
    printf("unique terms: %zu\n", tf_table.size);
    printf("table allocation: %zu\n", tf_table.alloc);

    size_t occupied = 0;
    size_t counted_tokens = 0;

    for (size_t i = 0; i < tf_table.alloc; i++) {
        if (tf_table.tf[i].freq == 0)
            continue;

        occupied++;
        counted_tokens += tf_table.tf[i].freq;
    }

    printf("occupied slots: %zu\n", occupied);
    printf("counted frequencies: %zu\n", counted_tokens);

    if (occupied != tf_table.size) {
        printf("ERROR: size mismatch\n");
    } else {
        printf("OK: size matches occupied slots\n");
    }

    if (counted_tokens != total_tokens) {
        printf("ERROR: frequency mismatch\n");
    } else {
        printf("OK: frequencies match token count\n");
    }

    printf("\n--- RANDOM LOOKUP VERIFY ---\n");


    String_View verify = {
        .data = extracted,
        .count = n
    };

    size_t missing = 0;

    for (;;) {
        String_View token = next_token(&verify);
        if (token.count == 0)
            break;

        Term_Frequency *tf = tf_table_search(&tf_table, token);

        if (!tf) {
            printf("Missing: %.*s\n",
                   (int)token.count,
                   token.data);
            missing++;
        }
    }

    if (missing == 0)
        printf("OK: all tokens found\n");
    else
        printf("ERROR: %zu missing tokens\n", missing);

    free(tf_table.tf);
    free(extracted);
    free(html_content);

    return 0;
}
