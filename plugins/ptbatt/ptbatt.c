#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <glib/gi18n.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <asm/ioctl.h>
#include "batt_sys.h"

#include "plugin.h"

//#define TEST_MODE
//#define CHARGE_ANIM

/* Plug-in global data */

typedef struct {
    GtkWidget *plugin;              /* Back pointer to the widget */
    LXPanel *panel;                 /* Back pointer to panel */
    GtkWidget *tray_icon;           /* Displayed image */
    config_setting_t *settings;     /* Plugin settings */
#ifdef CHARGE_ANIM
    int c_pos;                      /* Used for charging animation */
    int c_level;                    /* Used for charging animation */
#endif
    battery *batt;
    GdkPixbuf *plug;
    GdkPixbuf *flash;
#ifdef __arm__
    int i2c_handle;
#endif
} PtBattPlugin;

/* Battery states */

typedef enum
{
    STAT_UNKNOWN,
    STAT_DISCHARGING,
    STAT_CHARGING,
    STAT_EXT_POWER
} status_t;


/* i2c support for pi-top */

#ifdef __arm__

static int i2chandle (void)
{
    FILE *fp;
    int fd;

    fp = fopen ("/dev/i2c-1", "rb");
    if (fp)
    {
        fclose (fp);
        fd = open ("/dev/i2c-1", O_RDWR);
        if (fd >= 0)
        {
            if (ioctl (fd, 0x0703, 0x0b) >= 0) return fd;
        }
    }

    fp = fopen ("/dev/i2c-0", "rb");
    if (fp)
    {
        fclose (fp);
        fd = open ("/dev/i2c-1", O_RDWR);
        if (fd >= 0)
        {
            if (ioctl (fd, 0x0703, 0x0b) >= 0) return fd;
        }
    }
    return 0;
}

static int i2cget (int handle, int address)
{
    struct i2c_smbus_ioctl_data
    {
        char read_write;
        uint8_t command;
        int size;
        uint16_t *data;
    } args;
    uint16_t data;

    int count = 0;
    while (count++ < 20)
    {
        args.read_write = 1;
        args.command = address;
        args.size = 3;
        args.data = &data;
        if (!ioctl (handle, 0x0720, &args))
             return data & 0xFFFF;
        usleep (1000);
    }
    return -1;
}

#endif


/* Initialise measurements and check for a battery */

static int init_measurement (PtBattPlugin *pt)
{
#ifdef TEST_MODE
    return 1;
#endif
#ifdef __arm__
    pt->i2c_handle = i2chandle ();
    if (pt->i2c_handle)
    {
        // i2c available - look for a battery
        if (i2cget (pt->i2c_handle, 0x0d) > 0) return 1;

        // if none found, remove the handle
        pt->i2c_handle = 0;
    }
#endif
    int val;
    if (config_setting_lookup_int (pt->settings, "BattNum", &val))
        pt->batt = battery_get (val);
    else
        pt->batt = battery_get (0);
    if (pt->batt) return 1;

    return 0;
}


/* Read current capacity, status and time remaining from battery */

static int charge_level (PtBattPlugin *pt, status_t *status, int *tim)
{
#ifdef TEST_MODE
    *status = STAT_CHARGING;
    *tim = 30;
    return 50;
#endif
    *status = STAT_UNKNOWN;
    *tim = 0;
#ifdef __arm__
    if (pt->i2c_handle)
    {
        int capacity, current, time;

        // capacity
        capacity = i2cget (pt->i2c_handle, 0x0d);
        if (capacity > 100) capacity = -1;

        // current
        current = i2cget (pt->i2c_handle, 0x0a);
        if (current != -1)
        {
            if (current > 32767) current -= 65536;
            if (current > -5000 && current < 5000)
            {
                if (current < 0) *status = STAT_DISCHARGING;
                else if (current > 0) *status = STAT_CHARGING;
                else *status = STAT_EXT_POWER;
            }
        }

        // charging/discharging time
        if (*status == STAT_CHARGING)
            time = i2cget (pt->i2c_handle, 0x13);
        else if (*status == STAT_DISCHARGING)
            time = i2cget (pt->i2c_handle, 0x12);
        else time = 0;
        if (time == -1) *status = STAT_UNKNOWN;
        else if (time < 1000) *tim = time;

        return capacity;
    }
#endif
    battery *b = pt->batt;
    int mins;
    if (b)
    {
        battery_update (b);
        if (battery_is_charging (b))
        {
            if (strcasecmp (b->state, "full") == 0) *status = STAT_EXT_POWER;
            else *status = STAT_CHARGING;
        }
        else *status = STAT_DISCHARGING;
        mins = b->seconds;
        mins /= 60;
        *tim = mins;
        return b->percentage;
    }
    else return -1;
}


