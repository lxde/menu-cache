/*
 *      menu-compose.c : scans appropriate .desktop files and composes cache file.
 *
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "menu-tags.h"

#include <string.h>
#include <stdio.h>
#include <glib/gstdio.h>

static GSList *DEs = NULL;

static void menu_app_reset(MenuApp *app)
{
    g_free(app->filename);
    g_free(app->title);
    g_free(app->key);
    app->key = NULL;
    g_free(app->comment);
    g_free(app->icon);
    g_free(app->generic_name);
    g_free(app->exec);
    g_free(app->categories);
    g_free(app->show_in);
    g_free(app->hide_in);
}

static void menu_app_free(gpointer data)
{
    MenuApp *app = data;

    menu_app_reset(app);
    g_free(app->id);
    g_list_free(app->dirs);
    g_list_free(app->menus);
    g_slice_free(MenuApp, app);
}

static void _fill_menu_from_file(MenuMenu *menu, const char *path)
{
    GKeyFile *kf = g_key_file_new();

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
        goto exit;
    menu->title = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                               G_KEY_FILE_DESKTOP_KEY_NAME,
                                               language, NULL);
    menu->comment = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                                 G_KEY_FILE_DESKTOP_KEY_COMMENT,
                                                 language, NULL);
    menu->icon = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                       G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
    menu->layout.is_set = TRUE;
exit:
    g_key_file_free(kf);
}

static const char **menu_app_intern_key_file_list(GKeyFile *kf, const char *key,
                                                  gboolean add_to_des)
{
    gsize len, i;
    char **val = g_key_file_get_string_list(kf, G_KEY_FILE_DESKTOP_GROUP, key, &len, NULL);
    const char **res;

    if (val == NULL)
        return NULL;
    res = (const char **)g_new(char *, len + 1);
    for (i = 0; i < len; i++)
    {
        res[i] = g_intern_string(val[i]);
        if (add_to_des && g_slist_find(DEs, res[i]) == NULL)
            DEs = g_slist_append(DEs, (gpointer)res[i]);
    }
    res[i] = NULL;
    g_strfreev(val);
    return res;
}

static void _fill_app_from_key_file(MenuApp *app, GKeyFile *kf)
{
    app->title = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                              G_KEY_FILE_DESKTOP_KEY_NAME,
                                              language, NULL);
    app->comment = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                                G_KEY_FILE_DESKTOP_KEY_COMMENT,
                                                language, NULL);
    app->icon = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                      G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
    app->generic_name = g_key_file_get_locale_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                                     G_KEY_FILE_DESKTOP_KEY_GENERIC_NAME,
                                                     language, NULL);
    app->exec = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                      G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
    app->categories = menu_app_intern_key_file_list(kf, G_KEY_FILE_DESKTOP_KEY_CATEGORIES, FALSE);
    app->show_in = menu_app_intern_key_file_list(kf, G_KEY_FILE_DESKTOP_KEY_ONLY_SHOW_IN, TRUE);
    app->hide_in = menu_app_intern_key_file_list(kf, G_KEY_FILE_DESKTOP_KEY_NOT_SHOW_IN, TRUE);
    app->use_terminal = g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP,
                                               G_KEY_FILE_DESKTOP_KEY_TERMINAL, NULL);
    app->use_notification = g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP,
                                                   G_KEY_FILE_DESKTOP_KEY_STARTUP_NOTIFY, NULL);
    app->hidden = g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP,
                                         G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL);
}

static GHashTable *all_apps = NULL;

static void _fill_apps_from_dir(MenuMenu *menu, const char *dir, GString *prefix)
{
    GDir *gd = g_dir_open(dir, 0, NULL);
    const char *name;
    char *filename, *id;
    gsize prefix_len = prefix->len;
    MenuApp *app;
    GKeyFile *kf;

    if (gd == NULL)
        return;
    kf = g_key_file_new();
    /* Scan the directory with subdirs,
       ignore not .desktop files,
       ignore already present files that are allocated */
    while ((name = g_dir_read_name(gd)) != NULL)
    {
        filename = g_build_filename(dir, name, NULL);
        if (g_file_test(filename, G_FILE_TEST_IS_DIR))
        {
            /* recursion */
            g_string_append(prefix, name);
            g_string_append_c(prefix, '-');
            _fill_apps_from_dir(menu, g_intern_string(filename), prefix);
            g_string_truncate(prefix, prefix_len);
        }
        else if (!g_str_has_suffix(name, ".desktop") ||
                 !g_file_test(filename, G_FILE_TEST_IS_REGULAR) ||
                 !g_key_file_load_from_file(kf, filename,
                                            G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
            ; /* ignore not key files */
        else if ((id = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                             G_KEY_FILE_DESKTOP_KEY_TYPE, NULL)) == NULL ||
                 strcmp(id, G_KEY_FILE_DESKTOP_TYPE_APPLICATION) != 0)
            /* ignore non-applications */
            g_free(id);
        else
        {
            g_free(id);
            if (prefix_len > 0)
            {
                g_string_append(prefix, name);
                app = g_hash_table_lookup(all_apps, prefix->str);
            }
            else
                app = g_hash_table_lookup(all_apps, name);
            if (app == NULL)
            {
                if (!g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL))
                {
                    /* deleted file should be ignored */
                    app = g_slice_new0(MenuApp);
                    app->type = MENU_CACHE_TYPE_APP;
                    app->filename = (prefix_len > 0) ? g_strdup(name) : NULL;
                    app->id = g_strdup((prefix_len > 0) ? prefix->str : name);
                    g_hash_table_insert(all_apps, app->id, app);
                    app->dirs = g_list_prepend(NULL, (gpointer)dir);
                    _fill_app_from_key_file(app, kf);
                }
            }
            else if (app->allocated)
                g_warning("id '%s' already allocated for %s and requested to"
                          " change to %s, ignoring request", name,
                          (const char *)app->dirs->data, dir);
            else if (g_key_file_get_boolean(kf, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL))
                g_hash_table_remove(all_apps, name);
            else
            {
                /* reset the data */
                menu_app_reset(app);
                app->filename = (prefix_len > 0) ? g_strdup(name) : NULL;
                /* reorder dirs list */
                app->dirs = g_list_remove(app->dirs, dir);
                app->dirs = g_list_prepend(app->dirs, (gpointer)dir);
                _fill_app_from_key_file(app, kf);
            }
            if (prefix_len > 0)
                g_string_truncate(prefix, prefix_len);
        }
        g_free(filename);
    }
    g_key_file_free(kf);
}

