/*
 * canary-mqtt-publisher
 *
 * Reads the four sensor attributes exposed by the kernel drivers through
 * sysfs/IIO/hwmon and publishes their values to an MQTT broker as JSON.
 * Intended to run as a systemd service on the Pi alongside the broker.
 */

#include <dirent.h>
#include <errno.h>
#include <math.h>
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
 * Self-heating compensation constants. See CLAUDE.md "Self-heating compensation"
 * for derivation. FACTORs are calibrated for the open HAT with no enclosure.
 * Recalibrate if airflow regime changes.
 */
#define FACTOR_TMP119  3.31
#define FACTOR_SHT45   2.90

/*
 * EWMA smoothing for the SoC temperature input. Raw thermal_zone0/temp jumps
 * several C between samples; a single-pole filter at alpha=0.05 with 2s
 * sampling gives a ~39s time constant, killing the spike noise without
 * lagging real ramps.
 */
#define CPU_TEMP_PATH  "/sys/class/thermal/thermal_zone0/temp"
#define CPU_EWMA_ALPHA 0.05

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

static int read_sensor_value(const struct sensor *s, double *out) {
    long raw;
    if (read_long(s->path, &raw) != 0) {
        fprintf(stderr, "read %s failed: %s\n", s->path, strerror(errno));
        return -1;
    }

    double dynamic_scale = 1.0;
    if (s->scale_path[0] != '\0') {
        if (read_double(s->scale_path, &dynamic_scale) != 0) {
            fprintf(stderr, "read %s failed: %s\n", s->scale_path, strerror(errno));
            return -1;
        }
    }

    *out = (double)raw * dynamic_scale * s->scale;
    return 0;
}

static void publish_value(struct mosquitto *mosq, const char *topic,
                          const char *unit, double value, time_t ts) {
    char payload[160];
    int len = snprintf(payload, sizeof(payload),
                       "{\"ts\":%ld,\"value\":%.3f,\"unit\":\"%s\"}",
                       (long)ts, value, unit);

    int rc = mosquitto_publish(mosq, NULL, topic, len, payload, 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "publish %s: %s\n", topic, mosquitto_strerror(rc));
    }
}

/* SoC temperature in C; thermal_zone0/temp is reported in milli-C. */
static int read_cpu_temp(double *out) {
    long raw;
    if (read_long(CPU_TEMP_PATH, &raw) != 0) return -1;
    *out = (double)raw * 0.001;
    return 0;
}

/* Magnus saturation vapor pressure (hPa). RH ratio under temperature change
 * is e_s(T_sensor) / e_s(T_ambient); the 6.112 prefactor cancels but keeping
 * it makes the function reusable. */
static double magnus_es(double t_c) {
    return 6.112 * exp(17.62 * t_c / (243.12 + t_c));
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

    double cpu_filt = NAN;

    while (keep_running) {
        time_t now = time(NULL);

        double raw_tmp119 = NAN, raw_sht45_t = NAN, raw_sht45_rh = NAN;

        for (size_t i = 0; i < sensor_count; i++) {
            if (sensors[i].path[0] == '\0') continue;
            double v;
            if (read_sensor_value(&sensors[i], &v) != 0) continue;
            publish_value(mosq, sensors[i].topic, sensors[i].unit, v, now);

            if      (strcmp(sensors[i].topic, "canary/temp")       == 0) raw_tmp119   = v;
            else if (strcmp(sensors[i].topic, "canary/temp_sht45") == 0) raw_sht45_t  = v;
            else if (strcmp(sensors[i].topic, "canary/humidity")   == 0) raw_sht45_rh = v;
        }

        double cpu_raw;
        if (read_cpu_temp(&cpu_raw) == 0) {
            cpu_filt = isnan(cpu_filt)
                       ? cpu_raw
                       : CPU_EWMA_ALPHA * cpu_raw + (1.0 - CPU_EWMA_ALPHA) * cpu_filt;
        }

        if (!isnan(raw_tmp119) && !isnan(cpu_filt)) {
            double t = raw_tmp119 - (cpu_filt - raw_tmp119) / FACTOR_TMP119;
            publish_value(mosq, "canary/temp_corrected", "C", t, now);
        }

        double sht45_t_corr = NAN;
        if (!isnan(raw_sht45_t) && !isnan(cpu_filt)) {
            sht45_t_corr = raw_sht45_t - (cpu_filt - raw_sht45_t) / FACTOR_SHT45;
            publish_value(mosq, "canary/temp_sht45_corrected", "C", sht45_t_corr, now);
        }

        if (!isnan(raw_sht45_rh) && !isnan(sht45_t_corr)) {
            double rh = raw_sht45_rh * magnus_es(raw_sht45_t) / magnus_es(sht45_t_corr);
            publish_value(mosq, "canary/humidity_corrected", "%RH", rh, now);
        }

        sleep(PUBLISH_INTERVAL_SEC);
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
