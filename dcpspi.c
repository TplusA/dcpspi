/*
 * Copyright (C) 2015, 2016  T+A elektroakustik GmbH & Co. KG
 *
 * This file is part of DCPSPI.
 *
 * DCPSPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 as
 * published by the Free Software Foundation.
 *
 * DCPSPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DCPSPI.  If not, see <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>

#include "dcpspi_process.h"
#include "dcpdefs.h"
#include "spi.h"
#include "named_pipe.h"
#include "gpio.h"
#include "versioninfo.h"
#include "messages.h"

ssize_t (*os_read)(int fd, void *dest, size_t count) = read;
ssize_t (*os_write)(int fd, const void *buf, size_t count) = write;
int (*os_poll)(struct pollfd *fds, nfds_t nfds, int timeout) = poll;

static void show_version_info(void)
{
    printf("%s\n"
           "Revision %s%s\n"
           "         %s+%d, %s\n",
           PACKAGE_STRING,
           VCS_FULL_HASH, VCS_WC_MODIFIED ? " (tainted)" : "",
           VCS_TAG, VCS_TICK, VCS_DATE);
}

static void log_version_info(void)
{
    msg_info("Rev %s%s, %s+%d, %s",
             VCS_FULL_HASH, VCS_WC_MODIFIED ? " (tainted)" : "",
             VCS_TAG, VCS_TICK, VCS_DATE);
}

/*!
 * Global flag that gets cleared in the SIGTERM signal handler.
 *
 * For clean shutdown.
 */
static volatile bool keep_running = true;

/*!
 * Copy data back and forth.
 *
 * As long as no transaction is in progress, we are waiting on activities on
 * the named pipe and the request pin. A transaction is started if either the
 * request pin is activated or if some process is sending data to the named
 * pipe.
 *
 * Once a transaction has been started, data needs to be copied and
 * transformed. There are two cases:
 * - Transaction initiated by the slave ("slave transaction")
 * - Transaction initiated by the master ("master transaction")
 *
 * Slave transaction:
 * - Read four bytes long command from SPI
 * - In case of write command: Read optional data from SPI
 * - Transform for DCPD (unescape raw data)
 * - Send transformed command and data to named pipe
 * - In case of read command: Wait for answer from named pipe
 * - In case of read command: Transform for SPI (insert escape sequences)
 * - In case of read command: Send transformed data to SPI
 *
 * Master transaction (always write commands):
 * - Read four bytes long write command from DCPD
 * - Read optional data from DCPD
 * - Transform for SPI (insert escape sequences)
 * - Send transformed data to SPI
 *
 * \param fifo_in_fd
 *     File descriptor of the ingoing named pipe that DCPD is supposed to
 *     connect to for writing to us.
 *
 * \param fifo_out_fd
 *     File descriptor of the outgoing named pipe that DCPD is supposed to
 *     connect to for reading from us.
 *
 * \param spi_fd
 *     File descriptor of the SPI interface.
 *
 * \param gpio
 *     Structure that represents the request input pin the slave device is
 *     supposed to use for requesting data and data rate limitation.
 */
static void main_loop(const int fifo_in_fd, const int fifo_out_fd,
                      const int spi_fd, const struct gpio_handle *const gpio)
{
    msg_info("Accepting traffic");

    static uint8_t dcp_backing_buffer[DCPSYNC_HEADER_SIZE + DCP_HEADER_SIZE + DCP_PAYLOAD_MAXSIZE];
    static uint8_t spi_backing_buffer[(DCP_HEADER_SIZE + DCP_PAYLOAD_MAXSIZE) * 2];

    struct dcp_transaction transaction =
    {
        .dcp_buffer =
        {
            .buffer = dcp_backing_buffer,
            .size = sizeof(dcp_backing_buffer),
        },
        .spi_buffer =
        {
            .buffer = spi_backing_buffer,
            .size = sizeof(spi_backing_buffer),
        },
    };

    reset_transaction_struct(&transaction, true);

    struct slave_request_and_lock_data rldata =
    {
        .gpio = gpio,
        .gpio_fd = (gpio != NULL) ? gpio_get_poll_fd(gpio) : -1,
        .is_running_for_real = (gpio != NULL),
        .previous_gpio_state = (gpio != NULL) ? gpio_is_active(gpio) : false,
    };

    while(keep_running &&
          dcpspi_process(fifo_in_fd, fifo_out_fd, spi_fd,
                         &transaction, &rldata))
        ;
}

struct parameters
{
    const char *fifo_in_name;
    const char *fifo_out_name;
    const char *spidev_name;
    uint32_t spi_clock;
    unsigned int gpio_num;
    bool run_in_foreground;
    bool gpio_needs_debouncing;
    bool dummy_mode;
};

/*!
 * Open devices, daemonize.
 */
static int setup(const struct parameters *parameters,
                 int *fifo_in_fd, int *fifo_out_fd,
                 int *spi_fd, struct gpio_handle **gpio)
{
    msg_enable_syslog(!parameters->run_in_foreground);

    if(!parameters->run_in_foreground)
        openlog("dcpspi", LOG_PID, LOG_DAEMON);

    if(!parameters->run_in_foreground)
    {
        if(daemon(0, 0) < 0)
        {
            msg_error(errno, LOG_EMERG, "Failed to run as daemon");
            return -1;
        }
    }

    log_version_info();

    *fifo_in_fd = fifo_create_and_open(parameters->fifo_in_name, false);
    if(*fifo_in_fd < 0)
        return -1;

    *fifo_out_fd = fifo_create_and_open(parameters->fifo_out_name, true);
    if(*fifo_out_fd < 0)
        goto error_fifo_out;

    if(parameters->dummy_mode)
    {
        *spi_fd = -1;
        *gpio = NULL;
        return 0;
    }

    *spi_fd = spi_open_device(parameters->spidev_name);
    if(*spi_fd < 0)
        goto error_spi_open;

    *gpio = gpio_open(parameters->gpio_num, false);
    if(*gpio == NULL)
        goto error_gpio_open;

    if(parameters->gpio_needs_debouncing)
        gpio_enable_debouncing(*gpio);

    spi_set_speed_hz(parameters->spi_clock);

    return 0;

error_gpio_open:
    spi_close_device(*spi_fd);

error_spi_open:
    fifo_close_and_delete(fifo_out_fd, parameters->fifo_out_name);

error_fifo_out:
    fifo_close_and_delete(fifo_in_fd, parameters->fifo_in_name);
    return -1;
}