static int _compare_items(gconstpointer a, gconstpointer b)
{
    /* return negative value to reverse sort list */
    return -strcmp(((MenuApp*)a)->type == MENU_CACHE_TYPE_APP ? ((MenuApp*)a)->key
                                                              : ((MenuMenu*)a)->key,
                   ((MenuApp*)b)->type == MENU_CACHE_TYPE_APP ? ((MenuApp*)b)->key
                                                              : ((MenuMenu*)b)->key);
}

static gboolean menu_app_match_tag(MenuApp *app, FmXmlFileItem *it)
{
    FmXmlFileTag tag = fm_xml_file_item_get_tag(it);
    GList *children, *child;
    gboolean ok = FALSE;

    children = fm_xml_file_item_get_children(it);
    if (tag == menuTag_Or)
    {
        for (child = children; child; child = child->next)
            if (menu_app_match_tag(app, child->data))
                break;
        ok = (child != NULL);
    }
    else if (tag == menuTag_And)
    {
        for (child = children; child; child = child->next)
            if (!menu_app_match_tag(app, child->data))
                break;
        ok = (child == NULL);
    }
    else if (tag == menuTag_Not)
    {
        for (child = children; child; child = child->next)
            if (menu_app_match_tag(app, child->data))
                break;
        ok = (child == NULL);
    }
    else if (tag == menuTag_All)
        ok = TRUE;
    else if (tag == menuTag_Filename)
    {
        register const char *id = fm_xml_file_item_get_data(children->data, NULL);
        ok = (g_strcmp0(id, app->id) == 0);
    }
    else if (tag == menuTag_Category)
    {
        if (app->categories != NULL)
        {
            const char *cat = g_intern_string(fm_xml_file_item_get_data(children->data, NULL));
            const char **cats = app->categories;
            while (*cats)
                if (*cats == cat)
                    break;
            ok = (*cats != NULL);
        }
    }
    g_list_free(children);
    return ok;
}

