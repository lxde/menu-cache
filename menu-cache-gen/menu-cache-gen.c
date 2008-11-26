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
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <stdio.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gmenu-tree.h"
#include "menu-cache.h"

static char* ifile = NULL;
static char* ofile = NULL;
static char* lang = NULL;
static gboolean force = FALSE;

GOptionEntry opt_entries[] =
{
    {"force", 'f', 0, G_OPTION_ARG_NONE, &force, "Force regeneration of cache even if it's up-to-date.", NULL },
    {"input", 'i', 0, G_OPTION_ARG_FILENAME, &ifile, "Source *.menu file to read", NULL },
    {"output", 'o', 0, G_OPTION_ARG_FILENAME, &ofile, "Output file to write cache to", NULL },
    {"lang", 'l', 0, G_OPTION_ARG_STRING, &lang, "Language", NULL },
    {0}
};

GHashTable* dir_hash = NULL;

/* all used app dirs.
 * defined in entry-directories.c
 */
extern GSList* all_used_dirs;

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

void write_dir( FILE* of, GMenuTreeDirectory* dir )
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
        /* get the location of its desktop file. */
        str = g_path_get_dirname( gmenu_tree_directory_get_desktop_file_path( dir ) );
        fprintf( of, "%d\n", dirname_index( str ) );
        g_free( str );

        /* get basename of its desktop file. */
        str = g_path_get_basename( gmenu_tree_directory_get_desktop_file_path( dir ) );
        fprintf( of, "%s\n", str );
        g_free( str );

        write_item_ex_info( of, gmenu_tree_directory_get_desktop_file_path( dir ) );
    }
    else
    {
        fprintf( of, "-1\n\n" );
    }

    fprintf( of, "\n" );    /* end of item info */

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
            fprintf( of, "-%s\n", gmenu_tree_entry_get_desktop_file_id( (GMenuTreeEntry*)item ) );
            if( gmenu_tree_entry_get_is_nodisplay(item) /* || gmenu_tree_entry_get_is_excluded(item) */ )
                continue;

            fprintf( of, "%s\n", gmenu_tree_entry_get_name( item ) );
            str = gmenu_tree_entry_get_comment( item );
            fprintf( of, "%s\n", str ? str : "" );

            str = gmenu_tree_entry_get_icon( item );
            fprintf( of, "%s\n", str ? str : "" );

            if( gmenu_tree_entry_get_desktop_file_path( item ) )
            {
                str = g_path_get_dirname( gmenu_tree_entry_get_desktop_file_path( item ) );
                fprintf( of, "%d\n", dirname_index( str) );
                g_free( str );
            }
            else
            {
                fprintf( of, "-1\n" );
            }

            fprintf( of, "%s\n", gmenu_tree_entry_get_exec( item ) );
            fprintf( of, "%c\n", gmenu_tree_entry_get_launch_in_terminal( item ) ? '1' : '0' );
            fprintf( of, "%c\n", gmenu_tree_entry_get_use_startup_notify( item ) ? '1' : '0' );

            if( gmenu_tree_entry_get_desktop_file_path( item ) )
                write_item_ex_info(of, gmenu_tree_entry_get_desktop_file_path( item ));
            fputs( "\n", of );
        }
        else if( type == GMENU_TREE_ITEM_SEPARATOR )
            fputs( "-\n", of );
    }
    fputs( "\n", of );
}

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

    /* if the cache is already up-to-date, just leave it. */
    if( !force && is_menu_uptodate() )
    {
        g_print("upda-to-date, re-generation is not needed.");
        return 0;
    }

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

    fprintf( of, "%s\n", gmenu_tree_get_menu_file_full_path(menu_tree) );

    root_dir = gmenu_tree_get_root_directory( menu_tree );

    fprintf( of, "%d\n", g_slist_length(all_used_dirs) );
    for( l = all_used_dirs; l; l = l->next )
    {
        fprintf( of, "%s\n", (char*)l->data );
    }

    write_dir( of, root_dir );

    fclose( of );

    gmenu_tree_unref( menu_tree );

    if( g_rename( tmp, ofile ) == -1 )
    {
        g_print( "Error writing output file: %s\n", g_strerror( errno ) );
    }
    g_free( tmp );
    /* g_print("success!\n"); */
    return 0;
}
