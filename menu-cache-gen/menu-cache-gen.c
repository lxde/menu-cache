/*
 *      menu-cache-gen.c
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

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gmenu-tree.h"
#include "version.h"
#include "menu-cache.h"

static char* ifile = NULL;
static char* ofile = NULL;
static char* lang = NULL;
static gboolean force = FALSE;

GOptionEntry opt_entries[] =
{
/*
    {"force", 'f', 0, G_OPTION_ARG_NONE, &force, "Force regeneration of cache even if it's up-to-date.", NULL },
*/
    {"input", 'i', 0, G_OPTION_ARG_FILENAME, &ifile, "Source *.menu file to read", NULL },
    {"output", 'o', 0, G_OPTION_ARG_FILENAME, &ofile, "Output file to write cache to", NULL },
    {"lang", 'l', 0, G_OPTION_ARG_STRING, &lang, "Language", NULL },
    {0}
};

GHashTable* dir_hash = NULL;

/* all used app dirs.
 * defined in gmenu-tree.c
 */
extern GSList* all_used_dirs;
extern GSList* all_used_files;

static int n_known_de = N_KNOWN_DESKTOPS;
static GHashTable* de_hash = NULL;

/* Convert desktop environment names to bitmasks.
 * Since it's impossible to have OnlyShowIn=<a list of 32 desktop environments>,
 * I think a 32 bit integer is quite enough. If it's not, in the future we can
 * use guint64 here.
 */
guint32 menu_cache_get_de_flag( const char* de_name )
{
    gpointer val = g_hash_table_lookup(de_hash, de_name);
    guint32 flag;
    if( G_LIKELY(val) )
        flag = GPOINTER_TO_UINT(val);
    else
    {
        if( G_UNLIKELY(n_known_de > 31) )
        {
            g_debug("only up to 31 different DEs are supported");
            return 0;
        }
        flag = (1 << n_known_de);
        ++n_known_de;
        g_hash_table_insert(de_hash, g_strdup(de_name), GUINT_TO_POINTER(flag));
    }
    /* g_debug("flag of %s is %d", de_name, flag); */
    return flag;
}

static int dirname_index( const char* dir )
{
    GSList* l;
    int i = 0;
    for( l = all_used_dirs; l; l = l->next )
    {
        if( strcmp(dir, l->data) == 0 )
            return i;
        ++i;
    }
    return -1;
}

#if 0
void write_item_ex_info( FILE* of, const char* desktop_file )
{
    gsize len;
    GKeyFile* kf = g_key_file_new();
    if( g_key_file_load_from_file( kf, desktop_file, 0, NULL ) )
    {
        char** keys = g_key_file_get_keys( kf, "Desktop Entry", &len, NULL );
        char** key;
        char* val;
        char** vals;
        gsize n_vals;

        for( key = keys; *key; ++key )
        {
            if( g_str_has_prefix( *key, "X-" ) )
            {
                char* val = g_key_file_get_value( kf, "Desktop Entry", *key, NULL );
                fprintf( of, "%s=%s\n", *key, val );
                g_free( val );
            }
        }
        g_strfreev( keys );
    }
    g_key_file_free( kf );
}
#endif

static void write_entry( FILE* of, GMenuTreeEntry* item )
{
    char* str;
    int flags = 0;

    if( gmenu_tree_entry_get_is_nodisplay(item) /* || gmenu_tree_entry_get_is_excluded(item) */ )
        return;

    /* dekstop id, not necessarily the same as file basename */
    fprintf( of, "-%s\n", gmenu_tree_entry_get_desktop_file_id( item ) );

    /* Name */
    fprintf( of, "%s\n", gmenu_tree_entry_get_name( item ) );

    /* Comment */
    str = gmenu_tree_entry_get_comment( item );
    fprintf( of, "%s\n", str ? str : "" );

    /* Icon */
    str = gmenu_tree_entry_get_icon( item );
    fprintf( of, "%s\n", str ? str : "" );

    /* file dir/basename */
    if( gmenu_tree_entry_get_desktop_file_path( item ) )
    {
        /* file basenames are the same as desktop ids, except that sometimes
         * the '/' in paths are replaced with '-'.
         * for ex, /usr/share/applications/foo/bar.desktop has the app dir
         * /usr/share/applications, the filename foo/bar.desltop, and the
         * desktop id: foo-bar.desktop
         */

        /* filename */
        str = g_path_get_basename( gmenu_tree_entry_get_desktop_file_path( item ) );
        if( strcmp(str, gmenu_tree_entry_get_desktop_file_id(item) ) )
            fprintf( of, "%s\n", str );
        else
            fprintf( of, "\n" );
        g_free( str );

        /* dirname */
        str = g_path_get_dirname( gmenu_tree_entry_get_desktop_file_path( item ) );
        fprintf( of, "%d\n", dirname_index( str) );
        g_free( str );
    }
    else
    {
        fprintf( of, "\n-1\n" );
    }

    /* GenericName */
    str = gmenu_tree_entry_get_generic_name( item );
    fprintf( of, "%s\n", str ? str : "" );

    /* Exec */
    fprintf( of, "%s\n", gmenu_tree_entry_get_exec( item ) );

    /* Terminal/StartupNotify flags */
    if( gmenu_tree_entry_get_launch_in_terminal( item ) )
        flags |= FLAG_USE_TERMINAL;
    if( gmenu_tree_entry_get_use_startup_notify( item ) )
        flags |= FLAG_USE_SN;
    fprintf( of, "%u\n", flags );

    /* ShowIn info */
    fprintf( of, "%d\n", gmenu_tree_entry_get_show_in_flags(item) );
/*
    if( gmenu_tree_entry_get_desktop_file_path( item ) )
        write_item_ex_info(of, gmenu_tree_entry_get_desktop_file_path( item ));
    fputs( "\n", of );
*/

}

