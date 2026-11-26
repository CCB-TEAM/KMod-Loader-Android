param(
  [ValidateSet('dev','release')][string]$Flavor = 'dev',
  [string]$Root = 'D:\KARDS\default\android',
  [string]$InputApk = 'D:\KARDS\default\game\kards-1.apk',
  [string]$AndroidStudio = 'D:\AndroidStudio'
)
# Builds loader + core for the chosen flavour, repacks the APK, aligns and signs it.
#   dev     -> kards_mod_dev.apk  (Lua console, lua.log, hot-load core from kmod/dev, dev override of the built-in manager)
#   release -> kards_mod.apk      (bundled core and optional layout detector)
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$File, [string[]]$Arguments) {
  & $File @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}
$t = (Resolve-Path $Root).Path
$InputApk = (Resolve-Path $InputApk).Path
$sdk = "$env:LOCALAPPDATA\Android\Sdk"; $bt = "$sdk\build-tools\36.0.0"
$ndk = "$sdk\ndk\28.2.13676358"; $cm = 'C:\Program Files\CMake\bin\cmake.exe'
$devOn = if ($Flavor -eq 'dev') { 'ON' } else { 'OFF' }
$suffix = if ($Flavor -eq 'dev') { 'build' } else { 'build-release' }
$out = if ($Flavor -eq 'dev') { "$t\kards_mod_dev.apk" } else { "$t\kards_mod.apk" }
$common = @('-G','Unix Makefiles',"-DCMAKE_MAKE_PROGRAM=$ndk\prebuilt\windows-x86_64\bin\make.exe","-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake",'-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-28','-DCMAKE_BUILD_TYPE=Release',"-DKMOD_DEV=$devOn", "-DKMOD_SDK_DIR=$t\dump\out\CppSDK")

foreach ($proj in 'loader','core') {
  Invoke-Checked $cm (@('-S', "$t\$proj", '-B', "$t\$proj\$suffix") + $common)
  Invoke-Checked $cm (@('--build', "$t\$proj\$suffix", '--', '-j6') )
}
$strip = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
$coreOut = "$t\core\libkmodcore.$Flavor.so"; Copy-Item "$t\core\$suffix\libkmodcore.so" $coreOut -Force
Invoke-Checked $strip @('--strip-all', $coreOut)
# libs are added under their real names, so stage copies with the right file names
$stage = "$t\stage-$Flavor"; Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$t\loader\$suffix\libkmod.so" "$stage\libkmod.so"; Copy-Item $coreOut "$stage\libkmodcore.so"
$libs = @("$stage\libkmod.so", "$stage\libkmodcore.so")
$md7 = "$t\core\$suffix\libkmodmd7.so"
if (Test-Path $md7) { Copy-Item $md7 "$stage\libkmodmd7.so"; $libs += "$stage\libkmodmd7.so" }
$libs += (Get-ChildItem $t\loader\extra\*.so -ErrorAction SilentlyContinue | % FullName)

# SplashActivity.<clinit> -> System.loadLibrary("kmod"): regenerate the patched classes.dex from the original every time
$jbr = "$AndroidStudio\jbr\bin"
$dexJars = "$AndroidStudio\plugins\android\lib\smali-dexlib2-3.0.9.jar;$AndroidStudio\plugins\android\lib\smali-util-3.0.9.jar;$AndroidStudio\plugins\android\lib\jsr305-2.0.1.jar;$AndroidStudio\lib\*"
$env:JAVA_TOOL_OPTIONS = ''
Invoke-Checked "$jbr\java.exe" @('-cp', "$t\tools\out;$dexJars", 'DexTool', 'patch', "$t\x\classes.dex", "$t\classes_patched.dex", 'Lcom/epicgames/unreal/SplashActivity;', 'kmod')

$extra = @()
if ($Flavor -eq 'dev') {
  $androidJar = "$sdk\platforms\android-35\android.jar"
  $jo = "$t\overlay\out"; Remove-Item $jo -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $jo | Out-Null
  $env:JAVA_TOOL_OPTIONS = ''
  Invoke-Checked "$jbr\javac.exe" @('--release', '11', '-cp', $androidJar, '-d', $jo, "$t\overlay\kmod\Overlay.java")
  $classes = Get-ChildItem $jo -Recurse -Filter *.class | ForEach-Object FullName
  $env:JAVA_HOME = "$AndroidStudio\jbr"
  Invoke-Checked "$bt\d8.bat" (@('--release', '--lib', $androidJar, '--output', $jo) + $classes)
  Move-Item "$jo\classes.dex" "$jo\classes2.dex" -Force
  $extra = @('--classes2', "$jo\classes2.dex")
}
$un = "$t\kards_unaligned_$Flavor.apk"; $al = "$t\kards_aligned_$Flavor.apk"
Invoke-Checked 'python' (@('-I', "$t\tools\repack.py", $InputApk, $un, "$t\classes_patched.dex") + $libs + $extra)
Remove-Item $al, $out -ErrorAction SilentlyContinue
Invoke-Checked "$bt\zipalign.exe" @('-p', '-f', '4', $un, $al)
Invoke-Checked "$bt\apksigner.bat" @('sign', '--ks', "$t\dev.keystore", '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--ks-key-alias', 'dev', '--out', $out, $al)
Remove-Item $un, $al -ErrorAction SilentlyContinue
Get-Item $out | Select-Object Name, Length
