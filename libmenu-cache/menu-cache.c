/*
 *      menu-cache.c
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
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/fcntl.h>
#include <errno.h>

#include "menu-cache.h"

struct _MenuCacheItem
{
    guint n_ref;
    MenuCacheType type;
    char* id;
    char* name;
    char* comment;
    char* icon;
    GData* extended;
    MenuCacheDir* parent;
};

struct _MenuCacheDir
{
    MenuCacheItem item;
    const char* file_dir;
    char* file_name;
    GSList* children;
};

struct _MenuCacheApp
{
    MenuCacheItem item;
    const char* file_dir;
    char* exec;
    char* working_dir;
/*    char** categories;    */
    gboolean use_terminal : 1;
    gboolean use_sn : 1;
};

struct _MenuCache
{
    guint n_ref;
    MenuCacheDir* root_dir;
    char* menu_name;
    char md5[32];
    char* cache_file;
    char** all_used_files;
    int n_all_used_files;
    time_t mtime;
    time_t time;
    GSList* notifiers;
};

static int server_fd = -1;
static GIOChannel* server_ch = NULL;
static GHashTable* hash = NULL;


/* Don't call this API directly. Use menu_cache_lookup instead. */
static MenuCache* menu_cache_new( const char* cache_file, char** include, char** exclude );

static gboolean connect_server();
static MenuCache* register_menu_to_server( const char* menu_name, gboolean re_register );
static void unregister_menu_from_server( MenuCache* cache );


void menu_cache_init()
{

}

static MenuCacheItem* read_item(  FILE* f, MenuCache* cache );

static void read_item_extended(  FILE* f, MenuCacheItem* item )
{
    char line[4096];
    int len;

    while( fgets( line, G_N_ELEMENTS(line) - 1, f ) )
    {
        if( line[0] == '\n' || line[0] == '\0' ) /* end of item info */
            break;
        len = strlen( line );
        if( G_LIKELY(len > 1) )
        {
            char* sep = strchr( line, '=' );
            if( G_UNLIKELY( ! sep ) )
                continue;
            if( G_UNLIKELY( ! item->extended ) )
                g_datalist_init( &item->extended );
            line[len] = '\0';
            *sep = '\0';
            g_datalist_set_data_full( &item->extended, line, g_strdup( sep + 1 ), g_free );
        }
    }
}

static void read_dir( FILE* f, MenuCacheDir* dir, MenuCache* cache )
{
    char line[4096];
    int len, idx;
    MenuCacheItem* item;

    /* desktop file dir */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return;
    idx = atoi( line );
    if( G_LIKELY( idx >=0 && idx < cache->n_all_used_files ) )
        dir->file_dir = cache->all_used_files[ idx ];

    /* file name */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return;
    len = strlen( line );
    if( G_LIKELY(len > 1) )
        dir->file_name = g_strndup( line, len - 1 );

    /* load extended key/values */
    read_item_extended( f, dir );

    /* load child items in the dir */
    while( item = read_item( f, cache ) )
    {
        item->parent = menu_cache_item_ref(dir);
        dir->children = g_slist_prepend( dir->children, item );
    }

    dir->children = g_slist_reverse( dir->children );
}

static void read_app( FILE* f, MenuCacheApp* app, MenuCache* cache )
{
    char line[4096];
    int len, idx;

    /* desktop file dir */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return;
    idx = atoi( line );
    if( G_LIKELY( idx >=0 && idx < cache->n_all_used_files ) )
        app->file_dir = cache->all_used_files[ idx ];

    /* exec */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return;
    len = strlen( line );
    if( G_LIKELY(len > 1) )
        app->exec = g_strndup( line, len - 1 );

    /* terminal */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return;
    app->use_terminal = line[0] == '1' ? TRUE : FALSE;

    /* startup notify */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return;
    app->use_sn = line[0] == '1' ? TRUE : FALSE;

    read_item_extended( f, MENU_CACHE_ITEM(app) );
}

