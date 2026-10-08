# Third-party notices

## nlohmann/json

Version 3.11.3, https://github.com/nlohmann/json

The unmodified single header is in `vendor/json.hpp`. Copyright Niels Lohmann; MIT license included in `vendor/json.LICENSE.MIT` and the release package.

## System Informer SDK

https://github.com/winsiderss/systeminformer

SDK sources are downloaded on demand into the ignored `.deps/` directory. The build derives public declarations and an import library from pinned revision `bfc8145f2744a415319ccaaa8f1e32dd2cf1e596`. Upstream files retain their copyright notices and license. The release package includes the SDK's upstream license as `licenses/SystemInformer.LICENSE.txt`.

## Static Linux runtime libraries

The observer is built with GCC's C/C++ runtime and glibc on the build system. GCC runtime library components are covered by the GCC Runtime Library Exception where applicable; glibc is LGPL-2.1-or-later. Release packages include the corresponding available license notices and the observer's relocatable object files so recipients can relink the observer against modified compatible runtime libraries. See `relink/README.txt` in the package.
