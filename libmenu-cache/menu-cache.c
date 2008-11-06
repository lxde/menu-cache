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

#include <stdio.h>
#include <string.h>

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
};

struct _MenuCacheDir
{
	MenuCacheItem item;
	char* file;
	GSList* children;
};

struct _MenuCacheApp
{
	MenuCacheItem item;
	char* file_dir;
	char* exec;
	char* working_dir;
//	char** categories;
	gboolean use_terminal : 1;
	gboolean use_sn : 1;
};

void menu_cache_init()
{
	
}

static MenuCacheItem* read_item(  FILE* f );

static void read_item_extended(  FILE* f, MenuCacheItem* item )
{
	char line[4096];
	int len;

	while( fgets( line, G_N_ELEMENTS(line) - 1, f ) )
	{
		if( line[0] == '\n' || line[0] == '\0' ) /* end of item info */
			break;
		len = strlen( line );

		continue;

		if( G_LIKELY(len > 1) )
		{
			char* sep = strchr( line, '=' );
			if( G_UNLIKELY( ! sep ) )
				continue;
			if( G_UNLIKELY( ! item->extended ) )
				g_datalist_init( &item->extended );
			line[len] = '\0';
			*sep = '\0';
			g_datalist_set_data_full( item->extended, line, g_strdup( sep + 1 ), g_free );
		}
	}
}

static void read_dir( FILE* f, MenuCacheDir* dir )
{
	char line[4096];
	int len;
	MenuCacheItem* item;

	/* file path */
	if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
		return;
	len = strlen( line );
	if( G_LIKELY(len > 1) )
		dir->file = g_strndup( line, len - 1 );

	/* load extended key/values */
	read_item_extended( f, dir );

	/* load child items in the dir */
	while( item = read_item( f ) )
		dir->children = g_slist_prepend( dir->children, item );

	dir->children = g_slist_reverse( dir->children );
}

static void read_app( FILE* f, MenuCacheApp* app )
{
	char line[4096];
	int len;

	/* desktop file dir */
	if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
		return;
	len = strlen( line );
	if( G_LIKELY(len > 1) )
		app->file_dir = g_strndup( line, len - 1 );

	/* exec */
	if( G_UNLIKELY( ! fgets( line, G_N_ELEMENTS(line) - 1, f ) ))
		return;
	len = strlen( line );
	if( G_LIKELY(len > 1) )
		app->exec = g_strndup( line, len - 1 );

	read_item_extended( f, MENU_CACHE_ITEM(app) );
}

static MenuCacheItem* read_item(  FILE* f )
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
		read_dir( f, MENU_CACHE_DIR(item) );
	else if( item->type == MENU_CACHE_TYPE_APP )
		read_app( f, MENU_CACHE_APP(item) );

	return item;
}

MenuCacheDir* menu_cache_new( const char* cache_file, char** include, char** exclude )
{
	MenuCacheDir* dir;
	FILE* f = fopen( cache_file, "r" );
	if( ! f )
		return NULL;
	dir = (MenuCacheDir*)read_item( f );
	fclose( f );
	return dir;
}

MenuCacheItem* menu_cache_item_ref(MenuCacheItem* item)
{
	g_atomic_int_inc( item->n_ref );
}

void menu_cache_item_unref(MenuCacheItem* item)
{
	if( g_atomic_int_dec_and_test( item->n_ref ) )
	{
		g_free( item->id );
		g_free( item->name );
		g_free( item->comment );
		g_free( item->icon );

		if( item->extended )
			g_datalist_clear( item->extended );

		if( item->type == MENU_CACHE_TYPE_DIR )
		{
			MenuCacheDir* dir = MENU_CACHE_DIR(item);
			g_free( dir->file );
			g_slist_foreach( dir->children, (GFunc)menu_cache_item_unref, NULL );
			g_slist_free( dir->children );
			g_slice_free( MenuCacheDir, item );
		}
		else
		{
			MenuCacheApp* app = MENU_CACHE_APP(item);
			g_free( app->file_dir );
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
	if( ! item->extended )
		return NULL;
	return g_datalist_get_data( item->extended, key );
}

const char* menu_cache_item_get_qextended( MenuCacheItem* item, GQuark key )
{
	if( ! item->extended )
		return NULL;
	return g_datalist_id_get_data( item->extended, key );
}

GSList* menu_cache_dir_get_children( MenuCacheDir* dir )
{
	return dir->children;
}

const char* menu_cache_app_get_exec( MenuCacheApp* app )
{
	return app->exec;
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
	return FALSE;
}

gboolean menu_cache_app_get_use_sn( MenuCacheApp* app )
{
	return TRUE;
}

GSList* menu_cache_list_all_apps()
{
	return NULL;
}

MenuCacheApp* menu_cache_find_app_by_exec( const char* exec )
{
	return NULL;
}

