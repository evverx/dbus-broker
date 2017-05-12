/*
 * D-Bus Broker Launcher
 */

#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include <fcntl.h>
#include <getopt.h>
#include <glib.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>
#include "util/error.h"

typedef struct Manager Manager;
typedef struct Service Service;

enum {
        _MAIN_SUCCESS,
        MAIN_EXIT,
        MAIN_FAILED,
};

struct Service {
        Manager *manager;
        CRBNode rb;
        char *name;
        char *exec;
        char *unit;
        char id[];
};

struct Manager {
        sd_event *event;
        sd_bus *bus_controller;
        sd_bus *bus_regular;
        int fd_listen;
        CRBTree services;
        uint64_t service_ids;
};

static const char *     main_arg_broker = "/usr/bin/dbus-broker";
static bool             main_arg_force = false;
static const char *     main_arg_listen = NULL;
static const char *     main_arg_scope = "system";
static const char *     main_arg_servicedir = NULL;
static bool             main_arg_verbose = false;

static int service_compare(CRBTree *t, void *k, CRBNode *n) {
        Service *service = c_container_of(n, Service, rb);

        return strcmp(k, service->id);
}

static Service *service_free(Service *service) {
        if (!service)
                return NULL;

        c_rbtree_remove_init(&service->manager->services, &service->rb);
        free(service->unit);
        free(service->exec);
        free(service->name);
        free(service);

        return NULL;
}

C_DEFINE_CLEANUP(Service *, service_free);

static int service_new(Service **servicep, Manager *manager, const char *name, const char *exec, const char *unit) {
        _c_cleanup_(service_freep) Service *service = NULL;
        CRBNode **slot, *parent;

        service = calloc(1, sizeof(*service) + C_DECIMAL_MAX(uint64_t) + 1);
        if (!service)
                return error_origin(-ENOMEM);

        service->manager = manager;
        service->rb = (CRBNode)C_RBNODE_INIT(service->rb);
        sprintf(service->id, "%" PRIu64, ++manager->service_ids);

        service->name = strdup(name);
        if (!service->name)
                return error_origin(-ENOMEM);

        if (exec) {
                service->exec = strdup(exec);
                if (!service->exec)
                        return error_origin(-ENOMEM);
        }

        if (unit) {
                service->unit = strdup(unit);
                if (!service->unit)
                        return error_origin(-ENOMEM);
        }

        slot = c_rbtree_find_slot(&manager->services, service_compare, service->id, &parent);
        assert(slot);
        c_rbtree_add(&manager->services, parent, slot, &service->rb);

        *servicep = service;
        service = NULL;
        return 0;
}

static Manager *manager_free(Manager *manager) {
        Service *service;

        if (!manager)
                return NULL;

        while ((service = c_container_of(manager->services.root, Service, rb)))
                service_free(service);

        c_close(manager->fd_listen);
        sd_bus_unref(manager->bus_regular);
        sd_bus_unref(manager->bus_controller);
        sd_event_unref(manager->event);
        free(manager);

        return NULL;
}

C_DEFINE_CLEANUP(Manager *, manager_free);

static int manager_new(Manager **managerp) {
        _c_cleanup_(manager_freep) Manager *manager = NULL;
        int r;

        manager = calloc(1, sizeof(*manager));
        if (!manager)
                return error_origin(-ENOMEM);

        manager->fd_listen = -1;

        r = sd_event_default(&manager->event);
        if (r < 0)
                return error_origin(r);

        r = sd_event_add_signal(manager->event, NULL, SIGTERM, NULL, NULL);
        if (r < 0)
                return error_origin(r);

        r = sd_event_add_signal(manager->event, NULL, SIGINT, NULL, NULL);
        if (r < 0)
                return error_origin(r);

        r = sd_bus_new(&manager->bus_controller);
        if (r < 0)
                return error_origin(r);

        *managerp = manager;
        manager = NULL;
        return 0;
}

