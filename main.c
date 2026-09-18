#include <gtk/gtk.h>
#include <libfprint-2/fp-context.h>
#include <libfprint-2/fp-device.h>
#include <libfprint-2/fp-image.h>
#include <stdarg.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *device_combo;
    GtkWidget *device_info_label;
    GtkWidget *status_label;
    GtkWidget *preview_image;
    GtkWidget *preview_scroll;
    GtkWidget *show_minutiae_check;
    GtkWidget *start_button;
    GtkWidget *stop_button;
    FpContext *context;
    FpDevice *active_device;
    GCancellable *capture_cancellable;
    gboolean capture_running;
    gboolean capture_highres;
    gboolean show_minutiae;
    FpImage *current_image;
    FpImage *pending_minutiae_image;
    gboolean minutiae_detection_running;
    guint status_poll_id;
} AppData;

/*
 * Logging helpers.
 * These keep status updates in one place and also print to stderr for debugging.
 */
static void set_status(AppData *app, const char *format, ...) {
    va_list args;
    g_autofree gchar *message = NULL;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    g_printerr("[fprint-demo-gtk] %s\n", message);
    gtk_label_set_text(GTK_LABEL(app->status_label), message);
}

static void set_status_from_error(AppData *app, const char *prefix, GError *error) {
    if (error && error->message) {
        set_status(app, "%s: %s", prefix, error->message);
    } else {
        set_status(app, "%s", prefix);
    }
}

static gboolean is_fatal_device_error(GError *error) {
    if (!error) {
        return FALSE;
    }

    if (g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_TOO_HOT)) {
        return TRUE;
    }

    if (g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED)) {
        return TRUE;
    }

    if (g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_UNTRUSTED)) {
        return TRUE;
    }

    return FALSE;
}

static void handle_capture_error(AppData *app, GError *error, const char *fallback_message) {
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        set_status(app, "Capture cancelled.");
    } else if (error && g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY)) {
        set_status(app, "Device is busy or already in use. Stop any other capture process and try again.");
    } else if (error && g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED)) {
        set_status(app, "High-resolution mode is not supported on this device; falling back to standard mode.");
    } else if (is_fatal_device_error(error)) {
        set_status(app, "Fingerprint reader is in a fatal state. Stopping capture.");
    } else {
        set_status_from_error(app, fallback_message, error);
    }
}

static void free_pixels(guchar *pixels, gpointer user_data) {
    (void)user_data;
    g_free(pixels);
}

static GdkPixbuf *convert_fpimage_to_pixbuf(FpImage *image) {
    guint width = fp_image_get_width(image);
    guint height = fp_image_get_height(image);
    gsize length = 0;
    const guchar *data = fp_image_get_data(image, &length);

    if (!data || length < width * height) {
        return NULL;
    }

    guchar *pixels = g_malloc(width * height * 3);
    for (gsize i = 0; i < width * height; i++) {
        guchar value = data[i];
        pixels[i * 3 + 0] = value;
        pixels[i * 3 + 1] = value;
        pixels[i * 3 + 2] = value;
    }

    return gdk_pixbuf_new_from_data(pixels,
                                    GDK_COLORSPACE_RGB,
                                    FALSE,
                                    8,
                                    width,
                                    height,
                                    width * 3,
                                    free_pixels,
                                    NULL);
}

static gboolean extract_vid_pid(const gchar *text, gchar **vendor, gchar **product) {
    if (!text) {
        return FALSE;
    }

    const gchar *patterns[] = {
        "([0-9A-Fa-f]{4})[-:]([0-9A-Fa-f]{4})",
        "\\b([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})\\b",
        "VID[ \t]*[:=][ \t]*([0-9A-Fa-f]{4}).*PID[ \t]*[:=][ \t]*([0-9A-Fa-f]{4})",
        "ID[ \t]*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})",
        "idVendor[ \t]*[:=][ \t]*([0-9A-Fa-f]{4}).*idProduct[ \t]*[:=][ \t]*([0-9A-Fa-f]{4})",
        "usb[:/].*([0-9A-Fa-f]{4})[:/]([0-9A-Fa-f]{4})"
    };

    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        GError *error = NULL;
        GRegex *regex = g_regex_new(patterns[i], G_REGEX_CASELESS, 0, &error);
        if (!regex) {
            g_clear_error(&error);
            continue;
        }

        GMatchInfo *match_info = NULL;
        gboolean matched = g_regex_match(regex, text, 0, &match_info);
        if (matched && g_match_info_matches(match_info)) {
            *vendor = g_match_info_fetch(match_info, 1);
            *product = g_match_info_fetch(match_info, 2);
            g_match_info_free(match_info);
            g_regex_unref(regex);
            return TRUE;
        }

        g_match_info_free(match_info);
        g_regex_unref(regex);
    }

    return FALSE;
}

