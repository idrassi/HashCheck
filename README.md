# HashCheck Shell Extension #

### Installation ###

The latest installer for Windows (Vista and later) can be found here:

<https://github.com/idrassi/HashCheck/releases/latest>

You can also install HashCheck with WinGet:

```text
winget install --id idrassi.HashCheckShellExtension --exact
```

### Verifying checksum files ###

Open a checksum file in Explorer to verify the files it lists. The verification window can be resized or maximized and remembers its size and position. If your monitor setup changes, the window is kept within the available screen area.

The summary highlights mismatches in red and unreadable files in yellow. The match counter turns green only when every listed file matches. High contrast mode keeps system colors.

Use **Copy...** to copy all results or just the summary. To copy specific rows, select them and press **Ctrl+C**; **Ctrl+A** selects all rows. Results still being calculated appear as `PENDING` in copied text.

If a checksum file can't be loaded, the error message includes the underlying cause. Verification stops if there isn't enough memory to load every entry. Large checksum files still require substantial memory because HashCheck loads them into memory before verification.

### Creating checksum files without Explorer ###

The installer places `HashCheckPackageHost.exe` beside `HashCheck.dll`. It can
launch HashCheck checksum creation directly, without using Explorer's context
menu:

```text
"%ProgramFiles%\HashCheck\HashCheckPackageHost.exe" /create [/noqueue] "C:\path\to\file-or-folder" ["C:\another\path" ...]
```

Paths supplied without a command are treated as `/create`, so dragging files or
folders onto `HashCheckPackageHost.exe` opens the normal HashCheck save dialog.
The same launcher also supports `/verify <checksum-file>` and `/options`.

To create one checksum file next to each selected file, use `/separate`. This
opens the same interactive hash type dialog as Explorer's **Create Separate Checksum Files...** command:

```text
"%ProgramFiles%\HashCheck\HashCheckPackageHost.exe" /create /separate [/noqueue] "C:\path\to\file-or-folder" ["C:\another\path" ...]
```

To create a checksum file without showing the save/progress UI, provide an
output path. The hash is inferred from the output extension when possible and
falls back to SHA-256 otherwise:

```text
"%ProgramFiles%\HashCheck\HashCheckPackageHost.exe" /create /output "C:\path\checksums.sha256" [/noqueue] "C:\path\to\folder"
```

The non-interactive mode also accepts `/hash`, `/encoding`, and `/eol`.
Supported hash names are `crc32`, `md5`, `sha1`, `sha256`, `sha512`,
`sha3-256`, `sha3-512`, `blake3`, `xxh3`, and `xxh128`:

```text
"%ProgramFiles%\HashCheck\HashCheckPackageHost.exe" /create /output "C:\path\checksums.blake3" /hash blake3 /encoding utf8 /eol lf "C:\path\to\folder"
```

Checksum creation, verification, and Properties tab hashing share one local
HashCheck queue. Non-interactive `/create /output` calls wait for any earlier
HashCheck job in the same logon session before hashing. Use `/noqueue` with
`/create` or `/verify` to bypass this queue for that command-line invocation.
Other concurrent HashCheck jobs will still serialize themselves; `/noqueue`
only opts this invocation out.

Under Wine, run the launcher from the HashCheck install directory and pass Wine
paths, for example:

```text
wine HashCheckPackageHost.exe /create "Z:\home\user\data"
wine HashCheckPackageHost.exe /create /output "Z:\home\user\checksums.sha256" "Z:\home\user\data"
```

#### Contributors ####

Kai Liu
Christopher Gurnee
David B. Trout  
Tim Schlueter  
Mounir IDRASSI


## Building from source ##

#### Compiler ####

Microsoft Visual Studio 2019 (the free Community edition works well).

See [WinGet packaging](installer/winget/README.md) for uninstall identity details
and instructions for running the isolated registry tests.

#### Localizations ####

Translation strings are stored as string table resources. These tables can be modified by editing [HashCheckTranslations.rc](HashCheckTranslations.rc).

#### Translation Contributors ####

Català: [@Hiro5](https://github.com/Hiro5)  
中文 (简体): "yumeyao"  
中文 (繁體): Jack Chang and [@Chocobo1](https://github.com/Chocobo1)  
Čeština: Václav Veselý  
Deutsch: "Rolf"  
Ελληνικά: "XhmikosR"  
Español: "Phare"  
Français: "mooms" and "user_hidden"  
Italiano: "Botta" and [@scara](https://github.com/scara)  
日本語: "yumeyao"  
한국어: JaeHyung Lee  
Nederlands: "Edwin"  
Polski: "RedWine"  
Português (Brasil): "0d14r3"  
Português (Portugal): "LPCA"  
Română: Oprea Nicolae, a.k.a. "Jaff"  
Pусский: Yurii Petrashko  
Svenska: Stefan Friman  
Türkçe: M. Ömer Gölgeli  
Yкраїнська: Yurii Petrashko

#### License and miscellanea ####

Standard 3-Clause BSD License.

Please refer to [license.txt](license.txt) for details about distribution and modification.

This software is based on software originally distributed at:  
<http://code.kliu.org/hashcheck/>  
<https://github.com/gurnec/HashCheck>

