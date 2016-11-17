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

#define DEFAULT_AUTO_MODE           false
#define DEFAULT_BLOCK_COUNT         0
#define DEFAULT_BLOCK_SIZE          1024
#define DEFAULT_BLOCK_THRESHOLD     50
#define DEFAULT_OFFSET              0
#define DEFAULT_OUTPUT_FILE         stdout
#define DEFAULT_PADDING_BLOCKS      true
#define DEFAULT_SAMPLE_THRESHOLD    10
#define DEFAULT_VERBOSE_MODE        false

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
} options;

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
    "\n"
    );
}

void run()
{
  uint8_t *data, *data_a, *data_b;
  uint32_t acc, avg, count, event_count;
  uint64_t block_threshold, position;
  bool triggered;
  int i, n;
  uint8_t mag;

  // Two buffers lets us keep one block before a potential signal
  data_a = calloc(options.block_size, (sizeof(uint8_t) * 2));
  data_b = calloc(options.block_size, (sizeof(uint8_t) * 2));
  data = data_a;

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

  while ((n = fread(data, sizeof(uint8_t)*2, options.block_size, options.input_file)) > 0) {
    acc = 0;
    count = 0;
    for (i = 0; i < n; i++) {
      // Fast approximation of the magnitude of this sample
      mag = (uint8_t)(abs((uint16_t)data[i*2]   - INT8_MAX) +
                      abs((uint16_t)data[i*2+1] - INT8_MAX));
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

      // Write the previous block, if configured
      if (options.padding_blocks) {
        if (!triggered) {
          fwrite(data == data_a?data_b:data_a, sizeof(uint8_t)*2,
                 options.block_size, options.output_file);
        }
      }

      // Write this block
      fwrite(data, sizeof(uint8_t)*2, options.block_size, options.output_file);
      triggered = true;
    } else { // Block was not over the threshold
      if (options.verbose) {
        if (triggered) {
          fprintf(stderr, "\b\b\b%lu\n", position);
        }
      }

      // Write the block following the event, if configured
      if (triggered) {
        if (options.padding_blocks) {
          fwrite(data, sizeof(uint8_t)*2, options.block_size, options.output_file);
        }
      }

      // We try to only include blocks below the threshold in the average
      // to understand the background noise level
      if (options.auto_mode) {
        avg += acc / options.block_size;
        avg /= 2;
      }

      triggered = false;
    }

    position += n*(sizeof(uint8_t)*2);
    data = data == data_a ? data_b : data_a; // Swap buffers
  }

  if (options.verbose) {
    fprintf(stderr, "\n");
    fprintf(stderr, "%u events output\n", event_count);
  }

  free(data_a);
  free(data_b);
}

int main(int argc, char *argv[])
{
  int opt;

  options.auto_mode         = DEFAULT_AUTO_MODE;
  options.block_size        = DEFAULT_BLOCK_SIZE;
  options.block_count       = DEFAULT_BLOCK_COUNT;
  options.block_threshold   = DEFAULT_BLOCK_THRESHOLD;
  options.offset            = DEFAULT_OFFSET;
  options.output_file       = DEFAULT_OUTPUT_FILE;
  options.padding_blocks    = DEFAULT_PADDING_BLOCKS;
  options.sample_threshold  = DEFAULT_SAMPLE_THRESHOLD;
  options.verbose           = DEFAULT_VERBOSE_MODE;

  while ((opt = getopt(argc, argv, "ab:c:o:pm:s:t:v")) > 0) {
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
          options.output_file = fopen(optarg, "wb");
          if (options.output_file == NULL) {
            perror(optarg);
            exit(EXIT_FAILURE);
          }
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
      default:
        usage();
        exit(EXIT_FAILURE);
        break;
    }
  }

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
  }

  run();

  if (options.input_file != stdin) {
    fclose(options.input_file);
  }

  if (options.output_file != stdout) {
    fclose(options.output_file);
  }

  return EXIT_SUCCESS;
}
