#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>

static GtkStatusIcon *status_icon;
static cairo_surface_t *favicon_surface;
static int notifications, alert;
static int want_status_icon, want_notifications;

static GSList *assets[2];
static const GRegex *internal_urls_regex;

static gchar *languages[2][8];
static gboolean permissions[4];
static gdouble zoom = 1.0;

static gboolean permission_request (WebKitWebView *web_view,
                                    WebKitPermissionRequest *request,
                                    gpointer user_data)
{
    gboolean b;

    /* Look up if the user has granted permission for the specific
     * kind of request. */

    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request)) {
        b = permissions[0];
    } else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request)) {
        b = permissions[1];
    } else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
        WebKitUserMediaPermissionRequest *media;

        media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
        if (webkit_user_media_permission_is_for_audio_device(media)) {
            b = permissions[2];
        } else if (webkit_user_media_permission_is_for_video_device(media)) {
            b = permissions[3];
        }
    } else {
        b = FALSE;
    }

    /* Respond accordingly. */

    if (b) {
        webkit_permission_request_allow (request);
        g_debug("Allowed permission request.");
    } else {
        webkit_permission_request_deny (request);
        g_debug("Denied permission request.");
    }

    return TRUE;
}

static void update_status_icon(const char *new_title,
                               bool update_icon,
                               bool update_notifications,
                               bool update_alert)
{
    GdkPixbuf *pixbuf;

    cairo_t *cr;
    cairo_surface_t *destination;
    cairo_text_extents_t extents;

    const char *t;
    char *s;

    int i, w, h, size;

    if (new_title) {
        gtk_status_icon_set_title (status_icon, new_title);
    }

    /* Update the tooltip, if necessary. */

    if (new_title || update_notifications) {
        t = gtk_status_icon_get_title(status_icon);

        if (notifications > 0) {
            s = g_strdup_printf("%d notifications", notifications);
        } else {
            s = NULL;
        }

        if (t && s) {
            char *ts;

            ts = g_strconcat(t, " - ", s, NULL);

            gtk_status_icon_set_tooltip_text (status_icon, ts);

            g_free(s);
            g_free(ts);
        } else if (s) {
            gtk_status_icon_set_tooltip_text (status_icon, s);
            g_free(s);
        } else {
            gtk_status_icon_set_tooltip_text (status_icon, t);
        }
    }

    if (!update_icon && !update_notifications && !update_alert) {
        return;
    }

    /* Proceed to update the status icon, if necessary. */

    if (!favicon_surface || !gtk_status_icon_is_embedded (status_icon)) {
        g_debug("Status icon is not available\n");
        return;
    }

    size = gtk_status_icon_get_size (status_icon);

    if (size == 0) {
        g_debug("Status icon has zero size.  Won't update.\n");
        return;
    }

    /* Draw the favicon. */

    w = cairo_image_surface_get_width (favicon_surface);
    h = cairo_image_surface_get_height (favicon_surface);

    destination = cairo_surface_create_similar (favicon_surface,
                                                CAIRO_CONTENT_COLOR_ALPHA,
                                                size, size);
    cr = cairo_create (destination);
    cairo_scale(cr, (double)size / w, (double)size / h);
    cairo_set_source_surface (cr, favicon_surface, 0, 0);
    cairo_paint(cr);

    /* Draw the notification text. */

    if (notifications > 0 || alert) {
        char text[2] = "!";

        if (notifications < 10 && !alert) {
            text[0] = notifications + '0';
        }

        cairo_identity_matrix(cr);
        cairo_select_font_face(cr, "Sans",
                               CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);

        for (i = 1 ; i >= 0 ; i -= 1) {
            double delta;

            if (i == 1) {
                cairo_set_source_rgba(cr, 0, 0, 0, .8);
            } else {
                cairo_set_source_rgba(cr,  1.0000, 0.6936, 0.0000, 1);
            }

            cairo_set_font_size(cr, (1 + i * 0.1) * size);
            cairo_text_extents(cr, text, &extents);

            delta = (double)i * size / 10.0;
            cairo_move_to(cr,
                          (size - extents.width) / 2.0 - extents.x_bearing + delta,
                          size - (size - extents.height) / 2.0 + delta);

            cairo_show_text (cr, text);
        }
    }

    cairo_destroy(cr);

    g_debug("Icon size is: %d\n", size);

    if ((pixbuf = gdk_pixbuf_get_from_surface (destination,
                                               0, 0, size, size))) {
        gtk_status_icon_set_from_pixbuf (status_icon, pixbuf);
        g_object_unref(pixbuf);
    } else {
        g_debug ("Could not use favicon to set application window icon.");
    }

    cairo_surface_destroy (destination);
}

