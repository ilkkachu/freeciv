/***********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include <gtk/gtk.h>

/* utility */
#include "fcintl.h"

/* common */
#include "extras.h"
#include "game.h"
#include "movement.h"
#include "unit.h"

/* client */
#include "control.h"
#include "tilespec.h"

/* client/gui-gtk-3.22 */
#include "gui_main.h"
#include "gui_stuff.h"
#include "sprite.h"

#include "unitselextradlg.h"

struct unit_sel_extra_cb_data {
  GtkWidget *dlg;
  int tp_id;
};

/***********************************************************************//**
  Get an extra selection list item suitable description of the specified
  extra at the specified tile.
***************************************************************************/
static const char *tgt_extra_descr(const struct extra_type *tgt_extra,
                                   const struct tile *tgt_tile)
{
  static char buf[248] = "";
  static char buf2[248] = "";

  if (tile_has_extra(tgt_tile, tgt_extra)) {
    if (extra_owner(tgt_tile)) {
      /* TRANS: nation adjective for extra owner used below if the target
       * tile has the target extra and it has an owner. */
      fc_snprintf(buf2, sizeof(buf2), Q_("?eowner:%s"),
                  nation_adjective_for_player(extra_owner(tgt_tile)));
    } else {
      /* TRANS: used below if the target tile has the target extra but it
       * doesn't have an owner. */
      sz_strlcpy(buf2, _("target"));
    }
  } else {
    /* TRANS: used below if the target tile doesn't have the target
     * extra (so it is assumed that it will be created). */
    sz_strlcpy(buf2, _("create"));
  }

  /* TRANS: extra name ... one of the above strings depending on if the
   * target extra currently exists at the target tile and if it has an
   * owner. */
  fc_snprintf(buf, sizeof(buf), _("%s\n(%s)"),
              extra_name_translation(tgt_extra), buf2);

  return buf;
}

/************************************************************************//**
  Callback to handle toggling of one of the target extra buttons.
****************************************************************************/
static void unit_sel_extra_toggled(GtkToggleButton *tb, gpointer userdata)
{
  struct unit_sel_extra_cb_data *cbdata
          = (struct unit_sel_extra_cb_data *)userdata;

  log_normal("unit_sel_extra_toggled(): dialog data 'target' was %d", 
             GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cbdata->dlg), "target")));

  if (gtk_toggle_button_get_active(tb)) {
    g_object_set_data(G_OBJECT(cbdata->dlg), "target",
                      GINT_TO_POINTER(cbdata->tp_id));
  }

  log_normal("unit_sel_extra_toggled(): dialog data 'target' set to %d", 
             cbdata->tp_id);

}

#if 0
/************************************************************************//**
  Callback to handle clicking of one of the target extra buttons.
****************************************************************************/
static void unit_sel_extra_clicked(GtkToggleButton *tb, gpointer userdata)
{
  struct unit_sel_extra_cb_data *cbdata
          = (struct unit_sel_extra_cb_data *)userdata;

  log_normal("unit_sel_extra_clicked(): button for target %d was clicked.",
             cbdata->tp_id);

  log_normal("unit_sel_extra_clicked(): dialog data 'target' was %d", 
             GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cbdata->dlg), "target")));

  if (gtk_toggle_button_get_active(tb)) {
    g_object_set_data(G_OBJECT(cbdata->dlg), "target",
                      GINT_TO_POINTER(cbdata->tp_id));
    log_normal("unit_sel_extra_clicked(): dialog data 'target' set to %d", 
               cbdata->tp_id);
  }

}
#endif

/************************************************************************//**
  Callback to handle destruction of one of the target extra buttons.
****************************************************************************/
static void unit_sel_extra_destroyed(GtkWidget *radio, gpointer userdata)
{
  free(userdata);
}