static gboolean menu_app_match_excludes(MenuApp *app, GList *rules);

static gboolean menu_app_match(MenuApp *app, GList *rules, gboolean do_all)
{
    MenuRule *rule;
    GList *children, *child;

    for (; rules != NULL; rules = rules->next)
    {
        rule = rules->data;
        if (rule->type != MENU_CACHE_TYPE_NONE ||
            fm_xml_file_item_get_tag(rule->rule) != menuTag_Include)
            continue;
        children = fm_xml_file_item_get_children(rule->rule);
        for (child = children; child; child = child->next)
            if (menu_app_match_tag(app, child->data))
                break;
        g_list_free(children);
        if (child != NULL)
            return (!do_all || !menu_app_match_excludes(app, rules->next));
    }
    return FALSE;
}

static gboolean menu_app_match_excludes(MenuApp *app, GList *rules)
{
    MenuRule *rule;
    GList *children, *child;

    for (; rules != NULL; rules = rules->next)
    {
        rule = rules->data;
        if (rule->type != MENU_CACHE_TYPE_NONE ||
            fm_xml_file_item_get_tag(rule->rule) != menuTag_Exclude)
            continue;
        children = fm_xml_file_item_get_children(rule->rule);
        for (child = children; child; child = child->next)
            if (menu_app_match_tag(app, child->data))
                break;
        g_list_free(children);
        if (child != NULL)
            /* application might be included again later so check for it */
            return !menu_app_match(app, rules->next, TRUE);
    }
    return FALSE;
}

static void _free_leftovers(GList *item);

static void menu_menu_free(MenuMenu *menu)
{
    _free_layout_items(menu->layout.items);
    g_free(menu->name);
    g_free(menu->key);
    g_list_free(menu->id);
    _free_leftovers(menu->children);
    g_free(menu->title);
    g_free(menu->comment);
    g_free(menu->icon);
    g_slice_free(MenuMenu, menu);
}

static void _free_leftovers(GList *item)
{
    union {
        MenuMenu *menu;
        MenuRule *rule;
    } a = { NULL };

    while (item)
    {
        a.menu = item->data;
        if (a.rule->type == MENU_CACHE_TYPE_NONE)
            g_slice_free(MenuRule, a.rule);
        else if (a.rule->type == MENU_CACHE_TYPE_DIR)
            menu_menu_free(a.menu);
        /* MenuApp and MenuSep are not allocated in menu->children */
        item = g_list_delete_link(item, item);
    }
}

