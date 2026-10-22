# XNU for Bluejay

The current `BLUEJAY` machine configuration builds a plain arm64
`kernel.development.bluejay` with patches 0001–0085. The full kernel runs
iPhone X iOS 16.7.16 userland on Google Pixel 6a / GS101. Touch and clean GUI
presentation are confirmed on hardware.

The workflow in `.github/workflows/xnu.yml` uses the pinned
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build)
source and a macOS / Xcode 15 toolchain. The successful build used Xcode 15.0.1;
Xcode 16's `iig` does not match XNU 8796's IORPC ABI.

```sh
gh workflow run xnu.yml -R YOUR_ACCOUNT/ios-sweet --ref bluejay-port \
    -f machine_config=BLUEJAY -f macos_version=13.5
gh run download RUN_ID -R YOUR_ACCOUNT/ios-sweet -n xnu-kernel -D xnu/out-bluejay
(cd xnu/out-bluejay && sha256sum -c SHA256SUMS)
```

Use your own fork for builds. This port is maintained in
`dristotle/ios-bluejay`, branch `bluejay-port`; never push or merge Bluejay
work into the original `dipa-studio/ios-sweet` repository.

See the [project README](../README.md) for the verified boot image,
rootfs layout, build commands, ACPM touch driver and validation evidence.