static MenuCacheItem* read_item(  FILE* f, MenuCache* cache )
{
    MenuCacheItem* item;
    char line[4096];
    int len;

    /* desktop/menu id */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return NULL;
    len = strlen( line );

    if( G_LIKELY(len >= 2) )
    {
        if( line[0] == '+' ) /* menu dir */
        {
            item = (MenuCacheItem*)g_slice_new0( MenuCacheDir );
            item->n_ref = 1;
            item->type = MENU_CACHE_TYPE_DIR;
        }
        else if( line[0] == '-' ) /* menu item */
        {
            item = (MenuCacheItem*)g_slice_new0( MenuCacheApp );
            item->n_ref = 1;
            if( G_LIKELY( line[1] != '\n' ) ) /* application item */
                item->type = MENU_CACHE_TYPE_APP;
            else /* separator */
            {
                item->type = MENU_CACHE_TYPE_SEP;
                return item;
            }
        }
        else
            return NULL;

        item->id = g_strndup( line + 1, len - 2 );
    }
    else
        return NULL;

    /* name */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return item;
    len = strlen( line );
    if( G_LIKELY(len > 1) )
        item->name = g_strndup( line, len - 1 );

    /* comment */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return item;
    len = strlen( line );
    if( G_LIKELY(len > 1) )
        item->comment = g_strndup( line, len - 1 );

    /* icon */
    if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
        return item;
    len = strlen( line );
    if( G_LIKELY(len > 1) )
        item->icon = g_strndup( line, len - 1 );

    if( item->type == MENU_CACHE_TYPE_DIR )
        read_dir( f, MENU_CACHE_DIR(item), cache );
    else if( item->type == MENU_CACHE_TYPE_APP )
        read_app( f, MENU_CACHE_APP(item), cache );

    return item;
}

static gboolean read_all_used_files( FILE* f, int* n_all_used_files, char*** all_used_files )
{
    char line[ 4096 ];
    int i, n;
    char** dirs;

    if( ! fgets( line, G_N_ELEMENTS(line), f ) )
        return FALSE;

    *n_all_used_files = n = atoi( line );
    dirs = g_new0( char*, n + 1 );

    for( i = 0; i < n; ++i )
    {
        int len;
        if( ! fgets( line, G_N_ELEMENTS(line), f ) )
            return FALSE;

        len = strlen( line );
        if( len <= 1 )
            return FALSE;
        dirs[ i ] = g_strndup( line, len - 1 ); /* don't include \n */
    }
    *all_used_files = dirs;
    return TRUE;
}

MenuCache* menu_cache_new( const char* cache_file, char** include, char** exclude )
{
    MenuCache* cache;
    struct stat st;
    char line[4096];

    FILE* f = fopen( cache_file, "r" );
    if( ! f )
        return NULL;

    if( fstat( fileno( f ), &st ) == -1 )
    {
        fclose( f );
        return NULL;
    }

    if( ! fgets( line, G_N_ELEMENTS(line) ,f ) )
        return NULL;

    cache = g_slice_new0( MenuCache );
    cache->mtime = st.st_mtime;

    cache->cache_file = g_strdup( cache_file );
    /* cache->menu_file_path = g_strdup( strtok(line, "\n") ); */

    /* get all used files */
    if( ! read_all_used_files( f, &cache->n_all_used_files, &cache->all_used_files ) )
    {
        g_slice_free( MenuCache, cache );
        return NULL;
    }
    cache->n_ref = 1;
    cache->root_dir = (MenuCacheDir*)read_item( f, cache );
    cache->time = time(NULL);    /* save current time */
    fclose( f );
    return cache;
}

MenuCache* menu_cache_ref(MenuCache* cache)
{
    g_atomic_int_inc( &cache->n_ref );
    return cache;
}

void menu_cache_unref(MenuCache* cache)
{
    if( g_atomic_int_dec_and_test( &cache->n_ref ) )
    {
        unregister_menu_from_server( cache );
        g_hash_table_remove( hash, cache->menu_name );

        if( G_LIKELY(cache->root_dir) )
            menu_cache_item_unref( cache->root_dir );
        g_free( cache->cache_file );
        g_free( cache->menu_name );
        /* g_free( cache->menu_file_path ); */
        g_strfreev( cache->all_used_files );
        g_slice_free( MenuCache, cache );
    }
}

MenuCacheDir* menu_cache_get_root_dir( MenuCache* cache )
{
    return cache->root_dir;
}


MenuCacheItem* menu_cache_item_ref(MenuCacheItem* item)
{
    g_atomic_int_inc( &item->n_ref );
    return item;
}

typedef struct _CacheReloadNotifier
{
    GFunc func;
    gpointer user_data;
}CacheReloadNotifier;

gpointer menu_cache_add_reload_notify(MenuCache* cache, GFunc func, gpointer user_data)
{
    GSList* l = g_slist_alloc();
    CacheReloadNotifier* n = g_slice_new(CacheReloadNotifier);
    n->func = func;
    n->user_data = user_data;
    l->data = n;
    cache->notifiers = g_slist_concat( cache->notifiers, l );
    return l;
}

