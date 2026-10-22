# iOS on Google Pixel 6a (bluejay)

**iOS 16.7.16 boots on the Pixel 6a, with a working touchscreen.**

This fork ports Apple's open-source XNU 8796 kernel to Google's GS101/Tensor platform. It runs the iPhone X iOS 16.7.16 (20H392) userland, including dyld, launchd, backboardd and SpringBoard, from a journaled HFSX filesystem in `userdata`.

On **22 October 2026**, the phone reached the real iOS lock screen, the user confirmed touchscreen input, and console text stopped covering the GUI. Debug logging remains enabled and is saved to the persistent log. The successful kernel contains patches **0001–0085**.

The Bluejay work lives in [`dristotle/ios-bluejay`, branch `bluejay-port`](https://github.com/dristotle/ios-bluejay/tree/bluejay-port). It builds on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), the original Redmi Note 10 Pro port. Hardware details below refer to **Bluejay**; inherited SWEET drivers are not evidence of Pixel support.

## Verified status

| Area | Bluejay result |
| --- | --- |
| Boot | Unlocked Pixel bootloader → temporary `fastboot boot` → shim → XNU. Android boot header **v4**, **legacy LZ4** kernel compression |
| CPU and memory | Four Cortex-A55 cores, PSCI through SMC, GICv3 IRQs, 24 MHz timer, 16 KiB pages; kernel uses the upper 4 GiB RAM bank |
| Storage | GS101 UFS driver resumes the bootloader-initialized link and reads/writes the iOS HFSX root in `userdata` |
| iOS userspace | Real iPhone X 16.7.16 userland and shared cache; launchd, backboardd and SpringBoard run; lock screen observed |
| Display | Software rendering through IOSurface and IOMobileFramebuffer/AppleCLCD services, copied into the Pixel's 1080×2400 framebuffer. iOS sees an iPhone X display at 1125×2436 |
| Touch | ST FTM5 through GPIO SPI, ACPM PMIC power control and AP/AOC bus ownership handshake; input reaches the iOS HID digitizer |
| Logging | Kernel/debug and raw touch logs stay enabled. Text is hidden after GUI presentation; panic output can draw again |
| Watchdogs | Both inherited GS101 hardware watchdogs are stopped and checked by the shim. The shim's diagnostic timed reset is disabled |

The touch decoder supports ten contact IDs and enter/move/leave events. The user confirmed touch works; separate multitouch, suspend and long-duration tests have not been recorded.

There are no Bluejay drivers yet for Wi-Fi, Bluetooth, cellular, audio, camera, GPS, sensors, SEP/Face ID or GPU acceleration. Pixel power/volume input is not ported. Battery and several Apple platform/security services use bring-up stand-ins. The A78 and X1 cores are not enabled in the successful image. App navigation and daily-use behavior have not been fully assessed.

## How the port boots

```text
Pixel bootloader, unlocked
  -> fastboot boot: Android v4 boot image, legacy LZ4
     -> shim: live FDT from installed vendor_boot, framebuffer fix, watchdog stop
        -> embedded payload: plain arm64 XNU Mach-O + Apple Device Tree + boot args
           -> GS101 UFS -> journaled HFSX root in userdata
              -> dyld/shared cache -> launchd -> backboardd + SpringBoard
                 -> FTM5 events -> iOS HID digitizer
```

The shim applies the Bluejay framebuffer workaround: a 32-bit write of **`0x3061` to `0x1c300030`**. The framebuffer is at `0xfac00000`, with a 4320-byte stride.

The generated Apple Device Tree presents the iPhone X identity (`iPhone10,3` / `D22AP`) while supplying Pixel MMIO and CPU properties. `/chosen/mac-address-wifi0` provides a stable locally administered identity: without it, MobileGestalt waits for an unavailable network controller and LaunchServices never finishes seeding its database. This property supplies identity only; Wi-Fi still has no driver.

Touch power uses the firmware's **ACPM mailbox protocol**, including channel lookup by ID, validated 32-bit SRAM descriptors, queue bounds, sequence matching, barriers and PMIC status/readback checks. LDO25 and LDO26 supply 1.8 V and 3.3 V. The driver requests AP ownership and waits for the AOC grant before touching SPI, validates the FTM5 chip identity and SPI4 mode, then drains complete eight-byte FIFO events into `SweetHID`.

## Boot the saved installation

Use Android platform-tools on Windows or Linux. The successful boot image is:

```text
bluejay-xnu-0085-ios-v1-touch.img
```

With Bluejay in fastboot and the iOS root already installed:

```sh
fastboot getvar product             # must report bluejay
fastboot boot bluejay-xnu-0085-ios-v1-touch.img
```

The boot is temporary. On restart, use `fastboot boot` again to return to iOS. A halted diagnostic screen needs a manual restart because automatic reset is disabled.

The rootfs backup is **`ios16-bluejay-v1.simg`**, an Android sparse image that expands to a 16 GiB journaled HFSX volume. The existing installation does not need to be flashed again. To intentionally restore that root on the same verified layout, the command used successfully was:

```sh
fastboot -S 128M flash userdata ios16-bluejay-v1.simg
```

Restoring replaces `userdata`. Preserve **`win`, `esp`, GPT, boot/recovery, modem, persist and calibration partitions**. This port's `userdata` bounds are based on the verified device layout; inspect a different device's GPT before installing. Keep partition backups outside git.

## Build the kernel and boot image

Linux work was done in WSL **Ubuntu**. XNU needs a macOS toolchain, so the kernel is built through GitHub Actions. The tested run used `macos-14` and **Xcode 15.0.1**; its `iig` matches the XNU 8796 IORPC ABI. Xcode 16's generated ABI is incompatible with this source. Runner availability may require revisiting the toolchain later.

Fork this repository for your own CI. Dispatch the `BLUEJAY` configuration and download its artifact:

```sh
gh workflow run xnu.yml -R YOUR_ACCOUNT/ios-sweet --ref bluejay-port \
    -f macos_version=13.5 -f kernel_config=DEVELOPMENT \
    -f machine_config=BLUEJAY -f keep_going=true
gh run download RUN_ID -R YOUR_ACCOUNT/ios-sweet \
    -n xnu-kernel -D xnu/out-bluejay
(cd xnu/out-bluejay && sha256sum -c SHA256SUMS)
```

The workflow fetches [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) at a pinned commit, stages the pinned open-source dependencies and applies `xnu/patches/` in order. The output is **`kernel.development.bluejay`**, plain arm64 rather than arm64e.

Boot-image tools need Python 3, `aarch64-linux-gnu-gcc`, `lz4`, `mkbootimg`, GNU awk and cpio. Place the extracted iPhone X `d22ap.adt` at `research/ipsw-x/d22ap.adt` so the ADT builder can copy the real `/product` properties. The Apple device tree stays outside git.

From the repository root:

```sh
mkdir -p shim/out/bluejay
python3 tools/mkbluejayadt.py shim/out/bluejay/bluejay.adt
BOOT_ARGS='debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 ufslogsecs=2 sweetsample=lsd,installd,installcoordinationd,containermanagerd,mobileactivationd,SpringBoard,backboardd,logd,amfid sweetnull=lsd,installd,installcoordinationd,containermanagerd,mobileactivationd,SpringBoard,backboardd,logd,amfid sweetfb_noconsole=1'
python3 tools/mkpayload.py xnu/out-bluejay/kernel.development.bluejay \
    shim/out/bluejay/bluejay.adt shim/out/bluejay/payload-bluejay.bin --cmdline "$BOOT_ARGS"
shim/build-bluejay.sh out/bluejay/payload-bluejay.bin
# Output: shim/out/bluejay/boot.img
```

`build-bluejay.sh` verifies the v4 header, legacy LZ4 roundtrip and ARM64 Image size. The successful image used the boot arguments above and kept both debug channels and the two-second persistent-log flush enabled.

| Boot argument | Purpose |
| --- | --- |
| `cpus=4` | Use the four A55 cores |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Writable iOS root in `userdata` |
| `debug=0xc serial=3 -enable_kprintf_spam` | Keep kernel debug output enabled |
| `sweetfb_noconsole=1` | Hide framebuffer text after GUI swaps while retaining logs |
| `ufslogsecs=2` | Periodically flush the persistent log |
| `sweetsample=... sweetnull=...` | Retain focused userspace stack samples and daemon output |
| `wdt=-1 wdt_test=no_user_checkin` | Disable XNU watchdog check-in policy; the shim separately stops the physical GS101 watchdogs |

Touch and raw touch debug default to enabled. The tested image contains no argument disabling either.

## Root filesystem

The tested root was assembled from the **iPhone10,3/iPhone10,6 iOS 16.7.16 (20H392) IPSW**, combining OS, SystemOS and AppOS. It preserves numeric owners, modes, symlinks and hardlinks, exposes the cryptex libraries/shared cache, applies `sweet-root-tweaks.py`, and seeds Bluejay's ByHost migration preferences.

`tools/mkiosroot.sh` contains the common root assembly steps. `tools/hfs-root-build.py` contains the guest-side builder/finalizer used for Bluejay, including its migration identity and launch-critical file manifest. The guest needs the three read-only APFS trees at `/mnt/{os,sysos,appos}/root`, repository tools at `/mnt/tools`, and a separate blank image device for HFS construction. See [`NOTES.md`](NOTES.md) for the actual construction and verification record.

The immutable 16 GiB master passed the Apple HFS checker and a complete SHA256 readback after installation. On this host, the packaged Ubuntu checker returned an inconsistent primary-header error; rebuilding the recorded Apple source with the distro patches produced a successful full check. Source, checker and manifest hashes are recorded in the engineering notes. An existing verified root should be reused.

Apple firmware and the assembled iOS root stay in the user's local files and are not distributed through this repository.

## Debugging and validation

The persistent ring occupies the last **1025 × 4096-byte blocks of `userdata`**, outside the 16 GiB HFSX volume. After manually returning to a root-capable recovery, read it with:

```sh
mkdir -p research/logs
tools/sweet-log-pull.sh research/logs/boot-bluejay.txt
```

The log includes `BLUEJAY UFS`, `BLUEJAY ACPM`, `BLUEJAY TOUCH` and `SWEET HID` diagnostics. GUI console suppression changes drawing only; both normal console and direct `kprintf` output remain in the log, and panic drawing is retained. The final successful touch boot was left running at the user's request, so its recovery log has not been retrieved.

Before the hardware test, all four changed translation units compiled locally to arm64 Mach-O. The actual touch-driver host harness passed **55 checks under AddressSanitizer and UBSan**, covering ACPM queue validation, sequence wrap/stale replies, PMIC failures, GPIO preservation, denied AOC ownership and contact handling. CI source files matched those tested inputs, and the full macOS kernel build passed.

```sh
python3 tools/validate-repo.py
# After downloading/extracting the xnu-buildtree artifact:
python3 tools/check-bluejay-touch.py PATH_TO_PATCHED_XNU
```

`tools/xnu-cc.py` replays prior CI compile commands for local checks. QEMU helpers remain available for inherited kernel behavior; they do not emulate GS101 UFS, ACPM, FTM5 or the Pixel panel.

## Known successful artifacts

[GitHub Actions run 39795615846, attempt 2](https://github.com/dristotle/ios-bluejay/actions/runs/39795615846/attempts/2) built commit **`a61bc40858d544c520c4e9cf357dadc22dbdda0a`**. The user confirmed touch and removal of the console overlay on its first temporary boot.

| Artifact | SHA256 |
| --- | --- |
| `bluejay-xnu-0085-ios-v1-touch.img` | `ba415f83a8b5b4bf3a2fcea2d965bc9ce9c21a2f5d21075c74f805bd8ab34e09` |
| `ios16-bluejay-v1.simg` | `f93a24b968c392f90d888ab5eef5ab1a479a41b8a7dd366abeced35b71df8040` |
| Expanded 16 GiB root image | `f34aaadd00d92a2a3a3d3bb4d403671c895b8b648e4dc30c5581b85ffc1096fb` |
| `kernel.development.bluejay` | `85829ecccf0b6d7533c003223010cce409aee7b7f2a7534334c441558e39b05f` |

## Source layout and credits

| Path | Purpose |
| --- | --- |
| `shim/` | Boot shim, GS101 watchdog stop and framebuffer workaround; `build-bluejay.sh` packs header v4 |
| `xnu/patches/0001–0081` | Inherited XNU/iOS platform work; SWEET hardware drivers are guarded on Bluejay |
| `xnu/patches/0082–0083` | GS101 platform/UFS port and inherited build flags |
| `xnu/patches/0084` | FTM5, ACPM PMIC power, AP/AOC handshake and HID input |
| `xnu/patches/0085` | Persist direct `kprintf` while hiding text over the GUI |
| `tools/mkbluejayadt.py`, `mkpayload.py` | Pixel Apple Device Tree and kernel payload |
| `tools/hfs-root-build.py`, `sweet-root-tweaks.py` | Root assembly and bring-up configuration |
| `tools/check-bluejay-boot.py`, `check-bluejay-touch.py` | Boot-format and actual-driver checks |
| `NOTES.md` | Detailed bring-up record, evidence and hardware facts |

Thanks to the original [ios-sweet](https://github.com/dipa-studio/ios-sweet) project; Apple's open-source XNU, IOHIDFamily, libpthread and HFS; [BearSSL](https://bearssl.org); [darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build); [ipsw](https://github.com/blacktop/ipsw); and Google's published GS101 and touch sources. The user's Bluejay UFS and Windows touch drivers supplied the hardware starting points.

The touch protocol was checked against pinned Google commits: GS101 SoC **`5c48566f5564edbe69c2ea7c0b1dda9ce96a8816`**, FTS **`18f44fca40156b5acf95c0361421dce34bfcc654`**, and touch-common **`5b37ddff6b9e06e40cec2c094ec4d64882b222bc`**. Existing upstream licenses and source notices remain in place.