static void usage(const char *program_name)
{
    printf("Usage: %s --fifo name --spidev name --irq gpio\n"
           "\n"
           "Options:\n"
           "  --help         Show this help.\n"
           "  --version      Print version information to stdout.\n"
           "  --fg           Run in foreground, don't run as daemon.\n"
           "  --ififo name   Name of the named pipe the DCP daemon writes to.\n"
           "  --ofifo name   Name of the named pipe the DCP daemon reads from.\n"
           "  --spidev name  Name of the SPI device.\n"
           "  --spiclk hz    Clock frequency on SPI bus.\n"
           "  --gpio num     Number of the slave request pin.\n"
           "  --debounce     Enable software debouncing of request pin.\n",
           program_name);
}

static int process_command_line(int argc, char *argv[],
                                struct parameters *parameters)
{
    parameters->fifo_in_name = "/tmp/dcp_to_spi";
    parameters->fifo_out_name = "/tmp/spi_to_dcp";
    parameters->spidev_name = "/dev/spidev0.0";
    parameters->spi_clock = 0;
    parameters->gpio_num = 4;
    parameters->run_in_foreground = false;
    parameters->gpio_needs_debouncing = false;
    parameters->dummy_mode = false;

#define CHECK_ARGUMENT() \
    do \
    { \
        if(i + 1 >= argc) \
        { \
            fprintf(stderr, "Option %s requires an argument.\n", argv[i]); \
            return -1; \
        } \
        ++i; \
    } \
    while(0)

    for(int i = 1; i < argc; ++i)
    {
        if(strcmp(argv[i], "--help") == 0)
            return 1;
        else if(strcmp(argv[i], "--version") == 0)
            return 2;
        else if(strcmp(argv[i], "--fg") == 0)
            parameters->run_in_foreground = true;
        else if(strcmp(argv[i], "--ififo") == 0)
        {
            CHECK_ARGUMENT();
            parameters->fifo_in_name = argv[i];
        }
        else if(strcmp(argv[i], "--ofifo") == 0)
        {
            CHECK_ARGUMENT();
            parameters->fifo_out_name = argv[i];
        }
        else if(strcmp(argv[i], "--spidev") == 0)
        {
            CHECK_ARGUMENT();
            parameters->spidev_name = argv[i];
        }
        else if(strcmp(argv[i], "--spiclk") == 0)
        {
            CHECK_ARGUMENT();

            char *endptr;
            unsigned long temp = strtoul(argv[i], &endptr, 10);

            if(*endptr != '\0' || temp > UINT32_MAX || (temp == ULONG_MAX && errno == ERANGE))
            {
                fprintf(stderr, "Invalid value \"%s\". Please try --help.\n", argv[i]);
                return -1;
            }

            parameters->spi_clock = temp;
        }
        else if(strcmp(argv[i], "--gpio") == 0)
        {
            CHECK_ARGUMENT();

            char *endptr;
            unsigned long temp = strtoul(argv[i], &endptr, 10);

            if(*endptr != '\0' || temp > UINT_MAX || (temp == ULONG_MAX && errno == ERANGE))
            {
                fprintf(stderr, "Invalid value \"%s\". Please try --help.\n", argv[i]);
                return -1;
            }

            parameters->gpio_num = temp;
        }
        else if(strcmp(argv[i], "--debounce") == 0)
            parameters->gpio_needs_debouncing = true;
        else
        {
            fprintf(stderr, "Unknown option \"%s\". Please try --help.\n", argv[i]);
            return -1;
        }
    }

#undef CHECK_ARGUMENT

    if(parameters->spidev_name[0] == '-' && parameters->spidev_name[1] == '\0')
        parameters->dummy_mode = true;

    return 0;
}

static void signal_handler(int signum, siginfo_t *info, void *ucontext)
{
    keep_running = false;
}

int main(int argc, char *argv[])
{
    static struct parameters parameters;

    int ret = process_command_line(argc, argv, &parameters);

    if(ret == -1)
        return EXIT_FAILURE;
    else if(ret == 1)
    {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }
    else if(ret == 2)
    {
        show_version_info();
        return EXIT_SUCCESS;
    }

    int fifo_in_fd, fifo_out_fd, spi_fd;
    struct gpio_handle *gpio;

    if(setup(&parameters, &fifo_in_fd, &fifo_out_fd, &spi_fd, &gpio) < 0)
        return EXIT_FAILURE;

    static struct sigaction action =
    {
        .sa_sigaction = signal_handler,
        .sa_flags = SA_SIGINFO | SA_RESETHAND,
    };

    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    main_loop(fifo_in_fd, fifo_out_fd, spi_fd, gpio);

    msg_info("Terminated, shutting down");

    if(!parameters.dummy_mode)
        spi_close_device(spi_fd);

    fifo_close_and_delete(&fifo_in_fd, parameters.fifo_in_name);
    fifo_close_and_delete(&fifo_out_fd, parameters.fifo_out_name);

    if(!parameters.dummy_mode)
        gpio_close(gpio);

    return EXIT_SUCCESS;
}
