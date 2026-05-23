/*
 * Yart OS - app catalog.
 *
 * The dock and the drawer both look up apps in this single table.  An
 * app is just (name, icon, launch).  Pin status comes from g_config.
 */
#pragma once
#include <yart/icons.h>

typedef struct {
    const char *name;
    icon_id_t   icon;
    void      (*launch)(void);
} yart_app_t;

extern const yart_app_t yart_app_catalog[];
extern const int        yart_app_catalog_count;

const yart_app_t *app_find(const char *name);