static gboolean status_icon_activated (GtkStatusIcon *status_icon,
                                       GtkWindow *window)
{
    g_debug("Status icon clicked.\n");
    gtk_window_present(window);

    return TRUE;
}

static gboolean status_icon_size_changed (GtkStatusIcon *status_icon,
                                          gint size, gpointer user_data)
{
    g_debug("Status icon size changed to %d.\n", size);
    update_status_icon(NULL, TRUE, FALSE, FALSE);

    return TRUE;
}

static void notification_closed (WebKitNotification *notification,
                                 gpointer user_data)
{
    notifications -= 1;

    if (status_icon) {
        update_status_icon(NULL, FALSE, TRUE, FALSE);
    }

    g_debug("Notification closed (%d active).\n", notifications);
}


static gboolean show_notification (WebKitWebView *view,
                                   WebKitNotification *notification,
                                   gpointer user_data)
{
    /* Bind a handler to keep track of the notification's state, if
     * we're handling notifications that way. */

    if (want_notifications == 1) {
        g_signal_connect(notification, "closed",
                         G_CALLBACK(notification_closed), NULL);
    }

    notifications += 1;

    if (status_icon) {
        update_status_icon(NULL, FALSE, TRUE, FALSE);
    }

    g_debug("New notification (%d active).\n", notifications);

    return FALSE;
}

static gboolean decide_policy (WebKitWebView *view,
                               WebKitPolicyDecision *decision,
                               WebKitPolicyDecisionType decision_type,
                               gpointer user_data)
{
    WebKitNavigationPolicyDecision *navigation;
    WebKitNavigationAction *action;
    WebKitNavigationType type;
    WebKitURIRequest *request;
    const gchar *uri;

    switch (decision_type) {
    case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
        return FALSE;

    case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
        navigation = WEBKIT_NAVIGATION_POLICY_DECISION (decision);
        action = webkit_navigation_policy_decision_get_navigation_action (navigation);
        request = webkit_navigation_action_get_request(action);
        uri = webkit_uri_request_get_uri(request);
        type = webkit_navigation_action_get_navigation_type(action);

        switch (type) {
        case WEBKIT_NAVIGATION_TYPE_LINK_CLICKED:
            g_debug("Need to make a navigation policy decision for "
                    "link to '%s'.\n", uri);

            g_debug(decision_type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ?
                    "Should open in same window." : "Should open in new window.");

            if(internal_urls_regex &&
               g_regex_match (internal_urls_regex, uri, 0, NULL)) {
                g_debug("Opening URL '%s' internally.\n", uri);
                webkit_web_view_load_uri(view, uri);
                return FALSE;
            }

            g_debug("Launching URL '%s' externally.\n", uri);

            {
                GError *error = NULL;

                if (!g_app_info_launch_default_for_uri (uri, NULL, &error) &&
                    error != NULL) {
                    fprintf (stderr, "Could not launch URI: %s\n",
                             error->message);
                    g_error_free (error);
                }
            }

            break;

        case WEBKIT_NAVIGATION_TYPE_RELOAD:
        case WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED:
        case WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED:
        case WEBKIT_NAVIGATION_TYPE_OTHER:
            g_debug("Handling non-click navigation to URL '%s'.\n", uri);
            return FALSE;

        case WEBKIT_NAVIGATION_TYPE_BACK_FORWARD:
            webkit_policy_decision_ignore(decision);
            break;
        }

        break;
    default:
        /* Making no decision results in webkit_policy_decision_use(). */

        return FALSE;
    }

    return TRUE;
}

static gboolean focus_view (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    notifications = 0;

    if (status_icon) {
        update_status_icon(NULL, FALSE, TRUE, FALSE);
    }

    return FALSE;
}

static gboolean delete_window(GtkWidget *widget, GdkEvent *event,
                              gpointer user_data)
{
    if (0 && gtk_status_icon_is_embedded(status_icon)) {
        g_debug("Status icon is embedded; window minimizing window to tray.\n");
        gtk_widget_hide(widget);

        return TRUE;
    } else {
        return FALSE;
    }
}