static gchar *build_udev_rule(const gchar *device_id) {
    gchar *vendor = NULL;
    gchar *product = NULL;

    if (!extract_vid_pid(device_id, &vendor, &product)) {
        g_free(vendor);
        g_free(product);
        return g_strdup("");
    }

    gchar *rule = g_strdup_printf(
        "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"%s\", ATTR{idProduct}==\"%s\", MODE=\"0660\", GROUP=\"plugdev\"",
        vendor,
        product);
    g_free(vendor);
    g_free(product);
    return rule;
}

static const gchar *scan_type_to_string(FpScanType scan_type) {
    switch (scan_type) {
        case FP_SCAN_TYPE_SWIPE:
            return "Swipe";
        case FP_SCAN_TYPE_PRESS:
            return "Press";
        default:
            return "Unknown";
    }
}

static const gchar *temperature_to_string(FpTemperature temperature) {
    switch (temperature) {
        case FP_TEMPERATURE_COLD:
            return "Cold";
        case FP_TEMPERATURE_WARM:
            return "Warm";
        case FP_TEMPERATURE_HOT:
            return "Hot";
        default:
            return "Unknown";
    }
}

static void minutiae_detected(GObject *source_object, GAsyncResult *result, gpointer user_data);

static gchar *features_to_string(FpDeviceFeature features) {
    if (features == FP_DEVICE_FEATURE_NONE) {
        return g_strdup("none");
    }

    GString *builder = g_string_new(NULL);
    struct {
        FpDeviceFeature flag;
        const gchar *name;
    } map[] = {
        { FP_DEVICE_FEATURE_CAPTURE, "Capture" },
        { FP_DEVICE_FEATURE_IDENTIFY, "Identify" },
        { FP_DEVICE_FEATURE_VERIFY, "Verify" },
        { FP_DEVICE_FEATURE_STORAGE, "Storage" },
        { FP_DEVICE_FEATURE_STORAGE_LIST, "Storage list" },
        { FP_DEVICE_FEATURE_STORAGE_DELETE, "Storage delete" },
        { FP_DEVICE_FEATURE_STORAGE_CLEAR, "Storage clear" },
        { FP_DEVICE_FEATURE_DUPLICATES_CHECK, "Duplicates check" },
        { FP_DEVICE_FEATURE_ALWAYS_ON, "Always on" },
        { FP_DEVICE_FEATURE_UPDATE_PRINT, "Update print" },
    };

    for (guint i = 0; i < G_N_ELEMENTS(map); i++) {
        if (features & map[i].flag) {
            if (builder->len > 0) {
                g_string_append(builder, ", ");
            }
            g_string_append(builder, map[i].name);
        }
    }

    if (builder->len == 0) {
        g_string_append(builder, "none");
    }

    return g_string_free(builder, FALSE);
}

