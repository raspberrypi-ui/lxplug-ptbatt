#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#ifdef __arm__
#include <wiringPiI2C.h>
#else
#include "batt_sys.h"
#endif

#include "plugin.h"

#define ICON_BUTTON_TRIM 4

//#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) g_message("ptb: " fmt,##args)
#else
#define DEBUG
#endif

/* Plug-in global data */

typedef struct {
    GtkWidget *plugin;              /* Back pointer to the widget */
    LXPanel *panel;                 /* Back pointer to panel */
    GtkWidget *tray_icon;           /* Displayed image */
    config_setting_t *settings;     /* Plugin settings */
    int c_pos, c_level;
#ifdef __arm__
    int i2c_handle;
#else
    battery *batt;
#endif
} PtBattPlugin;


/* gdk_pixbuf_get_from_surface function from GDK+3 */

static cairo_format_t
gdk_cairo_format_for_content (cairo_content_t content)
{
  switch (content)
    {
    case CAIRO_CONTENT_COLOR:
      return CAIRO_FORMAT_RGB24;
    case CAIRO_CONTENT_ALPHA:
      return CAIRO_FORMAT_A8;
    case CAIRO_CONTENT_COLOR_ALPHA:
    default:
      return CAIRO_FORMAT_ARGB32;
    }
}

static cairo_surface_t *
gdk_cairo_surface_coerce_to_image (cairo_surface_t *surface,
                                   cairo_content_t  content,
                                   int              src_x,
                                   int              src_y,
                                   int              width,
                                   int              height)
{
  cairo_surface_t *copy;
  cairo_t *cr;

  copy = cairo_image_surface_create (gdk_cairo_format_for_content (content),
                                     width,
                                     height);

  cr = cairo_create (copy);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, surface, -src_x, -src_y);
  cairo_paint (cr);
  cairo_destroy (cr);

  return copy;
}

static void
convert_alpha (guchar *dest_data,
               int     dest_stride,
               guchar *src_data,
               int     src_stride,
               int     src_x,
               int     src_y,
               int     width,
               int     height)
{
  int x, y;

  src_data += src_stride * src_y + src_x * 4;

  for (y = 0; y < height; y++) {
    guint32 *src = (guint32 *) src_data;

    for (x = 0; x < width; x++) {
      guint alpha = src[x] >> 24;

      if (alpha == 0)
        {
          dest_data[x * 4 + 0] = 0;
          dest_data[x * 4 + 1] = 0;
          dest_data[x * 4 + 2] = 0;
        }
      else
        {
          dest_data[x * 4 + 0] = (((src[x] & 0xff0000) >> 16) * 255 + alpha / 2) / alpha;
          dest_data[x * 4 + 1] = (((src[x] & 0x00ff00) >>  8) * 255 + alpha / 2) / alpha;
          dest_data[x * 4 + 2] = (((src[x] & 0x0000ff) >>  0) * 255 + alpha / 2) / alpha;
        }
      dest_data[x * 4 + 3] = alpha;
    }

    src_data += src_stride;
    dest_data += dest_stride;
  }
}

static void
convert_no_alpha (guchar *dest_data,
                  int     dest_stride,
                  guchar *src_data,
                  int     src_stride,
                  int     src_x,
                  int     src_y,
                  int     width,
                  int     height)
{
  int x, y;

  src_data += src_stride * src_y + src_x * 4;

  for (y = 0; y < height; y++) {
    guint32 *src = (guint32 *) src_data;

    for (x = 0; x < width; x++) {
      dest_data[x * 3 + 0] = src[x] >> 16;
      dest_data[x * 3 + 1] = src[x] >>  8;
      dest_data[x * 3 + 2] = src[x];
    }

    src_data += src_stride;
    dest_data += dest_stride;
  }
}

/**
 * gdk_pixbuf_get_from_surface:
 * @surface: surface to copy from
 * @src_x: Source X coordinate within @surface
 * @src_y: Source Y coordinate within @surface
 * @width: Width in pixels of region to get
 * @height: Height in pixels of region to get
 *
 * Transfers image data from a #cairo_surface_t and converts it to an RGB(A)
 * representation inside a #GdkPixbuf. This allows you to efficiently read
 * individual pixels from cairo surfaces. For #GdkWindows, use
 * gdk_pixbuf_get_from_window() instead.
 *
 * This function will create an RGB pixbuf with 8 bits per channel.
 * The pixbuf will contain an alpha channel if the @surface contains one.
 *
 * Return value: (transfer full): A newly-created pixbuf with a reference
 *     count of 1, or %NULL on error
 */
