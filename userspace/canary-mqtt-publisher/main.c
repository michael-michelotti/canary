/*
 * canary-mqtt-publisher
 *
 * Reads the four sensor attributes exposed by the kernel drivers through
 * sysfs/IIO/hwmon and publishes their values to an MQTT broker as JSON.
 * Intended to run as a systemd service on the Pi alongside the broker.
 */

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mosquitto.h>

#define BROKER_HOST          "localhost"
#define BROKER_PORT          1883
#define PUBLISH_INTERVAL_SEC 2
#define KEEPALIVE_SEC        30

/*
 * Each entry says: which kernel subsystem to look in, what the driver's
 * `name` attribute reads as, which file under that device to read, where
 * to publish, what unit to stamp into the JSON, and a scale factor applied
 * to the raw integer (hwmon reports milli-units, so scale = 0.001).
 */
struct sensor {
    const char *subsystem;    /* "hwmon" or "iio" */
    const char *device_name;  /* contents of the `name` sysfs file */
    const char *attr;         /* sysfs file to read, e.g. "temp1_input" */
    const char *scale_attr;   /* optional IIO "_scale" sibling; NULL if unused */
    const char *topic;        /* MQTT topic */
    const char *unit;         /* unit string for JSON payload */
    double      scale;        /* static multiplier applied after dynamic scale */
    char        path[512];       /* resolved full path to `attr`, filled at startup */
    char        scale_path[512]; /* resolved full path to `scale_attr`, if any */
};

static struct sensor sensors[] = {
    /* tmp119: IIO temp channels report raw*scale in milli-C, so scale=0.001 gets us to C */
    { "iio",   "tmp119",    "in_temp_raw",                "in_temp_scale", "canary/temp",       "C",     0.001, "", "" },
    { "hwmon", "sht4x",     "temp1_input",                NULL,            "canary/temp_sht45", "C",     0.001, "", "" },
    { "hwmon", "sht4x",     "humidity1_input",            NULL,            "canary/humidity",   "%RH",   0.001, "", "" },
    { "iio",   "bmp380",    "in_pressure_input",          NULL,            "canary/pressure",   "kPa",   1.0,   "", "" },
    { "iio",   "sgp40_voc", "in_concentration_voc_input", NULL,            "canary/voc",        "index", 1.0,   "", "" },
};

static const size_t sensor_count = sizeof(sensors) / sizeof(sensors[0]);

static volatile sig_atomic_t keep_running = 1;

static void on_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

/*
 * Walk /sys/class/hwmon or /sys/bus/iio/devices, read each child's `name`
 * file, and record the full path to the requested attribute when we find
 * a match. Sysfs device numbering (hwmon0, iio:device2, ...) is not stable
 * across reboots, so we always resolve by name rather than hardcoding.
 */
static int resolve_sensor_path(struct sensor *s) {
    const char *base = (strcmp(s->subsystem, "hwmon") == 0)
                           ? "/sys/class/hwmon"
                           : "/sys/bus/iio/devices";

    DIR *d = opendir(base);
    if (!d) {
        fprintf(stderr, "opendir %s: %s\n", base, strerror(errno));
        return -1;
    }

    int found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char name_path[512];
        snprintf(name_path, sizeof(name_path), "%s/%s/name", base, e->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f) continue;

        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = '\0';
            if (strcmp(buf, s->device_name) == 0) {
                snprintf(s->path, sizeof(s->path), "%s/%s/%s",
                         base, e->d_name, s->attr);
                if (s->scale_attr) {
                    snprintf(s->scale_path, sizeof(s->scale_path), "%s/%s/%s",
                             base, e->d_name, s->scale_attr);
                }
                found = 0;
            }
        }
        fclose(f);
        if (found == 0) break;
    }
    closedir(d);

    if (found != 0) {
        fprintf(stderr, "could not resolve %s:%s\n",
                s->subsystem, s->device_name);
    }
    return found;
}

static int read_long(const char *path, long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = fscanf(f, "%ld", out);
    fclose(f);
    return (n == 1) ? 0 : -1;
}

static int read_double(const char *path, double *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = fscanf(f, "%lf", out);
    fclose(f);
    return (n == 1) ? 0 : -1;
}

static void publish_sensor(struct mosquitto *mosq, struct sensor *s, time_t ts) {
    long raw;
    if (read_long(s->path, &raw) != 0) {
        fprintf(stderr, "read %s failed: %s\n", s->path, strerror(errno));
        return;
    }

    double dynamic_scale = 1.0;
    if (s->scale_path[0] != '\0') {
        if (read_double(s->scale_path, &dynamic_scale) != 0) {
            fprintf(stderr, "read %s failed: %s\n", s->scale_path, strerror(errno));
            return;
        }
    }

    double value = (double)raw * dynamic_scale * s->scale;

    char payload[160];
    int len = snprintf(payload, sizeof(payload),
                       "{\"ts\":%ld,\"value\":%.3f,\"unit\":\"%s\"}",
                       (long)ts, value, s->unit);

    int rc = mosquitto_publish(mosq, NULL, s->topic, len, payload, 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "publish %s: %s\n", s->topic, mosquitto_strerror(rc));
    }
}

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    mosquitto_lib_init();

    struct mosquitto *mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mosquitto_new failed\n");
        return 1;
    }

    int rc = mosquitto_connect(mosq, BROKER_HOST, BROKER_PORT, KEEPALIVE_SEC);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "connect: %s\n", mosquitto_strerror(rc));
        return 1;
    }

    /* Background thread handles the protocol loop and auto-reconnect. */
    mosquitto_loop_start(mosq);

    for (size_t i = 0; i < sensor_count; i++) {
        if (resolve_sensor_path(&sensors[i]) == 0) {
            fprintf(stderr, "resolved %s -> %s\n", sensors[i].topic, sensors[i].path);
        }
    }

    while (keep_running) {
        time_t now = time(NULL);
        for (size_t i = 0; i < sensor_count; i++) {
            if (sensors[i].path[0] != '\0') {
                publish_sensor(mosq, &sensors[i], now);
            }
        }
        sleep(PUBLISH_INTERVAL_SEC);
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