static void draw_minutiae_on_pixbuf(GdkPixbuf *pixbuf, GPtrArray *minutiae) {
    gint width = gdk_pixbuf_get_width(pixbuf);
    gint height = gdk_pixbuf_get_height(pixbuf);
    gint rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    gint n_channels = gdk_pixbuf_get_n_channels(pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

    for (guint i = 0; i < minutiae->len; i++) {
        FpMinutia *minutia = g_ptr_array_index(minutiae, i);
        gint x, y;
        fp_minutia_get_coords(minutia, &x, &y);
        for (gint dy = -2; dy <= 2; dy++) {
            for (gint dx = -2; dx <= 2; dx++) {
                gint px = x + dx;
                gint py = y + dy;
                if (px < 0 || px >= width || py < 0 || py >= height) {
                    continue;
                }
                guchar *p = pixels + py * rowstride + px * n_channels;
                p[0] = 255;
                p[1] = 0;
                p[2] = 0;
            }
        }
    }
}

static void render_preview_from_image(AppData *app, FpImage *image) {
    if (!image) {
        return;
    }

    GdkPixbuf *pixbuf = convert_fpimage_to_pixbuf(image);
    if (!pixbuf) {
        set_status(app, "Unable to convert fingerprint image to display format.");
        return;
    }

    if (app->show_minutiae) {
        if (!app->minutiae_detection_running || app->pending_minutiae_image != image) {
            if (!app->minutiae_detection_running) {
                app->pending_minutiae_image = g_object_ref(image);
                app->minutiae_detection_running = TRUE;
                fp_image_detect_minutiae(app->pending_minutiae_image,
                                         NULL,
                                         minutiae_detected,
                                         app);
            }
        }
    }

    gtk_image_set_from_pixbuf(GTK_IMAGE(app->preview_image), pixbuf);
    gtk_widget_set_size_request(app->preview_image, gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(app->preview_scroll), gdk_pixbuf_get_width(pixbuf));
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(app->preview_scroll), gdk_pixbuf_get_height(pixbuf));
    g_object_unref(pixbuf);
}

static void minutiae_detected(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    AppData *app = user_data;
    GError *error = NULL;
    FpImage *image = FP_IMAGE(source_object);
    gboolean ok = fp_image_detect_minutiae_finish(image, result, &error);
    if (!ok) {
        if (error && error->message) {
            g_printerr("[fprint-demo-gtk] Minutiae detection warning: %s\n", error->message);
        } else {
            g_printerr("[fprint-demo-gtk] Minutiae detection warning: no minutiae found. Continuing capture.\n");
        }
        g_clear_error(&error);
        app->minutiae_detection_running = FALSE;
        if (app->pending_minutiae_image == image) {
            g_clear_object(&app->pending_minutiae_image);
        }
        return;
    }

    if (!app->show_minutiae || app->pending_minutiae_image != image) {
        app->minutiae_detection_running = FALSE;
        if (app->pending_minutiae_image == image) {
            g_clear_object(&app->pending_minutiae_image);
        }
        return;
    }

    GPtrArray *minutiae = fp_image_get_minutiae(image);
    GdkPixbuf *pixbuf = convert_fpimage_to_pixbuf(image);
    if (pixbuf && minutiae) {
        draw_minutiae_on_pixbuf(pixbuf, minutiae);
        gtk_image_set_from_pixbuf(GTK_IMAGE(app->preview_image), pixbuf);
    }
    if (pixbuf) {
        g_object_unref(pixbuf);
    }
    app->minutiae_detection_running = FALSE;
    g_clear_object(&app->pending_minutiae_image);
}

static void show_udev_rule_dialog(GtkWindow *parent, const gchar *device_id, const gchar *message) {
    gchar *rule = build_udev_rule(device_id);
    if (!rule || *rule == '\0') {
        g_printerr("[fprint-demo-gtk] No udev rule could be suggested for device id: %s\n", device_id ? device_id : "(none)");
        g_free(rule);
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Udev rule suggestion",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Copy to clipboard",
        GTK_RESPONSE_APPLY,
        "Close",
        GTK_RESPONSE_CLOSE,
        NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new(message);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(content_area), label, FALSE, FALSE, 6);

    GtkWidget *rule_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(rule_entry), rule);
    gtk_editable_set_editable(GTK_EDITABLE(rule_entry), FALSE);
    gtk_entry_set_width_chars(GTK_ENTRY(rule_entry), 80);
    gtk_box_pack_start(GTK_BOX(content_area), rule_entry, FALSE, FALSE, 6);

    gtk_widget_show_all(content_area);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_APPLY) {
        GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, rule, -1);
    }

    gtk_widget_destroy(dialog);
    g_free(rule);
}

static void update_device_info(AppData *app);
static void render_preview_from_image(AppData *app, FpImage *image);

static gboolean poll_device_info(gpointer user_data) {
    AppData *app = user_data;
    update_device_info(app);
    return G_SOURCE_CONTINUE;
}

static void on_show_minutiae_toggled(GtkToggleButton *button, gpointer user_data) {
    AppData *app = user_data;
    app->show_minutiae = gtk_toggle_button_get_active(button);
    if (app->current_image) {
        render_preview_from_image(app, app->current_image);
    }
}

