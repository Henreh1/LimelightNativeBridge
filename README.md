# Limelight Native Bridge

The native runtime companion for Limelight, the Dead as Disco mod manager.

The bridge runs through UE4SS and provides the native operations Limelight needs for live mod loading, including runtime IoStore mounting and refreshing loaded character assets.

## Status

This project is in private development and is intended to be distributed only as part of a compatible Limelight release. I will update this when needed, or not. I will probably forget.

## Compatibility

- Windows 64-bit
- Dead as Disco
- UE4SS experimental build `c2ac246447a8bcd92541070cb474044e7a2bbbe6`
- Limelight `0.1.x`

## Building

Run these commands from the repository folder:

```powershell
cd C:\LimelightNative\LimelightNativeBridge
```

```powershell
cmake -S . -B ..\bridge-repo-build -G "Visual Studio 17 2022" -A x64 -DUE4SS_ROOT="C:\LimelightNative\RE-UE4SS"
```

```powershell
cmake --build ..\bridge-repo-build --config Game__Shipping__Win64 --target LimelightNativeBridge --parallel
```

The compiled bridge is written to:

```text
C:\LimelightNative\bridge-repo-build\Game__Shipping__Win64\LimelightNativeBridge.dll
```

## Distribution

Limelight packages a tested bridge build with each compatible application release. Users should not need to download or configure the bridge separately.
