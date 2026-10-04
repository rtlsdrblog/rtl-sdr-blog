# R82xx AGC/VGA regression

Run from the repository root; no receiver is required:

```sh
cc -std=c99 -Wall -Wextra -I include \
  tests/r82xx_agc_vga_test.c src/tuner_r82xx.c -o /tmp/r82xx-agc-vga-test
/tmp/r82xx-agc-vga-test
```

The test links the actual tuner implementation with a mock I2C transport.
It checks that enabling tuner AGC, tuning, and enabling AGC again preserve
VGA gain. It also checks LNA/mixer mode bits, manual gain, and propagation
of a failed VGA write. Frequencies exercised here are software coverage,
not RF measurements. The unpatched driver fails with VGA index 11 before
tuning and index 8 afterwards.

The fix uses the existing fixed-VGA helper (currently index 8) in AGC mode.
This changes the previous auto-last gain; it does not establish that index 8
is optimal for every application. LNA and mixer remain automatic.
