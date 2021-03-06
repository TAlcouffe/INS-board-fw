#include <ch.h>
#include <hal.h>
#include <chprintf.h>
#include <shell.h>
#include <stdlib.h>

#include "sensors/onboardsensors.h"
#include "sensors/ms5611.h"
#include "serial-datagram/serial_datagram.h"
#include "parameter_print.h"
#include "git_revision.h"
#include "error.h"
#include "main.h"

#define SHELL_WA_SIZE   THD_WORKING_AREA_SIZE(2048)
static void cmd_mem(BaseSequentialStream *chp, int argc, char *argv[]) {
    size_t n, size;

    (void)argv;
    if (argc > 0) {
        chprintf(chp, "Usage: mem\r\n");
        return;
    }
    n = chHeapStatus(NULL, &size);
    chprintf(chp, "core free memory : %u bytes\r\n", chCoreGetStatusX());
    chprintf(chp, "heap fragments   : %u\r\n", n);
    chprintf(chp, "heap free total  : %u bytes\r\n", size);
}

static void cmd_threads(BaseSequentialStream *chp, int argc, char *argv[]) {
    static const char *states[] = {CH_STATE_NAMES};
    thread_t *tp;

    (void)argv;
    if (argc > 0) {
        chprintf(chp, "Usage: threads\r\n");
        return;
    }
    chprintf(chp, "    addr    stack prio refs     state name\r\n");
    tp = chRegFirstThread();
    do {
        chprintf(chp, "%08lx %08lx %4lu %4lu %9s %s\r\n",
                 (uint32_t)tp, (uint32_t)tp->p_ctx.r13,
                 (uint32_t)tp->p_prio, (uint32_t)(tp->p_refs - 1),
                 states[tp->p_state], tp->p_name);
        tp = chRegNextThread(tp);
    } while (tp != NULL);
}

static void cmd_version(BaseSequentialStream *chp, int argc, char *argv[]) {
    (void)chp;
    (void)argc;
    (void)argv;
    chprintf(chp, "git version: %s\n", build_git_version);
    chprintf(chp, "branch:      %s\n", build_git_branch);
    chprintf(chp, "full sha:    %s\n", build_git_sha);
    chprintf(chp, "build date:  %s\n", build_date);
    chprintf(chp, "compiler:    %s\n", PORT_COMPILER_NAME);
}

static void cmd_safemode(BaseSequentialStream *chp, int argc, char *argv[]) {
    (void)chp;
    (void)argc;
    (void)argv;
    reboot_in_safemode();
}

static void cmd_reboot(BaseSequentialStream *chp, int argc, char *argv[]) {
    (void)chp;
    (void)argc;
    (void)argv;
    NVIC_SystemReset();
}

static void cmd_bootloader(BaseSequentialStream *chp, int argc, char *argv[]) {
    (void)chp;
    (void)argc;
    (void)argv;
    reboot_st_bootloader();
}

static void cmd_panic(BaseSequentialStream *chp, int argc, char *argv[]) {
    (void)chp;
    (void)argc;
    (void)argv;
    chSysHalt("panic test shell command");
}

static void cmd_panic_get(BaseSequentialStream *chp, int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    chprintf(chp, "panic was: %s\n", get_panic_message());
}

static void cmd_parameter_list(BaseSequentialStream *stream, int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    parameter_print(stream, &parameters);
}

static void cmd_parameter_set(BaseSequentialStream *stream, int argc, char *argv[]) {
    if (argc < 2) {
        chprintf(stream, "usage: parameter_set name value\n");
        return;
    }
    parameter_t *p = parameter_find(&parameters, argv[0]);
    if (p == NULL) {
        chprintf(stream, "parameter doesn't exist\n");
        return;
    }
    if (p->type == _PARAM_TYPE_SCALAR) {
        parameter_scalar_set(p, strtof(argv[1], NULL));
    } else {
        chprintf(stream, "unsupported type %d\n", p->type);
    }
}

static void cmd_gyro(BaseSequentialStream *chp, int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    int i;
    for (i = 0; i < 100; i++) {
        chSysLock();
        int gx = 1000*mpu_gyro_sample.rate[0];
        int gy = 1000*mpu_gyro_sample.rate[1];
        int gz = 1000*mpu_gyro_sample.rate[2];
        chSysUnlock();
        chprintf(chp, "gyro %d %d %d\n", gx, gy, gz);
        chThdSleepMilliseconds(10);
    }
}

static const I2CConfig i2c_cfg = {
    .op_mode = OPMODE_I2C,
    .clock_speed = 400000,
    .duty_cycle = FAST_DUTY_CYCLE_2
};

static void cmd_barometer(BaseSequentialStream *chp, int argc, char *argv[])
{
    (void) argc;
    (void) argv;
    ms5611_t barometer;

    I2CDriver *driver = &I2CD1;

    i2cStart(driver, &i2c_cfg);
    i2cAcquireBus(driver);

    chprintf(chp, "ms5611 init\r\n");

    int init = ms5611_i2c_init(&barometer, driver, 0);

    if (init != 0) {
        i2cflags_t flags = i2cGetErrors(driver);
        chprintf(chp, "ms5611 init failed: %d, %u\r\n", init, (uint32_t)flags);
        i2cReleaseBus(driver);
        i2cStop(driver);
        return;
    } else {
        chprintf(chp, "ms5611 init succeeded\r\n");
    }

    chThdSleepMilliseconds(100);

    int i = 50;
    while (i-- > 0) {
        uint32_t raw_t, raw_p, press;
        int32_t temp;
        int16_t t;

        t = ms5611_adc_start(&barometer, MS5611_ADC_TEMP, MS5611_OSR_4096);
        if (t < 0) {
            continue;
        }

        chThdSleepMilliseconds((t - 1)/1000 + 1);

        ms5611_adc_read(&barometer, &raw_t);

        t = ms5611_adc_start(&barometer, MS5611_ADC_PRESS, MS5611_OSR_4096);
        if (t < 0) {
            continue;
        }

        chThdSleepMilliseconds((t - 1)/1000 + 1);

        ms5611_adc_read(&barometer, &raw_p);

        press = ms5611_calc_press(&barometer, raw_p, raw_t, &temp);

        chprintf(chp, "pressure: %u, temperature: %u\r\n", press, temp);

        chThdSleepMilliseconds(100);
    }

    i2cReleaseBus(driver);
    i2cStop(driver);
}

const ShellCommand shell_commands[] = {
  {"mem", cmd_mem},
  {"threads", cmd_threads},
  {"version", cmd_version},
  {"safemode", cmd_safemode},
  {"reboot", cmd_reboot},
  {"bootloader", cmd_bootloader},
  {"panic", cmd_panic},
  {"panic_get", cmd_panic_get},
  {"parameter_list", cmd_parameter_list},
  {"parameter_set", cmd_parameter_set},
  {"gyro", cmd_gyro},
  {"baro", cmd_barometer},
  {NULL, NULL}
};

