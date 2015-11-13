/*
 *      menu-cached.c
 *
 *      Copyright 2008 - 2010 PCMan <pcman.tw@gmail.com>
 *      Copyright 2009 Jürgen Hötzel <juergen@archlinux.org>
 *      Copyright 2012-2015 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
 *
 *      This file is a part of libmenu-cache package and created program
 *      should be not used without the library.
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2.1 of the License, or (at your option) any later version.
 *
 *      This library is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "menu-cache.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <utime.h>

#ifdef G_ENABLE_DEBUG
#define DEBUG(...)  g_debug(__VA_ARGS__)
#else
#define DEBUG(...)
#endif

typedef struct _Cache
{
    char md5[33]; /* cache id */
    /* environment */
    char* menu_name;
    char* lang_name;
    char** env; /* XDG- env variables */

    /* files involved, and their monitors */
    int n_files;
    char** files;
    GFileMonitor** mons;
    /* GFileMonitor* cache_mon; */

    gboolean need_reload;
    guint delayed_reload_handler;
    guint delayed_free_handler;

    /* clients requesting this menu cache */
    GSList* clients;

    /* cache file name for reference */
    char *cache_file;
}Cache;

static GMainLoop* main_loop = NULL;

static GHashTable* hash = NULL;

static char *socket_file = NULL;

static void on_file_changed( GFileMonitor* mon, GFile* gf, GFile* other,
                             GFileMonitorEvent evt, Cache* cache );