/* Draw the icon in relevant colour and fill level */

#define DIM 36

static void draw_icon (PtBattPlugin *pt, int lev, float r, float g, float b, int powered)
{
    // create and clear the drawing surface
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, DIM, DIM);
    cairo_t *cr = cairo_create (surface);
    cairo_set_source_rgba (cr, 0, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, DIM, DIM);
    cairo_fill (cr);

    // draw base icon on surface
    cairo_set_source_rgb (cr, b, g, r);

    cairo_rectangle (cr, 4, 11, 26, 14);
    cairo_rectangle (cr, 5, 10, 24, 1);
    cairo_rectangle (cr, 5, 25, 24, 1);
    cairo_rectangle (cr, 3, 12, 1, 12);
    cairo_rectangle (cr, 30, 12, 1, 12);
    cairo_rectangle (cr, 31, 15, 2, 6);
    cairo_fill (cr);

    // fill the battery
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_rectangle (cr, 5, 12, 24, 12);
    cairo_fill (cr);
    cairo_set_source_rgb (cr, b, g, r);
    cairo_rectangle (cr, 5, 12, lev, 12);
    cairo_fill (cr);

    // show icons
    if (powered == 1 && pt->flash)
    {
        gdk_cairo_set_source_pixbuf (cr, pt->flash, 2, 2);
        cairo_paint (cr);
    }
    if (powered == 2 && pt->plug)
    {
        gdk_cairo_set_source_pixbuf (cr, pt->plug, 2, 2);
        cairo_paint (cr);
    }

    // create a pixbuf from the cairo surface
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (cairo_image_surface_get_data (surface),
        GDK_COLORSPACE_RGB, TRUE, 8, DIM, DIM, DIM * 4, NULL, NULL);

    // copy the pixbuf to the icon resource
    g_object_ref_sink (pt->tray_icon);
    gtk_image_set_from_pixbuf (GTK_IMAGE (pt->tray_icon), pixbuf);

    g_object_ref_sink (pixbuf);
    cairo_destroy (cr);
}


/* Update the "filling" animation while charging */
#ifdef CHARGE_ANIM
static gboolean charge_anim (PtBattPlugin *pt)
{
    if (pt->c_pos)
    {
        if (pt->c_pos < pt->c_level) pt->c_pos++;
        else pt->c_pos = 1;
        draw_icon (pt, pt->c_pos, 1, 0.75, 0, 1);
        return TRUE;
    }
    else return FALSE;
}
#endif

/* Read the current charge state and update the icon accordingly */

static void update_icon (PtBattPlugin *pt)
{
    int capacity, w, time;
    status_t status;
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

#ifdef CHARGE_ANIM
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
#endif

    if (status == STAT_CHARGING)
    {
        if (time <= 0)
            sprintf (str, _("Charging : %d%%"), capacity);
        else if (time < 90)
            sprintf (str, _("Charging : %d%%\nTime remaining = %d minutes"), capacity, time);
        else
            sprintf (str, _("Charging : %d%%\nTime remaining = %0.1f hours"), capacity, ftime);
#ifndef CHARGE_ANIM
        draw_icon (pt, w, 1, 0.75, 0, 1);
#endif
    }
    else if (status == STAT_EXT_POWER)
    {
        sprintf (str, _("Charged : %d%%\nOn external power"), capacity);
        draw_icon (pt, w, 0, 0.8, 0, 2);
    }
    else
    {
        if (time <= 0)
            sprintf (str, _("Discharging : %d%%"), capacity);
        else if (time < 90)
            sprintf (str, _("Discharging : %d%%\nTime remaining = %d minutes"), capacity, time);
        else
            sprintf (str, _("Discharging : %d%%\nTime remaining = %0.1f hours"), capacity, ftime);
        if (capacity <= 20) draw_icon (pt, w, 1, 0, 0, 0);
        else draw_icon (pt, w, 0, 0.8, 0, 0);
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

    pt->settings = settings;
    
#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    /* Initialise measurements and check for a battery */
    if (!init_measurement (pt)) return NULL;

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

    /* Load the symbols */
    pt->plug = gdk_pixbuf_new_from_file ("/usr/share/lxpanel/images/plug.png", NULL);
    pt->flash = gdk_pixbuf_new_from_file ("/usr/share/lxpanel/images/flash.png", NULL);

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