static void destroy_window(GtkWidget *widget, GtkWidget *window)
{
    gtk_main_quit();
}

static gboolean close_web_view(WebKitWebView *view, GtkWidget *window)
{
    gtk_widget_destroy(window);
    return TRUE;
}

static gboolean get_favicon(WebKitWebView *view,
                            GParamSpec *pspec,
                            GtkWindow *window)
{
    GdkPixbuf *pixbuf;

    g_debug ("Favicon is available.");

    if(favicon_surface) {
        g_object_unref (favicon_surface);
        favicon_surface = NULL;
    }

    if (!(favicon_surface = webkit_web_view_get_favicon (view))) {
        g_debug ("Could not get favicon surface.");

        if (status_icon) {
            gtk_status_icon_set_visible(status_icon, FALSE);
            g_object_unref(G_OBJECT(status_icon));

            status_icon = NULL;
        }

        return TRUE;
    }

    if ((pixbuf = gdk_pixbuf_get_from_surface (
            favicon_surface, 0, 0,
            cairo_image_surface_get_width (favicon_surface),
            cairo_image_surface_get_height (favicon_surface)))) {
        gtk_window_set_icon (window, pixbuf);
        g_object_unref(pixbuf);
    } else {
        g_debug ("Could not use favicon to set application window icon.");
    }

    /* If we haven't already, create a status icon. */

    if (want_status_icon) {
        if (!status_icon) {
            status_icon = gtk_status_icon_new ();

            g_signal_connect(status_icon, "size-changed",
                             G_CALLBACK(status_icon_size_changed), NULL);

            g_signal_connect(status_icon, "activate",
                             G_CALLBACK(status_icon_activated), window);

            gtk_status_icon_set_visible (status_icon, TRUE);
        }

        update_status_icon(NULL, TRUE, FALSE, FALSE);
    }

    return TRUE;
}

static gboolean get_title(WebKitWebView *view,
                          GParamSpec *pspec,
                          GtkWindow *window)
{
    const char *title;

    title = webkit_web_view_get_title (view);

    gtk_window_set_title (window, title);

    if (status_icon) {
        update_status_icon(title, FALSE, FALSE, FALSE);
    }

    return TRUE;
}

static gboolean context_menu_handler (WebKitWebView *view,
                                      WebKitContextMenu *menu,
                                      GdkEvent *event,
                                      WebKitHitTestResult *hit_test_result,
                                      gpointer user_data)
{
    if (webkit_hit_test_result_context_is_image(hit_test_result) ||
        webkit_hit_test_result_context_is_link(hit_test_result)) {
        GList *l, *n;

        for (l = webkit_context_menu_get_items(menu) ; l ; l = n) {
            WebKitContextMenuAction a;

            n = l->next;
            a = webkit_context_menu_item_get_stock_action(l->data);

            if (a != WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD &&
                a != WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD &&
                a != WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD) {
                webkit_context_menu_remove(menu, l->data);
            }
        }

        return FALSE;
    } else {
        return !(webkit_hit_test_result_context_is_editable(hit_test_result) ||
                 webkit_hit_test_result_context_is_selection(hit_test_result));
    }
}

static void handle_script_message (WebKitUserContentManager *manager,
                                   WebKitJavascriptResult *js_result,
                                   gpointer user_data)
{
    JSGlobalContextRef global;
    JSValueRef value;

    value = webkit_javascript_result_get_value (js_result);
    global = webkit_javascript_result_get_global_context (js_result);

    if (JSValueIsString (global, value)) {
        JSStringRef js_string;
        gchar *message;
        gsize n;

        js_string = JSValueToStringCopy (global, value, NULL);
        n = JSStringGetMaximumUTF8CStringSize (js_string);
        message = (gchar *)g_malloc (n);
        JSStringGetUTF8CString (js_string, message, n);
        JSStringRelease (js_string);
        g_print ("Script result: %s\n", message);
        g_free (message);
    } else if (JSValueIsBoolean (global, value)) {
        alert = JSValueToBoolean(global, value);

        if (status_icon) {
            update_status_icon(NULL, FALSE, FALSE, TRUE);
        }

        g_debug ("Received bool script message: %d\n", alert);
    } else {
        g_warning ("Error running javascript: unexpected return value");
    }
}

