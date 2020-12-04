/*
iq-squelch - Suppress IQ samples below a certain threshold
Copyright (C) 2016 Shaun R. Hey <shaun@shaunhey.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <czmq.h>
#include <signal.h>
#include <gsl/gsl_matrix.h>

#define DEFAULT_AUTO_MODE           false
#define DEFAULT_BLOCK_COUNT         0
#define DEFAULT_BLOCK_SIZE          1024
#define DEFAULT_BLOCK_THRESHOLD     50
#define DEFAULT_OFFSET              0
#define DEFAULT_OUTPUT_FILE         stdout
#define DEFAULT_PADDING_BLOCKS      true
#define DEFAULT_SAMPLE_THRESHOLD    10
#define DEFAULT_VERBOSE_MODE        false
#define DEFAULT_ZMQ                 false

#define NUM_ZMQ_STREAMS             4

static volatile bool stop_program = false;

void handle_interrupt(int nop) {
    fprintf(stderr, "Cought interrupt. Ending program gracefully!");
    stop_program = true;
}

typedef enum {
    ROW_MAJOR,
    COLUMN_MAJOR
} OUTPUT_ORDER;

struct
{
    bool       auto_mode;
    uint32_t   block_count;
    uint32_t   block_size;
    uint8_t    block_threshold;
    FILE      *input_file;
    uint64_t   offset;
    FILE      *output_file;
    char      *output_filename;
    bool       padding_blocks;
    uint8_t    sample_threshold;
    bool       verbose;
    bool       zmq_enabled;
    char      *zmq_sub_url;
    char      *zmq_pub_url;
    bool      send_nullvec;
} options;

zsock_t *sub;
zsock_t *pub;

void usage()
{
    fprintf(stderr,
            "Suppress IQ samples below a certain threshold\n"
            "Usage: iq-squelch [options] FILE\n"
            "\n"
            "  FILE            Unsigned 8-bit IQ file to process (\"-\" for stdin)\n"
            "  -a              Auto mode (threshold is above the average noise level)\n"
            "  -b BLOCK_SIZE   Number of samples to read at a time (default: 1024)\n"
            "  -c BLOCK_COUNT  Limit the total number of blocks to process\n"
            "  -m MAGNITUDE    Sample magnitude threshold (0-255, default: 10)\n"
            "  -o OUTPUT_FILE  Output file to write samples (default: stdout)\n"
            "  -p              Output the block before and after a signal\n"
            "  -s OFFSET       Starting byte offset within the input file\n"
            "  -t THRESHOLD    Percentage of a block that must be over the threshold\n"
            "                  before that block is output (default: 50%%)\n"
            "  -v              Verbose mode\n"
            "  -y ZMQ-SUB-URL  ZMQ SUB URL from which data is read \n"
            "  -z ZMQ-PUB-URL  ZMQ PUB URL to which data is sent\n"
            "  -n              If enabled, suppressed signals will be replaced by 0x7F7F (= 127+127i)"
            "\n"
    );
}

void emit_data(byte* data, size_t size_of_element, uint16_t num_elements)
{
    if(options.zmq_enabled)
        zsock_send(pub, "b", data, num_elements * size_of_element);
    else
        fwrite(data, size_of_element, num_elements, options.output_file);
}
void gsl_mat_emit_data(gsl_matrix_ushort *matrix, const OUTPUT_ORDER order)
{
    uint rows = matrix->size1;
    uint columns = matrix->size2;
    uint16_t buf[rows * columns];
    if(order == ROW_MAJOR) {
        // output each as-is, ie. no interleaving:
        for(int r = 0; r < rows; r++)
        {
            for (int c = 0; c < columns; c++)
            {
                buf[r * columns + c] = gsl_matrix_ushort_get(matrix, r, c);
            }
        }
    } else {
        // output each as-is, ie. no interleaving:
        for (int c = 0; c < columns; c++){
            for (int r = 0; r < rows; r++){
                buf[c*rows + r] = gsl_matrix_ushort_get(matrix, r, c);
            }
        }
    }
    emit_data((byte*) buf, sizeof(uint16_t), rows*columns);
}

void gsl_matrix_to_stdout(const gsl_matrix_ushort_view * matrix, int rows, int columns, const char * hint) {
// print the first rows/columns to stdout
// set row or colum to zero or negative value if you want to print all values in that direction
    if(false) {
        if (rows <= 0)
            rows = matrix->matrix.size1;
        if (columns <= 0)
            columns = matrix->matrix.size2;

        fprintf(stdout, "Matrix: %s \n", hint);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < columns; c++) {
                fprintf(stdout, "%4X ", gsl_matrix_ushort_get(matrix, r, c));
            }
            fprintf(stdout, "\n");
        }
    }
}
void gsl_vector_to_stdout(const gsl_vector_ushort_view *vec, uint32_t elements, const char * hint) {
    if(false) {
        fprintf(stdout, "Vector: %s\n", hint);
        for (int e = 0; e < elements; e++) {
            fprintf(stdout, "%4X ", gsl_vector_ushort_get(vec, e));
        }
        fprintf(stdout, "\n");
    }
}

void run()
{
    gsl_matrix *data, *data_a, *data_b;
    uint32_t acc, avg, count, event_count;
    uint64_t block_threshold, position;
    int64_t recv_shorts_tot = 0, sent_shorts_tot = 0;
    bool triggered;
    int i, n;
    uint8_t mag;

    // Two buffers lets us keep one block before a potential signal
    //data_a = gsl_matrix_alloc(options.block_size * sizeof(uint8_t) * 2, NUM_ZMQ_STREAMS);
    //data_b = gsl_matrix_alloc(options.block_size * sizeof(uint8_t) * 2, NUM_ZMQ_STREAMS);;
    //data = data_a;

    acc = 0;
    avg = 0;
    count = 0;
    event_count = 0;
    block_threshold = options.block_size * options.block_threshold / 100;
    position = options.offset;
    triggered = false;

    if (options.offset) {
        fseek(options.input_file, options.offset, SEEK_SET);
    }

    //while ((n = fread(data, sizeof(uint8_t)*2, options.block_size, options.input_file)) > 0) {
    zframe_t *zdata;
    while (! stop_program) {
        // grab data and loop if nothing to do.
        zdata = zframe_recv(sub);
        n = zframe_size(zdata) / (2 * NUM_ZMQ_STREAMS);

        gsl_matrix_ushort_view full_data_tmp = gsl_matrix_ushort_view_array((unsigned short *) zframe_data(zdata), n, NUM_ZMQ_STREAMS);

        //gsl_matrix_ushort *full_data = &full_data_tmp.matrix;
        gsl_matrix_ushort *full_data = gsl_matrix_ushort_alloc(NUM_ZMQ_STREAMS, n);
        gsl_matrix_ushort_transpose_memcpy(full_data, &full_data_tmp.matrix);
        if(n == 0) {
            if(options.verbose){
                fprintf(stdout, "Skipping loop, n = 0\n");
            }
            continue;
        }


        gsl_matrix_to_stdout(full_data, -1, -1, "Input: ZFrame");

        gsl_vector_ushort_view ch0 = gsl_matrix_ushort_row(full_data, 0);
        if (options.verbose) {
            recv_shorts_tot += n;
            fprintf(stdout, "%zu x %zu shorts received (total %zu)\n", full_data->size1, full_data->size2, recv_shorts_tot);
        }
        gsl_vector_to_stdout(&ch0, 10, "Input: ZFrame, 1st Channel");

        int elements_done = 0;
        // divide all data in chunks and progress them one by one:
        while (n >= options.block_size) {
            //if(options.verbose)
            //  fprintf(stderr, "Processing data. n = %i, elements_done = %i, ch0 length: %li\n", n, elements_done, ch0.vector.size);

            gsl_vector_ushort_view data = gsl_vector_ushort_subvector(&ch0.vector, elements_done, options.block_size);
            gsl_matrix_ushort_view all_channels = gsl_matrix_ushort_submatrix(full_data, 0, elements_done, NUM_ZMQ_STREAMS, options.block_size);
            elements_done += options.block_size;
            n -= options.block_size;

            acc = 0;
            count = 0;
            for (i = 0; i < options.block_size; i++) {
                // Fast approximation of the magnitude of this sample
                uint16_t sample = gsl_vector_ushort_get(&data.vector, i);
                mag = (uint8_t) (abs((int16_t) (sample & 0xFF) - INT8_MAX) +
                                 abs((int16_t) (sample >> 8) - INT8_MAX));
                if (mag > options.sample_threshold) {
                    count++;
                }
                acc += mag;
            }

            // Did this block have enough samples over the threshold?
            if (count > block_threshold) {
                if (options.verbose) {
                    if (!triggered) {
                        fprintf(stderr, "Output triggered from byte offset %lu to ...", position);
                        event_count++;
                    }
                }

                // TODO: Reenable when working:
                /*
                // Write the previous block, if configured
                if (options.padding_blocks) {
                  if (!triggered) {
                    emit_data(data == data_a ? data_b : data_a, sizeof(uint8_t) * 2,
                           options.block_size);
                  }
                }
                */

                // Write this block
                gsl_mat_emit_data(&all_channels.matrix, COLUMN_MAJOR);
                if(options.verbose) {
                    sent_shorts_tot += all_channels.matrix.size2;
                    fprintf(stdout, "%zu x %zu shorts sent (total %zu, diff between recv and sent %zd) \n",
                            all_channels.matrix.size1, all_channels.matrix.size2, sent_shorts_tot, recv_shorts_tot - sent_shorts_tot);
                }

                gsl_matrix_to_stdout((const gsl_matrix_ushort_view *) &all_channels.matrix, 4, 10, "Output");
                triggered = true;
            } else { // Block was not over the threshold
                // emit 0's:
                if(options.send_nullvec) {
                    gsl_matrix_ushort *neutralvec = gsl_matrix_ushort_alloc(options.block_size, NUM_ZMQ_STREAMS);
                    gsl_matrix_ushort_set_all(neutralvec, 0x7F7F);
                    gsl_mat_emit_data(neutralvec, ROW_MAJOR);
                }

                if (options.verbose) {
                    if (triggered) {
                        fprintf(stderr, "\b\b\b%lu\n", position);
                    }
                }

                // TODO: Reenable when working!
                /*
                // Write the block following the event, if configured
                if (triggered) {
                  if (options.padding_blocks) {
                    emit_data(data, sizeof(uint8_t) * 2, options.block_size);
                  }
                }
                 */

                // We try to only include blocks below the threshold in the average
                // to understand the background noise level
                if (options.auto_mode) {
                    avg += acc / options.block_size;
                    avg /= 2;
                }

                triggered = false;
            }

            position += n * (sizeof(uint8_t) * 2);
            //data = data == data_a ? data_b : data_a; // Swap buffers
            if(options.verbose) {
                fprintf(stdout, "\n######################\n");
            }

        }
        if(options.verbose && n > 0)
            fprintf(stderr, "Caution discarding %i samples from the packet!\n", n);
        zframe_destroy(&zdata);
    }

    if (options.verbose) {
        fprintf(stderr, "\n");
        fprintf(stderr, "%u events output\n", event_count);
    }


    //free(data_a);
    //free(data_b);
}

