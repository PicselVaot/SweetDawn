#ifndef WEB_PAGE_H
#define WEB_PAGE_H

/*
 * web/page.h — Page de contrôle embarquée dans le service
 *
 * Le contenu vient de web/index.html : le Makefile le compresse en gzip
 * et le transforme en tableau C (bin/obj/web/page.c). Le service n'a
 * donc aucun fichier à lire ni rien à compresser à l'exécution.
 */

#include <stddef.h>

extern const unsigned char web_page_gzip[];
extern const size_t web_page_gzip_size;

#endif