static void write_dir( FILE* of, GMenuTreeDirectory* dir )
{
    GSList* l;
    const char* str;

    fprintf( of, "+%s\n", gmenu_tree_directory_get_menu_id( dir ) );
    fprintf( of, "%s\n", gmenu_tree_directory_get_name( dir ) );
    str = gmenu_tree_directory_get_comment( dir );
    fprintf( of, "%s\n", str ? str : "" );
    str = gmenu_tree_directory_get_icon( dir );
    fprintf( of, "%s\n", str ? str : "" );

    if( gmenu_tree_directory_get_desktop_file_path( dir ) )
    {
        /* get basename of its desktop file. */
        str = g_path_get_basename( gmenu_tree_directory_get_desktop_file_path( dir ) );
        fprintf( of, "%s\n", str );
        g_free( str );

        /* get the location of its desktop file. */
        str = g_path_get_dirname( gmenu_tree_directory_get_desktop_file_path( dir ) );
        fprintf( of, "%d\n", dirname_index( str ) );
        g_free( str );
    }
    else
    {
        fprintf( of, "\n-1\n" );
    }

    // fprintf( of, "\n" );    /* end of item info */

    for( l = gmenu_tree_directory_get_contents(dir); l; l = l->next )
    {
        GMenuTreeItem* item = (GMenuTreeItem*)l->data;
        GMenuTreeItemType type = gmenu_tree_item_get_type(item);

        if( type == GMENU_TREE_ITEM_DIRECTORY )
        {
            write_dir( of, (GMenuTreeDirectory*)item );
        }
        else if( type == GMENU_TREE_ITEM_ENTRY )
        {
            write_entry( of, (GMenuTreeEntry*)item );
        }
        else if( type == GMENU_TREE_ITEM_SEPARATOR )
            fputs( "-\n", of );
    }
    fputs( "\n", of );
}

#if 0
static gboolean is_src_newer( const char* src, const char* dest )
{
    struct stat src_st, dest_st;

    if( stat(dest, &dest_st) == -1 )
        return TRUE;

    if( stat(src, &src_st) == -1 )
        return TRUE;

    return (src_st.st_mtime > dest_st.st_mtime);
}

static gboolean is_menu_uptodate()
{
    MenuCacheDir* menu;
    struct stat menu_st, cache_st;
    GHashTable* hash;
    GList *dirs, *l;
    gboolean ret = TRUE;

    if( is_src_newer( ifile, ofile ) )
        return FALSE;

    return menu_cache_file_is_updated( ofile );
}
#endif

static void write_de_name(char* de_name, gpointer _flag, FILE* of)
{
    guint32 flag = GPOINTER_TO_UINT(_flag);
    if( flag >= (1 << N_KNOWN_DESKTOPS) ) /* only write it if it's not a known DE. */
    {
        fprintf( of, "%s;", de_name );
    }
}