static int manager_listen_inherit(Manager *manager) {
        _c_cleanup_(c_closep) int s = -1;
        int r, n;

        assert(manager->fd_listen < 0);

        n = sd_listen_fds(true);
        if (n < 0)
                return error_origin(n);

        if (n == 0) {
                fprintf(stderr, "No listener socket inherited\n");
                return MAIN_FAILED;
        }
        if (n > 1) {
                fprintf(stderr, "More than one listener socket passed\n");
                return MAIN_FAILED;
        }

        s = SD_LISTEN_FDS_START;

        r = sd_is_socket(s, PF_UNIX, SOCK_STREAM, 1);
        if (r < 0)
                return error_origin(r);

        if (!r) {
                fprintf(stderr, "Non unix-domain-socket passed as listener\n");
                return MAIN_FAILED;
        }

        r = fcntl(s, F_GETFL);
        if (r < 0)
                return error_origin(-errno);

        r = fcntl(s, F_SETFL, r | O_NONBLOCK);
        if (r < 0)
                return error_origin(-errno);

        manager->fd_listen = s;
        s = -1;
        return 0;
}

static int manager_listen_path(Manager *manager, const char *path) {
        _c_cleanup_(c_closep) int s = -1;
        struct sockaddr_un addr = {};
        int r;

        assert(manager->fd_listen < 0);

        s = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (s < 0)
                return error_origin(-errno);

        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path, strlen(path));
        r = bind(s, (struct sockaddr *)&addr, offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
        if (r < 0)
                return error_origin(-errno);

        r = listen(s, 256);
        if (r < 0)
                return error_origin(-errno);

        manager->fd_listen = s;
        s = -1;
        return 0;
}

static noreturn void manager_run_child(Manager *manager, int fd_controller) {
        char str_controller[C_DECIMAL_MAX(int) + 1];
        char *argv[] = {
                "dbus-broker",
                "-v",
                "--controller",
                str_controller,
                NULL,
        };
        int r;

        r = prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (r) {
                r = error_origin(-errno);
                goto exit;
        }

        r = fcntl(fd_controller, F_GETFD);
        if (r < 0) {
                r = error_origin(-errno);
                goto exit;
        }

        r = fcntl(fd_controller, F_SETFD, r & ~FD_CLOEXEC);
        if (r < 0) {
                r = error_origin(-errno);
                goto exit;
        }

        r = snprintf(str_controller, sizeof(str_controller), "%d", fd_controller);
        assert(r < (ssize_t)sizeof(str_controller));

        r = execve(main_arg_broker, argv, environ);
        r = error_origin(-errno);

exit:
        _exit(1);
}

static int manager_on_child_exit(sd_event_source *source, const siginfo_t *si, void *userdata) {
        if (main_arg_verbose)
                fprintf(stderr, "Caught SIGCHLD of broker\n");

        return sd_event_exit(sd_event_source_get_event(source), 0);
}

static int manager_fork(Manager *manager, int fd_controller) {
        pid_t pid;
        int r;

        pid = fork();
        if (pid < 0)
                return error_origin(-errno);

        if (!pid)
                manager_run_child(manager, fd_controller);

        r = sd_event_add_child(manager->event, NULL, pid, WEXITED, manager_on_child_exit, manager);
        if (r < 0)
                return error_origin(-errno);

        close(fd_controller);
        return 0;
}

static int manager_on_name_activate(Manager *manager, sd_bus_message *m, const char *id) {
        _c_cleanup_(sd_bus_message_unrefp) sd_bus_message *signal = NULL;
        Service *service;
        int r;

        service = c_rbtree_find_entry(&manager->services,
                                      service_compare,
                                      id,
                                      Service,
                                      rb);
        if (!service) {
                fprintf(stderr, "Activation request on unknown name '%s'\n", id);
                return 0;
        } else if (!strcmp(service->name, "org.freedesktop.systemd1")) {
                /* pid1 activation requests are silently ignored */
                return 0;
        } else if (!service->unit) {
                fprintf(stderr, "Missing systemd service to serve activation request on name '%s'\n", service->name);
                return 0;
        }

        if (main_arg_verbose)
                fprintf(stderr, "Activation request for '%s' -> '%s'\n", service->name, service->unit);

        r = sd_bus_message_new_signal(manager->bus_regular, &signal, "/org/freedesktop/DBus", "org.freedesktop.systemd1.Activator", "ActivationRequest");
        if (r < 0)
                return error_origin(r);

        r = sd_bus_message_append(signal, "s", service->unit);
        if (r < 0)
                return error_origin(r);

        r = sd_bus_message_set_destination(signal, "org.freedesktop.systemd1");
        if (r < 0)
                return error_origin(r);

        r = sd_bus_send(manager->bus_regular, signal, NULL);
        if (r < 0)
                return error_origin(r);

        return 0;
}