static gboolean delayed_cache_free(gpointer data)
{
    Cache* cache = data;
    int i;

    if(g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    g_hash_table_remove( hash, cache->md5 );
    /* DEBUG("menu cache freed"); */
    for(i = 0; i < cache->n_files; ++i)
    {
        g_file_monitor_cancel( cache->mons[i] );
        g_object_unref( cache->mons[i] );
    }
/*
    g_file_monitor_cancel(cache->cache_mon);
    g_object_unref(cache->cache_mon);
*/
    g_free( cache->mons );
    g_free(cache->menu_name);
    g_free(cache->lang_name);
    g_free(cache->cache_file);
    g_strfreev( cache->env );
    g_strfreev( cache->files );

    if( cache->delayed_reload_handler )
        g_source_remove( cache->delayed_reload_handler );

    g_slice_free( Cache, cache );

    if(g_hash_table_size(hash) == 0)
        g_main_loop_quit(main_loop);
    return FALSE;
}

static void cache_free(Cache* cache)
{
    /* shutdown cache in 10 minutes of inactivity */
    if(!cache->delayed_free_handler)
        cache->delayed_free_handler = g_timeout_add_seconds(600,
                                                            delayed_cache_free,
                                                            cache);
    DEBUG("menu %p cache unused, removing in 600s", cache);
}

static gboolean read_all_used_files( FILE* f, int* n_files, char*** used_files )
{
    char line[ 4096 ];
    int i, n, x;
    char** files;
    int ver_maj, ver_min;

    /* the first line of the cache file is version number */
    if( !fgets(line, G_N_ELEMENTS(line),f) )
        return FALSE;
    if( sscanf(line, "%d.%d", &ver_maj, &ver_min)< 2 )
        return FALSE;
    if (ver_maj != VER_MAJOR ||
        ver_min > VER_MINOR || ver_min < VER_MINOR_SUPPORTED)
        return FALSE;

    /* skip the second line containing menu name */
    if( ! fgets( line, G_N_ELEMENTS(line), f ) )
        return FALSE;

    /* num of files used */
    if( ! fgets( line, G_N_ELEMENTS(line), f ) )
        return FALSE;

    n = atoi( line );
    files = g_new0( char*, n + 1 );

    for( i = 0, x = 0; i < n; ++i )
    {
        int len;

        if( ! fgets( line, G_N_ELEMENTS(line), f ) )
            return FALSE;

        len = strlen( line );
        if( len <= 1 )
            return FALSE;
        files[ x ] = g_strndup( line, len - 1 ); /* don't include \n */
        if (g_file_test(files[x]+1, G_FILE_TEST_EXISTS))
            x++;
        else
        {
            DEBUG("ignoring not existant file from menu-cache-gen: %s", files[x]);
            g_free(files[x]);
            files[x] = NULL;
        }
    }
    *n_files = x;
    *used_files = files;
    return TRUE;
}

static void set_env( char* penv, const char* name )
{
    if( penv && *penv )
        g_setenv( name, penv, TRUE);
    else
        g_unsetenv( name );
}

static void pre_exec( gpointer user_data )
{
    char** env = (char**)user_data;
    set_env(*env, "XDG_CACHE_HOME");
    set_env(*++env, "XDG_CONFIG_DIRS");
    set_env(*++env, "XDG_MENU_PREFIX");
    set_env(*++env, "XDG_DATA_DIRS");
    set_env(*++env, "XDG_CONFIG_HOME");
    set_env(*++env, "XDG_DATA_HOME");
    set_env(*++env, "CACHE_GEN_VERSION");
}

static gboolean regenerate_cache( const char* menu_name,
                                  const char* lang_name,
                                  const char* cache_file,
                                  char** env,
                                  int* n_used_files,
                                  char*** used_files )
{
    FILE* f;
    int n_files, status = 0;
    char** files;
    const char* argv[] = {
        MENUCACHE_LIBEXECDIR "/menu-cache-gen",
        "-l", NULL,
        "-i", NULL,
        "-o", NULL,
        NULL};
    argv[2] = lang_name;
    argv[4] = menu_name;
    argv[6] = cache_file;

    /* DEBUG("cmd: %s", g_strjoinv(" ", argv)); */

    /* run menu-cache-gen */
    /* FIXME: is cast to (char**) valid here? */
    if( !g_spawn_sync(NULL, (char **)argv, NULL, 0,
                    pre_exec, env, NULL, NULL, &status, NULL ))
    {
        DEBUG("error executing menu-cache-gen");
    }
    /* DEBUG("exit status of menu-cache-gen: %d", status); */
    if( status != 0 )
        return FALSE;

    f = fopen( cache_file, "r" );
    if( f )
    {
        if( !read_all_used_files( f, &n_files, &files ) )
        {
            n_files = 0;
            files = NULL;
            /* DEBUG("error: read_all_used_files"); */
        }
        fclose(f);
    }
    else
        return FALSE;
    *n_used_files = n_files;
    *used_files = files;
    return TRUE;
}

static void do_reload(Cache* cache)
{
    GSList* l;
    char buf[38];
    int i;
    GFile* gf;

    /* DEBUG("Re-generation of cache is needed!"); */
    /* DEBUG("call menu-cache-gen to re-generate the cache"); */
    memcpy( buf, "REL:", 4 );
    memcpy( buf + 4, cache->md5, 32 );
    buf[36] = '\n';
    buf[37] = '\0';

    /* cancel old file monitors */
    g_strfreev(cache->files);
    for( i = 0; i < cache->n_files; ++i )
    {
        g_file_monitor_cancel( cache->mons[i] );
        g_signal_handlers_disconnect_by_func( cache->mons[i], on_file_changed, cache );
        g_object_unref( cache->mons[i] );
    }
/*
    g_file_monitor_cancel(cache->cache_mon);
    g_object_unref(cache->cache_mon);
*/
    if( ! regenerate_cache( cache->menu_name, cache->lang_name, cache->cache_file,
                            cache->env, &cache->n_files, &cache->files ) )
    {
        DEBUG("regeneration of cache failed.");
    }

    cache->mons = g_realloc( cache->mons, sizeof(GFileMonitor*)*(cache->n_files+1) );
    /* create required file monitors */
    for( i = 0; i < cache->n_files; ++i )
    {
        gf = g_file_new_for_path( cache->files[i] + 1 );
        if( cache->files[i][0] == 'D' )
            cache->mons[i] = g_file_monitor_directory( gf, 0, NULL, NULL );
        else
            cache->mons[i] = g_file_monitor_file( gf, 0, NULL, NULL );
        g_signal_connect(cache->mons[i], "changed",
                         G_CALLBACK(on_file_changed), cache);
        g_object_unref(gf);
    }
/*
    gf = g_file_new_for_path( cache_file );
    cache->cache_mon = g_file_monitor_file( gf, 0, NULL, NULL );
    g_signal_connect( cache->cache_mon, "changed", on_file_changed, cache);
    g_object_unref(gf);
*/

    /* notify the clients that reload is needed. */
    for( l = cache->clients; l; l = l->next )
    {
        GIOChannel* ch = (GIOChannel*)l->data;
        if(write(g_io_channel_unix_get_fd(ch), buf, 37) < 37)
            g_io_channel_shutdown(ch, FALSE, NULL);
    }
    cache->need_reload = FALSE;
}

static gboolean delayed_reload( Cache* cache )
{
    if(g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    cache->delayed_reload_handler = 0;
    if(cache->need_reload)
        do_reload(cache);

    return FALSE;
}

void on_file_changed( GFileMonitor* mon, GFile* gf, GFile* other,
                      GFileMonitorEvent evt, Cache* cache )
{
#ifdef G_ENABLE_DEBUG
    char *path = g_file_get_path(gf);
    g_debug("file %s is changed (%d).", path, evt);
    g_free(path);
#endif
    /* if( mon != cache->cache_mon ) */
    {
        /* Optimization: Some files in the dir are changed, but it
         * won't affect the content of the menu. So, just omit them,
         * and update the mtime of the cached file with utime.
         */
        int idx;
        /* dirty hack: get index of the monitor in array */
        for(idx = 0; idx < cache->n_files; ++idx)
        {
            if(mon == cache->mons[idx])
                break;
        }
        /* if the monitored file is a directory */
        if( G_LIKELY(idx < cache->n_files) && cache->files[idx][0] == 'D' )
        {
            char* changed_file = g_file_get_path(gf);
            char* dir_path = cache->files[idx]+1;
            int len = strlen(dir_path);
            /* if the changed file is a file in the monitored dir */
            if( strncmp(changed_file, dir_path, len) == 0 && changed_file[len] == '/' )
            {
                char* base_name = changed_file + len + 1;
                gboolean skip = TRUE;
                /* only *.desktop and *.directory files can affect the content of the menu. */
                if( g_str_has_suffix(base_name, ".desktop") )
                {
                        skip = FALSE;
                }
                else if( g_str_has_suffix(base_name, ".directory") )
                    skip = FALSE;

                if( skip )
                {
                    DEBUG("files are changed, but no re-generation is needed.");
                    return;
                }
            }
            g_free(changed_file);
        }
    }

    if( cache->delayed_reload_handler )
    {
        /* we got some change in last 3 seconds... not reload again */
        cache->need_reload = TRUE;
        g_source_remove(cache->delayed_reload_handler);
    }
    else
        /* no reload in last 3 seconds... good, do it immediately */
        do_reload(cache);

    cache->delayed_reload_handler = g_timeout_add_seconds_full( G_PRIORITY_LOW, 3, (GSourceFunc)delayed_reload, cache, NULL );
}

static gboolean cache_file_is_updated( const char* cache_file, int* n_used_files, char*** used_files )
{
    gboolean ret = FALSE;
    struct stat st;
#if 0
    time_t cache_mtime;
    char** files;
    int n, i;
#endif
    FILE* f;

    f = fopen( cache_file, "r" );
    if( f )
    {
        if( fstat( fileno(f), &st) == 0 )
        {
#if 0
            cache_mtime = st.st_mtime;
            if( read_all_used_files(f, &n, &files) )
            {
                for( i =0; i < n; ++i )
                {
                    /* files[i][0] is 'D' or 'F' indicating file type. */
                    if( stat( files[i] + 1, &st ) == -1 )
                        continue;
                    if( st.st_mtime > cache_mtime )
                        break;
                }
                if( i >= n )
                {
                    ret = TRUE;
                    *n_used_files = n;
                    *used_files = files;
                }
            }
#else
            ret = read_all_used_files(f, n_used_files, used_files);
#endif
        }
        fclose( f );
    }
    return ret;
}

static void get_socket_name( char* buf, int len )
{
    char* dpy = g_strdup(g_getenv("DISPLAY"));
    if(dpy && *dpy)
    {
        char* p = strchr(dpy, ':');
        for(++p; *p && *p != '.' && *p != '\n';)
            ++p;
        if(*p)
            *p = '\0';
    }
    g_snprintf( buf, len, "%s/.menu-cached-%s-%s", g_get_tmp_dir(),
                dpy ? dpy : ":0", g_get_user_name() );
    g_free(dpy);
}

static int create_socket(struct sockaddr_un *addr)
{
    int fd = -1;

    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        DEBUG("Failed to create socket");
        return -1;
    }

    /* remove previous socket file */
    if (unlink(addr->sun_path) < 0) {
        if (errno != ENOENT)
            g_error("Couldn't remove previous socket file %s", addr->sun_path);
    }
    /* remove of previous socket file successful */
    else
        g_warning("removed previous socket file %s", addr->sun_path);

    if(bind(fd, (struct sockaddr *)addr, sizeof(*addr)) < 0)
    {
        DEBUG("Failed to bind to socket");
        close(fd);
        return -1;
    }
    if(listen(fd, 30) < 0)
    {
        DEBUG( "Failed to listen to socket" );
        close(fd);
        return -1;
    }
    return fd;
}

static void on_client_closed(gpointer user_data)
{
    GIOChannel* ch = user_data;
    GHashTableIter it;
    char* md5;
    Cache* cache;
    GSList *l;
    DEBUG("client closed: %p", ch);
    g_hash_table_iter_init (&it, hash);
    while( g_hash_table_iter_next (&it, (gpointer*)&md5, (gpointer*)&cache) )
    {
        while((l = g_slist_find( cache->clients, ch )) != NULL)
        {
            /* FIXME: some clients are closed accidentally without
             * unregister the menu first due to crashes.
             * We need to do unref for them to prevent memory leaks.
             *
             * The behavior is not currently well-defined.
             * if a client do unregister first, and then was closed,
             * unref will be called twice and incorrect ref. counting
             * will happen.
             */
            cache->clients = g_slist_delete_link( cache->clients, l );
            DEBUG("remove channel from cache %p", cache);
            if(cache->clients == NULL)
                cache_free(cache);
        }
    }
    /* DEBUG("client closed"); */
}

static gboolean on_client_data_in(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    char *line;
    gsize len;
    GIOStatus st;
    const char* md5;
    Cache* cache;
    GFile* gf;
    gboolean ret = TRUE;

    if(cond & (G_IO_HUP|G_IO_ERR) )
    {
        return FALSE; /* remove the watch */
    }

retry:
    st = g_io_channel_read_line( ch, &line, &len, NULL, NULL );
    if( st == G_IO_STATUS_AGAIN )
        goto retry;
    if( st != G_IO_STATUS_NORMAL )
        return FALSE;

    --len;
    line[len] = 0;

    DEBUG("line(%d) = %s", (int)len, line);

    if( memcmp(line, "REG:", 4) == 0 )
    {
        int n_files = 0, i;
        char *pline = line + 4;
        char *sep, *menu_name, *lang_name, *cache_dir;
        char **files = NULL;
        char **env;
        char reload_cmd[38] = "REL:";

        len -= 4;
        /* Format of received string, separated by '\t'.
         * Menu Name
         * Language Name
         * XDG_CACHE_HOME
         * XDG_CONFIG_DIRS
         * XDG_MENU_PREFIX
         * XDG_DATA_DIRS
         * XDG_CONFIG_HOME
         * XDG_DATA_HOME
         * (optional) CACHE_GEN_VERSION
         * md5 hash */

        md5 = pline + len - 32; /* the md5 hash of this menu */

        cache = (Cache*)g_hash_table_lookup(hash, md5);
        if( !cache )
        {
            sep = strchr(pline, '\t');
            *sep = '\0';
            menu_name = pline;
            pline = sep + 1;

            sep = strchr(pline, '\t');
            *sep = '\0';
            lang_name = pline;
            pline = sep + 1;

            ((char *)md5)[-1] = '\0';
            env = g_strsplit(pline, "\t", 0);

            cache_dir = env[0]; /* XDG_CACHE_HOME */
            /* obtain cache dir from client's env */

            cache = g_slice_new0( Cache );
            cache->cache_file = g_build_filename(*cache_dir ? cache_dir : g_get_user_cache_dir(), "menus", md5, NULL );
            if( ! cache_file_is_updated(cache->cache_file, &n_files, &files) )
            {
                /* run menu-cache-gen */
                if(! regenerate_cache( menu_name, lang_name, cache->cache_file, env, &n_files, &files ) )
                {
                    DEBUG("regeneration of cache failed!!");
                }
            }
            else
            {
                /* file loaded, schedule update anyway */
                cache->need_reload = TRUE;
                cache->delayed_reload_handler = g_timeout_add_seconds_full(G_PRIORITY_LOW, 3,
                                                    (GSourceFunc)delayed_reload, cache, NULL);
            }
            memcpy( cache->md5, md5, 33 );
            cache->n_files = n_files;
            cache->files = files;
            cache->menu_name = g_strdup(menu_name);
            cache->lang_name = g_strdup(lang_name);
            cache->env = env;
            cache->mons = g_new0(GFileMonitor*, n_files+1);
            /* create required file monitors */
            DEBUG("%d files/dirs are monitored.", n_files);
            for( i = 0; i < n_files; ++i )
            {
                gf = g_file_new_for_path( files[i] + 1 );
                if( files[i][0] == 'D' )
                    cache->mons[i] = g_file_monitor_directory( gf, 0, NULL, NULL );
                else
                    cache->mons[i] = g_file_monitor_file( gf, 0, NULL, NULL );
                DEBUG("monitor: %s", g_file_get_path(gf));
                g_signal_connect(cache->mons[i], "changed",
                                 G_CALLBACK(on_file_changed), cache);
                g_object_unref(gf);
            }
            /*
            gf = g_file_new_for_path( cache_file );
            cache->cache_mon = g_file_monitor_file( gf, 0, NULL, NULL );
            g_signal_connect( cache->cache_mon, "changed", on_file_changed, cache);
            g_object_unref(gf);
            */
            g_hash_table_insert(hash, cache->md5, cache);
            DEBUG("new menu cache %p added to hash", cache);
        }
        else if (access(cache->cache_file, R_OK) != 0)
        {
            /* bug SF#657: if user deleted cache file we have to regenerate it */
            if (!regenerate_cache(cache->menu_name, cache->lang_name, cache->cache_file,
                                  cache->env, &cache->n_files, &cache->files))
            {
                DEBUG("regeneration of cache failed.");
            }
        }
        /* DEBUG("menu %s requested by client %d", md5, g_io_channel_unix_get_fd(ch)); */
        cache->clients = g_slist_prepend(cache->clients, ch);
        if(cache->delayed_free_handler)
        {
            g_source_remove(cache->delayed_free_handler);
            cache->delayed_free_handler = 0;
        }
        DEBUG("client %p added to cache %p", ch, cache);

        /* generate a fake reload notification */
        DEBUG("fake reload!");
        memcpy(reload_cmd + 4, md5, 32);

        reload_cmd[36] = '\n';
        reload_cmd[37] = '\0';

        DEBUG("reload command: %s", reload_cmd);
        ret = write(g_io_channel_unix_get_fd(ch), reload_cmd, 37) > 0;
    }
    else if( memcmp(line, "UNR:", 4) == 0 )
    {
        md5 = line + 4;
        DEBUG("unregister: %s", md5);
        cache = (Cache*)g_hash_table_lookup(hash, md5);
        if(cache && cache->clients)
        {
            /* remove the IO channel from the cache */
            cache->clients = g_slist_remove(cache->clients, ch);
            if(cache->clients == NULL)
                cache_free(cache);
        }
        else
            DEBUG("bug! client is not found.");
    }
    g_free( line );

    return ret;
}

static gboolean on_new_conn_incoming(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    int server, client;
    socklen_t client_addrlen;
    struct sockaddr client_addr;
    GIOChannel* child;

    server = g_io_channel_unix_get_fd(ch);

    client_addrlen = sizeof(client_addr);
    client = accept(server, &client_addr, &client_addrlen);
    if( client == -1 )
    {
        DEBUG("accept error");
        return TRUE;
    }

    child = g_io_channel_unix_new(client);
    g_io_channel_set_close_on_unref( child, TRUE );
    g_io_add_watch_full(child, G_PRIORITY_DEFAULT, G_IO_PRI|G_IO_IN|G_IO_HUP|G_IO_ERR,
                        on_client_data_in, child, on_client_closed);
    g_io_channel_unref(child);

    DEBUG("new client accepted: %p", child);
    return TRUE;
}

static void terminate(int sig)
{
/* #ifndef HAVE_ABSTRACT_SOCKETS */
    unlink(socket_file);
    exit(0);
/* #endif */
}

static gboolean on_server_conn_close(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    /* FIXME: is this possible? */
    /* the server socket is accidentally closed. terminate the server. */
    terminate(SIGTERM);
    return TRUE;
}

int main(int argc, char** argv)
{
    GIOChannel* ch;
    int server_fd;
    struct sockaddr_un addr;
#ifndef DISABLE_DAEMONIZE
    int fd, pid;

    long open_max;
    long i;
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (argc < 2)
        /* old way, not recommended! */
        get_socket_name( addr.sun_path, sizeof( addr.sun_path ) );
    else if (strlen(argv[1]) >= sizeof(addr.sun_path))
        /* ouch, it's too big! */
        return 1;
    else
        /* this is a good way! */
        strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);

    socket_file = addr.sun_path;

    server_fd = create_socket(&addr);

    if( server_fd < 0 )
        return 1;

#ifndef DISABLE_DAEMONIZE
    /* Become a daemon */
    if ((pid = fork()) < 0) {
        g_error("can't fork");
    }
    else if (pid != 0) {
        /* exit parent */
        exit(0);
    }

    /* reset session to forget about parent process completely */
    setsid();

    /* change working directory to root, so previous working directory
     * can be unmounted */
    if (chdir("/") < 0) {
        g_error("can't change directory to /");
    }

    open_max = sysconf (_SC_OPEN_MAX);
    for (i = 0; i < open_max; i++)
    {
        /* don't hold open fd opened besides server socket */
        if (i != server_fd)
            fcntl (i, F_SETFD, FD_CLOEXEC);
    }

    /* /dev/null for stdin, stdout, stderr */
    fd = open ("/dev/null", O_RDONLY);
    if (fd != -1)
    {
        dup2 (fd, 0);
        close (fd);
    }
    fd = open ("/dev/null", O_WRONLY);
    if (fd != -1)
    {
        dup2 (fd, 1);
        dup2 (fd, 2);
        close (fd);
    }
#endif

    signal(SIGHUP, terminate);
    signal(SIGINT, terminate);
    signal(SIGQUIT, terminate);
    signal(SIGTERM, terminate);
    signal(SIGPIPE, SIG_IGN);

    ch = g_io_channel_unix_new(server_fd);
    if(!ch)
        return 1;
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN|G_IO_PRI, on_new_conn_incoming, NULL);
    g_io_add_watch(ch, G_IO_ERR, on_server_conn_close, NULL);
    g_io_channel_unref(ch);

#if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
#endif

    hash = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

    main_loop = g_main_loop_new( NULL, TRUE );
    g_main_loop_run( main_loop );
    g_main_loop_unref( main_loop );

    unlink(addr.sun_path);

    g_hash_table_destroy(hash);
    return 0;
}
