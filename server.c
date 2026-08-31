/* The file is not ready to be included yet but it does have a working (unsafe)
 * server you can run. This file is meant to be included with the engine in
 * later commits, this is an intermediate stage. */
#if 0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#include "tdlib.h"

#define BACKLOG 10

typedef enum {
    GET,
} Request_Method;

typedef struct {
    Request_Method method;
    char target[4096];
} Request;

typedef struct {
    int status;
    const char *content_type;
    const char *body;
} Response;

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:
        return "Unknown request!";
    }
}

int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        p += n;
        len -= n;
    }
    return 1;
}

int req_parse(int fd, Request *req)
{
    char buf[16*1024];

    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    char method[16];
    if (sscanf(buf, "%15s %4095s", method, req->target) != 2)
        return 0;

    if (strcmp(method, "GET") == 0)
        req->method = GET;
    else
        return 0;

    return 1;
}

int res_send(int fd, Response *res)
{
    char header[1024];

    size_t body_len = strlen(res->body);
    int n = snprintf(header,
                     sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     res->status,
                     status_text(res->status),
                     res->content_type,
                     body_len);
    if (n < 0 || (size_t)n >= sizeof(header))
        return 0;
    
    if (!send_all(fd, header, n))
        return 0;
    if (!send_all(fd, res->body, body_len))
        return 0;

    return 1;
}

static size_t html_escape(char *dst, size_t cap, String src)
{
    size_t out = 0;

    for (size_t i = 0; i < src.size; ++i) {
        const char *replacement;
        size_t len;

        switch (src.data[i]) {
        case '&':
            replacement = "&amp;";
            len = 5;
            break;
        case '<':
            replacement = "&lt;";
            len = 4;
            break;
        case '>':
            replacement = "&gt;";
            len = 4;
            break;
        case '"':
            replacement = "&quot;";
            len = 6;
            break;
        case '\'':
            replacement = "&#39;";
            len = 5;
            break;
        default:
            replacement = &src.data[i];
            len = 1;
            break;
        }

        if (out + len >= cap)
            return 0;

        memcpy(dst + out, replacement, len);
        out += len;
    }

    dst[out] = '\0';
    return out;
}

void serve_home(int fd)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "\t<title>Search</title>"
        "</head>"
        "<body>"
        "\t<h1>Chercheva: <i>Local search engine</i></h1>"
        "\t<form method=\"GET\" action=\"/\">"
        "\t\t<input type=\"text\" name=\"q\">"
        "\t\t<button type=\"submit\">Search</button>"
        "\t</form>"
        "</body>"
        "</html>";

    Response res = {
        .status = 200,
        .content_type = "text/html",
        .body = html,
    };

    res_send(fd, &res);
}

void serve_query(int fd, const String query)
{
    char escaped[4096 * 6 + 1];
    if (!html_escape(escaped, sizeof(escaped), query))
        return;
    
    char buf[4096 * 6 + 1024];
    int n = snprintf(buf, sizeof(buf),
                     "<!DOCTYPE html>"
                     "<html>"
                     "<head>"
                     "\t<title>Search</title>"
                     "</head>"
                     "<body>"
                     "\t<h1>Chercheva: <i>Local search engine</i></h1>"
                     "\t<form method=\"GET\" action=\"/\">"
                     "\t\t<input type=\"text\" name=\"q\">"
                     "\t\t<button type=\"submit\">Search</button>"
                     "\t</form>"
                     "\t<h2>Showing search results for: \"%s\"</h2>"
                     "</body>"
                     "</html>",
                     escaped);

    if (n < 0 || (size_t)n >= sizeof(buf))
        return;

    Response res = {
        .status = 200,
        .content_type = "text/html",
        .body = buf,
    };

    res_send(fd, &res);
}

static int hex_value(char c)
{
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static int req_parse_query(Request *req, String *query)
{
    char *p = strchr(req->target, '?');

    if (!p || strncmp(p, "?q=", 3) != 0)
        return 0;
    p += 3;

    while (*p) {
        char c = *p++;

        if (c == '&')
            break;

        if (c == '+') {
            td_string_append(query, " ", 1);
            continue;
        }

        if (c == '%') {
            int hi, lo;

            if (!p[0] || !p[1])
                return 0;

            hi = hex_value(p[0]);
            lo = hex_value(p[1]);

            if (hi < 0 || lo < 0)
                return 0;

            c = (char)((hi << 4) | lo);
            if (c == '\0')
                return 0;
            p += 2;
        }
        
        td_string_append(query, &c, 1);
    }
    
    return query->size > 0;
}

static void handle_connection(int fd)
{
    Request req = { 0 };
            
    if (!req_parse(fd, &req)) {
        Response res = {
            .status = 400,
            .content_type = "text/plain",
            .body = status_text(400),
        };

        res_send(fd, &res);
    } else {
        if (req.method == GET) {
            printf("GET %s\n", req.target);

            if (strcmp(req.target, "/") == 0) {
                serve_home(fd);
            } else if (strncmp(req.target, "/?q=", 4) == 0) {
                String query = { 0 };
                        
                if (!req_parse_query(&req, &query)) {
                    Response res = {
                        .status = 400,
                        .content_type = "text/plain",
                        .body = "Missing query!",
                    };
                    res_send(fd, &res);
                } else {
                    printf("query: %s\n", query.data);

                    // @Todo Search will come here
                    serve_query(fd, query);
                }

                td_string_clear(&query);
            } else {
                Response res = {
                    .status = 404,
                    .content_type = "text/plain",
                    .body = status_text(404),
                };

                res_send(fd, &res);
            }
        } else {
            printf("Unsupported!\n");
        }
    }
}

void sigchld_handler(int s)
{
    (void)s;
    int saved_errno = errno;
    while(waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char **argv)
{
    int sockfd, newfd, rv, yes = 1;
    char *port;
    struct addrinfo hints, *servinfo, *p;
    struct sigaction sa;
    struct sockaddr_storage their_addr;
    socklen_t sin_size;
    char s[INET6_ADDRSTRLEN];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    port = argv[1];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("server: Accepting connections http://127.0.0.1:%s/...\n", port);
    while (1) {
        sin_size = sizeof(their_addr);
        newfd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (newfd < 0) {
            perror("accept");
            continue;
        }

        inet_ntop(their_addr.ss_family,
                  get_in_addr((struct sockaddr *)&their_addr),
                  s, sizeof(s));
        printf("server: got connection from %s\n", s);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(newfd);
            continue;
        }

        if (pid == 0) { /* child process */
            close(sockfd);
            handle_connection(newfd);
            close(newfd);
            exit(0);
        }

        close(newfd);
    }

    return 0;
}

#endif