int main(int argc, char** argv)
{
    GOptionContext* opt_ctx;
    GError* err = NULL;
    GMenuTree* menu_tree = NULL;
    GMenuTreeDirectory* root_dir;
    GSList* l;
    FILE *of;
    int ofd;
    char *tmp;
    char *dir;
    const gchar* const * xdg_cfg_dirs;
    const gchar* const * pdir;
    char* menu_prefix;
    char* menu_file;

    opt_ctx = g_option_context_new("Generate cache for freedeskotp.org compliant menus.");
    g_option_context_add_main_entries( opt_ctx, opt_entries, NULL );
    if( ! g_option_context_parse( opt_ctx, &argc, &argv, &err ) )
    {
        g_print( err->message );
        g_error_free( err );
        return 1;
    }

    if( lang )
        g_setenv( "LANGUAGE", lang, TRUE );
#if 0
    /* if the cache is already up-to-date, just leave it. */
    if( !force && is_menu_uptodate() )
    {
        g_print("upda-to-date, re-generation is not needed.");
        return 0;
    }
#endif

    /* some memory leaks happen here if g_free is not used to free the keys. */
    de_hash = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    g_hash_table_insert( de_hash, "LXDE", (gpointer)SHOW_IN_LXDE );
    g_hash_table_insert( de_hash, "GNOME", (gpointer)SHOW_IN_GNOME );
    g_hash_table_insert( de_hash, "KDE", (gpointer)SHOW_IN_KDE );
    g_hash_table_insert( de_hash, "XFCE", (gpointer)SHOW_IN_XFCE );
    g_hash_table_insert( de_hash, "ROX", (gpointer)SHOW_IN_ROX );

    menu_tree = gmenu_tree_lookup( ifile, GMENU_TREE_FLAGS_NONE | GMENU_TREE_FLAGS_INCLUDE_EXCLUDED );
    if( ! menu_tree )
    {
        g_print("Error loading source menu file: %s\n", ifile);
        return 1;
    }

    dir = g_path_get_dirname( ofile );
    if( !g_file_test( dir, G_FILE_TEST_EXISTS ) )
        g_mkdir_with_parents( dir, 0700 );
    g_free( dir );

    /* write the tree to cache. */
    tmp = g_malloc( strlen( ofile ) + 7 );
    strcpy( tmp, ofile );
    strcat( tmp, "XXXXXX" );
    ofd = g_mkstemp( tmp );
    if( ofd == -1 )
    {
        g_print( "Error writing output file: %s\n", g_strerror(errno) );
        return 1;
    }

    of = fdopen( ofd, "w" );
    if( ! of )
    {
        g_print( "Error writing output file: %s\n", ofile );
        return 1;
    }

    /* Version number should be added to the head of this cache file. */
    fprintf( of, "%d.%d\n", VER_MAJOR, VER_MINOR );

    /* the first line is menu name */
    fprintf( of, "%s\n", ifile );

    root_dir = gmenu_tree_get_root_directory( menu_tree );

    /* add the source menu file itself to the list of files requiring monitor */
    if( g_path_is_absolute(ifile) )
    {
        if( ! g_slist_find_custom(all_used_files, ifile, (GCompareFunc)strcmp ) )
            all_used_files = g_slist_prepend(all_used_files, g_strdup(ifile));
    }
    else
    {
        char* file_name;
        xdg_cfg_dirs = g_get_system_config_dirs();
        menu_prefix = g_getenv("XDG_MENU_PREFIX");
        file_name = menu_prefix ? g_strconcat(menu_prefix, ifile, NULL) : ifile;
        for( pdir = xdg_cfg_dirs; *pdir; ++pdir )
        {
            menu_file = g_build_filename( *pdir, "menus", file_name, NULL );
            if( ! g_slist_find_custom(all_used_dirs, menu_file, (GCompareFunc)strcmp ) )
                all_used_files = g_slist_prepend(all_used_files, menu_file);
            else
                g_free( menu_file );
        }
        menu_file = g_build_filename( g_get_user_config_dir(), "menus", file_name, NULL );
        if( file_name != ifile )
            g_free(file_name);

        if( ! g_slist_find_custom(all_used_dirs, menu_file, (GCompareFunc)strcmp ) )
            all_used_files = g_slist_prepend(all_used_files, menu_file);
        else
            g_free(menu_file);
    }
    
    /* write a list of all files which need to be monitored for changes. */
    /* write number of files first */
    fprintf( of, "%d\n", g_slist_length(all_used_dirs) + g_slist_length(all_used_files) );

    /* list all files.
     * add D or F at the begin of each line to indicate whether it's a
     * file or directory. */
    for( l = all_used_dirs; l; l = l->next )
    {
        fprintf( of, "D%s\n", (char*)l->data );
    }
    for( l = all_used_files; l; l = l->next )
    {
        fprintf( of, "F%s\n", (char*)l->data );
    }

    /* write all DE names in this menu. Known DEs such as LXDE, GNOME, and KDE don't need to be listed here */
    if( g_hash_table_size(de_hash) > N_KNOWN_DESKTOPS ) /* if there are some unknown DEs added to the hash */
        g_hash_table_foreach(de_hash, (GHashFunc)write_de_name, of );
    fputc('\n', of);

    /* write the whole menu tree */
    write_dir( of, root_dir );

    fclose( of );

    gmenu_tree_unref( menu_tree );

    g_hash_table_destroy(de_hash);

    if( g_rename( tmp, ofile ) == -1 )
    {
        g_print( "Error writing output file: %s\n", g_strerror( errno ) );
    }
    g_free( tmp );
    /* g_print("success!\n"); */
    return 0;
}
