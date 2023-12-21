rtl-sdr
turns your Realtek RTL2832 based DVB dongle into a SDR receiver
======================================================================

For more information see:
https://osmocom.org/projects/rtl-sdr/wiki

**********************************************************************

*****************************
Modified RTL-SDR Blog Version
*****************************

25-August-2023: Brought up to date with latest Osmocom upstream, and added some new features like auto-direct sampling. Also made the force bias tee function stronger, so it cant be turned off when the bias tee is forced, even when calling the bias tee function. Remove the rtl_tcp ringbuffer changes as this seems to cause more trouble that it helps.

1) VCO PLL current fix - Improves stability at frequencies above ~1.5 GHz https://www.rtl-sdr.com/beta-testing-a-modified-rtl-sdr-driver-for-l-band-heat-issues/
2) Enabled direct sampling for rtl_tcp
3) Hack to force the bias tee to always be on by setting the unused IR endpoint bit to 0 in the EEPROM. Example to force the BT to be always ON "rtl_eeprom -b 1", to remove forced BT "rtl_eeprom -b 0"
4) Repurposed "offset tuning" to toggle bias tee ON/OFF. We can now use the "offset tuning" button in SDR# and other programs to toggle the bias tee if there is no specific button in the GUI.
5) Support added for R828D RTL-SDR Blog V4 based dongles.
6) Auto direct sampling. For R820T/R860 dongles like the RTL-SDR Blog V3 the code will automatically change to direct sampling mode when the frequency is below 24 MHz. It will also automatically change back to normal sampling when the frequency is above 24 MHz. There is no need to manually change the sampling mode anymore for these dongles.

BIAS TEE NOTE: Always take care that you do not enable the bias tee when the device is connected to a short circuited antenna unless there is an inline LNA. However. if you did by accident, don't worry as the circuit is dually protected with a self-resetting thermal fuse and built in protection on the LDO. Just try not to short it out for days at a time, otherwise you could eventually degrade the thermal fuse.

Note that hack 3) will only work if your system is using this driver. If your system or software is using another driver fork, then the EEPROM information will not be read. So make sure you completely clean your system of the previous drivers first (with the information below) and ensure you run sudo make install after compiling. On Windows, make sure you are using this code's rtlsdr.dll file.

********************
Installation (Linux):
********************

***NOTE***
If you previously installed librtlsdr-dev via the package manager you should remove this first BEFORE installing these drivers. To completely remove these drivers use the following commands

sudo apt purge ^librtlsdr
sudo rm -rvf /usr/lib/librtlsdr* /usr/include/rtl-sdr* /usr/local/lib/librtlsdr* /usr/local/include/rtl-sdr* /usr/local/include/rtl_* /usr/local/bin/rtl_*

***Now install the drivers***

sudo apt update
sudo apt install libusb-1.0-0-dev git cmake pkg-config
git clone https://github.com/rtlsdrblog/rtl-sdr-blog
cd rtl-sdr-blog/
mkdir build
cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make
sudo make install
sudo cp ../rtl-sdr.rules /etc/udev/rules.d/
sudo ldconfig
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee --append /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf

***********************
Alternative Debian Package Installation Method:
***********************

If you have a system reliant on the Debian packages (eg. FlightRadar24, FlightAware, ADSBExchange images) you can update them directly using this method:

sudo apt update
sudo apt install libusb-1.0-0-dev git cmake
sudo apt install debhelper

git clone https://github.com/rtlsdrblog/rtl-sdr-blog
cd rtl-sdr-blog
sudo dpkg-buildpackage -b --no-sign
cd ..

sudo dpkg -i librtlsdr0_*.deb
sudo dpkg -i librtlsdr-dev_*.deb
sudo dpkg -i rtl-sdr_*.deb

**********************
Installation (MacOS):
**********************

First make sure you have installed homebrew and xcode. Open a terminal and run:

brew uninstall rtl-sdr
brew install cmake
brew install libusb
brew install pkgconfig
git clone https://github.com/rtlsdrblog/rtl-sdr-blog
cd rtl-sdr-blog
mkdir build
cd build/
cmake ../
make LIBRARY_PATH=/usr/local/lib
sudo make install

***********************
Installation (Windows):
***********************

Download the Release.zip file from the Releases page. For SDR# extract the rtlsdr.dll file from the x86 folder to the SDR#. For most other x64 programs, use the rtlsdr.dll file in the x64 folder.
