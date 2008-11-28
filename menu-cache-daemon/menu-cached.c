/*
 *      menu-cached.c
 *      
 *      Copyright 2008 PCMan <pcman@thinkpad>
 *      
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *      
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *      
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

typedef struct _MenuCache
{
    int n_ref;
    char md5[33]; /* cache id */
    /* environment */
    char* menu_name;
    char* lang_name;
    char** env; /* XDG- env variables */

    /* files involved, and their monitors */
    int n_files;
    char** files;
    GFileMonitor** mons;
    /* FIXME: the cached file itself should be monitored, too. */

    /* clients requesting this menu cache */
    GSList* clients;
}MenuCache;

static GHashTable* hash = NULL;
guint delayed_reload_handler = 0;

static void on_file_changed( GFileMonitor* mon, GFile* gf, GFile* other,
                             GFileMonitorEvent evt, MenuCache* cache );

static void menu_cache_unref(MenuCache* cache)
{
    --cache->n_ref;
    /* g_debug("menu cache unrefed: ref_count=%d", cache->n_ref); */
    if( cache->n_ref == 0 )
    {
        int i;
        /* g_debug("menu cache freed"); */
        g_slist_foreach( cache->clients, (GFunc)g_io_channel_unref, NULL );
        g_slist_free( cache->clients );
        for(i = 0; i < cache->n_files; ++i)
        {
            g_file_monitor_cancel( cache->mons[i] );
            g_object_unref( cache->mons[i] );
        }
        g_free( cache->mons );
        g_strfreev( cache->env );
        g_strfreev( cache->files );
        g_slice_free( MenuCache, cache );
    }
    else if( cache->n_ref == 1 ) /* the last ref count is held by hash table */
    {
        /* g_debug("menu cache removed from hash"); */
        g_hash_table_remove( hash, cache->md5 );
        if( g_hash_table_size(hash) == 0 ) /* no menu cache is in use. */
        {
            g_hash_table_destroy(hash);
            hash = NULL;
            if( delayed_reload_handler )
            {
                g_source_remove( delayed_reload_handler );
                delayed_reload_handler = 0;
            }
        }
    }
}

static gboolean read_all_used_files( FILE* f, int* n_files, char*** used_files )
{
    char line[ 4096 ];
    int i, n;
    char** files;

    /* skip menu name */
    if( ! fgets( line, G_N_ELEMENTS(line), f ) )
        return FALSE;

    if( ! fgets( line, G_N_ELEMENTS(line), f ) )
        return FALSE;

    *n_files = n = atoi( line );
    files = g_new0( char*, n + 1 );

    for( i = 0; i < n; ++i )
    {
        int len;
        if( ! fgets( line, G_N_ELEMENTS(line), f ) )
            return FALSE;

        len = strlen( line );
        if( len <= 1 )
            return FALSE;
        files[ i ] = g_strndup( line, len - 1 ); /* don't include \n */
    }
    *used_files = files;
    return TRUE;
}

static void set_env( char* penv, const char* name )
{
    if( *penv )
        g_setenv( name, penv, TRUE);
    else
        g_unsetenv( name );
}

static void pre_exec( gpointer user_data )
{
    char** env = (char*)user_data;
    set_env(*env, "XDG_CONFIG_DIRS");
    set_env(*++env, "XDG_MENU_PREFIX");
    set_env(*++env, "XDG_DATA_DIRS");
    set_env(*++env, "XDG_CONFIG_HOME");
    set_env(*++env, "XDG_DATA_HOME");
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
    const char* const * argv[] = {
        BINDIR "/menu-cache-gen",
        "-l", NULL,
        "-i", NULL,
        "-o", NULL,
        "-f", NULL};
    argv[2] = lang_name;
    argv[4] = menu_name;
    argv[6] = cache_file;

    /* run menu-cache-gen */
    if( !g_spawn_sync(NULL, argv, NULL, 0,
                    pre_exec, env, NULL, NULL, &status, NULL ))
    {
        g_debug("error executing menu-cache-gen");
    }
    if( status != 0 )
        return FALSE;

    f = fopen( cache_file, "r" );
    if( f )
    {
        if( !read_all_used_files( f, &n_files, &files ) )
        {
            n_files = 0;
            files = NULL;
        }
        fclose(f);
    }
    else
        return FALSE;
    *n_used_files = n_files;
    *used_files = files;
    return TRUE;
}

