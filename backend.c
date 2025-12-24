// backend.c - C WebSocket server for MTR using libwebsockets and jansson
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <libwebsockets.h>
#include <jansson.h>
#include <regex.h>
#include <getopt.h>

#define MAX_PAYLOAD 4096
#define MAX_TARGET_LEN 256
#define MAX_ARGS 12

static int interrupted = 0;
static struct lws_context *context = NULL;

struct per_session_data {
    pid_t mtr_pid;
    int pipe_fd;
    char target[MAX_TARGET_LEN];
    int running;
};

static void sigint_handler(int sig) {
    interrupted = 1;
    if (context) {
        lws_cancel_service(context);
    }
}

static int is_valid_target(const char *target) {
    regex_t hostname_regex, ip_regex;
    int ret;

    // Hostname pattern
    ret = regcomp(&hostname_regex, "^[a-zA-Z0-9][a-zA-Z0-9._-]{0,253}[a-zA-Z0-9]$", REG_EXTENDED);
    if (ret) return 0;

    // IP pattern
    ret = regcomp(&ip_regex, "^([0-9]{1,3}\\.){3}[0-9]{1,3}$", REG_EXTENDED);
    if (ret) {
        regfree(&hostname_regex);
        return 0;
    }

    int is_hostname = (regexec(&hostname_regex, target, 0, NULL, 0) == 0);
    int is_ip = (regexec(&ip_regex, target, 0, NULL, 0) == 0);

    regfree(&hostname_regex);
    regfree(&ip_regex);

    return is_hostname || is_ip;
}

static void send_json_message(struct lws *wsi, const char *type, const char *key, const char *value) {
    json_t *root = json_object();
    json_object_set_new(root, "type", json_string(type));
    if (key && value) {
        json_object_set_new(root, key, json_string(value));
    }

    char *json_str = json_dumps(root, JSON_COMPACT);
    if (json_str) {
        size_t len = strlen(json_str);
        unsigned char *buf = malloc(LWS_PRE + len);
        if (buf) {
            memcpy(buf + LWS_PRE, json_str, len);
            lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
            free(buf);
        }
        free(json_str);
    }
    json_decref(root);
}

static void delayed_waitpid_cb(struct lws_sorted_usec_list *sul) {
    while (waitpid(-1, 0, WNOHANG) > 0);
}

static void cleanup_mtr_process(struct per_session_data *pss) {
    static lws_sorted_usec_list_t sul_stagger;
    if (pss->mtr_pid > 0) {
        kill(pss->mtr_pid, SIGTERM);
        waitpid(pss->mtr_pid, NULL, WNOHANG);
        memset(&sul_stagger, 0, sizeof(lws_sorted_usec_list_t));
        lws_sul_schedule(context, 0, &sul_stagger, delayed_waitpid_cb, 15 * LWS_US_PER_SEC);
        pss->mtr_pid = 0;
    }
    if (pss->pipe_fd >= 0) {
        close(pss->pipe_fd);
        pss->pipe_fd = -1;
    }
    pss->running = 0;
}

static int start_mtr(struct lws *wsi, struct per_session_data *pss, const char *target, char *args[]) {
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        send_json_message(wsi, "error", "message", "Failed to create pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        send_json_message(wsi, "error", "message", "Failed to fork process");
        return -1;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execvp("mtr", args);

        // If exec fails
        fprintf(stderr, "Failed to execute mtr\n");
        exit(1);
    }

    // Parent process
    close(pipefd[1]); // Close write end
    pss->pipe_fd = pipefd[0];
    pss->mtr_pid = pid;
    pss->running = 1;
    strncpy(pss->target, target, MAX_TARGET_LEN - 1);

    // Set non-blocking
    int flags = fcntl(pss->pipe_fd, F_GETFL, 0);
    fcntl(pss->pipe_fd, F_SETFL, flags | O_NONBLOCK);

    send_json_message(wsi, "started", "target", target);

    return 0;
}