static gboolean add_language (const gchar *option_name,
                              const gchar *value,
                              gpointer data,
                              GError **error)
{
    int i, j;

    if (!strcmp(option_name, "--lang") ||
        !strcmp(option_name, "-l")) {
        j = 0;
    } else if (!strcmp(option_name, "--spell") ||
        !strcmp(option_name, "-s")) {
        j = 1;
    } else {
        g_assert_not_reached();
    }

    for (i = 0 ; languages[j][i] && i < 8 ; i += 1);

    if (i == 7) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_FAILED,
                    "Can't set more than 7 languages.");

        return FALSE;
    }

    languages[j][i] = g_strdup(value);

    return TRUE;
}

static gboolean add_permission (const gchar *option_name,
                                const gchar *value,
                                gpointer data,
                                GError **error)
{
    if (!strcmp(value, "geolocation")) {
        permissions[0] = TRUE;
    } else if (!strcmp(value, "notification")) {
        permissions[1] = TRUE;
    } else if (!strcmp(value, "audio")) {
        permissions[2] = TRUE;
    } else if (!strcmp(value, "video")) {
        permissions[3] = TRUE;
    } else {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    "Unknown type of permission request '%s' "
                    "supplied to --permit.", value);

        return FALSE;
    }

    return TRUE;
}

static gboolean select_notification_method (const gchar *option_name,
                                            const gchar *value,
                                            gpointer data,
                                            GError **error)
{
    if (!strcmp(value, "lifecycle")) {
        want_notifications = 1;
    } else if (!strcmp(value, "focus")) {
        want_notifications = 2;
    } else {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    "Unknown type of notification mode '%s' "
                    "supplied to --notifications.", value);

        return FALSE;
    }

    return TRUE;
}

static gboolean add_user_asset (const gchar *option_name,
                                const gchar *value,
                                gpointer data,
                                GError **error)
{
    int i;

    if (!strcmp(option_name, "--script") ||
        !strcmp(option_name, "-j")) {
        i = 0;
    } else if (!strcmp(option_name, "--style") ||
        !strcmp(option_name, "-c")) {
        i = 1;
    } else {
        g_assert_not_reached();
    }

    assets[i] = g_slist_prepend (assets[i], (gpointer)g_strdup(value));

    return TRUE;
}

static gboolean set_internal_url_pattern(const gchar *option_name,
                                         const gchar *value,
                                         gpointer data,
                                         GError **error)
{
    GError *regex_error = NULL;

    internal_urls_regex = g_regex_new(value, G_REGEX_OPTIMIZE,
                                    G_REGEX_MATCH_NOTEMPTY,
                                    &regex_error);

    if (!internal_urls_regex  && regex_error != NULL) {
        g_set_error(error,
                    G_OPTION_ERROR,
                    G_OPTION_ERROR_BAD_VALUE,
                    "Could not parse URL pattern '%s'.", value);

        g_error_free (regex_error);

        return FALSE;
    }

    return TRUE;
}

