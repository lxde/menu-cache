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
#include <stdio.h>
#include "gmenu-tree.h"

static char* ifile = NULL;
static char* ofile = NULL;

GOptionEntry opt_entries[] =
{
	{"force", 'f', 0, G_OPTION_ARG_NONE, NULL, "Force regeneration of cache even if it's up-to-date.", NULL },
	{"input", 'i', 0, G_OPTION_ARG_FILENAME, &ifile, "Source *.menu file to read", NULL },
	{"output", 'o', 0, G_OPTION_ARG_FILENAME, &ofile, "Output file to write cache to", NULL },
	{0}
};

static int level = 0;

FILE* indent( FILE* of )
{
	int i = 0;
	for( i = 0; i < level; ++i )
		fputc( '\t', of );	
	return of;
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
				fprintf( indent(of), "%s=%s\n", *key, val );
				g_free( val );
			}
		}
		g_strfreev( keys );
	}
	g_key_file_free( kf );
}

void write_dir_info( FILE* of, GMenuTreeDirectory* dir )
{
	fprintf( indent(of), "+%s {\n", gmenu_tree_directory_get_menu_id( dir ) );
	++level;
	
	fprintf( indent(of), "File=%s\n", gmenu_tree_directory_get_desktop_file_path( dir ) );
	fprintf( indent(of), "Name=%s\n", gmenu_tree_directory_get_name( dir ) );
	fprintf( indent(of), "Comment=%s\n", gmenu_tree_directory_get_comment( dir ) );
	write_item_ex_info( of, gmenu_tree_directory_get_desktop_file_path( dir ) );
}

void write_dir( FILE* of, GMenuTreeDirectory* dir )
{
	GSList* l;

    for( l = gmenu_tree_directory_get_contents(dir); l; l = l->next )
    {
        GMenuTreeItem* item = (GMenuTreeItem*)l->data;
        GMenuTreeItemType type = gmenu_tree_item_get_type(item);

        if( type == GMENU_TREE_ITEM_DIRECTORY )
		{
			write_dir_info( of, (GMenuTreeDirectory*)item );
			fputc( '\n', of );
			write_dir( of, (GMenuTreeDirectory*)item );
			--level;
			fputs( "}\n", indent(of) );
		}
        else if( type == GMENU_TREE_ITEM_ENTRY )
        {
			fprintf( indent(of), "-%s {\n", gmenu_tree_entry_get_desktop_file_id( (GMenuTreeEntry*)item ) );
            if( gmenu_tree_entry_get_is_nodisplay(item) || gmenu_tree_entry_get_is_excluded(item) )
                continue;
			++level;

			fprintf( indent(of), "File=%s\n", gmenu_tree_entry_get_desktop_file_path( item ) );
			fprintf( indent(of), "Name=%s\n", gmenu_tree_entry_get_name( item ) );
			fprintf( indent(of), "Comment=%s\n", gmenu_tree_entry_get_comment( item ) );
			write_item_ex_info(of, gmenu_tree_entry_get_desktop_file_path( item ));
			--level;
			fputs( "}\n", indent(of) );
        }
		else if( type == GMENU_TREE_ITEM_SEPARATOR )
			fputs( "-\n", indent(of) );
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

	opt_ctx = g_option_context_new("Generate cache for freedeskotp.org compliant menus.");
	g_option_context_add_main_entries( opt_ctx, opt_entries, NULL );
	if( ! g_option_context_parse( opt_ctx, &argc, &argv, &err ) )
	{
		g_print( err->message );
		g_error_free( err );
		return 1;
	}

    menu_tree = gmenu_tree_lookup( ifile, GMENU_TREE_FLAGS_NONE );
	if( ! menu_tree )
	{
		g_print("Error loading source menu file: %s\n", ifile);
		return 1;
	}

//    gmenu_tree_add_monitor( menu_tree, on_menu_tree_changed, NULL );
	
	/* write the tree to cache. */
	of = fopen( ofile, "w" );
	if( ! of )
	{
		g_print( "Error writing output file: %s\n", ofile );
		return 1;
	}

    root_dir = gmenu_tree_get_root_directory( menu_tree );
	write_dir( of, root_dir );

	fclose( of );
    gmenu_tree_unref( menu_tree );

	return 0;
}
