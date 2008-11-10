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
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

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
/*	char** categories;	*/
	gboolean use_terminal : 1;
	gboolean use_sn : 1;
};

struct _MenuCache
{
	guint n_ref;
	MenuCacheDir* root_dir;
	char** all_used_dirs;
	int n_all_used_dirs;
	char* menu_file_path;
	time_t mtime;
	time_t time;
};

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
	if( G_LIKELY( idx >=0 && idx < cache->n_all_used_dirs ) )
		dir->file_dir = cache->all_used_dirs[ idx ];

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
	if( G_LIKELY( idx >=0 && idx < cache->n_all_used_dirs ) )
		app->file_dir = cache->all_used_dirs[ idx ];

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

static gboolean read_all_used_dirs( FILE* f, int* n_all_used_dirs, char*** all_used_dirs )
{
	char line[ 4096 ];
	int i, n;
	char** dirs;

	if( ! fgets( line, G_N_ELEMENTS(line), f ) )
		return FALSE;

	*n_all_used_dirs = n = atoi( line );
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
	*all_used_dirs = dirs;
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

	cache->menu_file_path = g_strdup( strtok(line, "\n") );

	/* get all used dirs */
	if( ! read_all_used_dirs( f, &cache->n_all_used_dirs, &cache->all_used_dirs ) )
	{
		g_slice_free( MenuCache, cache );
		return NULL;
	}
	cache->n_ref = 1;
	cache->root_dir = (MenuCacheDir*)read_item( f, cache );
	cache->time = time(NULL);	/* save current time */
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
		if( G_LIKELY(cache->root_dir) )
			menu_cache_item_unref( cache->root_dir );
		g_strfreev( cache->all_used_dirs );
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
	char** names = g_strsplit( path, "/", -1 );
	int i = 0;
	MenuCacheDir* dir = NULL;

	for( ; names[i]; ++i )
	{
		GSList* l;
		for( l = cache->root_dir->children; l; l = l->next )
		{
			MenuCacheItem* item = MENU_CACHE_ITEM(l->data);
			if( item->type == MENU_CACHE_TYPE_DIR && g_str_equal( item->id, names[i] ) )
			{
				dir = item;
			}
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
			if( read_all_used_dirs( f, &n, &dirs ) )
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

gboolean menu_cache_is_updated( MenuCache* cache )
{
	struct stat st;
	int i;

	for( i = 0; i < cache->n_all_used_dirs; ++i )
	{
		if( stat( cache->all_used_dirs[i], &st ) == -1 )
			continue;

		if( st.st_mtime > cache->mtime )
			return FALSE;
	}
	return TRUE;
}