GdkPixbuf *
gdk_pixbuf_get_from_surface  (cairo_surface_t *surface,
                              gint             src_x,
                              gint             src_y,
                              gint             width,
                              gint             height)
{
  cairo_content_t content;
  GdkPixbuf *dest;

  /* General sanity checks */
  g_return_val_if_fail (surface != NULL, NULL);
  g_return_val_if_fail (width > 0 && height > 0, NULL);

  content = cairo_surface_get_content (surface) | CAIRO_CONTENT_COLOR;
  dest = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
                         !!(content & CAIRO_CONTENT_ALPHA),
                         8,
                         width, height);

  surface = gdk_cairo_surface_coerce_to_image (surface, content,
                                               src_x, src_y,
                                               width, height);
  cairo_surface_flush (surface);
  if (cairo_surface_status (surface) || dest == NULL)
    {
      cairo_surface_destroy (surface);
      return NULL;
    }

  if (gdk_pixbuf_get_has_alpha (dest))
    convert_alpha (gdk_pixbuf_get_pixels (dest),
                   gdk_pixbuf_get_rowstride (dest),
                   cairo_image_surface_get_data (surface),
                   cairo_image_surface_get_stride (surface),
                   0, 0,
                   width, height);
  else
    convert_no_alpha (gdk_pixbuf_get_pixels (dest),
                      gdk_pixbuf_get_rowstride (dest),
                      cairo_image_surface_get_data (surface),
                      cairo_image_surface_get_stride (surface),
                      0, 0,
                      width, height);

  cairo_surface_destroy (surface);
  return dest;
}

/* PiTop-specific functions */

#ifdef __arm__
int i2cget (int handle, int address, int *data)
{
    int res = wiringPiI2CReadReg16 (handle, address);
    if (res < 0)
        return -1;
    else {
        *data = res;
        return 0;
    }
}
#endif


#define MAX_COUNT       20                     // Maximum number of trials
#define SLEEP_TIME      500                    // time between two i2cget in microsec

#define STAT_UNKNOWN 0
#define STAT_DISCHARGING 1
#define STAT_CHARGING 2
#define STAT_EXT_POWER 3

int charge_level (PtBattPlugin *pt, int *status, int *tim)
{
#ifdef __arm__
    int count, result, capacity, current, time;

    // capacity
    capacity = -1;
    count = 0;
    while ((capacity  <  0) && (count++ < MAX_COUNT))
    {
        result = i2cget (pt->i2c_handle, 0x0d, &capacity);
        if (result == 0)
        {
            if ((capacity > 100) || (capacity < 0)) capacity = -1;
        }
        usleep (SLEEP_TIME);
    }

    // current
    *status = STAT_UNKNOWN;
    count = 0;
    while ((*status == STAT_UNKNOWN) && (count++ < MAX_COUNT))
    {
        result = i2cget (pt->i2c_handle, 0x0a, &current);
        if (result == 0)
        {
            if (current > 32767) current -= 65536;
            if ((current > -5000) && (current < 5000) && (current != -1))
            {
                if (current < 0)
                    *status = STAT_DISCHARGING;
                else if (current > 0)
                    *status = STAT_CHARGING;
                else
                    *status = STAT_EXT_POWER;
            }
            else current = -32767;
        }
        usleep (SLEEP_TIME);
    }

    // charging/discharging time
    count = 0;
    time = -1;
    *tim = 0;
    if (*status == STAT_CHARGING)
    {
        while ((time < 0) && (count++ < MAX_COUNT))
        {
            result = i2cget (pt->i2c_handle, 0x13, &time);
            if (result == 0)
            {
                if ((time > 0) || (time < 1000)) *tim = time;
            }
            usleep (SLEEP_TIME);
        }
    }
    else if (*status == STAT_DISCHARGING)
    {
        while ((time < 0) && (count++ < MAX_COUNT))
        {
            result = i2cget (pt->i2c_handle, 0x12, &time);
            if (result == 0)
            {
                if ((time > 0) || (time < 1000)) *tim = time;
            }
            usleep (SLEEP_TIME);
        }
    }

    if (time == -1 && current == -1) *status = STAT_UNKNOWN;
    return capacity;
#else
    battery *b = pt->batt;
    int mins;
    if (b)
    {
        battery_update (b);
        if (battery_is_charging (b))
        {
            if (b->seconds <= 0)
                *status = STAT_EXT_POWER;
            else
                *status = STAT_CHARGING;
        }
        else
            *status = STAT_DISCHARGING;
        mins = b->seconds;
        mins /= 60;
        *tim = mins;
        return b->percentage;
    }
#endif
}