static gboolean delayed_reload( MenuCache* cache )
{
    GSList* l;
    char buf[38];
    char* cache_file;
    int i;
    g_debug("Re-generation of cache is needed!");
    g_debug("call menu-cache-gen to re-generate the cache");
    memcpy( buf, "REL:", 4 );
    memcpy( buf + 4, cache->md5, 32 );
    buf[36] = '\n';
    buf[37] = '\0';

    cache_file = g_build_filename( g_get_user_cache_dir(), "menus", cache->md5, NULL );

    /* cancel old file monitors */
    g_strfreev(cache->files);
    for( i = 0; i < cache->n_files; ++i )
    {
        g_file_monitor_cancel( cache->mons[i] );
        g_signal_handlers_disconnect_by_func( cache->mons[i], on_file_changed, cache );
        g_object_unref( cache->mons[i] );
    }
    if( ! regenerate_cache( cache->menu_name, cache->lang_name, cache_file,
                            cache->env, &cache->n_files, &cache->files ) )
    {
        g_debug("regeneration of cache failed.");
    }
    g_free(cache_file);

    cache->mons = g_realloc( cache->mons, sizeof(GFileMonitor*)*(cache->n_files+1) );
    /* create required file monitors */
    for( i = 0; i < cache->n_files; ++i )
    {
        GFile* gf = g_file_new_for_path( cache->files[i] );
        cache->mons[i] = g_file_monitor( gf, 0, NULL, NULL );
        g_signal_connect( cache->mons[i], "changed", on_file_changed, cache);
        g_object_unref(gf);
    }
    /* notify the clients that reload is needed. */
    for( l = cache->clients; l; l = l->next )
    {
        GIOChannel* ch = (GIOChannel*)l->data;
        write(g_io_channel_unix_get_fd(ch), buf, 37 );
    }
    delayed_reload_handler = 0;
    return FALSE;
}

void on_file_changed( GFileMonitor* mon, GFile* gf, GFile* other,
                      GFileMonitorEvent evt, MenuCache* cache )
{
    /* g_debug("file %s is changed.", g_file_get_path(gf)); */
    if( delayed_reload_handler )
        g_source_remove(delayed_reload_handler);

    delayed_reload_handler = g_timeout_add_seconds_full( G_PRIORITY_LOW, 3, (GSourceFunc)delayed_reload, cache, NULL );
}

static gboolean menu_cache_file_is_updated( const char* menu_file, int* n_used_files, char** used_files )
{
    gboolean ret = FALSE;
    struct stat st;
    time_t cache_mtime;
    char** files;
    int n, i, l;
    FILE* f;
    char line[ 4096 ];

    f = fopen( menu_file, "r" );
    if( f )
    {
        if( fstat( fileno(f), &st) == 0 )
        {
            cache_mtime = st.st_mtime;
            if( read_all_used_files(f, &n, &files) )
            {
                for( i =0; i < n; ++i )
                {
                    if( stat( files[i], &st ) == -1 )
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
                else
                {
                    g_strfreev(files);
                }
            }
        }
_out:
        fclose( f );
    }
    return ret;
}

static void get_socket_name( char* buf, int len )
{
    char* dpy = g_getenv("DISPLAY");
    g_snprintf( buf, len, "/tmp/.menu-cached-%s-%s", dpy, g_get_user_name() );
}

static int create_socket()
{
    int fd = -1;
    struct sockaddr_un addr;

    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        g_print("Failed to create socket\n");
        return FALSE;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    get_socket_name( addr.sun_path, sizeof( addr.sun_path ) );

    if(bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
        g_debug("Failed to bind to socket");
        close(fd);
        return 1;
    }
    if(listen(fd, 30) < 0)
    {
        g_debug( "Failed to listen to socket" );
        close(fd);
        return 1;
    }
    return fd;
}

static gboolean on_client_data_in(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    char *line;
    gsize len;
    GIOStatus st;
    const char* md5;
    MenuCache* cache;

    g_debug("child data");
retry:
    st = g_io_channel_read_line( ch, &line, &len, NULL, NULL );
    if( st == G_IO_STATUS_AGAIN )
        goto retry;
    if( st != G_IO_STATUS_NORMAL )
        return FALSE;

    --len;
    line[len] = 0;

    if( memcmp(line, "REG:", 4) == 0 )
    {
        GChecksum *sum;
        int status = 0, n_files, i;
        char *pline = line + 4;
        char *sep, *menu_name, *lang_name, *cache_file;
        char **files;
        char **env;
        len -= 4;

        /* Format of received string, separated by '\t'.
         * Menu Name
         * Language Name
         * XDG_CONFIG_DIRS
         * XDG_MENU_PREFIX
         * XDG_DATA_DIRS
         * XDG_CONFIG_HOME
         * XDG_DATA_HOME */
        sum = g_checksum_new(G_CHECKSUM_MD5);
        g_checksum_update(sum, pline, len);
        md5 = g_checksum_get_string(sum);

        cache = (MenuCache*)g_hash_table_lookup(hash, md5);
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

            env = g_strsplit(pline, "\t", 0);

            /* FIXME: should obtain cache dir from client's env */
            cache_file = g_build_filename(g_get_user_cache_dir(), "menus", md5, NULL );
            if( ! menu_cache_file_is_updated(cache_file, &n_files, &files) )
            {
                /* run menu-cache-gen */
                if(! regenerate_cache( menu_name, lang_name, cache_file, env, &n_files, &files ) )
                {
                    g_debug("regeneration of cache failed!!");
                }
            }
            g_free(cache_file);
            cache = g_slice_new0( MenuCache );
            memcpy( cache->md5, md5, 33 );
            cache->n_files = n_files;
            cache->files = files;
            cache->menu_name = g_strdup(menu_name);
            cache->lang_name = g_strdup(lang_name);
            cache->env = env;
            cache->mons = g_new0(GFileMonitor*, n_files+1);
            /* create required file monitors */
            for( i = 0; i < n_files; ++i )
            {
                GFile* gf = g_file_new_for_path( files[i] );
                cache->mons[i] = g_file_monitor( gf, 0, NULL, NULL );
                g_signal_connect( cache->mons[i], "changed", on_file_changed, cache);
                g_object_unref(gf);
            }
            g_hash_table_insert(hash, cache->md5, cache);
            g_debug("new menu cache added to hash");
            cache->n_ref = 1;
        }
        /* g_debug("menu %s requested by client %d", md5, g_io_channel_unix_get_fd(ch)); */
        ++cache->n_ref;
        cache->clients = g_slist_prepend( cache->clients, ch );
        if( status  == 0 )
            write( g_io_channel_unix_get_fd(ch), md5, 32 );
        else
            write( g_io_channel_unix_get_fd(ch), "", 0 );
        g_checksum_free( sum );
    }
    else if( memcmp(line, "UNR:", 4) == 0 )
    {
        md5 = line + 4;
        g_debug("unregister: %s", md5);
        cache = (MenuCache*)g_hash_table_lookup(hash, md5);
        if( cache )
            menu_cache_unref(cache);
        else
            g_debug("bug! client is not found.");
    }
    g_free( line );

    return TRUE;
}