void menu_cache_remove_reload_notify(MenuCache* cache, gpointer notify_id)
{
    g_slice_free( CacheReloadNotifier, ((GSList*)notify_id)->data );
    cache->notifiers = g_list_delete_link( cache->notifiers, (GSList*)notify_id );
}

static void reload_notify( MenuCache* cache )
{
    GSList* l;
    for( l = cache->notifiers; l; l = l->next )
    {
        CacheReloadNotifier* n = (CacheReloadNotifier*)l->data;
        n->func( cache, n->user_data );
    }
}

gboolean menu_cache_reload( MenuCache* cache )
{
    struct stat st;
    char line[4096];

    FILE* f = fopen( cache->cache_file, "r" );
    if( ! f )
        return FALSE;

    if( fstat( fileno( f ), &st ) == -1 )
    {
        fclose( f );
        return FALSE;
    }

    if( ! fgets( line, G_N_ELEMENTS(line) ,f ) )
    {
        fclose( f );
        return FALSE;
    }
    cache->mtime = st.st_mtime;
    g_strfreev( cache->all_used_files );

    /* get all used files */
    if( ! read_all_used_files( f, &cache->n_all_used_files, &cache->all_used_files ) )
    {
        g_slice_free( MenuCache, cache );
        fclose(f);
        return FALSE;
    }
    menu_cache_item_unref( cache->root_dir );
    cache->root_dir = (MenuCacheDir*)read_item( f, cache );
    cache->time = time(NULL);    /* save current time */
    fclose( f );

    reload_notify(cache);
    
    return TRUE;
}

void menu_cache_item_unref(MenuCacheItem* item)
{
    if( g_atomic_int_dec_and_test( &item->n_ref ) )
    {
        g_free( item->id );
        g_free( item->name );
        g_free( item->comment );
        g_free( item->icon );
        
        if( item->parent )
            menu_cache_item_unref( MENU_CACHE_ITEM(item->parent) );

        if( item->extended )
            g_datalist_clear( item->extended );

        if( item->type == MENU_CACHE_TYPE_DIR )
        {
            MenuCacheDir* dir = MENU_CACHE_DIR(item);
            g_free( dir->file_name );
            g_slist_foreach( dir->children, (GFunc)menu_cache_item_unref, NULL );
            g_slist_free( dir->children );
            g_slice_free( MenuCacheDir, item );
        }
        else
        {
            MenuCacheApp* app = MENU_CACHE_APP(item);
            g_free( app->exec );
            g_slice_free( MenuCacheApp, item );
        }
    }
}

MenuCacheType menu_cache_item_get_type( MenuCacheItem* item )
{
    return item->type;
}

const char* menu_cache_item_get_id( MenuCacheItem* item )
{
    return item->id;
}

const char* menu_cache_item_get_name( MenuCacheItem* item )
{
    return item->name;
}

const char* menu_cache_item_get_comment( MenuCacheItem* item )
{
    return item->comment;
}

const char* menu_cache_item_get_icon( MenuCacheItem* item )
{
    return item->icon;
}

const char* menu_cache_item_get_extended( MenuCacheItem* item, const char* key )
{
    return g_datalist_get_data( &item->extended, key );
}

const char* menu_cache_item_get_qextended( MenuCacheItem* item, GQuark key )
{
    return g_datalist_id_get_data( &item->extended, key );
}

MenuCacheDir* menu_cache_item_get_parent( MenuCacheItem* item )
{
    return item->parent;
}

GSList* menu_cache_dir_get_children( MenuCacheDir* dir )
{
    return dir->children;
}

const char* menu_cache_dir_get_file_basename( MenuCacheDir* dir )
{
    return dir->file_name;
}

const char* menu_cache_dir_get_file_dirname( MenuCacheDir* dir )
{
    return dir->file_dir;
}

const char* menu_cache_app_get_exec( MenuCacheApp* app )
{
    return app->exec;
}

const char* menu_cache_app_get_file_dirname( MenuCacheApp* app )
{
    return app->file_dir;
}

const char* menu_cache_app_get_working_dir( MenuCacheApp* app )
{
    return app->working_dir;
}

char** menu_cache_app_get_categories( MenuCacheApp* app )
{
    return NULL;
}

gboolean menu_cache_app_get_use_terminal( MenuCacheApp* app )
{
    return app->use_terminal;
}

gboolean menu_cache_app_get_use_sn( MenuCacheApp* app )
{
    return app->use_sn;
}

GSList* menu_cache_list_all_apps()
{
    return NULL;
}

MenuCacheApp* menu_cache_find_app_by_exec( const char* exec )
{
    return NULL;
}