void draw_icon (PtBattPlugin *pt, int lev, int r, int g, int b, int powered)
{
    GdkPixbuf *new_pixbuf;
    cairo_t *cr;
    //cairo_format_t format;
    cairo_surface_t *surface;

    // get and clear the drawing surface
    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 36, 36);
    cr = cairo_create (surface);
    cairo_set_source_rgba (cr, 0, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, 36, 36);
    cairo_fill (cr);

    // draw base icon on surface
    cairo_set_source_rgb (cr, 0.5, 0.5, 0.5);
    cairo_rectangle (cr, 4, 11, 26, 14);
    cairo_rectangle (cr, 30, 15, 2, 6);
    cairo_fill (cr);

    // fill the battery
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_rectangle (cr, 5, 12, 24, 12);
    cairo_fill (cr);
    cairo_set_source_rgb (cr, r, g, b);
    cairo_rectangle (cr, 5, 12, lev, 12);
    cairo_fill (cr);

    if (powered)
    {
        cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
        cairo_rectangle (cr, 20, 13, 4, 10);
        cairo_rectangle (cr, 24, 15, 3, 2);
        cairo_rectangle (cr, 24, 19, 3, 2);
        cairo_rectangle (cr, 18, 14, 2, 8);
        cairo_rectangle (cr, 17, 15, 1, 6);
        cairo_rectangle (cr, 16, 16, 1, 4);
        cairo_rectangle (cr, 6, 17, 2, 2);
        cairo_rectangle (cr, 8, 16, 2, 2);
        cairo_rectangle (cr, 10, 17, 2, 2);
        cairo_rectangle (cr, 12, 18, 2, 2);
        cairo_rectangle (cr, 14, 17, 2, 2);
        cairo_fill (cr);
        cairo_set_source_rgba (cr, 0.3, 0.3, 0.3, 0.25);
        cairo_rectangle (cr, 17, 14, 1, 1);
        cairo_rectangle (cr, 17, 21, 1, 1);
        cairo_rectangle (cr, 19, 13, 1, 1);
        cairo_rectangle (cr, 19, 22, 1, 1);
        cairo_fill (cr);
        cairo_set_source_rgba (cr, 0.3, 0.3, 0.3, 0.25);
        cairo_rectangle (cr, 7, 16, 1, 1);
        cairo_rectangle (cr, 8, 18, 2, 1);
        cairo_rectangle (cr, 10, 16, 1, 1);
        cairo_rectangle (cr, 11, 19, 1, 1);
        cairo_rectangle (cr, 12, 17, 2, 1);
        cairo_rectangle (cr, 14, 19, 1, 1);
        cairo_fill (cr);
    }

    new_pixbuf = gdk_pixbuf_get_from_surface (surface, 0, 0, 36, 36);
    g_object_ref_sink (pt->tray_icon);
    gtk_image_set_from_pixbuf (GTK_IMAGE (pt->tray_icon), new_pixbuf);
    cairo_destroy (cr);
}

static gboolean charge_anim (PtBattPlugin *pt)
{
    if (pt->c_pos)
    {
        if (pt->c_pos < pt->c_level) pt->c_pos++;
        else pt->c_pos = 1;
        draw_icon (pt, pt->c_pos, 1, 1, 0, 1);
        return TRUE;
    }
    else return FALSE;
}