static gboolean on_client_closed(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    GHashTableIter it;
    char* md5;
    MenuCache* cache;
    GSList *l, *to_remove = NULL;
    g_hash_table_iter_init (&it, hash);
    while( g_hash_table_iter_next (&it, (gpointer*)&md5, (gpointer*)&cache) )
    {
        if( l = g_slist_find( cache->clients, ch ) )
        {
            to_remove = g_slist_prepend( to_remove, cache );
            cache->clients = g_slist_delete_link( cache->clients, l );
        }
    }
    /* g_debug("client closed"); */
    g_slist_foreach( to_remove, (GFunc)menu_cache_unref, NULL );
    g_slist_free( to_remove );
    return FALSE;
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
        g_debug("accept error");
        return TRUE;
    }

    child = g_io_channel_unix_new(client);
    g_io_add_watch( child, G_IO_PRI|G_IO_IN, on_client_data_in, NULL );
    g_io_add_watch( child, G_IO_HUP|G_IO_ERR, on_client_closed, NULL );
    g_io_channel_set_close_on_unref( child, TRUE );

    /* g_debug("new client accepted"); */
    return TRUE;
}

static void terminate(int sig)
{
/* #ifndef HAVE_ABSTRACT_SOCKETS */
    char path[256];
    get_socket_name(path, 256);
    unlink(path);
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
    GMainLoop* main_loop = g_main_loop_new( NULL, TRUE );
    GIOChannel* ch;
    int fd;

    signal(SIGHUP, terminate);
    signal(SIGINT, terminate);
    signal(SIGQUIT, terminate);
    signal(SIGTERM, terminate);
    signal(SIGKILL, terminate);
    signal(SIGPIPE, SIG_IGN);

    fd = create_socket();
    ch = g_io_channel_unix_new(fd);
    if(!ch)
        return 1;
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN|G_IO_PRI, on_new_conn_incoming, NULL);
    g_io_add_watch(ch, G_IO_ERR, on_server_conn_close, NULL);

    g_type_init();

    hash = g_hash_table_new_full(g_str_hash, g_str_equal,NULL,(GDestroyNotify)menu_cache_unref);

    g_main_loop_run( main_loop );
    g_main_loop_unref( main_loop );

    {
        char path[256];
        get_socket_name(path, 256 );
        unlink(path);
    }

    g_hash_table_destroy(hash);
    return 0;
}
