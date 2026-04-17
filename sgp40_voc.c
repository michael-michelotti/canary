// SPDX-License-Identifier: GPL-2.0+
/*
 * sgp40_voc.c - Sensirion SGP40 gas sensor with VOC Index Algorithm
 *
 * Copyright (C) 2026 Michael Michelotti <michael.michelotti4@gmail.com>
 *
 * I2C slave address: 0x59
 *
 * Datasheet:
 *   https://www.sensirion.com/file/datasheet_sgp40
 *
 * This driver is an alternative to the upstream sgp40 driver
 * (drivers/iio/chemical/sgp40.c by Andreas Klinger). The I2C wire
 * protocol layer (measure command, RH/temperature compensation tick
 * encoding, CRC-8 framing) is adapted from that driver.
 *
 * The difference is in how the VOC index is computed. The upstream
 * driver maps raw sensor resistance to a 0..500 index via a fixed
 * sigmoid around a user-settable calibration bias. This driver
 * implements Sensirion's VOC Index Algorithm
 * (github.com/Sensirion/gas-index-algorithm) ported to kernel-space
 * fixed-point integer math, with adaptive baseline tracking.
 *
 * Channels exposed via IIO:
 *   in_resistance_raw                 - raw sensor resistance (ticks)
 *   in_concentration_voc_processed    - VOC index, 0..500
 *   out_temp_raw                      - compensation: temperature (m°C)
 *   out_humidityrelative_raw          - compensation: RH (m%)
 */

#include <linux/delay.h>
#include <linux/crc8.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>

#define SGP40_CRC8_POLYNOMIAL 0x31
#define SGP40_CRC8_INIT 0xff

DECLARE_CRC8_TABLE(sgp40_crc8_table);

struct sgp40_voc_data
{
    struct device *dev;
    struct i2c_client *client;
    int rht;  /* compensation: RH in millipercent (0..100000) */
    int temp; /* compensation: temperature in millidegrees C */
    /* protects rht, temp, and algorithm state */
    struct mutex lock;
    /* TODO step 10: struct sgp40_voc_algo_state algo; */
};

struct sgp40_voc_tg_measure
{
    u8 command[2];
    __be16 rht_ticks;
    u8 rht_crc;
    __be16 temp_ticks;
    u8 temp_crc;
} __packed;

struct sgp40_voc_tg_result
{
    __be16 res_ticks;
    u8 res_crc;
} __packed;

static const struct iio_chan_spec sgp40_voc_channels[] = {
    {
        .type = IIO_CONCENTRATION,
        .channel2 = IIO_MOD_VOC,
        .modified = 1,
        .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
    },
    {
        .type = IIO_RESISTANCE,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
    },
    {
        .type = IIO_TEMP,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .output = 1,
    },
    {
        .type = IIO_HUMIDITYRELATIVE,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
        .output = 1,
    },
};

static int sgp40_voc_measure_resistance_raw(struct sgp40_voc_data *data,
                                            u16 *resistance_raw)
{
    struct i2c_client *client = data->client;
    struct sgp40_voc_tg_measure tg = {.command = {0x26, 0x0F}};
    struct sgp40_voc_tg_result tgres;
    u32 ticks;
    u16 ticks16;
    u8 crc;
    int ret;

    mutex_lock(&data->lock);

    /* RH: 0..100000 millipercent -> 0..65535 ticks */
    ticks = (data->rht / 10) * 65535 / 10000;
    ticks16 = (u16)clamp(ticks, 0u, 65535u);
    tg.rht_ticks = cpu_to_be16(ticks16);
    tg.rht_crc = crc8(sgp40_crc8_table, (u8 *)&tg.rht_ticks, 2,
                      SGP40_CRC8_INIT);

    /* Temp: -45000..+130000 millidegrees C -> 0..65535 ticks */
    ticks = ((data->temp + 45000) / 10) * 65535 / 17500;
    ticks16 = (u16)clamp(ticks, 0u, 65535u);
    tg.temp_ticks = cpu_to_be16(ticks16);
    tg.temp_crc = crc8(sgp40_crc8_table, (u8 *)&tg.temp_ticks, 2,
                       SGP40_CRC8_INIT);

    mutex_unlock(&data->lock);

    ret = i2c_master_send(client, (const char *)&tg, sizeof(tg));
    if (ret != sizeof(tg))
    {
        dev_warn(data->dev, "i2c_master_send ret: %d sizeof: %zu\n",
                 ret, sizeof(tg));
        return -EIO;
    }

    msleep(30);

    ret = i2c_master_recv(client, (u8 *)&tgres, sizeof(tgres));
    if (ret < 0)
        return ret;
    if (ret != sizeof(tgres))
    {
        dev_warn(data->dev, "i2c_master_recv ret: %d sizeof: %zu\n",
                 ret, sizeof(tgres));
        return -EIO;
    }

    crc = crc8(sgp40_crc8_table, (u8 *)&tgres.res_ticks, 2, SGP40_CRC8_INIT);
    if (crc != tgres.res_crc)
    {
        dev_err(data->dev, "CRC error while measure-raw\n");
        return -EIO;
    }

    *resistance_raw = be16_to_cpu(tgres.res_ticks);

    return 0;
}

