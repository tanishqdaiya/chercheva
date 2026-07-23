#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "lib/lexbor/html/html.h"
#include "lib/lexbor/dom/dom.h"

typedef struct {
    char *data;
    size_t count;
} String_View;

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

int main(void)
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

    fread(html_content, 1, fsize, fp);
    html_content[fsize] = '\0';
    fclose(fp);

    char *extracted = html_extract(html_content);
    size_t n = strlen(extracted);
    to_uppercase(extracted, n);
    
    String_View corpus = { .data = extracted, .count = n };
    for (;;) {
        String_View token = next_token(&corpus);
        if (token.count == 0)
            break;
        
        printf("%.*s\n", (int)token.count, token.data);
    }
    
    free(html_content);
    
    return 0;
}