int main(int argc, char *argv[])
{
    int opt;
    signal(SIGINT, handle_interrupt);

    bool zmq_sub_isset = false;  // just to track if the options are set correctly.
    bool zmq_pub_isset = false;

    options.auto_mode         = DEFAULT_AUTO_MODE;
    options.block_size        = DEFAULT_BLOCK_SIZE;
    options.block_count       = DEFAULT_BLOCK_COUNT;
    options.block_threshold   = DEFAULT_BLOCK_THRESHOLD;
    options.offset            = DEFAULT_OFFSET;
    options.output_file       = DEFAULT_OUTPUT_FILE;
    options.padding_blocks    = DEFAULT_PADDING_BLOCKS;
    options.sample_threshold  = DEFAULT_SAMPLE_THRESHOLD;
    options.verbose           = DEFAULT_VERBOSE_MODE;
    options.zmq_enabled       = DEFAULT_ZMQ;
    options.send_nullvec      = false;

    while ((opt = getopt(argc, argv, "ab:c:o:pm:s:t:vy:z:n")) > 0) {
        switch (opt) {
            case 'a':
                options.auto_mode = true;
                break;
            case 'b':
                options.block_size = strtoul(optarg, NULL, 0);
                break;
            case 'c':
                options.block_count = strtoul(optarg, NULL, 0);
                break;
            case 'o':
                if (strcmp(optarg, "-") == 0) {
                    options.output_file = stdout;
                } else {
                    options.output_filename = optarg;
                }
                break;
            case 'p':
                options.padding_blocks = true;
                break;
            case 'm':
                options.sample_threshold = (uint8_t)strtoul(optarg, NULL, 0);
                break;
            case 's':
                options.offset = strtoull(optarg, NULL, 0);
                break;
            case 't':
                options.block_threshold = (uint8_t)strtoul(optarg, NULL, 0);
                break;
            case 'v':
                options.verbose = true;
                break;
            case 'y':
                options.zmq_enabled = true;
                options.zmq_sub_url = optarg;
                zmq_sub_isset = true;
                break;
            case 'z':
                options.zmq_enabled = true;
                options.zmq_pub_url = optarg;
                zmq_pub_isset = true;
                break;
            case 'n':
                options.send_nullvec = true;
                break;
            default:
                usage();
                exit(EXIT_FAILURE);
                break;
        }
    }




    if(options.zmq_enabled) {
        // verify that both SUB and PUB url are set:
        if (!(zmq_pub_isset && zmq_sub_isset)) {
            fprintf(stderr, "You specified one of the two ZMQ options. Both -y and -z must be set if you use ZMQ... Exiting");
            exit(EXIT_FAILURE);
        }

        sub = zsock_new_sub(options.zmq_sub_url, "");
        pub = zsock_new_pub(options.zmq_pub_url);

        // check that opening the sockets was sucessful:
        if(sub == NULL)
        {
            fprintf(stderr, "Something went wrong. ZMQ SUB socket %s not opened successfully!", options.zmq_sub_url);
            exit(EXIT_FAILURE);
        }

    } else {
        if (optind < argc) {
            if (strcmp(argv[optind], "-") == 0) {
                options.input_file = stdin;
            } else {
                options.input_file = fopen(argv[optind], "rb");
                if (options.input_file == NULL) {
                    perror(argv[optind]);
                    exit(EXIT_FAILURE);
                }
            }
        } else {
            usage();
            exit(EXIT_FAILURE);
        }

        options.output_file = fopen(optarg, "wb");
        if (options.output_file == NULL) {
            perror(optarg);
            exit(EXIT_FAILURE);
        }
    }

    if (options.verbose) {
        fprintf(stderr, "      Block Size: %u samples\n", options.block_size);
        if (options.block_count) {
            fprintf(stderr, "     Block Count: %u blocks\n", options.block_count);
        }
        fprintf(stderr, "          Offset: %lu\n", options.offset);
        fprintf(stderr, "Sample Threshold: %u\n", options.sample_threshold);
        fprintf(stderr, " Block Threshold: %u%%\n", options.block_threshold);
        fprintf(stderr, "      Input File: %s\n",
                options.input_file == stdin ? "stdin" : argv[optind]);
        fprintf(stderr, "     Output File: %s\n",
                options.output_file == stdout ? "stdout" : options.output_filename);
        fprintf(stderr, "\n");

        // TODO: add ZMQ output stuff.
    }

    run();

    if (! options.zmq_enabled && options.input_file != stdin) {
        fclose(options.input_file);
    }

    if (! options.zmq_enabled && options.output_file != stdout) {
        fclose(options.output_file);
    }

    if(options.zmq_enabled)
    {
        zmq_close(&pub);
        zmq_close(&sub);
    }

    return EXIT_SUCCESS;
}