/************************************************************************//**
  Create a dialog where a unit select what extra to act on.
****************************************************************************/
bool select_tgt_extra(struct unit *actor, struct tile *ptile,
                      bv_extras potential_tgt_extras,
                      struct extra_type *suggested_tgt_extra,
                      const gchar *dlg_title,
                      const gchar *actor_label,
                      const gchar *tgt_label,
                      const gchar *do_label,
                      GCallback do_callback)
{
  GtkWidget *dlg;
  GtkWidget *main_box;
  GtkWidget *box;
  GtkWidget *icon;
  GtkWidget *lbl;
  GtkWidget *sep;
  GtkWidget *radio;
  GtkWidget *default_option = NULL;
  int default_target = -1;  /* 0 would already be a valid extra */
  GtkWidget *first_option = NULL;
  struct sprite *spr;
  struct unit_type *actor_type = unit_type_get(actor);
  int tcount;

  dlg = gtk_dialog_new_with_buttons(dlg_title, NULL, 0,
                                    _("Close"), GTK_RESPONSE_NO,
                                    do_label, GTK_RESPONSE_YES,
                                    NULL);
  setup_dialog(dlg, toplevel);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_NO);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);

  main_box = gtk_grid_new();
  gtk_orientable_set_orientation(GTK_ORIENTABLE(main_box),
                                 GTK_ORIENTATION_VERTICAL);
  box = gtk_grid_new();
  gtk_orientable_set_orientation(GTK_ORIENTABLE(box),
                                 GTK_ORIENTATION_HORIZONTAL);

  lbl = gtk_label_new(actor_label);
  gtk_grid_attach(GTK_GRID(box), lbl, 0, 0, 1, 1);

  spr = get_unittype_sprite(tileset, actor_type,
                            direction8_invalid(), TRUE);
  if (spr != NULL) {
    icon = gtk_image_new_from_pixbuf(sprite_get_pixbuf(spr));
  } else {
    icon = gtk_image_new();
  }
  gtk_grid_attach(GTK_GRID(box), icon, 1, 0, 1, 1);

  lbl = gtk_label_new(utype_name_translation(actor_type));
  gtk_grid_attach(GTK_GRID(box), lbl, 2, 0, 1, 1);

  gtk_container_add(GTK_CONTAINER(main_box), box);

  sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_container_add(GTK_CONTAINER(main_box), sep);

  lbl = gtk_label_new(tgt_label);
  gtk_container_add(GTK_CONTAINER(main_box), lbl);

  //box = gtk_grid_new();
  GtkWidget *outerbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  tcount = 0;
  extra_type_iterate(ptgt) {
    GdkPixbuf *tubuf;
    GtkWidget *innerbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    if (!BV_ISSET(potential_tgt_extras, extra_number(ptgt))) {
      continue;
    }

    struct unit_sel_extra_cb_data *cbdata
            = fc_malloc(sizeof(struct unit_sel_extra_cb_data));

    cbdata->tp_id = ptgt->id;
    cbdata->dlg = dlg;

    //radio = gtk_radio_button_new_with_label_from_widget(
    //      GTK_RADIO_BUTTON(first_option),
    //      tgt_extra_descr(ptgt, ptile));
    radio = gtk_radio_button_new_from_widget(
          GTK_RADIO_BUTTON(first_option));

    if (first_option == NULL) {
      first_option = radio;
      default_option = first_option;
      default_target = ptgt->id;
    }
    
    log_normal("select_tgt_extra(): created button, active status = %d",
               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio)));
    /* The lists must be the same length if they contain the same
     * elements. */
    fc_assert_msg(g_slist_length(gtk_radio_button_get_group(
                                   GTK_RADIO_BUTTON(radio)))
                  == g_slist_length(gtk_radio_button_get_group(
                                      GTK_RADIO_BUTTON(first_option))),
                  "The radio button for '%s' is broken.",
                  extra_rule_name(ptgt));
    //g_signal_connect(radio, "clicked",
    //                G_CALLBACK(unit_sel_extra_clicked), cbdata);
    g_signal_connect(radio, "toggled",
                     G_CALLBACK(unit_sel_extra_toggled), cbdata);
    g_signal_connect(radio, "destroy",
                     G_CALLBACK(unit_sel_extra_destroyed), cbdata);
    if (ptgt == suggested_tgt_extra) {
      default_option = radio;
      default_target = ptgt->id;
    }
    //gtk_grid_attach(GTK_GRID(box), radio, 0, tcount, 1, 1);
    gtk_box_pack_start(GTK_BOX(outerbox), radio, TRUE, FALSE, 0);

    tubuf = create_extra_pixbuf(ptgt);
    if (tubuf != NULL) {
      icon = gtk_image_new_from_pixbuf(tubuf);
      g_object_unref(tubuf);
    } else {
      icon = gtk_image_new();
    }
    //gtk_button_set_image(GTK_BUTTON(radio), icon);
    //gtk_button_set_always_show_image(GTK_BUTTON(radio), TRUE);
    
    //gtk_grid_attach(GTK_GRID(buttonbox), icon, 1, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(innerbox), icon, TRUE, FALSE, 0);


    lbl = gtk_label_new(tgt_extra_descr(ptgt, ptile));
    //gtk_grid_attach(GTK_GRID(buttonbox), lbl, 2, 0, 1, 1);
    
    gtk_box_pack_start(GTK_BOX(innerbox), lbl, TRUE, FALSE, 0);
    
    gtk_container_add(GTK_CONTAINER(radio), innerbox);

    tcount++;