/* apps and dirs are in order "first is more relevant" */
static void _stage1(MenuMenu *menu, GList *dirs, GList *apps)
{
    GList *child, *_dirs = NULL, *_apps = NULL, *l, *available = NULL, *result;
    const char *id;
    char *filename;
    GString *prefix;
    MenuApp *app;
    FmXmlFileTag tag;
    GHashTableIter iter;

    /* Gather our dirs : DirectoryDir AppDir LegacyDir KDELegacyDirs */
    for (child = menu->children; child; child = child->next)
    {
        MenuRule *rule = child->data;
        if (rule->type != MENU_CACHE_TYPE_NONE)
            continue;
        tag = fm_xml_file_item_get_tag(rule->rule);
        if (tag == menuTag_DirectoryDir) {
            id = g_intern_string(fm_xml_file_item_get_data(fm_xml_file_item_find_child(rule->rule,
                                                           FM_XML_FILE_TEXT), NULL));
            if (_dirs == NULL)
                _dirs = g_list_copy(dirs);
            /* replace and reorder the list */
            _dirs = g_list_remove(_dirs, id);
            _dirs = g_list_prepend(_dirs, (gpointer)id);
        } else if (tag == menuTag_AppDir) {
            id = g_intern_string(fm_xml_file_item_get_data(fm_xml_file_item_find_child(rule->rule,
                                                           FM_XML_FILE_TEXT), NULL));
            _apps = g_list_prepend(_apps, (gpointer)id);
        } else if (tag == menuTag_LegacyDir) {
            /* FIXME */
        } else if (tag == menuTag_KDELegacyDirs) {
            /* FIXME */
        }
    }
    /* Gather data from files in the dirs */
    if (_dirs != NULL) dirs = _dirs;
    for (l = menu->id; l; l = l->next)
    {
        /* scan dirs now for availability of any of ids */
        filename = NULL;
        for (child = dirs; child; child = child->next)
        {
            filename = g_build_filename(child->data, l->data, NULL);
            if (g_file_test(filename, G_FILE_TEST_IS_REGULAR))
                break;
            g_free(filename);
            filename = NULL;
        }
        if (filename != NULL)
        {
            _fill_menu_from_file(menu, filename);
            g_free(filename);
            if (!menu->layout.is_set)
                continue;
            menu->dir = child->data;
            if (l != menu->id) /* relocate matched id to top if required */
            {
                menu->id = g_list_remove_link(menu->id, l);
                menu->id = g_list_concat(menu->id, l);
            }
            break;
        }
    }
    prefix = g_string_new("");
    for (l = _apps; l; l = l->next)
    {
        /* scan and fill the list */
        _fill_apps_from_dir(menu, l->data, prefix);
    }
    apps = _apps = g_list_concat(_apps, g_list_copy(apps));
    /* Gather all available files (some in $all_apps may be not in $apps) */
    g_hash_table_iter_init(&iter, all_apps);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&app))
    {
        app->matched = FALSE;
        /* check every dir if it is in $apps */
        for (child = app->dirs; child; child = child->next)
        {
            for (l = apps; l; l = l->next)
                if (l->data == child->data)
                    break;
            if (l != NULL) /* found one */
                break;
        }
        if (child == NULL) /* not matched */
            continue;
        /* Check matching : Include And Or Not All */
        app->matched = menu_app_match(app, menu->children, FALSE);
        if (!app->matched)
            continue;
        app->allocated = TRUE;
        /* Mark it by Exclude And Or Not All */
        app->excluded = menu_app_match_excludes(app, menu->children);
        if (!app->excluded)
            available = g_list_prepend(available, app);
    }
    /* Compose layout using available list and replace menu->children */
    result = NULL;
    for (child = menu->layout.items; child; child = child->next)
    {
        GList *next;
        app = child->data; /* either: MenuMenuname, MemuFilename, MenuSep, MenuMerge */
        switch (app->type) {
        case MENU_CACHE_TYPE_DIR: /* MenuMenuname */
            for (l = menu->children; l; l = l->next)
                if (((MenuMenu *)l->data)->layout.type == MENU_CACHE_TYPE_DIR &&
                    strcmp(((MenuMenuname *)app)->name, ((MenuMenu *)l->data)->name) == 0)
                    break;
            if (l == NULL)
            {
                /* we have to do this because it might be added by Merge */
                for (l = result; l; l = l->next)
                    if (((MenuMenu *)l->data)->layout.type == MENU_CACHE_TYPE_DIR &&
                        strcmp(((MenuMenuname *)app)->name, ((MenuMenu *)l->data)->name) == 0)
                        break;
                if (l == NULL) /* not found, ignoring it */
                    break;
                /* move it ahead then */
                result = g_list_remove_link(result, l);
            }
            else
                /* remove from menu->children */
                menu->children = g_list_remove_link(menu->children, l);
            /* prepend to result */
            result = g_list_concat(l, result);
            break;
        case MENU_CACHE_TYPE_APP: /* MemuFilename */
            app = g_hash_table_lookup(all_apps, ((MenuFilename *)l->data)->id);
            if (app == NULL || !app->matched || app->excluded)
                /* not available, ignoring it */
                break;
            l = g_list_find(result, app); /* this might be slow but we have
                                             to do this because app might be
                                             already added into result */
            if (l != NULL)
                /* move it out to this place */
                result = g_list_remove_link(result, l);
            else
            {
                l = g_list_find(available, app);
                if (l != NULL)
                    available = g_list_remove_link(available, l);
            }
            result = g_list_concat(l, result);
            break;
        case MENU_CACHE_TYPE_SEP: /* MenuSep */
            result = g_list_prepend(result, l->data);
            break;
        case MENU_CACHE_TYPE_NONE: /* MenuMerge */
            next = NULL;
            switch (((MenuMerge *)l->data)->merge_type) {
            case MERGE_FILES:
                tag = 1; /* use it as mark to not add dirs */
                break;
            case MERGE_ALL:
                for (l = available; l; l = l->next)
                {
                    app = l->data;
                    if (app->key == NULL)
                        app->key = g_utf8_collate_key(app->title, -1);
                    app->menus = g_list_prepend(app->menus, menu);
                }
                next = available;
                available = NULL;
                /* continue with menus */
            case MERGE_MENUS:
                for (l = menu->children; l; )
                {
                    if (((MenuMenu *)l->data)->layout.type == MENU_CACHE_TYPE_DIR)
                    {
                        GList *this = l;
                        if (((MenuMenu *)l->data)->key == NULL)
                            ((MenuMenu *)l->data)->key = g_utf8_collate_key(((MenuMenu *)l->data)->title, -1);
                        l = l->next;
                        /* move out from menu->children into result */
                        menu->children = g_list_remove_link(menu->children, this);
                        next = g_list_concat(this, next);
                    }
                    else
                        l = l->next;
                }
                result = g_list_concat(g_list_sort(next, _compare_items), result);
                break;
            default: ;
            }
        }
    }
    _free_leftovers(menu->children);
    menu->children = g_list_reverse(result);
    /* NOTE: now only menus are allocated in menu->children */
    /* Do recursion for all submenus */
    for (l = menu->children; l; l = l->next)
    {
        menu = l->data; /* we can reuse the pointer now */
        if (menu->layout.type == MENU_CACHE_TYPE_DIR)
            _stage1(menu, dirs, apps);
    }
    /* Do cleanup */
    g_list_free(available);
    g_list_free(_dirs);
    g_list_free(_apps);
    g_string_free(prefix, TRUE);
}

