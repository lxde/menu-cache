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

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

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

static char* set_env( char* penv, const char* name )
{
    char* sep = strchr(penv, '\t');
    if( G_LIKELY(sep) )
        *sep = '\0';
    if( *penv )
        g_setenv( name, penv, TRUE);
    else
        g_unsetenv( name );
    return (sep + 1);
}

static void pre_exec( gpointer user_data )
{
    char* env = (char*)user_data;
    env = set_env(env, "XDG_CONFIG_DIRS");
    env = set_env(env, "XDG_MENU_PREFIX");
    env = set_env(env, "XDG_DATA_DIRS");
    env = set_env(env, "XDG_CONFIG_HOME");
    set_env(env, "XDG_DATA_HOME");
}

static gboolean on_client_in(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    char *line;
    gsize len;
    GIOStatus st;
    const char* md5;

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
        int status = 0;
        char *argv[] = {"/usr/bin/menu-cache-gen", "-l", NULL, "-i", NULL, "-o", NULL, NULL};
        char *pline = line + 4;
        char *sep, *menu_name, *lang_name;
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

        sep = strchr(pline, '\t');
        *sep = '\0';
        menu_name = pline;
        pline = sep + 1;

        sep = strchr(pline, '\t');
        *sep = '\0';
        lang_name = pline;
        pline = sep + 1;

        argv[2] = lang_name;
        argv[4] = menu_name;
        /* FIXME: should obtain cache dir from client's env */
        argv[6] = g_build_filename(g_get_user_cache_dir(), "menus", md5, NULL );

        if( !g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                        pre_exec, pline, NULL, NULL, &status, NULL ))
        {
            g_debug("error executing menu-cache-gen");
        }
        g_free( argv[6] );

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
    }
    g_free( line );

    return TRUE;
}

static gboolean on_client_closed(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    g_debug("client closed");
    return FALSE;
}

static gboolean on_conn_in(GIOChannel* ch, GIOCondition cond, gpointer user_data)
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
    g_io_add_watch( child, G_IO_PRI|G_IO_IN, on_client_in, NULL );
    g_io_add_watch( child, G_IO_HUP|G_IO_ERR, on_client_closed, NULL );
    g_io_channel_set_close_on_unref( child, TRUE );

    return TRUE;
}

static gboolean on_conn_close(GIOChannel* ch, GIOCondition cond, gpointer user_data)
{
    return TRUE;
}

int main(int argc, char** argv)
{
    GMainLoop* main_loop = g_main_loop_new( NULL, TRUE );
    GIOChannel* ch;
    int fd = create_socket();
    ch = g_io_channel_unix_new(fd);
    if(!ch)
        return 1;
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN|G_IO_PRI, on_conn_in, NULL);
    g_io_add_watch(ch, G_IO_ERR, on_conn_close, NULL);

    g_main_loop_run( main_loop );
    g_main_loop_unref( main_loop );

    {
        char path[256];
        get_socket_name(path, 256 );
        unlink(path);
    }
    return 0;
}