#if 0
    if(GTK_IS_RADIO_BUTTON(radio)) {
      log_normal("select_tgt_extra(): radio button is radio button");
    }

    if(GTK_IS_BUTTON(radio)) {
      log_normal("select_tgt_extra(): radio button is button");
    }

    if(GTK_IS_BIN(radio)) {
      log_normal("select_tgt_extra(): radio button is a Bin");
    }

    if(GTK_IS_CONTAINER(radio)) {
      log_normal("select_tgt_extra(): radio button is container");
    }
#endif
 
    GtkWidget *widget = radio;
    log_normal("select_tgt_extra(): children of radio button:");

    while (GTK_IS_BIN(widget)) {
      GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
      log_normal("select_tgt_extra(): ... %s",
                 G_OBJECT_TYPE_NAME(child));
      widget = child;

    }
    if (GTK_IS_CONTAINER(widget)) {
      log_normal("select_tgt_extra(): that's a container, children:");
      GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
      GList *l = children;
      for ( ; l != NULL ; l = l->next ) {
        log_normal("select_tgt_extra(): ... %s",
                 G_OBJECT_TYPE_NAME(GTK_WIDGET(l->data)));
      }
    } 
/*
    if(GTK_IS_BIN(radio)) {
      GtkWidget *child = gtk_bin_get_child(GTK_BIN(radio));
      log_normal("select_tgt_extra(): child of radio button is %s",
                 G_OBJECT_TYPE_NAME(child));
    }
*/
    
  } extra_type_iterate_end;
  gtk_container_add(GTK_CONTAINER(main_box), outerbox);

  fc_assert_ret_val(default_option, FALSE);
  
  log_normal("select_tgt_extra(): default button active status = %d",
             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(default_option)));
  
  //gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(default_option), TRUE);
  //gtk_toggle_button_toggled(GTK_TOGGLE_BUTTON(default_option));
  //gtk_toggle_button_toggled(GTK_TOGGLE_BUTTON(default_option));
  
  gtk_container_add(
              GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
              main_box);

  g_object_set_data(G_OBJECT(dlg), "actor", GINT_TO_POINTER(actor->id));
  g_object_set_data(G_OBJECT(dlg), "tile", ptile);
  g_object_set_data(G_OBJECT(dlg), "target", GINT_TO_POINTER(default_target));

  g_signal_connect(dlg, "response", do_callback, actor);

  log_normal("select_tgt_extra(): dialog data 'target' = %d", 
             GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dlg), "target")));

  gtk_widget_show_all(gtk_dialog_get_content_area(GTK_DIALOG(dlg)));
  gtk_widget_show(dlg);

  return TRUE;
}