static void _stage2(MenuMenu *menu)
{
    GList *child = menu->children;
    MenuApp *app;

    while (child)
    {
        app = child->data;
        switch (app->type) {
        case MENU_CACHE_TYPE_APP: /* Menu App */
            if (menu->layout.only_unallocated && app->menus->next != NULL)
            {
                /* it is more than in one menu */
                GList *next = child->next;
                menu->children = g_list_delete_link(menu->children, child);
                child = next;
                app->menus = g_list_remove(app->menus, menu);
                break;
            }
            child = child->next;
            break;
        case MENU_CACHE_TYPE_DIR: /* MenuMenu */
            /* do recursion */
            _stage2(child->data);
        default:
            /* separator */
            child = child->next;
        }
    }
}

#define NONULL(a) (a == NULL) ? "" : a

static gboolean write_app(FILE *f, MenuApp *app, gboolean with_hidden)
{
    int index;
    MenuCacheItemFlag flags = 0;
    MenuCacheShowFlag show = SHOW_IN_LXDE;

    if (app->hidden && !with_hidden)
        return TRUE;
    index = MAX(g_slist_index(AppDirs, app->dirs->data), 0) + g_slist_length(DirDirs);
    if (app->use_terminal)
        flags |= FLAG_USE_TERMINAL;
    if (app->hidden)
        flags |= FLAG_IS_NODISPLAY;
    if (app->use_notification)
        flags |= FLAG_USE_SN;
    /* FIXME: compose show flags */
    return fprintf(f, "-%s\n%s\n%s\n%s\n%s\n%d\n%s\n%s\n%u\n%d\n", app->id,
                   NONULL(app->title), NONULL(app->comment), NONULL(app->icon),
                   NONULL(app->filename), index, NONULL(app->generic_name),
                   NONULL(app->exec), flags, (int)show) > 0;
}