MenuCacheDir* menu_cache_get_dir_from_path( MenuCache* cache, const char* path )
{
    char** names = g_strsplit( path + 1, "/", -1 );
    int i = 0;
    MenuCacheDir* dir = NULL;

    /* the topmost dir of the path should be the root menu dir. */
    if( strcmp(names[0], MENU_CACHE_ITEM(cache->root_dir)->id) )
        return NULL;

    dir = cache->root_dir;
    for( ++i; names[i]; ++i )
    {
        GSList* l;
        for( l = dir->children; l; l = l->next )
        {
            MenuCacheItem* item = MENU_CACHE_ITEM(l->data);
            if( item->type == MENU_CACHE_TYPE_DIR && 0 == strcmp( item->id, names[i] ) )
                dir = item;
        }
        if( ! dir )
            return NULL;
    }
    return dir;
}

char* menu_cache_dir_make_path( MenuCacheDir* dir )
{
    GString* path = g_string_sized_new(1024);

    while( dir ) /* this is not top dir */
    {
        g_string_prepend( path, menu_cache_item_get_id(dir) );
        g_string_prepend_c( path, '/' );
        dir = MENU_CACHE_ITEM(dir)->parent;
    }
    return g_string_free( path, FALSE );
}

gboolean menu_cache_file_is_updated( const char* menu_file )
{
    gboolean ret = TRUE;
    struct stat st;
    time_t cache_mtime;
    char** dirs;
    int n, i;
    FILE* f;

    f = fopen( menu_file, "r" );
    if( f )
    {
        if( fstat( fileno(f), &st) == 0 )
        {
            char line[1024];
            /* skip menu name on first line. */
            if( ! fgets(line, 1024, f) )
            {
                fclose(f);
                return FALSE;
            }
            if( read_all_used_files( f, &n, &dirs ) )
            {
                cache_mtime = st.st_mtime;
                for( i = 0; i < n; ++i )
                {
                    if( stat( dirs[i], &st ) == -1 )
                        continue;

                    if( st.st_mtime > cache_mtime )
                    {
                        ret = FALSE;
                        break;
                    }
                }
            }
        }
        fclose( f );
        g_strfreev( dirs );
    }
    return ret;
}

static void get_socket_name( char* buf, int len )
{
    char* dpy = g_getenv("DISPLAY");
    g_snprintf( buf, len, "/tmp/.menu-cached-%s-%s", dpy, g_get_user_name() );
}

#define MAX_RETRIES 25

static gboolean fork_server()
{
    const char *server_path = g_find_program_in_path("menu-cached");
    int ret, pid, status;

    if (!server_path)
    {
        g_print("failed to find menu-cached\n");
    }

    /* Become a daemon */
    pid = fork();
    if (pid == 0)
    {
        int fd;
        long open_max;
        long i;

        /* don't hold open fd opened from the client of the library */
        open_max = sysconf (_SC_OPEN_MAX);
        for (i = 0; i < open_max; i++)
            fcntl (i, F_SETFD, FD_CLOEXEC);

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
        setsid();
        if (fork() == 0)
        {
            execl( server_path, server_path, NULL);
            g_print("failed to exec %s\n", server_path);
        }
        _exit(0);
    }

    /*
     * do a waitpid on the intermediate process to avoid zombies.
     */
retry_wait:
    ret = waitpid(pid, &status, 0);
    if (ret < 0) {
        if (errno == EINTR)
            goto retry_wait;
    }
    return TRUE;
}

static gboolean on_server_io(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    GIOStatus st;
    char* line;
    gsize len;

    if( cond & (G_IO_ERR|G_IO_HUP) )
    {
reconnect:
        g_debug("IO error, try to re-connect.");
        g_io_channel_unref(ch);
        server_fd = -1;
        if( ! connect_server() )
        {
            g_print("fail to re-connect to the server.\n");
        }
        else
        {
            GHashTableIter it;
            char* menu_name;
            MenuCache* cache;
            g_debug("successfully restart server.\nre-register menus.");
            /* re-register all menu caches */
            g_hash_table_iter_init(&it, hash);
            while(g_hash_table_iter_next(&it, (gpointer*)&menu_name, (gpointer*)&cache)) 
                register_menu_to_server( cache->md5, TRUE );
        }
        return FALSE;
    }

    if( cond & (G_IO_IN|G_IO_PRI) )
    {
    retry:
        st = g_io_channel_read_line( ch, &line, &len, NULL, NULL );
        if( st == G_IO_STATUS_AGAIN )
            goto retry;
        if ( st != G_IO_STATUS_NORMAL || len < 4 )
        {
            g_debug("server IO error!!");
            goto reconnect;
            return FALSE;
        }
        g_debug("server line: %s", line);
        if( 0 == memcmp( line, "REL:", 4 ) ) /* reload */
        {
            GHashTableIter it;
            char* menu_name;
            MenuCache* cache;
            line[len - 1] = '\0';
            char* menu_cache_id = line + 4;
            g_debug("server ask us to reload cache: %s", menu_cache_id);

            g_hash_table_iter_init(&it, hash);
            while(g_hash_table_iter_next(&it, (gpointer*)&menu_name, (gpointer*)&cache))
            {
                if(0 == memcmp(cache->md5, menu_cache_id, 32))
                {
                    g_debug("RELOAD!");
                    menu_cache_reload(cache);
                    break;
                }
            }
        }

        g_free( line );
    }
    return TRUE;
}