static void on_device_combo_changed(GtkComboBox *combo, gpointer user_data) {
    (void)combo;
    AppData *app = user_data;
    update_device_info(app);
}

/*
 * Device info and discovery.
 */
static void update_device_info(AppData *app) {
    gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->device_combo));
    if (active < 0) {
        gtk_label_set_text(GTK_LABEL(app->device_info_label), "Device info: none selected.");
        return;
    }

    fp_context_enumerate(app->context);
    GPtrArray *devices = fp_context_get_devices(app->context);
    if (!devices || active >= (gint)devices->len) {
        gtk_label_set_text(GTK_LABEL(app->device_info_label), "Device info: selected device is unavailable.");
        return;
    }

    FpDevice *device = g_object_ref(g_ptr_array_index(devices, active));

    const gchar *name = fp_device_get_name(device);
    const gchar *driver = fp_device_get_driver(device);
    const gchar *id = fp_device_get_device_id(device);
    gboolean is_open = fp_device_is_open(device);
    const gchar *scan_type = scan_type_to_string(fp_device_get_scan_type(device));
    const gchar *temperature = temperature_to_string(fp_device_get_temperature(device));
    gint stages = fp_device_get_nr_enroll_stages(device);
    gchar *features = features_to_string(fp_device_get_features(device));

    gchar *info = g_strdup_printf(
        "Device: %s | Driver: %s | ID: %s\nState: %s | Scan: %s | Temp: %s\nFeatures: %s | Stages: %d",
        name ? name : "Unknown",
        driver ? driver : "unknown",
        id ? id : "unknown",
        is_open ? "Open" : "Closed",
        scan_type,
        temperature,
        features,
        stages);
    gtk_label_set_text(GTK_LABEL(app->device_info_label), info);
    g_free(features);
    g_free(info);
    g_clear_object(&device);
}

static void set_control_state(AppData *app) {
    gtk_widget_set_sensitive(app->start_button, !app->capture_running);
    gtk_widget_set_sensitive(app->stop_button, app->capture_running);
}

static void close_active_device(AppData *app) {
    if (app->active_device) {
        fp_device_close_sync(app->active_device, NULL, NULL);
        g_clear_object(&app->active_device);
    }
    g_clear_object(&app->capture_cancellable);
}

static void start_capture_loop(AppData *app);

static void stop_capture_and_close(AppData *app) {
    app->capture_running = FALSE;
    set_control_state(app);
    close_active_device(app);
    update_device_info(app);
}

/*
 * Capture control.
 */

static void capture_complete(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    (void)source_object;
    AppData *app = user_data;
    GError *error = NULL;
    FpImage *image = fp_device_capture_finish(app->active_device, result, &error);

    if (!image) {
        gboolean fatal_error = is_fatal_device_error(error);
        gboolean highres_not_supported = error && g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_NOT_SUPPORTED);
        gboolean retry_in_standard_mode = highres_not_supported && app->capture_highres;

        handle_capture_error(app, error, "Capture failed");
        g_clear_error(&error);

        if (fatal_error) {
            stop_capture_and_close(app);
            return;
        }

        if (retry_in_standard_mode) {
            app->capture_highres = FALSE;
            if (app->capture_running) {
                start_capture_loop(app);
            } else {
                stop_capture_and_close(app);
            }
            return;
        }

        if (app->capture_running) {
            start_capture_loop(app);
            return;
        }

        stop_capture_and_close(app);
        return;
    }

    g_clear_object(&app->current_image);
    app->current_image = g_object_ref(image);

    GdkPixbuf *pixbuf = convert_fpimage_to_pixbuf(image);
    if (!pixbuf) {
        g_object_unref(image);
        set_status(app, "Unable to convert fingerprint image to display format.");
        stop_capture_and_close(app);
        return;
    }

    render_preview_from_image(app, image);
    update_device_info(app);

    gint pixbuf_width = gdk_pixbuf_get_width(pixbuf);
    gint pixbuf_height = gdk_pixbuf_get_height(pixbuf);
    set_status(app, "Captured %ux%u raw fingerprint image.",
               pixbuf_width,
               pixbuf_height);

    gint win_w, win_h;
    gtk_window_get_size(GTK_WINDOW(app->window), &win_w, &win_h);
    gint needed_w = pixbuf_width + 120;
    gint needed_h = pixbuf_height + 180;
    if (needed_w > win_w || needed_h > win_h) {
        gtk_window_resize(GTK_WINDOW(app->window), MAX(win_w, needed_w), MAX(win_h, needed_h));
    }

    g_object_unref(pixbuf);
    g_object_unref(image);

    if (app->capture_running) {
        start_capture_loop(app);
    } else {
        close_active_device(app);
    }
}

