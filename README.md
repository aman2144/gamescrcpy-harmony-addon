# GameScrcpy 鸿蒙 USB 预览扩展

在 Windows 上通过 GameScrcpy 查看已授权鸿蒙手机的屏幕，支持点击、滑动、返回和主页。可选的鸿蒙声音助手通过系统授权采集手机播放的音频，输出 48 kHz 双声道 PCM。

这是面向开发者的非官方实验扩展，提供两种投屏方式。DevEco 模式通过本机 DevEco Studio 的投屏组件获取连续 H.264 视频，支持实时单指按下、移动、松开。UITest 兼容模式通过 HDC 逐张截屏，在电脑端编码为 H.264，松手时发送点击或滑动，适合查看界面。两种模式都需要手机解锁并授权 USB 调试。

## Windows 用户从这里开始

1. 在本项目的 [Releases](../../releases/latest) 下载 **GameScrcpy-Harmony-Windows-x64.zip**，全部解压到自己可写的文件夹。在[上游发行页](https://github.com/NuoRan/GameScrcpy/releases/tag/v2.2.1)获取 GameScrcpy **2.2.1 Windows x64**（安装版或完整解压的便携版）。
2. 双击 `Harmony-USB.cmd`，输入 **1**，选择上一步的 `GameScrcpy.exe`。脚本从你的 GameScrcpy 目录读取已有运行库，生成并校验同目录下的 `harmony-preview.gsext`。在 GameScrcpy 的扩展页导入这个文件、确认权限并启用扩展，随后退出 GameScrcpy。
3. 手机解锁，开启开发者模式和 USB 调试，用数据线连接并同意调试授权。再次双击 `Harmony-USB.cmd`，输入 **2**，选择同一个 `GameScrcpy.exe`。在启动的 GameScrcpy 里选「鸿蒙 USB 投屏」及「鸿蒙 USB（单指）」。

| 所选方式 | 需要准备 |
| --- | --- |
| 选 2，连续投屏 | [DevEco Studio](https://developer.huawei.com/consumer/cn/deveco-studio/) 的 Windows 完整安装及 HarmonyOS SDK（含 HDC、JDK 和投屏组件）。已验证的组件版本见下文。 |
| 选 3，逐张截屏 | PATH 中可运行的 HDC 和带 libx264 的 FFmpeg。完整 DevEco Studio 和 JDK 可省略。 |
| 启动菜单 | 默认会使用 DevEco Studio 所附的 Python，或系统中的 Python 3.10+。将 DevEco 安装在自定义位置且未装 Python 的用户可设置 `HARMONY_STUDIO` 为其安装目录。 |

Windows 发布包已包含本扩展的 x64 原生模块，使用者无需安装 Zig、下载 GameScrcpy 源码或编译 C++。手机声音是可选功能，需要另外签名安装声音助手；普通 USB 投屏和触控不需要手机端 HAP。若 GameScrcpy 已打开，先将它完全退出，以便新进程读取本次桥接会话。

## 从源码构建

1. 准备 Windows x64、Python 3.10 或更新版本、Zig 0.15.2 和 DevEco Studio。使用 UITest 兼容模式还需要包含 libx264 的 FFmpeg 命令行工具。可将 `hdc` 和 `ffmpeg` 加入 PATH，或启动时分别以 `--hdc` 和 `--ffmpeg` 指定路径。
2. 从上游获取 GameScrcpy v2.2.1 的源码和 x64 Windows 程序。源码中的 `third_party/ffmpeg` 目录应包含头文件和导入库。[上游版本](https://github.com/NuoRan/GameScrcpy/tree/v2.2.1)
3. 指定这两个目录，构建脚本会编译原生扩展、运行宿主测试，生成供本机导入的 `harmony-preview.gsext` 和供其他 Windows 用户下载的 `GameScrcpy-Harmony-Windows-x64.zip`。

```powershell
.\build-addon.ps1 -GameScrcpySource <GameScrcpy源码目录> -GameScrcpyInstall <含gs.exe的程序目录>
```

4. 在 GameScrcpy 中导入生成的扩展包，查看其权限并启用，再退出 GameScrcpy。

## 使用

手机用数据线接入电脑，解锁并允许 USB 调试。仅连接要投屏的一台设备，选用以下命令之一。

```powershell
python bridge.py --game "<GameScrcpy.exe的路径>" --studio "<DevEco Studio安装目录>"
```

此命令用本机 DevEco 自带的 JDK 临时编译项目的 Java 适配器，运行时加载本机安装的投屏组件。适配器编译测试覆盖 `hosScrcpy-1.0.20-beta`；该组件随 Studio 版本变化，相关接口的长期兼容性取决于 Studio。若本机版本不兼容，可以使用 UITest 模式。

```powershell
python bridge.py --game "<GameScrcpy.exe的路径>"
```

UITest 模式要求手机支持 `hdc shell uitest screenCap` 和 `uitest uiInput`。手机和电脑分别使用临时截图文件，桥接正常结束时清理这些文件。非正常掉电可能留下最后一张临时截图。

启动后，在 GameScrcpy 中选扩展提供的「HarmonyOS USB」视频源和单指触控方式。关闭通过脚本启动的 GameScrcpy 即结束会话。

## 可选的声音助手

在 DevEco Studio 打开 `phone-companion`，使用 SDK 26 和你自己的调试签名编译并安装。再次运行启动命令，加上 `--audio`。手机弹出声音助手后，点「开启声音传输」，完成系统权限提示，并在 GameScrcpy 中选择「HarmonyOS playback audio」。系统和具体应用的播放捕获策略会影响声音是否可用。

手机与电脑内部的套接字均绑定到回环地址。使用短期随机令牌识别本次会话，音频经 HDC 端口转发。测试预览需要可用的本机 TCP 52781，开启音频另需电脑端 52783、手机端 52782。

## 验证范围

本版已检查 x64 原生模块编译、清单、GameScrcpy 隔离宿主加载和扩展打包。Java 适配器使用本机 DevEco 库完成编译，声音助手已编译成未签名 HAP。UITest 桥接使用合成画面和模拟触控进行离线验证。根据 [v0.2.0 发布说明](../../releases/tag/v0.2.0)，发布页的 Windows ZIP 已在一台授权的鸿蒙真机上验证 UITest 兼容模式：成功采集屏幕帧、编码为 H.264 并在 Windows 上解码。连续投屏模式的真机帧率、真机触控、游戏兼容性和手机音频仍待实测。

扩展和声音助手源码采用 Apache-2.0。GameScrcpy、FFmpeg 和其他外部依赖分别遵循各自的许可证。[第三方说明](NOTICE.md)