static int manager_on_message(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        Manager *manager = userdata;
        const char *path, *suffix;
        int r = 0;

        path = sd_bus_message_get_path(m);
        if (!path)
                return 0;

        suffix = c_string_prefix(path, "/org/bus1/DBus/Name/");
        if (suffix) {
                if (sd_bus_message_is_signal(m, "org.bus1.DBus.Name", "Activate"))
                        r = manager_on_name_activate(manager, m, suffix);
        }

        return error_trace(r);
}

static int manager_load_service(Manager *manager, const char *path) {
        gchar *name = NULL, *exec = NULL, *user = NULL, *unit = NULL;
        _c_cleanup_(service_freep) Service *service = NULL;
        _c_cleanup_(c_freep) char *object_path = NULL;
        GKeyFile *f;
        int r;

        /*
         * There seems to be no trivial way to properly parse D-Bus service
         * files. Hence, we resort to glib GKeyFile to parse it as a Desktop
         * File compatible ini-file.
         *
         * Preferably, we'd not have the glib dependency here, but it does not
         * hurt much either. If anyone cares, feel free to provide `c-ini'.
         */

        if (main_arg_verbose)
                fprintf(stderr, "Loading service '%s'\n", path);

        f = g_key_file_new();

        if (!g_key_file_load_from_file(f, path, G_KEY_FILE_NONE, NULL)) {
                fprintf(stderr, "Cannot load service file '%s'\n", path);
                r = 0;
                goto exit;
        }

        name = g_key_file_get_string(f, "D-BUS Service", "Name", NULL);
        exec = g_key_file_get_string(f, "D-BUS Service", "Exec", NULL);
        user = g_key_file_get_string(f, "D-BUS Service", "User", NULL);
        unit = g_key_file_get_string(f, "D-BUS Service", "SystemdService", NULL);

        if (!name) {
                fprintf(stderr, "Missing name in service file '%s'\n", path);
                r = 0;
                goto exit;
        }

        /*
         * XXX: @user is unused so far, and we pass `0' as uid to dbus-broker.
         *      Preferably, we would resolve @user to a uid, but we also do not
         *      want to call into NSS..
         *      For now, using 'root' seems good enough.
         */

        r = service_new(&service, manager, name, exec, unit);
        if (r) {
                r = error_trace(r);
                goto exit;
        }

        r = asprintf(&object_path, "/org/bus1/DBus/Name/%s", service->id);
        if (r < 0) {
                r = error_origin(-ENOMEM);
                goto exit;
        }

        r = sd_bus_call_method(manager->bus_controller,
                               NULL,
                               "/org/bus1/DBus/Broker",
                               "org.bus1.DBus.Broker",
                               "AddName",
                               NULL,
                               NULL,
                               "osu",
                               object_path,
                               service->name,
                               0);
        if (r < 0) {
                r = error_origin(r);
                goto exit;
        }

        service = NULL;
        r = 0;

exit:
        g_free(unit);
        g_free(user);
        g_free(exec);
        g_free(name);
        g_key_file_free(f);
        return r;
}

static int manager_load(Manager *manager) {
        const char suffix[] = ".service";
        _c_cleanup_(c_closedirp) DIR *dir;
        const char *dirpath;
        struct dirent *de;
        char *path;
        size_t n;
        int r;

        if (main_arg_servicedir)
                dirpath = main_arg_servicedir;
        else if (!strcmp(main_arg_scope, "user"))
                dirpath = "/usr/share/dbus-1/services";
        else if (!strcmp(main_arg_scope, "system"))
                dirpath = "/usr/share/dbus-1/system-services";
        else
                return error_origin(-ENOTRECOVERABLE);

        dir = opendir(dirpath);
        if (!dir) {
                if (errno == ENOENT || errno == ENOTDIR)
                        return 0;
                else
                        return error_origin(-errno);
        }

        for (errno = 0, de = readdir(dir);
             de;
             errno = 0, de = readdir(dir)) {
                if (de->d_name[0] == '.')
                        continue;

                n = strlen(de->d_name);
                if (n <= strlen(suffix))
                        continue;
                if (strcmp(de->d_name + n - strlen(suffix), suffix))
                        continue;

                r = asprintf(&path, "%s/%s", dirpath, de->d_name);
                if (r < 0)
                        return error_origin(-ENOMEM);

                r = manager_load_service(manager, path);
                free(path);
                if (r)
                        return error_trace(r);
        }
        if (errno > 0)
                return error_origin(-errno);

        return 0;
}