static void read_mtr_output(struct lws *wsi, struct per_session_data *pss) {
    char buffer[1024];
    ssize_t n;

    while ((n = read(pss->pipe_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';

        // Split by newlines and send each line
        char *line = strtok(buffer, "\n");
        while (line) {
            if (strlen(line) > 0) {
                send_json_message(wsi, "data", "line", line);
            }
            line = strtok(NULL, "\n");
        }
    }

    if (n == 0) {
        // EOF - process finished
        send_json_message(wsi, "complete", NULL, NULL);
        cleanup_mtr_process(pss);
    }
}

static int append_arg(char *args[], char *arg) {
    for (int i=0; i<MAX_ARGS; i++) {
        if (args[i] == NULL) {
            args[i] = arg;
            return 1;
        }
    }
    return 0;
}

static int callback_mtr(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
    struct per_session_data *pss = (struct per_session_data *)user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            lwsl_user("WebSocket connection established\n");
            pss->mtr_pid = 0;
            pss->pipe_fd = -1;
            pss->running = 0;
            break;

        case LWS_CALLBACK_RECEIVE:
            {
                json_error_t error;
                json_t *root = json_loadb(in, len, 0, &error);

                if (!root) {
                    send_json_message(wsi, "error", "message", "Invalid JSON");
                    break;
                }

                json_t *action = json_object_get(root, "action");
                if (action && json_is_string(action)) {
                    const char *action_str = json_string_value(action);

                    if (strcmp(action_str, "start") == 0) {
                        json_t *target = json_object_get(root, "target");
                        json_t *packets = json_object_get(root, "packets");
                        json_t *protocol = json_object_get(root, "protocol");
                        json_t *noDns = json_object_get(root, "noDns");
                        json_t *ipv4 = json_object_get(root, "ipv4");
                        json_t *ipv6 = json_object_get(root, "ipv6");

                        int packet_cnt = 10;
                        if (packets && json_is_integer(packets))
                            packet_cnt = json_integer_value(packets);

                        if (target && json_is_string(target)) {
                            const char *target_str = json_string_value(target);
                            char packet_str[16];
                            snprintf(packet_str, sizeof(packet_str), "%d", packet_cnt);
                            char *args[MAX_ARGS] = {
                                "mtr",
                                "--split",
                                "--report-cycles",
                                packet_str,
                                (char *)target_str,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                            };

                            if (protocol && json_is_string(protocol)) {
                                const char *protocol_str = json_string_value(protocol);
                                if (strcmp(protocol_str, "udp") == 0) {
                                    append_arg(args, "--udp");
                                }
                                if (strcmp(protocol_str, "tcp") == 0) {
                                    append_arg(args, "--tcp");
                                }
                                if (strcmp(protocol_str, "sctp") == 0) {
                                    append_arg(args, "--sctp");
                                }
                            }

                            if (noDns && json_is_boolean(noDns) && json_is_true(noDns)) {
                                append_arg(args, "--no-dns");
                            }

                            if (ipv4 && json_is_boolean(ipv4) && json_is_true(ipv4)) {
                                append_arg(args, "-4");
                            } else if (ipv6 && json_is_boolean(ipv6) && json_is_true(ipv6)) {
                                append_arg(args, "-6");
                            }

                            if (!is_valid_target(target_str)) {
                                send_json_message(wsi, "error", "message",
                                    "Invalid target hostname or IP address");
                            } else {
                                if (pss->running) {
                                    cleanup_mtr_process(pss);
                                }
                                start_mtr(wsi, pss, target_str, args);
                                lws_callback_on_writable(wsi);
                            }
                        }
                    }
                }

                json_decref(root);
            }
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (pss->running && pss->pipe_fd >= 0) {
                read_mtr_output(wsi, pss);
                if (pss->running) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;

        case LWS_CALLBACK_CLOSED:
            lwsl_user("WebSocket connection closed\n");
            cleanup_mtr_process(pss);
            break;

        default:
            break;
    }

    return 0;
}

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_HTTP:
            {
                const char *uri = (const char *)in;
                lwsl_user("HTTP request: %s\n", uri);

                if (strcmp(uri, "/") == 0) {
                    if (lws_serve_http_file(wsi, "./index.html",
                        "text/html", NULL, 0)) {
                        return -1;
                    }
                } else {
                    lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
                    return -1;
                }
            }
            break;

        case LWS_CALLBACK_HTTP_WRITEABLE:
            if (lws_http_transaction_completed(wsi))
                return -1;
            break;

        default:
            break;
    }

    return 0;
}

static struct lws_protocols protocols[] = {
    {
        "http",
        callback_http,
        0,
        0,
    },
    {
        "mtr",
        callback_mtr,
        sizeof(struct per_session_data),
        MAX_PAYLOAD,
    },
    { NULL, NULL, 0, 0 }
};

static const struct lws_http_mount mount = {
    .mount_next = NULL,
    .mountpoint = "/",
    .origin = "",
    .def = "index.html",
    .protocol = NULL,
    .cgienv = NULL,
    .extra_mimetypes = NULL,
    .interpret = NULL,
    .cgi_timeout = 0,
    .cache_max_age = 0,
    .auth_mask = 0,
    .cache_reusable = 0,
    .cache_revalidate = 0,
    .cache_intermediaries = 0,
    .origin_protocol = LWSMPRO_FILE,
    .mountpoint_len = 1,
    .basic_auth_login_file = NULL,
};

int main(int argc, char **argv) {
    struct lws_context_creation_info info;
    int port = 8080;
    int c;

    static struct option long_options[] = {
        {"port", no_argument, 0, 'p'},
        {0, 0, 0, 0}
    };

    int option_index = 0;

    while (1) {
        c = getopt_long(argc, argv, "p:", long_options, &option_index);
        if (c == -1) break;
        switch (c) {
            case 'p':
                port = atoi(optarg);
                break;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_USER, NULL);

    memset(&info, 0, sizeof(info));
    info.port = port;
    info.protocols = protocols;
    info.mounts = &mount;

    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("Failed to create libwebsocket context\n");
        return 1;
    }

    lwsl_user("MTR WebSocket server started on port %d\n", port);
    lwsl_user("Make sure 'mtr' is installed on your system\n");
    lwsl_user("Press Ctrl+C to quit\n");

    while (!interrupted) {
        lws_service(context, 50);
    }

    lws_context_destroy(context);
    lwsl_user("Server stopped\n");

    return 0;
}