static gboolean write_menu(FILE *f, MenuMenu *menu, gboolean with_hidden)
{
    int index;
    GList *child;
    gboolean ok = TRUE;

    if (menu->children == NULL && !with_hidden)
        return TRUE;
    index = g_slist_index(DirDirs, menu->dir);
    if (fprintf(f, "+%s\n%s\n%s\n%s\n%s\n%d\n", menu->name, NONULL(menu->title),
                NONULL(menu->comment), NONULL(menu->icon),
                menu->id ? (const char *)menu->id->data : "", index) < 0)
        return FALSE;
    for (child = menu->children; ok && child != NULL; child = child->next)
    {
        index = ((MenuApp *)child->data)->type;
        if (index == MENU_CACHE_TYPE_DIR)
            ok = write_menu(f, child->data, with_hidden);
        else if (index == MENU_CACHE_TYPE_APP)
            ok = write_app(f, child->data, with_hidden);
        else if (child->next != NULL && child != menu->children &&
                 ((MenuApp *)child->next->data)->type != MENU_CACHE_TYPE_SEP)
            /* separator - not add duplicates nor at start nor at end */
            fprintf(f, "-\n");
    }
    return ok;
}


/*
 * we handle here only:
 * - menuTag_DirectoryDir : for directory files list
 * - menuTag_AppDir menuTag_LegacyDir menuTag_KDELegacyDirs : for app files list
 * - menuTag_Include menuTag_Exclude menuTag_And menuTag_Or menuTag_Not menuTag_All :
 *      as matching rules
 */
gboolean save_menu_cache(MenuMenu *layout, const char *menuname, const char *file,
                         gboolean with_hidden)
{
    const char *de_names[N_KNOWN_DESKTOPS] = { "LXDE",
                                               "GNOME",
                                               "KDE",
                                               "XFCE",
                                               "ROX" };
    char *tmp = NULL;
    FILE *f;
    GSList *l;
    int i;
    gboolean ok = FALSE;

    all_apps = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, menu_app_free);
    for (i = 0; i < N_KNOWN_DESKTOPS; i++)
        DEs = g_slist_append(DEs, (gpointer)g_intern_static_string(de_names[i]));
    /* Recursively add files into layout, don't take OnlyUnallocated into account */
    _stage1(layout, NULL, NULL);
    /* Recursively remove non-matched files by OnlyUnallocated flag */
    _stage2(layout);
    /* Prepare temporary file for safe creation */
    tmp = g_path_get_dirname(file);
    if (tmp != NULL && !g_file_test(tmp, G_FILE_TEST_EXISTS))
        g_mkdir_with_parents(tmp, 0700);
    g_free(tmp);
    tmp = g_strdup_printf("%sXXXXXX", file);
    i = g_mkstemp(tmp);
    if (i < 0)
        goto failed;
    /* Compose created layout into output file */
    f = fdopen(i, "w");
    if (f == NULL)
        goto failed;
    /* Write common data */
    fprintf(f, "1.1\n%s%s\n%d\n", /* FIXME: use CACHE_GEN_VERSION */
            menuname, with_hidden ? "+hidden" : "",
            g_slist_length(DirDirs) + g_slist_length(AppDirs) + g_slist_length(MenuFiles));
    for (l = DirDirs; l; l = l->next)
        if (fprintf(f, "D%s\n", (const char *)l->data) < 0)
            goto failed;
    for (l = AppDirs; l; l = l->next)
        if (fprintf(f, "D%s\n", (const char *)l->data) < 0)
            goto failed;
    for (l = MenuFiles; l; l = l->next)
        if (fprintf(f, "F%s\n", (const char *)l->data) < 0)
            goto failed;
    for (l = DEs; l; l = l->next)
        if (fprintf(f, "%s;", (const char *)l->data) < 0)
            goto failed;
    fputc('\n', f);
    /* Write the menu tree */
    ok = write_menu(f, layout, with_hidden);
failed:
    if (f != NULL)
        fclose(f);
    if (ok)
        ok = g_rename(tmp, file) == 0;
    else if (tmp)
        g_unlink(tmp);
    /* Free all the data */
    g_free(tmp);
    g_hash_table_destroy(all_apps);
    menu_menu_free(layout);
    g_slist_free(DEs);
    return ok;
}
