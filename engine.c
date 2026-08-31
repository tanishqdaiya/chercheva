#define TDLIB_IMPLEMENTATION
#include "tdlib.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "lib/lexbor/html/html.h"
#include "lib/lexbor/dom/dom.h"
#include "lib/cJSON/cJSON.h"

#include "model.h"
#include "tokenizer.h"

#define INDEX_FPATH "index.json"
#define MIN_SCORE 1e-6

typedef struct {
    size_t doc_index;
    f64 score;
} Search_Result;

typedef struct {
    Search_Result *data;
    size_t size, alloc;
} Search_Result_Vector;

typedef struct {
    String *data;
    size_t size, alloc;
} String_Vector;

static int load_documents_from_dir(const char *dirname, Document_Vector *docs)
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
        String content = { 0 };
        if (td_read_entire_file(&content, path) == 0)
            TD_FATAL("td_read_entire_file: error loading document!\n");
        doc.path.data = strdup(path);
        doc.path.size = strlen(path);
        doc.content = content;

        td_vec_append(docs, doc);
    }

    closedir(dir);
    return 1;
}

static void index_documents(Document_Vector *docs)
{
    for (size_t i = 0; i < docs->size; ++i) {
        Document *d = &docs->data[i];

        for (;;) {
            String token = next_token(&d->content);
            if (!token.data)
                break;

            td_string_toupper(token);
            tf_insert(&d->tf, (TF_Entry) {
                    .term = token,
                    .freq = 1,
                });
        }
    }
}

int save_doc_to_json(Document_Vector docs, const char *path)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        goto cleanup;

    for (size_t i = 0; i < docs.size; ++i) {
        Document *src = &docs.data[i];
        
        cJSON  *doc = cJSON_CreateObject();
        if (!doc)
            goto cleanup;
        cJSON_AddItemToObject(root, src->path.data, doc);

        for (size_t j = 0; j < src->tf.alloc; ++j) {
            TF_Entry *entry = &src->tf.data[j];
            if (entry->freq <= 0)
                continue;
            
            cJSON_AddNumberToObject(doc, entry->term.data, (double)entry->freq);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        fprintf(stderr, "Failed to generate json\n");
        goto cleanup;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json);
        goto cleanup;
    }

    int ok = fputs(json, fp) != EOF;
    
    fclose(fp);
    free(json);

    cJSON_Delete(root);
    return ok;

cleanup:
    cJSON_Delete(root);
    return 0;
}

int load_doc_from_json(Document_Vector *docs, const char *path)
{
    String json = { 0 };
    if (!td_read_entire_file(&json, path))
        TD_FATAL("td_read_entire_file: error loading document\n");

    cJSON *root = cJSON_Parse(json.data);
    if (!root)
        return 0;

    cJSON *doc_json;
    cJSON_ArrayForEach(doc_json, root) {
        Document doc = { 0 };
        doc.path = td_string_from_cstr(doc_json->string);
        
        cJSON *tf_json;
        cJSON_ArrayForEach(tf_json, doc_json) {
            TF_Entry entry = {
                .term = td_string_from_cstr(tf_json->string),
                .freq = tf_json->valueint,
            };
            
            tf_insert(&doc.tf, entry);
        }

        td_vec_append(docs, doc);
    }

    cJSON_Delete(root);
    return 0;
}

// SEARCH
static Search_Result_Vector search(Document_Vector docs, String query)
{
    Search_Result_Vector results = { 0 };

    String_Vector query_tokens = { 0 };
    for (;;) {
        String token = next_token(&query);
        if (!token.data)
            break;
        td_vec_append(&query_tokens, token);
    }
    
    for (size_t i = 0; i < docs.size; ++i) {
        f64 score = 0.0;

        for (size_t j = 0; j < query_tokens.size; ++j) {
            String token = query_tokens.data[j];

            f64 tf = tf_weight(token, &docs.data[i]);
            f64 idf = idf_weight(token, docs);
            score += tf * idf;
        }

        if (score < MIN_SCORE) // skip irrelevant
            continue;

        td_vec_append(&results, ((Search_Result) {
                    .doc_index = i,
                    .score = score,
                }));
    }
    
    return results;
}

static s32 search_result_cmp(const void *a, const void *b)
{
    const Search_Result *ra = a, *rb = b;
    return rb->score - ra->score;
}

int main(void)
{
    /* SEARCH ENGINE
         1. Load documents from directory or index.
              - Read supported files from directory
              - Tokenize the files using the lexer to generate term-frequencies
              - Create a term-frequency index
              - Index the read files into persistent storage
              - From now on, read from the index if the index exists
         2. Receive the query and return the results.
              - Input a string
              - Tokenize the said string in the same way
              - Run tf-idf(t, d) where t is the term from the string and d is
              the document, for each document in index.
     */

    Document_Vector docs = { 0 };

    int index_found = access(INDEX_FPATH, F_OK) == 0;
    if (!index_found) {
        if (load_documents_from_dir("gdb_docs", &docs)) {
            printf("Loaded %zu documents\n", docs.size);
            index_documents(&docs);
        }
        save_doc_to_json(docs, INDEX_FPATH);
        printf("Saved cache to %s\n", INDEX_FPATH);
    } else {
        printf("Loading from cache at %s\n", INDEX_FPATH);
        load_doc_from_json(&docs, INDEX_FPATH);
    }

    while (1) {
        char input[1024];

        printf("Enter query: ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            break;
        input[strcspn(input, "\n")] = '\0';
        
        String query = td_string_from_cstr(input);
        td_string_toupper(query);

        printf("Showing search results for \"%s\"\n", query.data);
        
        Search_Result_Vector results = search(docs, query);
        qsort(results.data, results.size, sizeof(*results.data), search_result_cmp);

        for (size_t i = 0; i < results.size; ++i) {
            Document *doc = &docs.data[results.data[i].doc_index];
            printf("[%02zu] \033]8;;file://%s\033\\%-24s\033]8;;\033\\ %.6f\n",
                   i + 1,
                   doc->path.data,
                   doc->path.data,
                   results.data[i].score);

            //printf("[%02zu] %-24s %.6f\n",
            //i + 1, doc->path.data, results.data[i].score);
        }

        td_string_clear(&query);
        free(results.data);
    }

    //docs_free(&docs);
        
    return 0;
}

