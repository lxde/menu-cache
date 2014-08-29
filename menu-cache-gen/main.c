/*
 *      main.c : the main() function for menu-cache-gen binary.
 *
 *      Copyright 2013-2014 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
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
#include <locale.h>

/* GLib options parser data is taken from previous menu-cache-gen code
 *
 *      Copyright 2008 PCMan <pcman.tw@google.com>
 */
static char* ifile = NULL;
static char* ofile = NULL;
char* language = NULL;

GOptionEntry opt_entries[] =
{
/*
    {"force", 'f', 0, G_OPTION_ARG_NONE, &force, "Force regeneration of cache even if it's up-to-dat
e.", NULL },
*/
    {"input", 'i', 0, G_OPTION_ARG_FILENAME, &ifile, "Source *.menu file to read", NULL },
    {"output", 'o', 0, G_OPTION_ARG_FILENAME, &ofile, "Output file to write cache to", NULL },
    {"lang", 'l', 0, G_OPTION_ARG_STRING, &language, "Language", NULL },
    { NULL }
};

int main(int argc, char **argv)
{
    FmXmlFile *xmlfile = NULL;
    GOptionContext *opt_ctx;
    GError *err = NULL;
    MenuMenu *menu;
    int rc = 1;
    gboolean with_hidden = FALSE;

    /* wish we could use some POSIX parser but there isn't one for long options */
    opt_ctx = g_option_context_new("Generate cache for freedesktop.org compliant menus.");
    g_option_context_add_main_entries(opt_ctx, opt_entries, NULL);
    if (!g_option_context_parse(opt_ctx, &argc, &argv, &err))
    {
        g_printerr("menu-cache-gen: %s\n", err->message);
        g_error_free(err);
        return 1;
    }

    /* do with -l: if language is NULL then query it from environment */
    if (language == NULL)
        language = setlocale(LC_ALL, NULL);
    setlocale(LC_ALL, language);

    /* do with files: both ifile and ofile should be set correctly */
    if (ifile == NULL || ofile == NULL)
    {
        g_printerr("menu-cache-gen: failed: both input and output files must be defined.\n");
        return 1;
    }
    with_hidden = g_str_has_suffix(ifile, "+hidden");
    if (with_hidden)
        ifile[strlen(ifile)-7] = '\0';
    if (G_LIKELY(!g_path_is_absolute(ifile)))
    {
        /* resolv the path */
        char *path = g_build_filename(g_get_user_config_dir(), "menus", ifile, NULL);
        gboolean found = g_file_test(path, G_FILE_TEST_IS_DIR);
        const gchar * const *dirs = g_get_system_config_dirs();

        while (!found && dirs[0] != NULL)
        {
            g_free(path);
            path = g_build_filename(dirs[0], "menus", ifile, NULL);
            found = g_file_test(path, G_FILE_TEST_IS_DIR);
        }
        if (!found)
        {
            g_printerr("menu-cache-gen: failed: cannot find file '%s'\n", ifile);
            return 1;
        }
        g_free(ifile);
        ifile = path;
    }

    /* load, merge menu file, and create menu */
    menu = get_merged_menu(ifile, &xmlfile, &err);
    if (menu == NULL)
    {
        g_printerr("menu-cache-gen: %s\n", err->message);
        g_error_free(err);
        return 1;
    }

    /* save the layout */
    rc = !save_menu_cache(menu, ifile, ofile, with_hidden);
    if (xmlfile != NULL)
        g_object_unref(xmlfile);
    return rc;
}