static int manager_connect(Manager *manager) {
        _c_cleanup_(sd_bus_unrefp) sd_bus *b = NULL;
        _c_cleanup_(c_closep) int s = -1;
        struct sockaddr_un addr;
        socklen_t n_addr = sizeof(addr);
        int r;

        assert(!manager->bus_regular);

        s = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (s < 0)
                return error_origin(-errno);

        r = getsockname(manager->fd_listen, (struct sockaddr *)&addr, &n_addr);
        if (r < 0)
                return error_origin(-r);

        r = connect(s, (struct sockaddr *)&addr, n_addr);
        if (r < 0)
                return error_origin(-r);

        r = sd_bus_new(&b);
        if (r < 0)
                return error_origin(r);

        r = sd_bus_set_fd(b, s, s);
        if (r < 0)
                return error_origin(r);

        s = -1;

        r = sd_bus_set_bus_client(b, true);
        if (r < 0)
                return error_origin(r);

        r = sd_bus_start(b);
        if (r < 0)
                return error_origin(r);

        manager->bus_regular = b;
        b = NULL;
        return 0;
}

static int manager_run(Manager *manager) {
        int r, controller[2];

        assert(manager->fd_listen >= 0);

        r = socketpair(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, controller);
        if (r < 0)
                return error_origin(-errno);

        /* consumes FD controller[0] */
        r = sd_bus_set_fd(manager->bus_controller, controller[0], controller[0]);
        if (r < 0) {
                close(controller[0]);
                close(controller[1]);
                return error_origin(r);
        }

        /* consumes FD controller[1] */
        r = manager_fork(manager, controller[1]);
        if (r) {
                close(controller[1]);
                return error_trace(r);
        }

        r = sd_bus_add_filter(manager->bus_controller, NULL, manager_on_message, manager);
        if (r < 0)
                return error_origin(r);

        r = sd_bus_start(manager->bus_controller);
        if (r < 0)
                return error_origin(r);

        r = manager_load(manager);
        if (r)
                return error_trace(r);

        r = sd_bus_call_method(manager->bus_controller,
                               NULL,
                               "/org/bus1/DBus/Broker",
                               "org.bus1.DBus.Broker",
                               "AddListener",
                               NULL,
                               NULL,
                               "oh",
                               "/org/bus1/DBus/Listener/0",
                               manager->fd_listen);
        if (r < 0)
                return error_origin(r);

        r = manager_connect(manager);
        if (r)
                return error_trace(r);

        r = sd_bus_attach_event(manager->bus_controller, manager->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return error_origin(r);

        r = sd_bus_attach_event(manager->bus_regular, manager->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return error_origin(r);

        r = sd_event_loop(manager->event);
        if (r < 0)
                return error_origin(r);

        return 0;
}

static void help(void) {
        printf("%s [GLOBALS...] ...\n\n"
               "Linux D-Bus Message Broker Launcher\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "  -v --verbose          Print progress to terminal\n"
               "     --listen PATH      Specify path of listener socket\n"
               "  -f --force            Ignore existing listener sockets\n"
               "     --scope SCOPE      Scope of message bus\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_LISTEN,
                ARG_SCOPE,
        };
        static const struct option options[] = {
                { "help",               no_argument,            NULL,   'h'                     },
                { "version",            no_argument,            NULL,   ARG_VERSION             },
                { "verbose",            no_argument,            NULL,   'v'                     },
                { "listen",             required_argument,      NULL,   ARG_LISTEN              },
                { "force",              no_argument,            NULL,   'f'                     },
                { "scope",              required_argument,      NULL,   ARG_SCOPE               },
                {}
        };
        int c;

        while ((c = getopt_long(argc, argv, "hvf", options, NULL)) >= 0) {
                switch (c) {
                case 'h':
                        help();
                        return MAIN_EXIT;

                case ARG_VERSION:
                        printf("dbus-broker-launch %d\n", PACKAGE_VERSION);
                        return MAIN_EXIT;

                case 'v':
                        main_arg_verbose = true;
                        break;

                case ARG_LISTEN:
                        main_arg_listen = optarg;
                        break;

                case 'f':
                        main_arg_force = true;
                        break;

                case ARG_SCOPE:
                        if (strcmp(optarg, "system") &&
                            strcmp(optarg, "user")) {
                                fprintf(stderr, "%s: invalid message bus scope -- '%s'\n", program_invocation_name, optarg);
                                return MAIN_FAILED;
                        }

                        main_arg_scope = optarg;
                        break;

                case '?':
                        /* getopt_long() prints warning */
                        return MAIN_FAILED;

                default:
                        return error_origin(-EINVAL);
                }
        }

        if (optind != argc) {
                fprintf(stderr, "%s: invalid arguments -- '%s'\n", program_invocation_name, argv[optind]);
                return MAIN_FAILED;
        }

        return 0;
}

static int run(void) {
        _c_cleanup_(manager_freep) Manager *manager = NULL;
        _c_cleanup_(c_freep) char *listen_path = NULL;
        const char *t, *path = NULL, *unlink_path = NULL;
        int r;

        r = manager_new(&manager);
        if (r)
                return error_trace(r);

        if (main_arg_listen) {
                path = main_arg_listen;
        } else if (!strcmp(main_arg_scope, "user")) {
                t = getenv("XDG_RUNTIME_DIR");
                if (t)
                        r = asprintf(&listen_path, "%s/bus", t);
                else
                        r = asprintf(&listen_path, "/var/run/user/%u/bus", getuid());
                if (r < 0)
                        return error_origin(-ENOMEM);
        } else if (!strcmp(main_arg_scope, "system")) {
                path = "/var/run/dbus/system_bus_socket";
        } else {
                return error_origin(-ENOTRECOVERABLE);
        }

        if (!strcmp(path, "inherit")) {
                r = manager_listen_inherit(manager);
                if (r)
                        return error_trace(r);

                if (main_arg_verbose)
                        fprintf(stderr, "Listening on inherited socket\n");
        } else if (path[0] == '/') {
                if (main_arg_force) {
                        r = unlink(path);
                        if (r < 0) {
                                if (errno != ENOENT)
                                        return error_origin(-errno);
                                else if (main_arg_verbose)
                                        fprintf(stderr, "No conflict on socket '%s'\n", path);
                        } else if (main_arg_verbose) {
                                fprintf(stderr, "Forcibly removed conflicting socket '%s'\n", path);
                        }
                }

                r = manager_listen_path(manager, path);
                if (r)
                        return error_trace(r);

                unlink_path = path;

                if (main_arg_verbose)
                        fprintf(stderr, "Listening on socket '%s'\n", path);
        } else {
                fprintf(stderr, "Invalid listener socket '%s'\n", path);
                return MAIN_FAILED;
        }

        r = manager_run(manager);
        r = error_trace(r);

        if (unlink_path) {
                r = unlink(unlink_path);
                if (r < 0)
                        return error_origin(-errno);

                if (main_arg_verbose)
                        fprintf(stderr, "Cleaned up listener socket '%s'\n", unlink_path);
        }

        return r;

}

int main(int argc, char **argv) {
        sigset_t mask_new, mask_old;
        int r;

        r = parse_argv(argc, argv);
        if (r)
                goto exit;

        sigemptyset(&mask_new);
        sigaddset(&mask_new, SIGCHLD);
        sigaddset(&mask_new, SIGTERM);
        sigaddset(&mask_new, SIGINT);

        sigprocmask(SIG_BLOCK, &mask_new, &mask_old);
        r = run();
        sigprocmask(SIG_SETMASK, &mask_old, NULL);

exit:
        r = error_trace(r);
        if (r < 0 && main_arg_verbose)
                fprintf(stderr, "Exiting due to fatal error: %d\n", r);
        return (r == 0 || r == MAIN_EXIT) ? 0 : 1;
}