static void start_capture_loop(AppData *app) {
    if (!app->active_device || !app->capture_running) {
        return;
    }

    if (!app->capture_cancellable) {
        app->capture_cancellable = g_cancellable_new();
    }

    fp_device_capture(app->active_device,
                      app->capture_highres,
                      app->capture_cancellable,
                      capture_complete,
                      app);
}

static void refresh_device_list(AppData *app) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(app->device_combo));
    fp_context_enumerate(app->context);
    GPtrArray *devices = fp_context_get_devices(app->context);

    if (!devices || devices->len == 0) {
        set_status(app, "No libfprint devices found.");
        gtk_label_set_text(GTK_LABEL(app->device_info_label), "Device info: none selected.");
        return;
    }

    for (guint i = 0; i < devices->len; ++i) {
        FpDevice *device = FP_DEVICE(g_ptr_array_index(devices, i));
        const gchar *name = fp_device_get_name(device);
        const gchar *driver = fp_device_get_driver(device);
        const gchar *id = fp_device_get_device_id(device);
        gchar *label = g_strdup_printf("%s (%s) [%s]",
                                       name ? name : "Unknown",
                                       driver ? driver : "unknown",
                                       id ? id : "unknown");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->device_combo), label);
        g_free(label);
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
    set_status(app, "Select a device and press Start capture.");
    update_device_info(app);
}

static void on_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppData *app = user_data;
    gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(app->device_combo));
    if (active < 0) {
        set_status(app, "Select a device first.");
        return;
    }

    fp_context_enumerate(app->context);
    GPtrArray *devices = fp_context_get_devices(app->context);
    if (!devices || active >= (gint)devices->len) {
        set_status(app, "Selected device is unavailable.");
        return;
    }

    app->active_device = g_object_ref(g_ptr_array_index(devices, active));
    if (!app->active_device) {
        set_status(app, "Failed to select the device.");
        return;
    }

    GError *error = NULL;
    if (!fp_device_open_sync(app->active_device, NULL, &error)) {
        if (error && g_error_matches(error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_BUSY)) {
            set_status(app, "Device is busy or already in use. Stop any other capture process and try again.");
        } else {
            set_status_from_error(app, "Unable to open device", error);
        }
        const gchar *device_id = fp_device_get_device_id(app->active_device);
        const gchar *detail = error && error->message ? error->message : "Device could not be opened.";
        const gchar *id_for_suggestion = NULL;
        if (device_id && device_id[0] != '\0' && g_strcmp0(device_id, "0") != 0 && g_strcmp0(device_id, "unknown") != 0) {
            id_for_suggestion = device_id;
        } else {
            id_for_suggestion = detail;
        }
        show_udev_rule_dialog(GTK_WINDOW(app->window), id_for_suggestion, detail);
        g_clear_error(&error);
        g_clear_object(&app->active_device);
        return;
    }

    app->capture_running = TRUE;
    app->capture_cancellable = g_cancellable_new();
    set_control_state(app);
    update_device_info(app);
    if (app->current_image) {
        render_preview_from_image(app, app->current_image);
    }
    set_status(app, "Starting repeated capture. Place your finger whenever ready...");
    start_capture_loop(app);
}

static void on_stop_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppData *app = user_data;
    if (!app->capture_running) {
        return;
    }

    app->capture_running = FALSE;
    if (app->capture_cancellable) {
        g_cancellable_cancel(app->capture_cancellable);
    }
    set_status(app, "Stopping capture...");
    set_control_state(app);
    update_device_info(app);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppData *app = user_data;
    refresh_device_list(app);
}

/*
 * GTK UI setup.
 */
