/*
 *      test.c
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
#include "menu-cache.h"

static int level = 0;

static void indent()
{
	int i = 0;
	for( i = 0; i < level; ++i )
		printf( "\t" );
}

static void print_menu(MenuCacheDir* dir)
{
	GSList* l;
	indent();
	printf( "dir:%s\n", menu_cache_item_get_name(dir) );
	indent();
	printf( "comment:%s\n", menu_cache_item_get_comment(dir) );
	indent();
	printf( "n_children = %d\n", g_slist_length( menu_cache_dir_get_children(dir) ) );

	++level;
	for( l = menu_cache_dir_get_children(dir); l; l = l->next )
	{
		MenuCacheItem* item;
		item = MENU_CACHE_ITEM(l->data);
		if( menu_cache_item_get_type(item) == MENU_CACHE_TYPE_DIR )
			print_menu( item );
		else if( menu_cache_item_get_type(item) == MENU_CACHE_TYPE_APP )
		{
			indent();
			printf( "app:%s\n", menu_cache_item_get_name(item) );
			indent();
			printf( "comment:%s\n", menu_cache_item_get_comment(item) );
			indent();
			printf( "icon:%s\n", menu_cache_item_get_icon(item) );
			indent();
			printf( "exec:%s\n", menu_cache_app_get_exec(item) );
			indent();
			printf("end of app\n");
		}
		else if( menu_cache_item_get_type(item) == MENU_CACHE_TYPE_SEP )
		{
			indent();
			printf( "seperator\n" );
		}
		printf("\n");
	}
	printf("\n");
	indent();
	printf("end of dir\n");

	--level;
}

int main(int argc, char** argv)
{
	MenuCacheDir* menu = menu_cache_new( argv[1], NULL, NULL );
	print_menu( menu );
	menu_cache_item_unref( menu );
	return 0;
}