static int sgp40_voc_read_raw(struct iio_dev *indio_dev,
                              struct iio_chan_spec const *chan,
                              int *val, int *val2, long mask)
{
    struct sgp40_voc_data *data = iio_priv(indio_dev);
    u16 resistance_raw;
    int ret;

    switch (mask)
    {
    case IIO_CHAN_INFO_RAW:
        switch (chan->type)
        {
        case IIO_RESISTANCE:
            ret = sgp40_voc_measure_resistance_raw(data, &resistance_raw);
            if (ret)
                return ret;
            *val = resistance_raw;
            return IIO_VAL_INT;
        case IIO_TEMP:
            mutex_lock(&data->lock);
            *val = data->temp;
            mutex_unlock(&data->lock);
            return IIO_VAL_INT;
        case IIO_HUMIDITYRELATIVE:
            mutex_lock(&data->lock);
            *val = data->rht;
            mutex_unlock(&data->lock);
            return IIO_VAL_INT;
        default:
            return -EINVAL;
        }
    case IIO_CHAN_INFO_PROCESSED:
        /* TODO step 10: run Sensirion VOC Index Algorithm */
        return -EINVAL;
    default:
        return -EINVAL;
    }
}

static int sgp40_voc_write_raw(struct iio_dev *indio_dev,
                               struct iio_chan_spec const *chan,
                               int val, int val2, long mask)
{
    struct sgp40_voc_data *data = iio_priv(indio_dev);

    switch (mask)
    {
    case IIO_CHAN_INFO_RAW:
        switch (chan->type)
        {
        case IIO_TEMP:
            /* SGP40 datasheet: compensation temperature range -45..+130 °C,
             * expressed here in millidegrees C */
            if (val < -45000 || val > 130000)
                return -EINVAL;
            mutex_lock(&data->lock);
            data->temp = val;
            mutex_unlock(&data->lock);
            return 0;
        case IIO_HUMIDITYRELATIVE:
            /* SGP40 datasheet: compensation RH range 0..100 %,
             * expressed here in millipercent */
            if (val < 0 || val > 100000)
                return -EINVAL;
            mutex_lock(&data->lock);
            data->rht = val;
            mutex_unlock(&data->lock);
            return 0;
        default:
            return -EINVAL;
        }
    default:
        return -EINVAL;
    }
}

static const struct iio_info sgp40_voc_info = {
    .read_raw  = sgp40_voc_read_raw,
    .write_raw = sgp40_voc_write_raw,
};

static int sgp40_voc_probe(struct i2c_client *client)
{
    const struct i2c_device_id *id = i2c_client_get_device_id(client);
    struct device *dev = &client->dev;
    struct iio_dev *indio_dev;
    struct sgp40_voc_data *data;
    int ret;

    indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
    if (!indio_dev)
        return -ENOMEM;

    data = iio_priv(indio_dev);
    data->client = client;
    data->dev = dev;

    crc8_populate_msb(sgp40_crc8_table, SGP40_CRC8_POLYNOMIAL);
    mutex_init(&data->lock);

    /* sensible defaults until userspace feeds real compensation values */
    data->rht  = 50000;     /* 50 % RH */
    data->temp = 25000;     /* 25 °C */

    /* TODO step 10: sgp40_voc_algorithm_init(data); */

    indio_dev->info         = &sgp40_voc_info;
    indio_dev->name         = id ? id->name : "sgp40_voc";
    indio_dev->modes        = INDIO_DIRECT_MODE;
    indio_dev->channels     = sgp40_voc_channels;
    indio_dev->num_channels = ARRAY_SIZE(sgp40_voc_channels);

    ret = devm_iio_device_register(dev, indio_dev);
    if (ret)
        dev_err(dev, "failed to register iio device\n");

    return ret;
}

static const struct i2c_device_id sgp40_voc_id[] = {
    { "sgp40_voc" },
    { }
};
MODULE_DEVICE_TABLE(i2c, sgp40_voc_id);

static const struct of_device_id sgp40_voc_dt_ids[] = {
    { .compatible = "sensirion,sgp40" },
    { }
};
MODULE_DEVICE_TABLE(of, sgp40_voc_dt_ids);

static struct i2c_driver sgp40_voc_driver = {
    .driver = {
        .name = "sgp40_voc",
        .of_match_table = sgp40_voc_dt_ids,
    },
    .probe    = sgp40_voc_probe,
    .id_table = sgp40_voc_id,
};
module_i2c_driver(sgp40_voc_driver);

MODULE_AUTHOR("Michael Michelotti <michael.michelotti4@gmail.com>");
MODULE_DESCRIPTION("Sensirion SGP40 gas sensor with VOC Index Algorithm");
MODULE_LICENSE("GPL v2");