static void build_capture_controls(AppData *app, GtkWidget *main_box) {
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(main_box), button_box, FALSE, FALSE, 0);

    app->start_button = gtk_button_new_with_label("Start capture");
    g_signal_connect(app->start_button, "clicked", G_CALLBACK(on_start_clicked), app);
    gtk_box_pack_start(GTK_BOX(button_box), app->start_button, TRUE, TRUE, 0);

    app->stop_button = gtk_button_new_with_label("Stop capture");
    gtk_widget_set_sensitive(app->stop_button, FALSE);
    g_signal_connect(app->stop_button, "clicked", G_CALLBACK(on_stop_clicked), app);
    gtk_box_pack_start(GTK_BOX(button_box), app->stop_button, TRUE, TRUE, 0);
}

static void build_device_controls(AppData *app, GtkWidget *main_box) {
    GtkWidget *control_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(main_box), control_box, FALSE, FALSE, 0);

    GtkWidget *device_label = gtk_label_new("Fingerprint reader:");
    gtk_box_pack_start(GTK_BOX(control_box), device_label, FALSE, FALSE, 0);

    app->device_combo = gtk_combo_box_text_new();
    g_signal_connect(app->device_combo, "changed", G_CALLBACK(on_device_combo_changed), app);
    gtk_box_pack_start(GTK_BOX(control_box), app->device_combo, TRUE, TRUE, 0);

    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), app);
    gtk_box_pack_start(GTK_BOX(control_box), refresh_button, FALSE, FALSE, 0);

    app->show_minutiae_check = gtk_check_button_new_with_label("Show minutiae");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->show_minutiae_check), FALSE);
    g_signal_connect(app->show_minutiae_check, "toggled", G_CALLBACK(on_show_minutiae_toggled), app);
    gtk_box_pack_start(GTK_BOX(control_box), app->show_minutiae_check, FALSE, FALSE, 0);
}

static void build_status_area(AppData *app, GtkWidget *main_box) {
    app->device_info_label = gtk_label_new("Device info: none selected.");
    gtk_label_set_xalign(GTK_LABEL(app->device_info_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(app->device_info_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(app->device_info_label), 80);
    gtk_box_pack_start(GTK_BOX(main_box), app->device_info_label, FALSE, FALSE, 0);

    app->status_label = gtk_label_new("Initializing device list...");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(main_box), app->status_label, FALSE, FALSE, 0);
}

static void build_preview_area(AppData *app, GtkWidget *main_box) {
    GtkWidget *frame = gtk_frame_new("Fingerprint preview");
    gtk_widget_set_hexpand(frame, TRUE);
    gtk_widget_set_vexpand(frame, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), frame, TRUE, TRUE, 0);

    app->preview_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->preview_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(app->preview_scroll, TRUE);
    gtk_widget_set_vexpand(app->preview_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(frame), app->preview_scroll);

    app->preview_image = gtk_image_new();
    gtk_widget_set_halign(app->preview_image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->preview_image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(app->preview_scroll), app->preview_image);
}

static void build_main_window(AppData *app) {
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "libfprint Raw Image Capture");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 700, 540);
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 12);
    gtk_container_add(GTK_CONTAINER(app->window), main_box);

    build_device_controls(app, main_box);
    build_capture_controls(app, main_box);
    build_status_area(app, main_box);
    build_preview_area(app, main_box);

    GdkGeometry hints = {0};
    hints.min_width = 640;
    hints.min_height = 520;
    gtk_window_set_geometry_hints(GTK_WINDOW(app->window), app->window, &hints, GDK_HINT_MIN_SIZE);

    g_signal_connect(app->window, "destroy", G_CALLBACK(on_stop_clicked), app);
    g_signal_connect_swapped(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
}

static int initialize_app(AppData *app) {
    app->capture_running = FALSE;
    app->capture_highres = TRUE;
    app->context = fp_context_new();
    if (!app->context) {
        g_printerr("Failed to initialize libfprint context.\n");
        return 1;
    }

    build_main_window(app);
    return 0;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    AppData app = {0};
    if (initialize_app(&app) != 0) {
        return 1;
    }

    refresh_device_list(&app);
    app.status_poll_id = g_timeout_add_seconds(1, poll_device_info, &app);
    gtk_widget_show_all(app.window);
    gtk_main();

    if (app.capture_running && app.capture_cancellable) {
        g_cancellable_cancel(app.capture_cancellable);
    }
    close_active_device(&app);
    g_clear_object(&app.context);
    return 0;
}