static gboolean connect_server()
{
    int fd;
    struct sockaddr_un addr;
    int retries = 0;

    if( server_fd != -1 )
        return TRUE;

retry:
    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        g_print("Failed to create socket\n");
        return FALSE;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    get_socket_name( addr.sun_path, sizeof( addr.sun_path ) );

    if( connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        if(retries == 0)
        {
            close(fd);
            unlink(addr.sun_path); /* remove previous socket file */
            fork_server();
            ++retries;
            goto retry;
        }
        if (retries < MAX_RETRIES)
        {
            close(fd);
            usleep(5000);
            ++retries;
            goto retry;
        }
        g_print("Unable to connect\n");
        close(fd);
        return FALSE;
    }
    server_fd = fd;
    server_ch = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(server_ch, TRUE);
    g_io_add_watch(server_ch, G_IO_IN|G_IO_PRI|G_IO_ERR|G_IO_HUP, on_server_io, NULL);
    return TRUE;
}

MenuCache* register_menu_to_server( const char* menu_name, gboolean re_register )
{
    MenuCache* cache;
    const gchar * const * langs = g_get_language_names();
    const char* xdg_cfg = g_getenv("XDG_CONFIG_DIRS");
    const char* xdg_prefix = g_getenv("XDG_MENU_PREFIX");
    const char* xdg_data = g_getenv("XDG_DATA_DIRS");
    const char* xdg_cfg_home = g_getenv("XDG_CONFIG_HOME");
    const char* xdg_data_home = g_getenv("XDG_DATA_HOME");
    char* buf;
    char md5[36];
    char* file_name;
    int len = 0, r;

    if( !xdg_cfg )
        xdg_cfg = "";
    if( ! xdg_prefix )
        xdg_prefix = "";
    if( ! xdg_data )
        xdg_data = "";
    if( ! xdg_cfg_home )
        xdg_cfg_home = "";
    if( ! xdg_data_home )
        xdg_data_home = "";

    /* get rid of the encoding part of locale name. */
    while( strchr(langs[0], '.') )
        ++langs;

    buf = g_strdup_printf( "REG:%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                            menu_name,
                            *langs,
                            xdg_cfg,
                            xdg_prefix,
                            xdg_data,
                            xdg_cfg_home,
                            xdg_data_home );
    write( server_fd, buf, strlen(buf) );

    while( (r=read(server_fd, md5 + len, 32 - len)) > 0 && len < 32 )
        len += r;

    md5[32] = '\0';

    if( r == -1 )
    {
        g_debug("server error!");
        return NULL;
    }
    g_free( buf );

    if( len != 32 || re_register )
        return NULL;

    file_name = g_build_filename( g_get_user_cache_dir(), "menus", md5, NULL );
    g_debug("cache file_name = %s", file_name);
    cache = menu_cache_new( file_name, NULL, NULL );
    memcpy( cache->md5, md5, 32 );
    cache->menu_name = g_strdup(menu_name);
    g_free( file_name );

    g_hash_table_insert( hash, g_strdup(menu_name), cache );
    return cache;
}

void unregister_menu_from_server( MenuCache* cache )
{
    char buf[38];
    g_snprintf( buf, "UNR:%s\n", cache->md5 );
    write( server_fd, buf, 37 );
}

MenuCache* menu_cache_lookup( const char* menu_name )
{
    MenuCache* cache;
    char* file_name;

    /* lookup in a hash table for already loaded menus */
    if( G_UNLIKELY( ! hash ) )
        hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, menu_cache_unref );
    else
    {
        cache = (MenuCache*)g_hash_table_lookup(hash, menu_name);
        if( cache )
            return menu_cache_ref(cache);
    }

    if( !connect_server() )
    {
        g_print("unable to connect to menu-cached.\n");
        return NULL;
    }
    return register_menu_to_server( menu_name, FALSE );
}
