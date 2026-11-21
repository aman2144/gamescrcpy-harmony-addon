# Third-party notices

The GameScrcpy native extension API header is the v2.2.1 interface from [NuoRan/GameScrcpy](https://github.com/NuoRan/GameScrcpy/tree/v2.2.1), licensed under Apache-2.0. The host application is an external dependency. This project is unofficial.

The source distribution includes the extension and optional phone companion, licensed under Apache-2.0. The native module links dynamically to the FFmpeg codec and utility libraries provided by the GameScrcpy distribution. FFmpeg, the Microsoft runtime, and the Zig runtime have their own terms. When independently redistributing binaries, retain the appropriate notices and source or source offers required by those components. FFmpeg CLI and DevEco Studio/HDC are installed separately.

The UITest backend uses publicly documented screen capture and input commands. See the [Huawei UITest guide](https://developer.huawei.com/consumer/en/doc/doccenter-capabilities/uitest-guidelines). The optional DevEco backend contains original client adapter source. It depends on the separately installed DevEco Studio mirroring component. Huawei's proprietary component and JARs are supplied by the user's installation and are subject to Huawei's terms.

The Windows release ZIP contains the project's compiled x64 extension and the Zig, mingw-w64 and LLVM C++ runtime notices in `addon/licenses`. The preparation step copies FFmpeg and the Microsoft Visual C++ runtime and their notices from the user's own GameScrcpy 2.2.1 installation, then asks GameScrcpy to package and test them locally. Download GameScrcpy, DevEco Studio, optional FFmpeg CLI and standalone Python directly from their respective publishers.