int main(int argc, char* argv[])
{
    GtkWidget *window;
    WebKitWebView *view;
    WebKitWebContext *context;
    WebKitUserContentManager *content;

    GError *error = NULL;
    GSList *s;

    char *url;
    int i;

    GOptionEntry options[] = {
        {"script", 'j', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &add_user_asset, "Add a user script", "FILE"},
        {"style", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &add_user_asset, "Add a user style sheet", "FILE"},
        {"permit", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &add_permission, "Permit a certain type of request", "REQUEST"},
        {"spell", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &add_language, "Add spell checker language", "LANG"},
        {"lang", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &add_language, "Add preferred language", "LANG"},
        {"url", 'u', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &url,
         "The wrapped URL", "URL"},
        {"tray", 't', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &want_status_icon,
         "Add an icon to the system tray", NULL},
        {"zoom", 'z', G_OPTION_FLAG_NONE, G_OPTION_ARG_DOUBLE, &zoom,
         "The zoom level", NULL},
        {"notifications", 'n', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &select_notification_method,
         "Track pending notifications with the specified method.", "METHOD"},
        {"internal", 'i', G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK,
         &set_internal_url_pattern,
         "Set pattern of URLs to open within the wrapper.", "REGEX"},
        {NULL}
    };

    if (!gtk_init_with_args(&argc, &argv,
                            "- wrap a WebKit view around a URL",
                            options, NULL, &error)) {
        fprintf (stderr, "%s: %s\n", g_get_prgname(), error->message);
        g_error_free (error);

        return 1;
    }

    /* Create a user content manager. */

    content = webkit_user_content_manager_new ();

    g_signal_connect (content, "script-message-received::alerts",
                      G_CALLBACK (handle_script_message), NULL);
    webkit_user_content_manager_register_script_message_handler (
        content, "alerts");

    /* Load user-provided scripts and style sheets. */

    for (i = 0 ; i < 2 ; i += 1) {
        for (s = assets[i] ; s ; s = s->next) {
            gchar *source;

            GError *error = NULL;

            if (!g_file_get_contents (s->data, &source, NULL, &error)) {
                fprintf (stderr, "Could not read asset source: %s\n",
                         error->message);
                g_error_free (error);
            } else if (i == 0) {
                WebKitUserScript *script;

                script = webkit_user_script_new (
                    source,
                    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                    NULL, NULL);

                webkit_user_content_manager_add_script(content, script);

                g_debug ("Added user script %s.\n", (gchar *)s->data);
            } else {
                WebKitUserStyleSheet *sheet;

                sheet = webkit_user_style_sheet_new (
                    source,
                    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                    WEBKIT_USER_STYLE_LEVEL_USER,
                    NULL, NULL);

                webkit_user_content_manager_add_style_sheet(content, sheet);

                g_debug ("Added user style sheet %s.\n", (gchar *)s->data);
            }

            g_free(source);
        }

        g_slist_free_full (assets[i], g_free);
    }
    /* Create a WebKit context. */

    {
        WebKitWebsiteDataManager *manager;
        char *base, *home;

        int i, j;

        home = getenv("HOME");
        if (!home) {
            home = "/tmp";
        }

        base = strcat(home, "/.cache/webkit-wrapper");
        g_debug("Setting base directory to %s", base);

        manager = webkit_website_data_manager_new ("base-cache-directory", base,
                                                   "base-data-directory", base,
                                                   NULL);

        context = webkit_web_context_new_with_website_data_manager(manager);
        webkit_web_context_set_favicon_database_directory(context, base);

        /* Set langauge-related stuff. */

        if(languages[0][0]) {
            webkit_web_context_set_preferred_languages(
                context,
                (const gchar * const *)languages[0]);
        }

        if(languages[1][0]) {
            webkit_web_context_set_spell_checking_languages (
                context, (const gchar * const *)languages[1]);

            webkit_web_context_set_spell_checking_enabled (context, TRUE);
        }

        /* Free the language vectors. */

        for (j = 0 ; j < 2 ; j += 1) {
            for (i = 0 ; languages[j][i] ; i += 1) {
                g_free(languages[j][i]);
            }
        }
    }

    /* Initialize the Gtk and create a window and view. */

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                        "user-content-manager", content,
                                        "web-context", context,
                                        NULL
));
    webkit_web_view_set_zoom_level(view, zoom);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

    /* Set up signals. */

    g_signal_connect(window, "delete_event", G_CALLBACK (delete_window), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(destroy_window), NULL);
    g_signal_connect(view, "close", G_CALLBACK(close_web_view), window);
    g_signal_connect(view, "permission-request",
                     G_CALLBACK(permission_request), NULL);
    g_signal_connect (view, "decide-policy",
                      G_CALLBACK (decide_policy), NULL);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(get_favicon), window);
    g_signal_connect(view, "notify::title", G_CALLBACK(get_title), window);
    g_signal_connect (view, "context-menu",
                      G_CALLBACK (context_menu_handler), NULL);

    /* Bind a handler to keep track of notifications, if requested. */

    if (want_notifications) {
        g_signal_connect (view, "show-notification",
                          G_CALLBACK (show_notification), NULL);
    }

    /* Bind a handler to keep track of the view's focus state, to
     * clear pending notifications if that mode was selected. */

    if (want_notifications == 2) {
        g_signal_connect(view, "focus-in-event", G_CALLBACK(focus_view), NULL);
    }

    /* Load the specified web page and show the window. */

    g_debug("Loading URL.");

    webkit_web_view_load_uri(view, url);

    gtk_widget_grab_focus(GTK_WIDGET(view));
    gtk_widget_show_all(window);

    /* Run the main GTK+ event loop. */

    gtk_main();

    if (status_icon) {
        gtk_status_icon_set_visible(status_icon, FALSE);
        g_object_unref(G_OBJECT(status_icon));
    }

    return 0;
}
