/*
 *      Copyright 2014 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
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

#include <libfm/fm-extra.h>
#include <menu-cache.h>

FmXmlFileTag menuTag_Menu;
FmXmlFileTag menuTag_AppDir;
FmXmlFileTag menuTag_DefaultAppDirs;
FmXmlFileTag menuTag_DirectoryDir;
FmXmlFileTag menuTag_DefaultDirectoryDirs;
FmXmlFileTag menuTag_Include;
FmXmlFileTag menuTag_Exclude;
FmXmlFileTag menuTag_Filename;
FmXmlFileTag menuTag_Or;
FmXmlFileTag menuTag_And;
FmXmlFileTag menuTag_Not;
FmXmlFileTag menuTag_Category;
FmXmlFileTag menuTag_MergeFile;
FmXmlFileTag menuTag_MergeDir;
FmXmlFileTag menuTag_DefaultMergeDirs;
FmXmlFileTag menuTag_Directory;
FmXmlFileTag menuTag_Name;
FmXmlFileTag menuTag_Deleted;
FmXmlFileTag menuTag_NotDeleted;
FmXmlFileTag menuTag_OnlyUnallocated;
FmXmlFileTag menuTag_NotOnlyUnallocated;
FmXmlFileTag menuTag_All;
FmXmlFileTag menuTag_LegacyDir;
FmXmlFileTag menuTag_KDELegacyDirs;
FmXmlFileTag menuTag_Move;
FmXmlFileTag menuTag_Old;
FmXmlFileTag menuTag_New;
FmXmlFileTag menuTag_Layout;
FmXmlFileTag menuTag_DefaultLayout;
FmXmlFileTag menuTag_Menuname;
FmXmlFileTag menuTag_Separator;
FmXmlFileTag menuTag_Merge;

typedef enum {
    MERGE_NONE, /* starting value */
    MERGE_FILES, /* first set */
    MERGE_MENUS,
    MERGE_ALL, /* only set */
    MERGE_FILES_MENUS, /* second set */
    MERGE_MENUS_FILES
} MenuMergeType;

typedef struct {
    MenuCacheType type : 2; /* used by MenuMenu, MENU_CACHE_TYPE_DIR */
    gboolean only_unallocated : 1; /* for Menuname: TRUE if show_empty is set */
    gboolean is_set : 1; /* used by MenuMenu, for Menuname: TRUE if is_inline is set */
    gboolean show_empty : 1;
    gboolean is_inline : 1;
    gboolean inline_header : 1;
    gboolean inline_alias : 1;
    gboolean inline_header_is_set : 1; /* for Menuname */
    gboolean inline_alias_is_set : 1; /* for Menuname */
    gboolean inline_limit_is_set : 1; /* for Menuname */
    int inline_limit;
    GList *items; /* items are MenuItem : Menuname or Filename or Separator or Merge */
} MenuLayout;

/* Menuname item */
typedef struct {
    MenuLayout layout;
    char *name;
} MenuMenuname;

/* Filename item in layout */
typedef struct {
    MenuCacheType type : 2; /* MENU_CACHE_TYPE_APP */
    gboolean excluded : 1;
    char *id;
} MenuFilename;

/* Separator item */
typedef struct {
    MenuCacheType type : 2; /* MENU_CACHE_TYPE_SEP */
} MenuSep;

/* Merge item */
typedef struct {
    MenuCacheType type : 2; /* MENU_CACHE_TYPE_NONE */
    MenuMergeType merge_type;
} MenuMerge;

/* Menu item */
typedef struct {
    MenuLayout layout; /* copied from hash on </Menu> */
    char *name;
    /* next fields are only for Menu */
    GList *id; /* <Directory> for <Menu>, may be NULL, first is most relevant */
    /* next fields are only for composer */
    GList *files; /* loaded MenuApp items - allocated only here, not in children */
    GList *children; /* items are MenuItem : MenuApp, MenuMenu, MenuSep, MenuRule */
    char *title;
    char *comment;
    char *icon;
    int dir_index;
} MenuMenu;

/* File item in menu */
typedef struct {
    MenuCacheType type : 2; /* MENU_CACHE_TYPE_APP */
    gboolean excluded : 1;
    gboolean allocated : 1; /* set by composer */
    gboolean use_terminal : 1; /* set by composer */
    gboolean use_notification : 1; /* set by composer */
    gboolean hidden : 1; /* set by composer */
    int dir_index; /* set by composer */
    MenuMenu *parent;
    char *id;
    /* next fields are only for composer */
    char *title;
    char *comment;
    char *icon;
    char *generic_name;
    char *exec;
    char **categories;
    char **show_in;
    char **hide_in;
} MenuApp;

/* a placeholder for matching */
typedef struct {
    MenuCacheType type : 2; /* MENU_CACHE_TYPE_NONE */
    FmXmlFileItem *rule;
} MenuRule;

/* list of available app dirs */
GSList *AppDirs;

/* list of available dir dirs */
GSList *DirDirs;

/* parse and merge menu files */
MenuMenu *get_merged_menu(const char *file, FmXmlFile **xmlfile, GError **error);

/* parse all files into layout and save cache file */
void save_menu_cache(MenuMenu *layout, const char *file);

/* free layout data */
//void free_menu_layout(MenuMenu *layout);