void update_icon (PtBattPlugin *pt)
{
    int capacity, status, w, time;
    float ftime;
    char str[255];

    // read the charge status
    capacity = charge_level (pt, &status, &time);
    if (status == STAT_UNKNOWN) return;
    ftime = time / 60.0;

    // fill the battery symbol
    if (capacity < 0) w = 0;
    else if (capacity > 100) w = 24;
    else w = (99 * capacity) / 400;

    if (status == STAT_CHARGING)
    {
        if (pt->c_pos == 0)
        {
            pt->c_pos = 1;
            g_timeout_add (250, (GSourceFunc) charge_anim, (gpointer) pt);
        }
        pt->c_level = w;
    }
    else
    {
        if (pt->c_pos != 0) pt->c_pos = 0;
    }

    if (status == STAT_CHARGING)
    {
        if (time < 90)
            sprintf (str, _("Charging : %d%%\nTime remaining = %d minutes"), capacity, time);
        else
            sprintf (str, _("Charging : %d%%\nTime remaining = %0.1f hours"), capacity, ftime);
    }
    else if (status == STAT_EXT_POWER)
    {
        sprintf (str, _("Charged : %d%%\nOn external power"), capacity);
        draw_icon (pt, w, 0, 1, 0, 1);
    }
    else
    {
        if (time < 90)
            sprintf (str, _("Discharging : %d%%\nTime remaining = %d minutes"), capacity, time);
        else
            sprintf (str, _("Discharging : %d%%\nTime remaining = %0.1f hours"), capacity, ftime);
        if (capacity <= 20) draw_icon (pt, w, 1, 0, 0, 0);
        else draw_icon (pt, w, 0, 1, 0, 0);
    }

    // set the tooltip
    gtk_widget_set_tooltip_text (pt->tray_icon, str);
}

static gboolean timer_event (PtBattPlugin *pt)
{
    update_icon (pt);
    return TRUE;
}

/* Plugin functions */

/* Handler for menu button click */
static gboolean ptbatt_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel)
{
    PtBattPlugin *pt = lxpanel_plugin_get_data (widget);

#ifdef ENABLE_NLS
    textdomain ( GETTEXT_PACKAGE );
#endif

    return FALSE;
}

/* Handler for system config changed message from panel */
static void ptbatt_configuration_changed (LXPanel *panel, GtkWidget *p)
{
    PtBattPlugin *pt = lxpanel_plugin_get_data (p);
    update_icon (pt);
}

/* Plugin destructor. */
static void ptbatt_destructor (gpointer user_data)
{
    PtBattPlugin *pt = (PtBattPlugin *) user_data;

    /* Deallocate memory */
    g_free (pt);
}

/* Plugin constructor. */
static GtkWidget *ptbatt_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    PtBattPlugin *pt = g_new0 (PtBattPlugin, 1);
    GtkWidget *p;
    int batt_found = 0;
    
#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

#ifdef __arm__
    pt->i2c_handle = wiringPiI2CSetup (0x0b);
    int count = 0;
    while (wiringPiI2CReadReg16 (pt->i2c_handle, 0x0d) < 0 && count++ < MAX_COUNT)
    {
        usleep (SLEEP_TIME);
    }
    if (count < MAX_COUNT) batt_found = 1;
#else
    pt->batt = battery_get (0);
    if (pt->batt) batt_found = 1;
#endif

    if (!batt_found) return NULL;

    pt->tray_icon = gtk_image_new ();
    gtk_widget_set_visible (pt->tray_icon, TRUE);

    /* Allocate top level widget and set into Plugin widget pointer. */
    pt->panel = panel;
    pt->plugin = p = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (pt->plugin), GTK_RELIEF_NONE);
    g_signal_connect (pt->plugin, "button-press-event", G_CALLBACK(ptbatt_button_press_event), NULL);
    pt->settings = settings;
    lxpanel_plugin_set_data (p, pt, ptbatt_destructor);
    gtk_widget_add_events (p, GDK_BUTTON_PRESS_MASK);

    /* Allocate icon as a child of top level */
    gtk_container_add (GTK_CONTAINER(p), pt->tray_icon);

    /* Show the widget */
    gtk_widget_show_all (p);

    /* Start timed events to monitor status */
    g_timeout_add (5000, (GSourceFunc) timer_event, (gpointer) pt);

    return p;
}

FM_DEFINE_MODULE(lxpanel_gtk, ptbatt)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Battery (pi-top / laptop)"),
    .description = N_("Monitors battery for pi-top and laptops"),
    .new_instance = ptbatt_constructor,
    .reconfigure = ptbatt_configuration_changed,
    .button_press_event = ptbatt_button_press_event,
    .gettext_package = GETTEXT_PACKAGE
};
