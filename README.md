# iq-squelch
iq-squelch is a tool for suppressing IQ data below a certain threshold. Put another way, it only outputs data over a certain threshold. It is useful for taking a large rtl_sdr recording and extracting events for later investigation, or to use between rtl_sdr and a more computationally expensive DSP application.

## Installation
```
make install
```
## Usage
```
$ iq-squelch [options] FILE

  FILE            Unsigned 8-bit IQ file to process ("-" for stdin)
  -a              Auto mode (threshold is above the average noise level)
  -b BLOCK_SIZE   Number of samples to read at a time (default: 1024)
  -c BLOCK_COUNT  Limit the total number of blocks to process
  -m MAGNITUDE    Sample magnitude threshold (0-255, default: 10)
  -o OUTPUT_FILE  Output file to write samples (default: stdout)
  -p              Output the block before and after a signal
  -s OFFSET       Starting byte offset within the input file
  -t THRESHOLD    Percentage of a block that must be over the threshold
                  before that block is output (default: 50%)
  -v              Verbose mode
```
Extracting events from a recording (must be in 8-bit unsigned format):
```
$./iq-squelch -o interesting_data.cu8 large_recording.cu8
```

Running between rtl_sdr and another application:
```
$rtl_sdr -f 915000000 -s 2400000 - | ./iq-squelch - | ./complex_dsp_app -
```
## Contributing
Contributions are welcome! Feel free to submit pull requests.
## License
iq-squelch is licenced under the GNU General Public License, Version 3. See LICENSE for more information.
