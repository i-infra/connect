Nonolith Connect
====================

This server runs in the background and manages USB communication with Nonolith
devices and provides their services to clients that connect over WebSocket or
REST. Multiple programs can access devices simultaneously, for example
controlling a connected device with a custom script while simultaneously
monitoring and collecting data with
[Pixelpulse][pixelpulse].

Nonolith Connect is licensed under the terms of the [GNU GPLv3+][gpl].

[pixelpulse]: http://github.com/nonolith/pixelpulse
[gpl]: http://www.gnu.org/licenses/gpl.html

Dependencies
------------

`nonolith-connect` is written in C++ (C++17) and builds with CMake. It requires:

  * [Boost](https://www.boost.org/) (1.70+) -- `asio`, `beast`, `regex`, `thread`
  * [libusb](https://libusb.info/) (1.0.16+) -- system install via package manager
  * [libjson](http://sourceforge.net/projects/libjson/) -- included in-tree

### macOS

    brew install boost libusb cmake pkg-config

### Linux (Debian/Ubuntu)

    sudo apt install cmake pkg-config libboost-regex-dev libboost-thread-dev libusb-1.0-0-dev

Build instructions
------------------

    git clone https://github.com/nonolith/connect.git
    cd connect
    cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

The `nonolith-connect` executable will be in `build/`.

Installation notes
------------------

*This section documents the behavior of each platform's binary installers. Unless
you are installing for the first time from source, these are taken care of by the installer.*

### Windows

The CEE's VID/PID (0x59e3:0xCEE1 and 0x59e3:0xBBBB) must be registered with WinUSB using a [libwdi][libwdi]
such as Zadig. A shortcut is placed in the Start Menu's application folder so it launches on startup.

[libwdi]: http://sourceforge.net/apps/mediawiki/libwdi/index.php?title=Main_Page

### Linux

A user and group `nonolithd` are created to sandbox the daemon. An init script is installed to
`/etc/init.d/nonolith-connect ` and configured to start on boot.

Place the following in `/lib/udev.d/60-nonolith-connect.rules`:

    SUBSYSTEM=="usb", ATTR{idVendor}=="59e3", ATTR{idProduct}=="cee1", GROUP:="nonolithd", MODE="0666"
    SUBSYSTEM=="usb", ATTR{idVendor}=="59e3", ATTR{idProduct}=="bbbb", GROUP:="nonolithd", MODE="0666"
    
to configure the permissions of the device when attached. Restart `udev` after installing the rules file.

### Mac OS X

A Launch Agent is installed (`/Library/LaunchAgents/com.nonolith.connect.agent.plist`) to launch
Nonolith Connect on boot.

Command line flags
------------------

The following flags can be passed to `nonolith-connect`

**debug** - dump JSON communications to the console  
**allow-remote** - listen on remote interfaces, rather than just localhost  
**allow-any-origin** - disable origin checking, allowing scripts from any web page origin domain to connect  
 
API Documentation
-----------------

  * [REST API v1](http://wiki.nonolithlabs.com/cee/Simple_REST_API_v1/) (recommended)
  * [Websocket API v0](http://wiki.nonolithlabs.com/cee/Websocket_API_V0/)